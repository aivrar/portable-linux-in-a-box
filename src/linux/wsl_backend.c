/*
 * WSL2 Backend — Persistent Shell
 *
 * Launches ONE bash process at startup via WslLaunch and keeps it alive
 * for the entire session. All exec() calls pipe commands through stdin
 * and read output from stdout. Zero new processes after startup.
 *
 * This eliminates the console window flashing that occurred when each
 * exec() spawned a separate wsl.exe process.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "backend.h"
#include <stdio.h>
#include <stdarg.h>

/* --------------------------------------------------------------------------
 * WSL API typedefs
 * -------------------------------------------------------------------------- */
typedef BOOL  (WINAPI *PFN_WslIsDistributionRegistered)(PCWSTR);
typedef HRESULT (WINAPI *PFN_WslRegisterDistribution)(PCWSTR, PCWSTR);
typedef HRESULT (WINAPI *PFN_WslLaunch)(PCWSTR, PCWSTR, BOOL,
                                        HANDLE, HANDLE, HANDLE, HANDLE *);
typedef HRESULT (WINAPI *PFN_WslConfigureDistribution)(PCWSTR, ULONG, ULONG);

#define WSL_FLAG_ENABLE_INTEROP       0x1
#define WSL_FLAG_APPEND_NT_PATH       0x2
#define WSL_FLAG_ENABLE_DRIVE_MOUNTING 0x4
#define WSL_FLAGS_DEFAULT (WSL_FLAG_ENABLE_INTEROP | WSL_FLAG_APPEND_NT_PATH | \
                           WSL_FLAG_ENABLE_DRIVE_MOUNTING)

#define READ_BUF_SIZE 4096
#define DEFAULT_TIMEOUT_MS 30000
#define SHELL_PIPE_SIZE (256 * 1024)

/* --------------------------------------------------------------------------
 * Private state
 * -------------------------------------------------------------------------- */
typedef struct {
    HMODULE wslapi;
    wchar_t distro_name[256];
    int     running;
    char    last_error_buf[512];
    const linux_config_t *config;

    PFN_WslIsDistributionRegistered pIsRegistered;
    PFN_WslRegisterDistribution    pRegister;
    PFN_WslLaunch                  pLaunch;
    PFN_WslConfigureDistribution   pConfigure;

    /* Persistent shell */
    HANDLE  shell_process;
    HANDLE  shell_stdin_write;
    HANDLE  shell_stdout_read;
    CRITICAL_SECTION exec_lock;
    int     shell_alive;
    DWORD   cmd_sequence;
    int     cs_initialized;
} wsl_state_t;

/* --------------------------------------------------------------------------
 * Helpers
 * -------------------------------------------------------------------------- */
static void wsl_set_error(wsl_state_t *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->last_error_buf, sizeof(state->last_error_buf), fmt, ap);
    va_end(ap);
}

static int utf8_to_utf16(const char *utf8, wchar_t *out, int out_chars) {
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_chars);
}

static void wsl_set_hresult_error(wsl_state_t *state, const char *context,
                                  HRESULT hr) {
    char sys_msg[256] = {0};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, (DWORD)hr, 0, sys_msg, sizeof(sys_msg) - 1, NULL);
    size_t len = strlen(sys_msg);
    while (len > 0 && (sys_msg[len-1] == '\n' || sys_msg[len-1] == '\r'))
        sys_msg[--len] = '\0';
    wsl_set_error(state, "%s: HRESULT 0x%08lX -- %s",
                  context, (unsigned long)hr, sys_msg[0] ? sys_msg : "Unknown error");
}

/* Read from pipe with PeekNamedPipe (non-blocking check) + ReadFile.
 * Returns bytes read, 0 on no data available, -1 on pipe error. */
static int pipe_read_available(HANDLE pipe, char *buf, int max) {
    DWORD avail = 0;
    if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL))
        return -1;
    if (avail == 0) return 0;
    DWORD to_read = avail < (DWORD)max ? avail : (DWORD)max;
    DWORD bytes_read = 0;
    if (!ReadFile(pipe, buf, to_read, &bytes_read, NULL))
        return -1;
    return (int)bytes_read;
}

/* --------------------------------------------------------------------------
 * Persistent shell management
 * -------------------------------------------------------------------------- */
static int shell_launch(wsl_state_t *state) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };

    /* Create stdin pipe (we write, child reads) */
    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdin_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&stdin_read, &stdin_write, &sa, SHELL_PIPE_SIZE)) {
        wsl_set_error(state, "CreatePipe(stdin) failed: %lu", GetLastError());
        return -1;
    }
    SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0);

    /* Create stdout pipe (child writes, we read) — also used for stderr */
    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&stdout_read, &stdout_write, &sa, SHELL_PIPE_SIZE)) {
        CloseHandle(stdin_read); CloseHandle(stdin_write);
        wsl_set_error(state, "CreatePipe(stdout) failed: %lu", GetLastError());
        return -1;
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

    /* Launch wsl.exe with CREATE_NO_WINDOW to prevent visible console.
     * We use CreateProcessW instead of WslLaunch so we can control
     * the window flags — WslLaunch always shows a console. */
    wchar_t cmdline[512];
    _snwprintf(cmdline, 512,
        L"wsl.exe -d %s -- /bin/bash --norc --noprofile",
        state->distro_name);
    cmdline[511] = L'\0';

    STARTUPINFOW si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(NULL, cmdline, NULL, NULL, TRUE,
                              CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    /* Close child-side handles */
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!ok) {
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        wsl_set_error(state, "CreateProcess(wsl.exe bash) failed: %lu",
                      GetLastError());
        return -1;
    }
    CloseHandle(pi.hThread);
    HANDLE process = pi.hProcess;

    state->shell_process = process;
    state->shell_stdin_write = stdin_write;
    state->shell_stdout_read = stdout_read;

    /* Wait for shell to be ready — send probe and wait for response */
    const char *probe = "echo __SHELL_READY__\n";
    DWORD written;
    WriteFile(stdin_write, probe, (DWORD)strlen(probe), &written, NULL);

    /* Read until __SHELL_READY__ appears (timeout 10s) */
    growbuf_t buf;
    growbuf_init(&buf, 1024);
    DWORD start = GetTickCount();
    int ready = 0;

    while ((GetTickCount() - start) < 10000 && !ready) {
        char tmp[READ_BUF_SIZE];
        int n = pipe_read_available(stdout_read, tmp, sizeof(tmp) - 1);
        if (n > 0) {
            tmp[n] = '\0';
            growbuf_append(&buf, tmp, (size_t)n);
            if (strstr(buf.data, "__SHELL_READY__"))
                ready = 1;
        } else if (n < 0) {
            break; /* pipe error */
        } else {
            Sleep(50);
        }
    }
    growbuf_free(&buf);

    if (!ready) {
        wsl_set_error(state, "Shell did not respond within 10 seconds");
        TerminateProcess(process, 1);
        CloseHandle(process);
        CloseHandle(stdin_write);
        CloseHandle(stdout_read);
        state->shell_process = NULL;
        state->shell_stdin_write = INVALID_HANDLE_VALUE;
        state->shell_stdout_read = INVALID_HANDLE_VALUE;
        return -1;
    }

    state->shell_alive = 1;
    return 0;
}

static void shell_close(wsl_state_t *state) {
    if (state->shell_alive) {
        /* Try graceful exit */
        const char *exit_cmd = "exit\n";
        DWORD written;
        WriteFile(state->shell_stdin_write, exit_cmd,
                  (DWORD)strlen(exit_cmd), &written, NULL);
        if (WaitForSingleObject(state->shell_process, 2000) == WAIT_TIMEOUT)
            TerminateProcess(state->shell_process, 1);
        state->shell_alive = 0;
    }
    if (state->shell_stdin_write != INVALID_HANDLE_VALUE) {
        CloseHandle(state->shell_stdin_write);
        state->shell_stdin_write = INVALID_HANDLE_VALUE;
    }
    if (state->shell_stdout_read != INVALID_HANDLE_VALUE) {
        CloseHandle(state->shell_stdout_read);
        state->shell_stdout_read = INVALID_HANDLE_VALUE;
    }
    if (state->shell_process) {
        CloseHandle(state->shell_process);
        state->shell_process = NULL;
    }
}

/* --------------------------------------------------------------------------
 * Backend interface: start
 * -------------------------------------------------------------------------- */
static linux_error_t wsl_start(linux_backend_t *self,
                               const linux_config_t *config) {
    wsl_state_t *state = (wsl_state_t *)self->opaque;

    if (state->running)
        return LINUX_ERR_ALREADY_RUNNING;

    state->config = config;

    /* Convert distro name to UTF-16 */
    const char *name = config->distro_name ? config->distro_name : "linux-template";
    if (!utf8_to_utf16(name, state->distro_name,
                       (int)(sizeof(state->distro_name) / sizeof(wchar_t)))) {
        wsl_set_error(state, "Failed to convert distro name to UTF-16");
        return LINUX_ERR_INVALID_ARG;
    }

    LINUX_LOG(config, "WSL2: checking for distro '%s'", name);

    /* Check if already registered */
    BOOL registered = state->pIsRegistered(state->distro_name);
    if (!registered) {
        if (config->tar_gz_path) {
            wchar_t tar_path_w[MAX_PATH];
            if (!utf8_to_utf16(config->tar_gz_path, tar_path_w, MAX_PATH)) {
                wsl_set_error(state, "Failed to convert tar.gz path to UTF-16");
                return LINUX_ERR_INVALID_ARG;
            }
            LINUX_LOG(config, "WSL2: registering distro from '%s'",
                      config->tar_gz_path);
            HRESULT hr = state->pRegister(state->distro_name, tar_path_w);
            if (FAILED(hr)) {
                wsl_set_hresult_error(state, "WslRegisterDistribution", hr);
                return LINUX_ERR_START_FAILED;
            }
            state->pConfigure(state->distro_name, 0, WSL_FLAGS_DEFAULT);
        } else {
            wsl_set_error(state,
                "Distribution '%s' is not registered and no tar.gz path provided.\n"
                "Either provide a tar_gz_path in config, or install a WSL distro:\n"
                "  wsl --install -d Ubuntu", name);
            return LINUX_ERR_START_FAILED;
        }
    }

    /* Initialize concurrency */
    InitializeCriticalSection(&state->exec_lock);
    state->cs_initialized = 1;
    state->cmd_sequence = 0;

    /* Launch persistent shell */
    LINUX_LOG(config, "WSL2: launching persistent shell");
    if (shell_launch(state) != 0) {
        return LINUX_ERR_START_FAILED;
    }

    LINUX_LOG(config, "WSL2: persistent shell ready for '%s'", name);
    state->running = 1;
    return LINUX_OK;
}

/* --------------------------------------------------------------------------
 * Backend interface: exec (through persistent shell)
 * -------------------------------------------------------------------------- */
static linux_error_t wsl_exec(linux_backend_t *self,
                              const char *command,
                              char **stdout_buf,
                              char **stderr_buf,
                              int *exit_code) {
    wsl_state_t *state = (wsl_state_t *)self->opaque;

    if (!state->running) {
        wsl_set_error(state, "Backend not started");
        return LINUX_ERR_NOT_RUNNING;
    }

    if (stdout_buf) *stdout_buf = NULL;
    if (stderr_buf) *stderr_buf = NULL;
    if (exit_code)  *exit_code = -1;

    LINUX_LOG(state->config, "WSL2 exec: %s", command);

    EnterCriticalSection(&state->exec_lock);

    /* Restart shell if it died */
    if (!state->shell_alive) {
        LINUX_LOG(state->config, "WSL2: shell died, restarting...");
        shell_close(state);
        if (shell_launch(state) != 0) {
            LeaveCriticalSection(&state->exec_lock);
            return LINUX_ERR_EXEC_FAILED;
        }
    }

    linux_error_t result = LINUX_OK;
    DWORD seq = ++state->cmd_sequence;

    /* Build unique markers for this command */
    char start_marker[64], end_prefix[64];
    snprintf(start_marker, sizeof(start_marker),
             "__XCMD_%lu_START__", (unsigned long)seq);
    snprintf(end_prefix, sizeof(end_prefix),
             "__XCMD_%lu_END_", (unsigned long)seq);

    /* Wrap the command:
     * echo <START>; (command) 2>&1; echo "<END>$?__" */
    size_t cmd_len = strlen(command) + 256;
    char *wrapped = (char *)malloc(cmd_len);
    if (!wrapped) {
        LeaveCriticalSection(&state->exec_lock);
        return LINUX_ERR_OUT_OF_MEMORY;
    }
    snprintf(wrapped, cmd_len,
        "echo %s; (%s) 2>&1; echo \"%s$?__\"\n",
        start_marker, command, end_prefix);

    /* Write command to shell stdin */
    DWORD written;
    BOOL ok = WriteFile(state->shell_stdin_write, wrapped,
                        (DWORD)strlen(wrapped), &written, NULL);
    free(wrapped);

    if (!ok) {
        state->shell_alive = 0;
        LeaveCriticalSection(&state->exec_lock);
        wsl_set_error(state, "Failed to write to shell stdin");
        return LINUX_ERR_PIPE_FAILED;
    }

    /* Read output until end marker appears */
    growbuf_t output;
    growbuf_init(&output, READ_BUF_SIZE);
    char read_buf[READ_BUF_SIZE];
    int found_start = 0, found_end = 0;
    int cmd_exit_code = -1;
    DWORD timeout = state->config->timeout_ms ? state->config->timeout_ms
                                              : DEFAULT_TIMEOUT_MS;
    DWORD start_time = GetTickCount();

    while (!found_end) {
        /* Check timeout */
        if ((GetTickCount() - start_time) > timeout) {
            /* Send Ctrl+C to interrupt the running command */
            char ctrlc = '\x03';
            WriteFile(state->shell_stdin_write, &ctrlc, 1, &written, NULL);
            result = LINUX_ERR_TIMEOUT;
            wsl_set_error(state, "Command timed out after %lu ms", timeout);
            break;
        }

        /* Check for available data without blocking */
        int n = pipe_read_available(state->shell_stdout_read,
                                     read_buf, sizeof(read_buf) - 1);
        if (n > 0) {
            read_buf[n] = '\0';
            growbuf_append(&output, read_buf, (size_t)n);

            /* Look for start marker (skip everything before it) */
            if (!found_start) {
                char *sp = strstr(output.data, start_marker);
                if (sp) {
                    sp += strlen(start_marker);
                    if (*sp == '\n') sp++;
                    size_t skip = (size_t)(sp - output.data);
                    memmove(output.data, sp, output.len - skip + 1);
                    output.len -= skip;
                    found_start = 1;
                }
            }

            /* Look for end marker */
            if (found_start) {
                char *ep = strstr(output.data, end_prefix);
                if (ep) {
                    char *code_str = ep + strlen(end_prefix);
                    cmd_exit_code = atoi(code_str);
                    /* Truncate output at marker */
                    *ep = '\0';
                    output.len = (size_t)(ep - output.data);
                    /* Strip trailing newline */
                    while (output.len > 0 &&
                           output.data[output.len - 1] == '\n')
                        output.data[--output.len] = '\0';
                    found_end = 1;
                }
            }
        } else if (n < 0) {
            /* Pipe broken — shell died */
            state->shell_alive = 0;
            result = LINUX_ERR_PIPE_FAILED;
            wsl_set_error(state, "Shell pipe broken");
            break;
        } else {
            /* No data yet — brief sleep to avoid busy-wait */
            Sleep(10);
        }
    }

    if (exit_code) *exit_code = cmd_exit_code;

    if (stdout_buf)
        *stdout_buf = growbuf_finish(&output);
    else
        growbuf_free(&output);

    /* stderr is merged into stdout via 2>&1 */
    if (stderr_buf) *stderr_buf = NULL;

    LeaveCriticalSection(&state->exec_lock);
    return result;
}

/* --------------------------------------------------------------------------
 * Backend interface: stop, destroy, etc.
 * -------------------------------------------------------------------------- */
static linux_error_t wsl_stop(linux_backend_t *self) {
    wsl_state_t *state = (wsl_state_t *)self->opaque;
    shell_close(state);
    state->running = 0;
    return LINUX_OK;
}

static int wsl_is_running(linux_backend_t *self) {
    wsl_state_t *state = (wsl_state_t *)self->opaque;
    return state->running;
}

static const char *wsl_last_error(linux_backend_t *self) {
    wsl_state_t *state = (wsl_state_t *)self->opaque;
    return state->last_error_buf;
}

static void wsl_destroy(linux_backend_t *self) {
    if (!self) return;
    wsl_state_t *state = (wsl_state_t *)self->opaque;
    if (state) {
        if (state->running)
            wsl_stop(self);
        if (state->cs_initialized)
            DeleteCriticalSection(&state->exec_lock);
        if (state->wslapi)
            FreeLibrary(state->wslapi);
        free(state);
    }
    free(self);
}

/* --------------------------------------------------------------------------
 * Constructor
 * -------------------------------------------------------------------------- */
linux_backend_t *linux_backend_create_wsl(void) {
    HMODULE lib = LoadLibraryExW(L"wslapi.dll", NULL,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib) return NULL;

    PFN_WslIsDistributionRegistered pIsReg =
        (PFN_WslIsDistributionRegistered)GetProcAddress(lib,
            "WslIsDistributionRegistered");
    PFN_WslRegisterDistribution pReg =
        (PFN_WslRegisterDistribution)GetProcAddress(lib,
            "WslRegisterDistribution");
    PFN_WslLaunch pLaunch =
        (PFN_WslLaunch)GetProcAddress(lib, "WslLaunch");
    PFN_WslConfigureDistribution pCfg =
        (PFN_WslConfigureDistribution)GetProcAddress(lib,
            "WslConfigureDistribution");

    if (!pIsReg || !pReg || !pLaunch || !pCfg) {
        FreeLibrary(lib);
        return NULL;
    }

    linux_backend_t *b = (linux_backend_t *)calloc(1, sizeof(linux_backend_t));
    if (!b) { FreeLibrary(lib); return NULL; }

    wsl_state_t *state = (wsl_state_t *)calloc(1, sizeof(wsl_state_t));
    if (!state) { free(b); FreeLibrary(lib); return NULL; }

    state->wslapi       = lib;
    state->pIsRegistered = pIsReg;
    state->pRegister     = pReg;
    state->pLaunch       = pLaunch;
    state->pConfigure    = pCfg;
    state->shell_process = NULL;
    state->shell_stdin_write = INVALID_HANDLE_VALUE;
    state->shell_stdout_read = INVALID_HANDLE_VALUE;
    state->shell_alive = 0;
    state->cmd_sequence = 0;

    b->type       = LINUX_BACKEND_WSL2;
    b->name       = "WSL2";
    b->opaque     = state;
    b->start      = wsl_start;
    b->stop       = wsl_stop;
    b->is_running = wsl_is_running;
    b->destroy    = wsl_destroy;
    b->exec       = wsl_exec;
    b->last_error = wsl_last_error;

    return b;
}

#endif /* _WIN32 */
