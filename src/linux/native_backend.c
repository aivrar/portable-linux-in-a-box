#if defined(__linux__) || defined(__unix__)

#include "backend.h"
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define READ_BUF_SIZE 4096

typedef struct {
    int  running;
    char last_error_buf[512];
    const linux_config_t *config;
} native_state_t;

static void native_set_error(native_state_t *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->last_error_buf, sizeof(state->last_error_buf), fmt, ap);
    va_end(ap);
}

static linux_error_t native_start(linux_backend_t *self,
                                  const linux_config_t *config) {
    native_state_t *state = (native_state_t *)self->opaque;
    if (state->running)
        return LINUX_ERR_ALREADY_RUNNING;

    state->config = config;
    state->running = 1;
    LINUX_LOG(config, "Native backend started");
    return LINUX_OK;
}

static linux_error_t native_stop(linux_backend_t *self) {
    native_state_t *state = (native_state_t *)self->opaque;
    state->running = 0;
    return LINUX_OK;
}

static int native_is_running(linux_backend_t *self) {
    native_state_t *state = (native_state_t *)self->opaque;
    return state->running;
}

static const char *native_last_error(linux_backend_t *self) {
    native_state_t *state = (native_state_t *)self->opaque;
    return state->last_error_buf;
}

static linux_error_t native_exec(linux_backend_t *self,
                                 const char *command,
                                 char **stdout_buf,
                                 char **stderr_buf,
                                 int *exit_code) {
    native_state_t *state = (native_state_t *)self->opaque;

    if (!state->running) {
        native_set_error(state, "Backend not started");
        return LINUX_ERR_NOT_RUNNING;
    }

    if (stdout_buf) *stdout_buf = NULL;
    if (stderr_buf) *stderr_buf = NULL;
    if (exit_code)  *exit_code = -1;

    LINUX_LOG(state->config, "exec: %s", command);

    /* Create pipes for stdout and stderr */
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) < 0) {
        native_set_error(state, "pipe() failed for stdout: %s", strerror(errno));
        return LINUX_ERR_PIPE_FAILED;
    }
    if (pipe(err_pipe) < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        native_set_error(state, "pipe() failed for stderr: %s", strerror(errno));
        return LINUX_ERR_PIPE_FAILED;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        native_set_error(state, "fork() failed: %s", strerror(errno));
        return LINUX_ERR_EXEC_FAILED;
    }

    if (pid == 0) {
        /* Child: redirect stdout/stderr to pipes, exec command */
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[1]);
        close(err_pipe[1]);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent: read from pipes */
    close(out_pipe[1]);
    close(err_pipe[1]);

    growbuf_t out_buf, err_buf;
    growbuf_init(&out_buf, READ_BUF_SIZE);
    growbuf_init(&err_buf, READ_BUF_SIZE);

    char tmp[READ_BUF_SIZE];
    ssize_t n;

    /* Read stdout */
    while ((n = read(out_pipe[0], tmp, sizeof(tmp))) > 0)
        growbuf_append(&out_buf, tmp, (size_t)n);
    close(out_pipe[0]);

    /* Read stderr */
    while ((n = read(err_pipe[0], tmp, sizeof(tmp))) > 0)
        growbuf_append(&err_buf, tmp, (size_t)n);
    close(err_pipe[0]);

    /* Wait for child */
    int status = 0;
    waitpid(pid, &status, 0);

    if (exit_code) {
        if (WIFEXITED(status))
            *exit_code = WEXITSTATUS(status);
        else
            *exit_code = -1;
    }

    if (stdout_buf)
        *stdout_buf = growbuf_finish(&out_buf);
    else
        growbuf_free(&out_buf);

    if (stderr_buf)
        *stderr_buf = growbuf_finish(&err_buf);
    else
        growbuf_free(&err_buf);

    return LINUX_OK;
}

static void native_destroy(linux_backend_t *self) {
    if (!self) return;
    free(self->opaque);
    free(self);
}

linux_backend_t *linux_backend_create_native(void) {
    linux_backend_t *b = (linux_backend_t *)calloc(1, sizeof(linux_backend_t));
    if (!b) return NULL;

    native_state_t *state = (native_state_t *)calloc(1, sizeof(native_state_t));
    if (!state) { free(b); return NULL; }

    b->type       = LINUX_BACKEND_NATIVE;
    b->name       = "Native";
    b->opaque     = state;
    b->start      = native_start;
    b->stop       = native_stop;
    b->is_running = native_is_running;
    b->destroy    = native_destroy;
    b->exec       = native_exec;
    b->last_error = native_last_error;

    return b;
}

#endif /* __linux__ || __unix__ */
