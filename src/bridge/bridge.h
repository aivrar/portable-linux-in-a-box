#ifndef LINUX_BRIDGE_H
#define LINUX_BRIDGE_H

#include "../linux/backend.h"

/* Execute a command on the Linux backend and capture output.
 * stdout_out/stderr_out: if non-NULL, receive heap-allocated output.
 *                        Caller frees with free().
 * exit_code: if non-NULL, receives the process exit code. */
linux_error_t bridge_exec(linux_backend_t *backend,
                          const char *command,
                          char **stdout_out,
                          char **stderr_out,
                          int *exit_code);

/* Execute a command and print output to the console.
 * Convenience for Phase 1 demo. */
linux_error_t bridge_exec_print(linux_backend_t *backend,
                                const char *command);

#endif /* LINUX_BRIDGE_H */
