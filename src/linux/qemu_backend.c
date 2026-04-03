/*
 * QEMU Backend — manages QEMU as a subprocess
 *
 * Auto-detects WHPX (Hyper-V) for hardware acceleration.
 * Falls back to TCG (software emulation) when WHPX is unavailable.
 * Communicates with the guest Linux via SSH over a forwarded port.
 *
 * This backend allows the template to run a full Linux environment
 * on ANY Windows machine — no WSL required.
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "backend.h"
#include <stdio.h>
#include <stdarg.h>

#pragma comment(lib, "ws2_32.lib")

#define QEMU_SSH_PORT     2222
#define QEMU_MONITOR_PORT 4444
#define QEMU_DEFAULT_MEM  "512M"
#define QEMU_DEFAULT_CPUS 2

typedef struct {
    int     running;
    char    last_error_buf[512];
    const linux_config_t *config;

    /* QEMU process */
    HANDLE  process;
    HANDLE  job;        /* Job object to ensure cleanup */

    /* Paths */
    char    qemu_exe[MAX_PATH];
    char    kernel_path[MAX_PATH];
    char    rootfs_path[MAX_PATH];

    /* Connection */
    int     ssh_port;
    int     whpx_available;
} qemu_state_t;

static void qemu_set_error(qemu_state_t *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->last_error_buf, sizeof(state->last_error_buf), fmt, ap);
    va_end(ap);
}

/* Check if WHPX acceleration is available */
static int detect_whpx(void) {
    HMODULE h = LoadLibraryExW(L"WinHvPlatform.dll", NULL,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) return 0;

    /* Check if the hypervisor is actually present */
    typedef HRESULT (WINAPI *PFN_WHvGetCapability)(int, void*, UINT32, UINT32*);
    PFN_WHvGetCapability pGetCap = (PFN_WHvGetCapability)
        GetProcAddress(h, "WHvGetCapability");

    int available = 0;
    if (pGetCap) {
        /* WHvCapabilityCodeHypervisorPresent = 0 */
        BOOL present = FALSE;
        UINT32 written = 0;
        HRESULT hr = pGetCap(0, &present, sizeof(present), &written);
        if (SUCCEEDED(hr) && present)
            available = 1;
    }
    FreeLibrary(h);
    return available;
}

/* Find QEMU executable — check bundled location, then PATH */
static int find_qemu(qemu_state_t *state) {
    /* Check bundled: same directory as our exe */
    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *last_slash = strrchr(exe_dir, '\\');
    if (last_slash) *last_slash = '\0';

    /* Try qemu/qemu-system-x86_64.exe relative to our exe */
    snprintf(state->qemu_exe, sizeof(state->qemu_exe),
             "%s\\qemu\\qemu-system-x86_64.exe", exe_dir);
    if (GetFileAttributesA(state->qemu_exe) != INVALID_FILE_ATTRIBUTES)
        return 1;

    /* Try just in the same directory */
    snprintf(state->qemu_exe, sizeof(state->qemu_exe),
             "%s\\qemu-system-x86_64.exe", exe_dir);
    if (GetFileAttributesA(state->qemu_exe) != INVALID_FILE_ATTRIBUTES)
        return 1;

    /* Try PATH via SearchPath */
    char found[MAX_PATH];
    if (SearchPathA(NULL, "qemu-system-x86_64.exe", NULL,
                    MAX_PATH, found, NULL)) {
        strncpy(state->qemu_exe, found, sizeof(state->qemu_exe) - 1);
        state->qemu_exe[sizeof(state->qemu_exe) - 1] = '\0';
        return 1;
    }

    return 0;
}

/* Build QEMU command line */
static char *build_qemu_cmdline(qemu_state_t *state) {
    const char *accel = state->whpx_available ? "whpx,kernel-irqchip=off" : "tcg";
    const char *mem = QEMU_DEFAULT_MEM;
    int cpus = QEMU_DEFAULT_CPUS;

    /* Size the command line buffer */
    size_t sz = 2048 + strlen(state->qemu_exe) + strlen(state->rootfs_path);
    if (state->kernel_path[0]) sz += strlen(state->kernel_path);
    char *cmd = (char *)malloc(sz);
    if (!cmd) return NULL;

    if (state->kernel_path[0]) {
        /* Direct kernel boot (faster, no BIOS) */
        snprintf(cmd, sz,
            "\"%s\" -accel %s -m %s -smp %d "
            "-kernel \"%s\" "
            "-drive file=\"%s\",format=raw,if=virtio "
            "-append \"root=/dev/vda rw console=ttyS0 quiet\" "
            "-netdev user,id=net0,hostfwd=tcp:127.0.0.1:%d-:22 "
            "-device virtio-net-pci,netdev=net0 "
            "-nographic -serial mon:stdio "
            "-no-reboot",
            state->qemu_exe, accel, mem, cpus,
            state->kernel_path,
            state->rootfs_path,
            state->ssh_port);
    } else {
        /* Boot from disk image (needs bootloader in image) */
        snprintf(cmd, sz,
            "\"%s\" -accel %s -m %s -smp %d "
            "-drive file=\"%s\",format=qcow2,if=virtio "
            "-netdev user,id=net0,hostfwd=tcp:127.0.0.1:%d-:22 "
            "-device virtio-net-pci,netdev=net0 "
            "-nographic -serial mon:stdio "
            "-no-reboot",
            state->qemu_exe, accel, mem, cpus,
            state->rootfs_path,
            state->ssh_port);
    }
    return cmd;
}

static linux_error_t qemu_start(linux_backend_t *self,
                                const linux_config_t *config) {
    qemu_state_t *state = (qemu_state_t *)self->opaque;
    if (state->running) return LINUX_ERR_ALREADY_RUNNING;
    state->config = config;

    /* Find QEMU */
    if (!find_qemu(state)) {
        qemu_set_error(state,
            "QEMU not found. Place qemu-system-x86_64.exe in a 'qemu' "
            "subdirectory next to this executable, or install QEMU and "
            "add it to PATH.\n"
            "Download: https://qemu.weilnetz.de/w64/");
        return LINUX_ERR_NOT_AVAILABLE;
    }

    LINUX_LOG(config, "QEMU found: %s", state->qemu_exe);

    /* Check for Linux image */
    if (config->rootfs_path) {
        strncpy(state->rootfs_path, config->rootfs_path,
                sizeof(state->rootfs_path) - 1);
        state->rootfs_path[sizeof(state->rootfs_path) - 1] = '\0';
    } else {
        /* Look for bundled image next to exe */
        char exe_dir[MAX_PATH];
        GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
        char *s = strrchr(exe_dir, '\\');
        if (s) *s = '\0';
        snprintf(state->rootfs_path, sizeof(state->rootfs_path),
                 "%s\\linux\\rootfs.qcow2", exe_dir);
    }

    if (GetFileAttributesA(state->rootfs_path) == INVALID_FILE_ATTRIBUTES) {
        qemu_set_error(state,
            "Linux disk image not found at: %s\n"
            "Provide --rootfs path or place rootfs.qcow2 in the linux/ directory.",
            state->rootfs_path);
        return LINUX_ERR_START_FAILED;
    }

    if (config->kernel_path) {
        strncpy(state->kernel_path, config->kernel_path,
                sizeof(state->kernel_path) - 1);
        state->kernel_path[sizeof(state->kernel_path) - 1] = '\0';
        LINUX_LOG(config, "Kernel: %s", state->kernel_path);
    }

    /* Detect acceleration */
    state->whpx_available = detect_whpx();
    LINUX_LOG(config, "WHPX acceleration: %s",
              state->whpx_available ? "available" : "not available (using TCG)");

    state->ssh_port = QEMU_SSH_PORT;

    /* Build command line */
    char *cmdline = build_qemu_cmdline(state);
    if (!cmdline) {
        qemu_set_error(state, "Out of memory building QEMU command");
        return LINUX_ERR_OUT_OF_MEMORY;
    }
    LINUX_LOG(config, "QEMU command: %s", cmdline);

    /* Create a job object so QEMU dies if we crash */
    state->job = CreateJobObject(NULL, NULL);
    if (state->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {0};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(state->job,
            JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }

    /* Launch QEMU as a background process */
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(cmdline);

    if (!ok) {
        qemu_set_error(state, "Failed to start QEMU: error %lu", GetLastError());
        return LINUX_ERR_START_FAILED;
    }

    CloseHandle(pi.hThread);
    state->process = pi.hProcess;

    if (state->job)
        AssignProcessToJobObject(state->job, state->process);

    /* Wait for SSH to become available (guest boot) */
    LINUX_LOG(config, "Waiting for guest to boot (SSH on port %d)...",
              state->ssh_port);

    /* Poll for SSH connectivity — try connecting for up to 60 seconds */
    int booted = 0;
    DWORD timeout = config->timeout_ms ? config->timeout_ms : 60000;
    DWORD start_time = GetTickCount();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    while ((GetTickCount() - start_time) < timeout) {
        /* Check if QEMU is still running */
        DWORD exit_code;
        if (GetExitCodeProcess(state->process, &exit_code) &&
            exit_code != STILL_ACTIVE) {
            qemu_set_error(state, "QEMU exited prematurely with code %lu",
                          exit_code);
            CloseHandle(state->process);
            state->process = NULL;
            WSACleanup();
            return LINUX_ERR_START_FAILED;
        }

        /* Try a TCP connection to the SSH port */
        SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock != INVALID_SOCKET) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((unsigned short)state->ssh_port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                booted = 1;
                closesocket(sock);
                break;
            }
            closesocket(sock);
        }
        Sleep(1000);
    }

    WSACleanup();

    if (!booted) {
        LINUX_LOG(config, "Guest did not respond on SSH port within timeout");
        /* Don't fail — QEMU might still be booting, user can retry */
    }

    state->running = 1;
    LINUX_LOG(config, "QEMU backend started%s",
              booted ? " (guest SSH ready)" : " (guest may still be booting)");
    return LINUX_OK;
}

static linux_error_t qemu_stop(linux_backend_t *self) {
    qemu_state_t *state = (qemu_state_t *)self->opaque;
    if (state->process) {
        TerminateProcess(state->process, 0);
        WaitForSingleObject(state->process, 5000);
        CloseHandle(state->process);
        state->process = NULL;
    }
    state->running = 0;
    return LINUX_OK;
}

static int qemu_is_running(linux_backend_t *self) {
    qemu_state_t *state = (qemu_state_t *)self->opaque;
    return state->running;
}

static const char *qemu_last_error(linux_backend_t *self) {
    qemu_state_t *state = (qemu_state_t *)self->opaque;
    return state->last_error_buf;
}

static linux_error_t qemu_exec(linux_backend_t *self,
                               const char *command,
                               char **stdout_buf,
                               char **stderr_buf,
                               int *exit_code) {
    qemu_state_t *state = (qemu_state_t *)self->opaque;
    if (!state->running) {
        qemu_set_error(state, "Backend not started");
        return LINUX_ERR_NOT_RUNNING;
    }

    if (stdout_buf) *stdout_buf = NULL;
    if (stderr_buf) *stderr_buf = NULL;
    if (exit_code)  *exit_code = -1;

    LINUX_LOG(state->config, "QEMU exec via SSH: %s", command);

    /* Escape the command for safe embedding in double quotes.
     * Characters that are special inside double quotes: " \ $ ` */
    size_t raw_len = strlen(command);
    char *esc_cmd = (char *)malloc(raw_len * 2 + 1);
    if (!esc_cmd) return LINUX_ERR_OUT_OF_MEMORY;
    {
        char *p = esc_cmd;
        for (size_t i = 0; i < raw_len; i++) {
            if (command[i] == '"' || command[i] == '\\' ||
                command[i] == '$' || command[i] == '`')
                *p++ = '\\';
            *p++ = command[i];
        }
        *p = '\0';
    }

    /* Execute via SSH: ssh -p PORT root@localhost "escaped_command 2>&1"
     * The 2>&1 merges stderr into stdout on the remote side, avoiding
     * a pipe deadlock when reading stdout and stderr sequentially. */
    size_t cmd_len = strlen(esc_cmd) + 256;
    char *ssh_cmd = (char *)malloc(cmd_len);
    if (!ssh_cmd) { free(esc_cmd); return LINUX_ERR_OUT_OF_MEMORY; }

    snprintf(ssh_cmd, cmd_len,
        "ssh -p %d -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL "
        "-o ConnectTimeout=5 -o BatchMode=yes "
        "root@127.0.0.1 \"%s 2>&1\"",
        state->ssh_port, esc_cmd);
    free(esc_cmd);

    /* Run ssh command via CreateProcess with pipes */
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE out_read = INVALID_HANDLE_VALUE, out_write = INVALID_HANDLE_VALUE;
    HANDLE err_read = INVALID_HANDLE_VALUE, err_write = INVALID_HANDLE_VALUE;
    linux_error_t result = LINUX_OK;

    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        free(ssh_cmd);
        return LINUX_ERR_PIPE_FAILED;
    }
    if (!CreatePipe(&err_read, &err_write, &sa, 0)) {
        CloseHandle(out_read); CloseHandle(out_write);
        free(ssh_cmd);
        return LINUX_ERR_PIPE_FAILED;
    }
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = out_write;
    si.hStdError = err_write;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessA(NULL, ssh_cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(ssh_cmd);

    if (!ok) {
        qemu_set_error(state, "Failed to run ssh: error %lu", GetLastError());
        CloseHandle(out_read); CloseHandle(out_write);
        CloseHandle(err_read); CloseHandle(err_write);
        return LINUX_ERR_EXEC_FAILED;
    }

    CloseHandle(pi.hThread);
    CloseHandle(out_write);
    CloseHandle(err_write);

    /* Read output */
    growbuf_t out_gb, err_gb;
    growbuf_init(&out_gb, 4096);
    growbuf_init(&err_gb, 4096);

    char tmp[4096];
    DWORD bytes_read;
    while (ReadFile(out_read, tmp, sizeof(tmp), &bytes_read, NULL) && bytes_read)
        growbuf_append(&out_gb, tmp, bytes_read);
    while (ReadFile(err_read, tmp, sizeof(tmp), &bytes_read, NULL) && bytes_read)
        growbuf_append(&err_gb, tmp, bytes_read);

    CloseHandle(out_read);
    CloseHandle(err_read);

    DWORD timeout = state->config->timeout_ms ? state->config->timeout_ms : 30000;
    WaitForSingleObject(pi.hProcess, timeout);

    DWORD code;
    if (exit_code && GetExitCodeProcess(pi.hProcess, &code))
        *exit_code = (int)code;

    CloseHandle(pi.hProcess);

    if (stdout_buf) *stdout_buf = growbuf_finish(&out_gb);
    else growbuf_free(&out_gb);
    if (stderr_buf) *stderr_buf = growbuf_finish(&err_gb);
    else growbuf_free(&err_gb);

    return LINUX_OK;
}

static void qemu_destroy(linux_backend_t *self) {
    if (!self) return;
    qemu_state_t *state = (qemu_state_t *)self->opaque;
    if (state) {
        if (state->process) {
            TerminateProcess(state->process, 0);
            CloseHandle(state->process);
        }
        if (state->job) CloseHandle(state->job);
        free(state);
    }
    free(self);
}

linux_backend_t *linux_backend_create_qemu(void) {
    linux_backend_t *b = (linux_backend_t *)calloc(1, sizeof(linux_backend_t));
    if (!b) return NULL;

    qemu_state_t *state = (qemu_state_t *)calloc(1, sizeof(qemu_state_t));
    if (!state) { free(b); return NULL; }

    b->type       = LINUX_BACKEND_QEMU;
    b->name       = "QEMU";
    b->opaque     = state;
    b->start      = qemu_start;
    b->stop       = qemu_stop;
    b->is_running = qemu_is_running;
    b->destroy    = qemu_destroy;
    b->exec       = qemu_exec;
    b->last_error = qemu_last_error;

    return b;
}

#endif /* _WIN32 */
