#ifndef LINUX_BACKEND_H
#define LINUX_BACKEND_H

#include "backend_types.h"

/* --------------------------------------------------------------------------
 * Abstract Linux backend interface
 *
 * Each backend (WSL2, native, WHPX, TinyEMU) provides a constructor that
 * returns a heap-allocated linux_backend_t with all function pointers filled.
 * The caller interacts only through these function pointers.
 * -------------------------------------------------------------------------- */
typedef struct linux_backend {
    linux_backend_type_t type;
    const char          *name;   /* Human-readable, e.g. "WSL2", "Native" */

    /* Lifecycle */
    linux_error_t (*start)(struct linux_backend *self, const linux_config_t *config);
    linux_error_t (*stop)(struct linux_backend *self);
    int           (*is_running)(struct linux_backend *self);
    void          (*destroy)(struct linux_backend *self);

    /* Execute a command in the Linux environment.
     * stdout_buf/stderr_buf: if non-NULL, receive heap-allocated output.
     *                        Caller frees with free().
     *                        Pass NULL to discard that stream.
     * exit_code: if non-NULL, receives the process exit code. */
    linux_error_t (*exec)(struct linux_backend *self,
                          const char *command,
                          char **stdout_buf,
                          char **stderr_buf,
                          int *exit_code);

    /* Human-readable detail about the most recent error.
     * Owned by the backend, valid until the next operation. */
    const char   *(*last_error)(struct linux_backend *self);

    /* Private data for the backend implementation */
    void *opaque;
} linux_backend_t;

/* --------------------------------------------------------------------------
 * Backend constructors
 * Return heap-allocated backend, or NULL on allocation failure.
 * -------------------------------------------------------------------------- */
#ifdef _WIN32
linux_backend_t *linux_backend_create_wsl(void);
linux_backend_t *linux_backend_create_qemu(void);
#ifdef HAVE_WHPX
linux_backend_t *linux_backend_create_whpx(void);
#endif
linux_backend_t *linux_backend_create_tinyemu(void);
#endif

#if defined(__linux__) || defined(__unix__)
linux_backend_t *linux_backend_create_native(void);
#endif

#if defined(__APPLE__) || (defined(__unix__) && !defined(__linux__))
linux_backend_t *linux_backend_create_qemu_posix(void);
#endif

linux_backend_t *linux_backend_create_stub(linux_backend_type_t placeholder_type);

#endif /* LINUX_BACKEND_H */
