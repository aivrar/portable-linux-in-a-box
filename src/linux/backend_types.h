#ifndef LINUX_BACKEND_TYPES_H
#define LINUX_BACKEND_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Error codes
 * -------------------------------------------------------------------------- */
typedef enum {
    LINUX_OK = 0,
    LINUX_ERR_NOT_AVAILABLE,
    LINUX_ERR_ALREADY_RUNNING,
    LINUX_ERR_NOT_RUNNING,
    LINUX_ERR_START_FAILED,
    LINUX_ERR_EXEC_FAILED,
    LINUX_ERR_PIPE_FAILED,
    LINUX_ERR_TIMEOUT,
    LINUX_ERR_OUT_OF_MEMORY,
    LINUX_ERR_INVALID_ARG,
    LINUX_ERR_INTERNAL
} linux_error_t;

/* --------------------------------------------------------------------------
 * Backend types
 * -------------------------------------------------------------------------- */
typedef enum {
    LINUX_BACKEND_NONE = 0,
    LINUX_BACKEND_NATIVE,   /* Linux host: fork/exec, zero overhead       */
    LINUX_BACKEND_WSL2,     /* Windows: WSL2 via wslapi.dll               */
    LINUX_BACKEND_WHPX,     /* Windows: reserved for future raw WHPX VMM  */
    LINUX_BACKEND_QEMU,     /* Windows: QEMU subprocess (WHPX or TCG)     */
    LINUX_BACKEND_TINYEMU   /* Reserved for future embedded emulator       */
} linux_backend_type_t;

/* --------------------------------------------------------------------------
 * Configuration passed to backend start()
 * -------------------------------------------------------------------------- */
typedef struct linux_config {
    const char *distro_name;   /* WSL distro name, e.g. "linux-template"       */
    const char *tar_gz_path;   /* Path to distro tarball (WSL registration)     */
    const char *kernel_path;   /* Path to Linux kernel (WHPX/TinyEMU)          */
    const char *rootfs_path;   /* Path to rootfs image (WHPX/TinyEMU)          */
    uint32_t    timeout_ms;    /* Operation timeout, 0 = 30000ms default       */
    int         verbose;       /* Nonzero enables debug logging to stderr      */
} linux_config_t;

/* --------------------------------------------------------------------------
 * Verbose logging macro
 * -------------------------------------------------------------------------- */
#define LINUX_LOG(config, fmt, ...)                                           \
    do {                                                                      \
        if ((config) && (config)->verbose)                                    \
            fprintf(stderr, "[linux-template] " fmt "\n", ##__VA_ARGS__);    \
    } while (0)

/* --------------------------------------------------------------------------
 * Growing buffer for pipe reads
 * -------------------------------------------------------------------------- */
#define GROWBUF_MAX_SIZE (64 * 1024 * 1024)  /* 64 MB hard limit */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} growbuf_t;

static inline int growbuf_init(growbuf_t *b, size_t initial_cap) {
    b->data = (char *)malloc(initial_cap);
    if (!b->data) return -1;
    b->len = 0;
    b->cap = initial_cap;
    return 0;
}

static inline int growbuf_append(growbuf_t *b, const char *data, size_t len) {
    if (b->len + len > GROWBUF_MAX_SIZE)
        return -1;  /* Refuse to grow beyond hard limit */
    if (b->len + len + 1 > b->cap) {
        size_t new_cap = b->cap * 2;
        if (new_cap <= b->cap) /* overflow guard */
            new_cap = b->len + len + 1;
        if (new_cap < b->len + len + 1)
            new_cap = b->len + len + 1;
        char *tmp = (char *)realloc(b->data, new_cap);
        if (!tmp) return -1;
        b->data = tmp;
        b->cap = new_cap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
    return 0;
}

/* Returns owned data pointer. Caller frees with free(). Resets the buffer. */
static inline char *growbuf_finish(growbuf_t *b) {
    if (!b->data) return NULL;
    b->data[b->len] = '\0';
    char *result = b->data;
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    return result;
}

static inline void growbuf_free(growbuf_t *b) {
    free(b->data);
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
}

/* Convert error code to human-readable string */
const char *linux_error_string(linux_error_t err);

#endif /* LINUX_BACKEND_TYPES_H */
