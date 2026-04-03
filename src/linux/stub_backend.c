#include "backend.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    char last_error_buf[256];
} stub_state_t;

static linux_error_t stub_start(linux_backend_t *self, const linux_config_t *config) {
    stub_state_t *state = (stub_state_t *)self->opaque;
    (void)config;
    snprintf(state->last_error_buf, sizeof(state->last_error_buf),
             "%s backend is not yet implemented", self->name);
    return LINUX_ERR_NOT_AVAILABLE;
}

static linux_error_t stub_stop(linux_backend_t *self) {
    (void)self;
    return LINUX_ERR_NOT_AVAILABLE;
}

static int stub_is_running(linux_backend_t *self) {
    (void)self;
    return 0;
}

static linux_error_t stub_exec(linux_backend_t *self, const char *command,
                               char **stdout_buf, char **stderr_buf,
                               int *exit_code) {
    (void)self; (void)command;
    if (stdout_buf) *stdout_buf = NULL;
    if (stderr_buf) *stderr_buf = NULL;
    if (exit_code)  *exit_code = -1;
    return LINUX_ERR_NOT_AVAILABLE;
}

static const char *stub_last_error(linux_backend_t *self) {
    stub_state_t *state = (stub_state_t *)self->opaque;
    return state->last_error_buf;
}

static void stub_destroy(linux_backend_t *self) {
    if (!self) return;
    free(self->opaque);
    free(self);
}

linux_backend_t *linux_backend_create_stub(linux_backend_type_t placeholder_type) {
    linux_backend_t *b = (linux_backend_t *)calloc(1, sizeof(linux_backend_t));
    if (!b) return NULL;

    stub_state_t *state = (stub_state_t *)calloc(1, sizeof(stub_state_t));
    if (!state) { free(b); return NULL; }

    b->type       = placeholder_type;
    b->opaque     = state;
    b->start      = stub_start;
    b->stop       = stub_stop;
    b->is_running = stub_is_running;
    b->destroy    = stub_destroy;
    b->exec       = stub_exec;
    b->last_error = stub_last_error;

    switch (placeholder_type) {
    case LINUX_BACKEND_WHPX:    b->name = "WHPX (not implemented)";    break;
    case LINUX_BACKEND_TINYEMU: b->name = "TinyEMU (not implemented)"; break;
    default:                    b->name = "Unknown (not implemented)";  break;
    }

    snprintf(state->last_error_buf, sizeof(state->last_error_buf),
             "%s backend is not yet implemented", b->name);

    return b;
}
