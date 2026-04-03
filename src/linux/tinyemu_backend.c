/*
 * TinyEMU Backend — Software emulation fallback
 *
 * Uses TinyEMU (Fabrice Bellard's RISC-V system emulator) as a subprocess.
 * This is the universal fallback: works on ANY machine with no special
 * hardware or Windows features required.
 *
 * TinyEMU boots a RISC-V Linux kernel and communicates via stdin/stdout
 * pipes connected to the emulated serial console.
 *
 * Performance: ~5-20x slower than native (software emulation), but
 * it ALWAYS works. No GPU access. Suitable for CLI tools, scripts,
 * small AI models, and development tasks.
 *
 * Requirements:
 *   - temu executable (bundled or in PATH)
 *   - RISC-V Linux kernel (bbl64.bin or similar)
 *   - Root filesystem image (ext2 or similar)
 *
 * Distribution size: ~5-20MB (emulator + kernel + rootfs)
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "backend.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define TINYEMU_DEFAULT_MEM     256
#define READ_BUF_SIZE           4096
#define BOOT_DETECT_TIMEOUT_MS  30000
#define DEFAULT_TIMEOUT_MS      30000

typedef struct {
    int     running;
    char    last_error_buf[512];
    const linux_config_t *config;

    /* TinyEMU process */
    HANDLE  process;
    HANDLE  job;

    /* Stdio pipes */
    HANDLE  stdin_write;    /* We write commands here → TinyEMU stdin */
    HANDLE  stdout_read;    /* We read output here ← TinyEMU stdout */

    /* Paths */
    char    temu_exe[MAX_PATH];
    char    kernel_path[MAX_PATH];
    char    rootfs_path[MAX_PATH];
    char    config_path[MAX_PATH]; /* Generated JSON config file */

    int     booted;         /* Shell prompt detected */
    CRITICAL_SECTION exec_lock;
    int     cs_initialized;
} tinyemu_state_t;

static void temu_set_error(tinyemu_state_t *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->last_error_buf, sizeof(st->last_error_buf), fmt, ap);
    va_end(ap);
}

/* Find TinyEMU executable — check bundled location, then PATH */
static int find_temu(tinyemu_state_t *st) {
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *s = strrchr(exe_dir, '\\');
    if (s) *s = '\0';

    /* Try temu/temu.exe relative to our exe */
    snprintf(st->temu_exe, sizeof(st->temu_exe),
             "%s\\temu\\temu.exe", exe_dir);
    if (GetFileAttributesA(st->temu_exe) != INVALID_FILE_ATTRIBUTES)
        return 1;

    /* Try same directory */
    snprintf(st->temu_exe, sizeof(st->temu_exe),
             "%s\\temu.exe", exe_dir);
    if (GetFileAttributesA(st->temu_exe) != INVALID_FILE_ATTRIBUTES)
        return 1;

    /* Try PATH */
    char found[MAX_PATH];
    if (SearchPathA(NULL, "temu.exe", NULL, MAX_PATH, found, NULL)) {
        strncpy(st->temu_exe, found, sizeof(st->temu_exe) - 1);
        st->temu_exe[sizeof(st->temu_exe) - 1] = '\0';
        return 1;
    }

    return 0;
}

/* Generate TinyEMU JSON config file */
static int generate_config(tinyemu_state_t *st) {
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *s = strrchr(exe_dir, '\\');
    if (s) *s = '\0';

    snprintf(st->config_path, sizeof(st->config_path),
             "%s\\linux\\temu_config.json", exe_dir);

    FILE *f = fopen(st->config_path, "w");
    if (!f) {
        temu_set_error(st, "Cannot create config file: %s", st->config_path);
        return 0;
    }

    /* Convert backslashes to forward slashes for JSON */
    char kernel_json[MAX_PATH * 2], rootfs_json[MAX_PATH * 2];
    strncpy(kernel_json, st->kernel_path, sizeof(kernel_json) - 1);
    kernel_json[sizeof(kernel_json) - 1] = '\0';
    strncpy(rootfs_json, st->rootfs_path, sizeof(rootfs_json) - 1);
    rootfs_json[sizeof(rootfs_json) - 1] = '\0';
    for (char *p = kernel_json; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = rootfs_json; *p; p++) if (*p == '\\') *p = '/';

    fprintf(f,
        "{\n"
        "    \"version\": 1,\n"
        "    \"machine\": \"riscv64\",\n"
        "    \"memory_size\": %d,\n"
        "    \"bios\": \"%s\",\n",
        TINYEMU_DEFAULT_MEM, kernel_json);

    /* If rootfs exists, add it as a drive */
    if (st->rootfs_path[0])
        fprintf(f,
            "    \"drive0\": { \"file\": \"%s\" },\n",
            rootfs_json);

    fprintf(f,
        "    \"cmdline\": \"console=hvc0 root=/dev/vda rw\"\n"
        "}\n");

    fclose(f);
    return 1;
}

/* Read from pipe with timeout. Returns bytes read, 0 on timeout, -1 on error. */
static int read_pipe_timeout(HANDLE pipe, char *buf, int max, DWORD timeout_ms) {
    DWORD avail = 0;
    DWORD start = GetTickCount();

    while ((GetTickCount() - start) < timeout_ms) {
        if (PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD to_read = avail < (DWORD)max ? avail : (DWORD)max;
            DWORD bytes_read;
            if (ReadFile(pipe, buf, to_read, &bytes_read, NULL))
                return (int)bytes_read;
            return -1;
        }
        Sleep(50);
    }
    return 0;
}

/* ============================================================
 * Backend interface
 * ============================================================ */
static linux_error_t temu_start(linux_backend_t *self,
                                const linux_config_t *config) {
    tinyemu_state_t *st = (tinyemu_state_t *)self->opaque;
    if (st->running) return LINUX_ERR_ALREADY_RUNNING;
    st->config = config;

    /* Find TinyEMU */
    if (!find_temu(st)) {
        temu_set_error(st,
            "TinyEMU (temu.exe) not found. Place it in a 'temu' subdirectory "
            "next to this executable, or add it to PATH.\n"
            "Download: https://bellard.org/tinyemu/");
        return LINUX_ERR_NOT_AVAILABLE;
    }

    LINUX_LOG(config, "TinyEMU found: %s", st->temu_exe);

    /* Find kernel */
    if (config->kernel_path) {
        strncpy(st->kernel_path, config->kernel_path,
                sizeof(st->kernel_path) - 1);
        st->kernel_path[sizeof(st->kernel_path) - 1] = '\0';
    } else {
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        char *s = strrchr(exe_dir, '\\');
        if (s) *s = '\0';
        /* Try multiple names */
        const char *names[] = {"bbl64.bin", "fw_jump.bin", "kernel-riscv64.bin", NULL};
        st->kernel_path[0] = '\0';
        for (int i = 0; names[i]; i++) {
            snprintf(st->kernel_path, sizeof(st->kernel_path),
                     "%s\\linux\\%s", exe_dir, names[i]);
            if (GetFileAttributesA(st->kernel_path) != INVALID_FILE_ATTRIBUTES)
                break;
            st->kernel_path[0] = '\0';
        }
    }

    if (!st->kernel_path[0]) {
        temu_set_error(st,
            "RISC-V kernel not found. Provide --kernel path/to/bbl64.bin "
            "or place it in the linux/ subdirectory.");
        return LINUX_ERR_START_FAILED;
    }

    /* Find rootfs */
    if (config->rootfs_path) {
        strncpy(st->rootfs_path, config->rootfs_path,
                sizeof(st->rootfs_path) - 1);
        st->rootfs_path[sizeof(st->rootfs_path) - 1] = '\0';
    } else {
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        char *s = strrchr(exe_dir, '\\');
        if (s) *s = '\0';
        snprintf(st->rootfs_path, sizeof(st->rootfs_path),
                 "%s\\linux\\rootfs-riscv64.ext2", exe_dir);
        if (GetFileAttributesA(st->rootfs_path) == INVALID_FILE_ATTRIBUTES)
            st->rootfs_path[0] = '\0';
    }

    LINUX_LOG(config, "TinyEMU kernel: %s", st->kernel_path);
    LINUX_LOG(config, "TinyEMU rootfs: %s",
              st->rootfs_path[0] ? st->rootfs_path : "(none)");

    /* Generate config */
    if (!generate_config(st))
        return LINUX_ERR_START_FAILED;

    /* Create pipes for stdin/stdout */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE stdin_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&stdin_read, &st->stdin_write, &sa, 0)) {
        temu_set_error(st, "Failed to create stdin pipe: error %lu", GetLastError());
        return LINUX_ERR_PIPE_FAILED;
    }
    if (!CreatePipe(&st->stdout_read, &stdout_write, &sa, 0)) {
        CloseHandle(stdin_read);
        CloseHandle(st->stdin_write);
        st->stdin_write = INVALID_HANDLE_VALUE;
        temu_set_error(st, "Failed to create stdout pipe: error %lu", GetLastError());
        return LINUX_ERR_PIPE_FAILED;
    }
    SetHandleInformation(st->stdin_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(st->stdout_read, HANDLE_FLAG_INHERIT, 0);

    /* Create job object for cleanup */
    st->job = CreateJobObject(NULL, NULL);
    if (st->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(st->job,
            JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    /* Build command line */
    char cmdline[MAX_PATH * 3];
    snprintf(cmdline, sizeof(cmdline), "\"%s\" \"%s\"",
             st->temu_exe, st->config_path);

    LINUX_LOG(config, "TinyEMU command: %s", cmdline);

    /* Launch TinyEMU */
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stdout_write;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);

    CloseHandle(stdin_read);
    CloseHandle(stdout_write);

    if (!ok) {
        temu_set_error(st, "Failed to start TinyEMU: error %lu", GetLastError());
        return LINUX_ERR_START_FAILED;
    }

    CloseHandle(pi.hThread);
    st->process = pi.hProcess;
    if (st->job)
        AssignProcessToJobObject(st->job, st->process);

    /* Wait for boot — look for shell prompt in output */
    LINUX_LOG(config, "TinyEMU: waiting for Linux to boot...");

    growbuf_t boot_output;
    growbuf_init(&boot_output, 4096);
    DWORD timeout = config->timeout_ms ? config->timeout_ms : BOOT_DETECT_TIMEOUT_MS;
    DWORD start_time = GetTickCount();
    st->booted = 0;

    while ((GetTickCount() - start_time) < timeout && !st->booted) {
        char buf[READ_BUF_SIZE];
        int n = read_pipe_timeout(st->stdout_read, buf, sizeof(buf), 500);
        if (n > 0) {
            growbuf_append(&boot_output, buf, (size_t)n);
            /* Detect shell prompt: "#", "$", "login:", or "~ #" */
            if (strstr(boot_output.data, "# ") ||
                strstr(boot_output.data, "$ ") ||
                strstr(boot_output.data, "login:")) {
                st->booted = 1;
            }
        } else if (n < 0) {
            break;
        }

        /* Check process is still running */
        DWORD exit_code;
        if (GetExitCodeProcess(st->process, &exit_code) &&
            exit_code != STILL_ACTIVE) {
            char *boot_text = growbuf_finish(&boot_output);
            temu_set_error(st, "TinyEMU exited with code %lu. Output:\n%.500s",
                          exit_code, boot_text ? boot_text : "(none)");
            free(boot_text);
            return LINUX_ERR_START_FAILED;
        }
    }

    growbuf_free(&boot_output);

    /* If login prompt detected, send root login */
    if (st->booted) {
        DWORD written;
        const char *login_cmd = "root\n";
        WriteFile(st->stdin_write, login_cmd, (DWORD)strlen(login_cmd),
                  &written, NULL);
        Sleep(500);

        /* Drain any login output */
        char drain[READ_BUF_SIZE];
        read_pipe_timeout(st->stdout_read, drain, sizeof(drain), 500);
    }

    InitializeCriticalSection(&st->exec_lock);
    st->cs_initialized = 1;
    st->running = 1;
    LINUX_LOG(config, "TinyEMU backend started%s",
              st->booted ? " (shell ready)" : " (may still be booting)");
    return LINUX_OK;
}

static linux_error_t temu_exec(linux_backend_t *self, const char *command,
                               char **stdout_buf, char **stderr_buf,
                               int *exit_code) {
    tinyemu_state_t *st = (tinyemu_state_t *)self->opaque;
    if (!st->running) return LINUX_ERR_NOT_RUNNING;

    if (stdout_buf) *stdout_buf = NULL;
    if (stderr_buf) *stderr_buf = NULL;
    if (exit_code)  *exit_code = -1;

    LINUX_LOG(st->config, "TinyEMU exec: %s", command);

    EnterCriticalSection(&st->exec_lock);

    /* Drain any pending output first */
    char drain[READ_BUF_SIZE];
    while (read_pipe_timeout(st->stdout_read, drain, sizeof(drain), 100) > 0)
        ;

    /* Send command with exit-code marker */
    size_t cmd_len = strlen(command) + 64;
    char *full_cmd = (char *)malloc(cmd_len);
    if (!full_cmd) return LINUX_ERR_OUT_OF_MEMORY;
    snprintf(full_cmd, cmd_len, "%s 2>&1; echo __EXIT:$?__\n", command);

    DWORD written;
    WriteFile(st->stdin_write, full_cmd, (DWORD)strlen(full_cmd), &written, NULL);
    free(full_cmd);

    /* Read output until exit marker */
    growbuf_t output;
    growbuf_init(&output, 4096);

    DWORD timeout = st->config->timeout_ms ? st->config->timeout_ms
                                           : DEFAULT_TIMEOUT_MS;
    DWORD start_time = GetTickCount();
    int found_marker = 0;

    while ((GetTickCount() - start_time) < timeout && !found_marker) {
        char buf[READ_BUF_SIZE];
        int n = read_pipe_timeout(st->stdout_read, buf, sizeof(buf), 500);
        if (n > 0) {
            growbuf_append(&output, buf, (size_t)n);

            /* Check for exit marker */
            char *marker = strstr(output.data, "__EXIT:");
            if (marker) {
                char *end = strstr(marker, "__\r");
                if (!end) end = strstr(marker, "__\n");
                if (!end) end = strstr(marker + 7, "__");
                if (end) {
                    int code = atoi(marker + 7);
                    if (exit_code) *exit_code = code;
                    *marker = '\0';
                    output.len = (size_t)(marker - output.data);
                    found_marker = 1;
                }
            }
        } else if (n < 0) {
            break;
        }
    }

    /* Strip echoed command from beginning */
    char *result = growbuf_finish(&output);
    if (result && stdout_buf) {
        /* Find first newline (end of echoed command) */
        char *nl = strchr(result, '\n');
        if (nl) {
            *stdout_buf = strdup(nl + 1);
            free(result);
        } else {
            *stdout_buf = result;
        }
    } else {
        free(result);
    }

    LeaveCriticalSection(&st->exec_lock);
    return found_marker ? LINUX_OK : LINUX_ERR_TIMEOUT;
}

static linux_error_t temu_stop(linux_backend_t *self) {
    tinyemu_state_t *st = (tinyemu_state_t *)self->opaque;

    if (st->stdin_write != INVALID_HANDLE_VALUE) {
        /* Try graceful shutdown */
        DWORD written;
        const char *cmd = "poweroff\n";
        WriteFile(st->stdin_write, cmd, (DWORD)strlen(cmd), &written, NULL);
        if (st->process)
            WaitForSingleObject(st->process, 3000);
    }

    if (st->process) {
        TerminateProcess(st->process, 0);
        WaitForSingleObject(st->process, 2000);
        CloseHandle(st->process);
        st->process = NULL;
    }
    st->running = 0;
    return LINUX_OK;
}

static int temu_is_running(linux_backend_t *self) {
    tinyemu_state_t *st = (tinyemu_state_t *)self->opaque;
    return st->running;
}

static const char *temu_last_error(linux_backend_t *self) {
    tinyemu_state_t *st = (tinyemu_state_t *)self->opaque;
    return st->last_error_buf;
}

static void temu_destroy(linux_backend_t *self) {
    if (!self) return;
    tinyemu_state_t *st = (tinyemu_state_t *)self->opaque;
    if (st) {
        if (st->running) temu_stop(self);
        if (st->cs_initialized)
            DeleteCriticalSection(&st->exec_lock);
        if (st->stdin_write != INVALID_HANDLE_VALUE)
            CloseHandle(st->stdin_write);
        if (st->stdout_read != INVALID_HANDLE_VALUE)
            CloseHandle(st->stdout_read);
        if (st->job) CloseHandle(st->job);
        /* Clean up generated config */
        if (st->config_path[0])
            DeleteFileA(st->config_path);
        free(st);
    }
    free(self);
}

/* ============================================================
 * Constructor
 * ============================================================ */
linux_backend_t *linux_backend_create_tinyemu(void) {
    linux_backend_t *b = (linux_backend_t *)calloc(1, sizeof(linux_backend_t));
    if (!b) return NULL;

    tinyemu_state_t *st = (tinyemu_state_t *)calloc(1, sizeof(tinyemu_state_t));
    if (!st) { free(b); return NULL; }

    st->stdin_write = INVALID_HANDLE_VALUE;
    st->stdout_read = INVALID_HANDLE_VALUE;

    b->type       = LINUX_BACKEND_TINYEMU;
    b->name       = "TinyEMU";
    b->opaque     = st;
    b->start      = temu_start;
    b->stop       = temu_stop;
    b->is_running = temu_is_running;
    b->destroy    = temu_destroy;
    b->exec       = temu_exec;
    b->last_error = temu_last_error;

    return b;
}

#endif /* _WIN32 */
