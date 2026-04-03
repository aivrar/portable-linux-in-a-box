/*
 * QEMU Backend — POSIX (macOS / Linux)
 *
 * Same architecture as the Windows QEMU backend, but uses POSIX APIs:
 *   fork/exec instead of CreateProcess
 *   pipe/read/write instead of HANDLE pipes
 *   kill/waitpid instead of TerminateProcess/WaitForSingleObject
 *   BSD sockets (already POSIX) for SSH port probing
 *
 * Communicates with the guest Linux via SSH over a forwarded port.
 */

#if defined(__APPLE__) || (defined(__unix__) && !defined(__linux__))

#include "backend.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>

#define QEMU_SSH_PORT     2222
#define QEMU_DEFAULT_MEM  "512M"
#define QEMU_DEFAULT_CPUS 2
#define READ_BUF_SIZE     4096

typedef struct {
    int     running;
    char    last_error_buf[512];
    const linux_config_t *config;

    /* QEMU process */
    pid_t   pid;

    /* Paths */
    char    qemu_exe[1024];
    char    kernel_path[1024];
    char    rootfs_path[1024];

    /* Connection */
    int     ssh_port;
} qemu_posix_state_t;

static void qemu_set_error(qemu_posix_state_t *st, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(st->last_error_buf, sizeof(st->last_error_buf), fmt, ap);
    va_end(ap);
}

static unsigned long tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* --------------------------------------------------------------------------
 * Find QEMU executable
 * -------------------------------------------------------------------------- */
static int find_qemu(qemu_posix_state_t *st) {
    /* Homebrew on ARM Mac */
    if (access("/opt/homebrew/bin/qemu-system-x86_64", X_OK) == 0) {
        strncpy(st->qemu_exe, "/opt/homebrew/bin/qemu-system-x86_64",
                sizeof(st->qemu_exe) - 1);
        return 1;
    }
    /* Homebrew on Intel Mac / Linux */
    if (access("/usr/local/bin/qemu-system-x86_64", X_OK) == 0) {
        strncpy(st->qemu_exe, "/usr/local/bin/qemu-system-x86_64",
                sizeof(st->qemu_exe) - 1);
        return 1;
    }
    /* System PATH */
    if (access("/usr/bin/qemu-system-x86_64", X_OK) == 0) {
        strncpy(st->qemu_exe, "/usr/bin/qemu-system-x86_64",
                sizeof(st->qemu_exe) - 1);
        return 1;
    }
    /* Try 'which' as fallback */
    FILE *p = popen("which qemu-system-x86_64 2>/dev/null", "r");
    if (p) {
        if (fgets(st->qemu_exe, (int)sizeof(st->qemu_exe), p)) {
            size_t len = strlen(st->qemu_exe);
            while (len > 0 && (st->qemu_exe[len-1] == '\n' ||
                               st->qemu_exe[len-1] == '\r'))
                st->qemu_exe[--len] = '\0';
            pclose(p);
            if (len > 0 && access(st->qemu_exe, X_OK) == 0)
                return 1;
        } else {
            pclose(p);
        }
    }
    return 0;
}

/* Find the directory containing our executable */
static int get_exe_dir(char *buf, size_t bufsz) {
#ifdef __APPLE__
    /* macOS: _NSGetExecutablePath */
    uint32_t sz = (uint32_t)bufsz;
    extern int _NSGetExecutablePath(char *, uint32_t *);
    if (_NSGetExecutablePath(buf, &sz) != 0) return -1;
    char *sl = strrchr(buf, '/');
    if (sl) *sl = '\0';
    return 0;
#else
    /* Linux/other: /proc/self/exe */
    ssize_t len = readlink("/proc/self/exe", buf, bufsz - 1);
    if (len < 0) return -1;
    buf[len] = '\0';
    char *sl = strrchr(buf, '/');
    if (sl) *sl = '\0';
    return 0;
#endif
}

/* --------------------------------------------------------------------------
 * Backend interface: start
 * -------------------------------------------------------------------------- */
static linux_error_t qemu_start(linux_backend_t *self,
                                const linux_config_t *config) {
    qemu_posix_state_t *st = (qemu_posix_state_t *)self->opaque;
    if (st->running) return LINUX_ERR_ALREADY_RUNNING;
    st->config = config;

    /* Find QEMU */
    if (!find_qemu(st)) {
        qemu_set_error(st,
            "QEMU not found. Install it with:\n"
            "  macOS:  brew install qemu\n"
            "  Linux:  sudo apt-get install qemu-system-x86");
        return LINUX_ERR_NOT_AVAILABLE;
    }
    LINUX_LOG(config, "QEMU found: %s", st->qemu_exe);

    /* Find rootfs image */
    if (config->rootfs_path) {
        strncpy(st->rootfs_path, config->rootfs_path,
                sizeof(st->rootfs_path) - 1);
    } else {
        char exe_dir[1024];
        if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0) {
            snprintf(st->rootfs_path, sizeof(st->rootfs_path),
                     "%s/linux/rootfs.qcow2", exe_dir);
        }
    }
    if (access(st->rootfs_path, R_OK) != 0) {
        qemu_set_error(st,
            "Linux disk image not found at: %s\n"
            "Provide a rootfs or place rootfs.qcow2 in the linux/ directory.",
            st->rootfs_path);
        return LINUX_ERR_START_FAILED;
    }

    /* Find kernel (optional — direct boot) */
    if (config->kernel_path) {
        strncpy(st->kernel_path, config->kernel_path,
                sizeof(st->kernel_path) - 1);
    } else {
        char exe_dir[1024];
        if (get_exe_dir(exe_dir, sizeof(exe_dir)) == 0) {
            snprintf(st->kernel_path, sizeof(st->kernel_path),
                     "%s/linux/bzImage", exe_dir);
            if (access(st->kernel_path, R_OK) != 0)
                st->kernel_path[0] = '\0';
        }
    }

    st->ssh_port = QEMU_SSH_PORT;

    /* Build argv for QEMU */
    /* Use HVF acceleration on macOS if available, TCG otherwise */
#ifdef __APPLE__
    const char *accel = "hvf";  /* Apple Hypervisor.framework */
#else
    const char *accel = "tcg";
#endif

    LINUX_LOG(config, "QEMU accel: %s", accel);

    /* Fork and exec QEMU */
    pid_t pid = fork();
    if (pid < 0) {
        qemu_set_error(st, "fork() failed: %s", strerror(errno));
        return LINUX_ERR_START_FAILED;
    }

    if (pid == 0) {
        /* Child: redirect stdout/stderr to /dev/null, exec QEMU */
        int devnull = open("/dev/null", 0 /* O_RDONLY */);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        char port_fwd[64];
        snprintf(port_fwd, sizeof(port_fwd),
                 "user,id=net0,hostfwd=tcp:127.0.0.1:%d-:22", st->ssh_port);

        if (st->kernel_path[0]) {
            execl(st->qemu_exe, "qemu-system-x86_64",
                  "-accel", accel,
                  "-m", QEMU_DEFAULT_MEM, "-smp", "2",
                  "-kernel", st->kernel_path,
                  "-drive", st->rootfs_path, /* simplified; format auto-detected */
                  "-append", "root=/dev/vda rw console=ttyS0 quiet",
                  "-netdev", port_fwd,
                  "-device", "virtio-net-pci,netdev=net0",
                  "-nographic", "-no-reboot",
                  (char *)NULL);
        } else {
            execl(st->qemu_exe, "qemu-system-x86_64",
                  "-accel", accel,
                  "-m", QEMU_DEFAULT_MEM, "-smp", "2",
                  "-drive", st->rootfs_path,
                  "-netdev", port_fwd,
                  "-device", "virtio-net-pci,netdev=net0",
                  "-nographic", "-no-reboot",
                  (char *)NULL);
        }
        _exit(127); /* exec failed */
    }

    /* Parent */
    st->pid = pid;
    LINUX_LOG(config, "QEMU started (pid %d), waiting for SSH on port %d...",
              pid, st->ssh_port);

    /* Poll for SSH connectivity */
    unsigned long timeout = config->timeout_ms ? config->timeout_ms : 60000;
    unsigned long start_time = tick_ms();
    int booted = 0;

    while ((tick_ms() - start_time) < timeout) {
        /* Check if QEMU is still alive */
        int status;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r > 0) {
            qemu_set_error(st, "QEMU exited prematurely (status %d)", status);
            st->pid = 0;
            return LINUX_ERR_START_FAILED;
        }

        /* Try TCP connect to SSH port */
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock >= 0) {
            struct sockaddr_in addr = {0};
            addr.sin_family = AF_INET;
            addr.sin_port = htons((unsigned short)st->ssh_port);
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                booted = 1;
                close(sock);
                break;
            }
            close(sock);
        }
        usleep(1000 * 1000); /* 1 second */
    }

    if (!booted)
        LINUX_LOG(config, "Guest did not respond on SSH within timeout");

    st->running = 1;
    LINUX_LOG(config, "QEMU backend started%s",
              booted ? " (SSH ready)" : " (may still be booting)");
    return LINUX_OK;
}

/* --------------------------------------------------------------------------
 * Backend interface: exec (via SSH)
 * -------------------------------------------------------------------------- */
static linux_error_t qemu_exec(linux_backend_t *self,
                               const char *command,
                               char **stdout_buf,
                               char **stderr_buf,
                               int *exit_code) {
    qemu_posix_state_t *st = (qemu_posix_state_t *)self->opaque;
    if (!st->running) {
        qemu_set_error(st, "Backend not started");
        return LINUX_ERR_NOT_RUNNING;
    }

    if (stdout_buf) *stdout_buf = NULL;
    if (stderr_buf) *stderr_buf = NULL;
    if (exit_code)  *exit_code = -1;

    LINUX_LOG(st->config, "QEMU exec via SSH: %s", command);

    /* Build SSH command with stderr merged into stdout */
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", st->ssh_port);

    /* Create pipes for capturing output */
    int out_pipe[2];
    if (pipe(out_pipe) < 0) {
        qemu_set_error(st, "pipe() failed: %s", strerror(errno));
        return LINUX_ERR_PIPE_FAILED;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        qemu_set_error(st, "fork() failed: %s", strerror(errno));
        return LINUX_ERR_EXEC_FAILED;
    }

    if (pid == 0) {
        /* Child: redirect stdout+stderr to pipe, exec ssh */
        close(out_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);

        /* Build the remote command with 2>&1 to merge streams */
        size_t cmd_len = strlen(command) + 16;
        char *remote_cmd = (char *)malloc(cmd_len);
        if (remote_cmd) {
            snprintf(remote_cmd, cmd_len, "%s 2>&1", command);
        }

        execlp("ssh", "ssh",
               "-p", port_str,
               "-o", "StrictHostKeyChecking=no",
               "-o", "UserKnownHostsFile=/dev/null",
               "-o", "ConnectTimeout=5",
               "-o", "BatchMode=yes",
               "-o", "LogLevel=ERROR",
               "root@127.0.0.1",
               remote_cmd ? remote_cmd : command,
               (char *)NULL);
        _exit(127);
    }

    /* Parent: read output */
    close(out_pipe[1]);

    growbuf_t out_gb;
    growbuf_init(&out_gb, READ_BUF_SIZE);
    char tmp[READ_BUF_SIZE];
    ssize_t n;
    while ((n = read(out_pipe[0], tmp, sizeof(tmp))) > 0)
        growbuf_append(&out_gb, tmp, (size_t)n);
    close(out_pipe[0]);

    /* Wait for SSH process */
    int status = 0;
    waitpid(pid, &status, 0);

    if (exit_code) {
        if (WIFEXITED(status))
            *exit_code = WEXITSTATUS(status);
        else
            *exit_code = -1;
    }

    if (stdout_buf)
        *stdout_buf = growbuf_finish(&out_gb);
    else
        growbuf_free(&out_gb);

    /* stderr is merged via 2>&1 */
    if (stderr_buf) *stderr_buf = NULL;

    return LINUX_OK;
}

/* --------------------------------------------------------------------------
 * Backend interface: stop, destroy, etc.
 * -------------------------------------------------------------------------- */
static linux_error_t qemu_stop(linux_backend_t *self) {
    qemu_posix_state_t *st = (qemu_posix_state_t *)self->opaque;
    if (st->pid > 0) {
        kill(st->pid, SIGTERM);
        /* Give it 5 seconds to shut down gracefully */
        int waited = 0;
        for (int i = 0; i < 50; i++) {
            int status;
            if (waitpid(st->pid, &status, WNOHANG) > 0) {
                waited = 1; break;
            }
            usleep(100 * 1000);
        }
        if (!waited) {
            kill(st->pid, SIGKILL);
            waitpid(st->pid, NULL, 0);
        }
        st->pid = 0;
    }
    st->running = 0;
    return LINUX_OK;
}

static int qemu_is_running(linux_backend_t *self) {
    qemu_posix_state_t *st = (qemu_posix_state_t *)self->opaque;
    return st->running;
}

static const char *qemu_last_error(linux_backend_t *self) {
    qemu_posix_state_t *st = (qemu_posix_state_t *)self->opaque;
    return st->last_error_buf;
}

static void qemu_destroy(linux_backend_t *self) {
    if (!self) return;
    qemu_posix_state_t *st = (qemu_posix_state_t *)self->opaque;
    if (st) {
        if (st->pid > 0) {
            kill(st->pid, SIGKILL);
            waitpid(st->pid, NULL, 0);
        }
        free(st);
    }
    free(self);
}

/* --------------------------------------------------------------------------
 * Constructor
 * -------------------------------------------------------------------------- */
linux_backend_t *linux_backend_create_qemu_posix(void) {
    linux_backend_t *b = (linux_backend_t *)calloc(1, sizeof(linux_backend_t));
    if (!b) return NULL;

    qemu_posix_state_t *st = (qemu_posix_state_t *)calloc(1, sizeof(qemu_posix_state_t));
    if (!st) { free(b); return NULL; }

    b->type       = LINUX_BACKEND_QEMU;
    b->name       = "QEMU";
    b->opaque     = st;
    b->start      = qemu_start;
    b->stop       = qemu_stop;
    b->is_running = qemu_is_running;
    b->destroy    = qemu_destroy;
    b->exec       = qemu_exec;
    b->last_error = qemu_last_error;

    return b;
}

#endif /* __APPLE__ || (__unix__ && !__linux__) */
