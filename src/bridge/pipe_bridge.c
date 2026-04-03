#include "bridge.h"
#include <stdio.h>
#include <stdlib.h>

linux_error_t bridge_exec(linux_backend_t *backend,
                          const char *command,
                          char **stdout_out,
                          char **stderr_out,
                          int *exit_code) {
    if (!backend)  return LINUX_ERR_INVALID_ARG;
    if (!command)  return LINUX_ERR_INVALID_ARG;
    if (!backend->exec) return LINUX_ERR_NOT_AVAILABLE;

    return backend->exec(backend, command, stdout_out, stderr_out, exit_code);
}

linux_error_t bridge_exec_print(linux_backend_t *backend,
                                const char *command) {
    char *out = NULL;
    char *err = NULL;
    int   code = -1;

    linux_error_t rc = bridge_exec(backend, command, &out, &err, &code);
    if (rc != LINUX_OK) {
        fprintf(stderr, "exec failed: %s", linux_error_string(rc));
        if (backend->last_error)
            fprintf(stderr, " -- %s", backend->last_error(backend));
        fprintf(stderr, "\n");
        free(out);
        free(err);
        return rc;
    }

    if (out && out[0]) {
        printf("%s", out);
        /* ensure trailing newline */
        if (out[strlen(out) - 1] != '\n')
            printf("\n");
    }

    if (err && err[0]) {
        fprintf(stderr, "%s", err);
        if (err[strlen(err) - 1] != '\n')
            fprintf(stderr, "\n");
    }

    if (code != 0)
        fprintf(stderr, "[exit code: %d]\n", code);

    free(out);
    free(err);
    return LINUX_OK;
}
