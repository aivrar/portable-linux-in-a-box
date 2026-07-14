#include "fs_bridge.h"
#include "shell_escape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

static char *base64_encode(const unsigned char *data, size_t len, size_t *out_len);

char *fs_win_to_linux(linux_backend_t *backend, const char *win_path) {
    if (!win_path) return NULL;
    (void)backend;

    /* Handle drive letter paths: C:\foo -> /mnt/c/foo */
    if (((win_path[0] >= 'A' && win_path[0] <= 'Z') ||
         (win_path[0] >= 'a' && win_path[0] <= 'z')) && win_path[1] == ':') {

        size_t len = strlen(win_path);
        char *out = (char *)malloc(len + 8); /* /mnt/X/ + rest */
        if (!out) return NULL;

        char drive = (char)tolower((unsigned char)win_path[0]);
        snprintf(out, len + 8, "/mnt/%c", drive);
        size_t pos = 5;

        for (size_t i = 2; i < len; i++) {
            out[pos++] = (win_path[i] == '\\') ? '/' : win_path[i];
        }
        out[pos] = '\0';
        return out;
    }

    /* UNC paths or other: just swap backslashes */
    size_t len = strlen(win_path);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++)
        out[i] = (win_path[i] == '\\') ? '/' : win_path[i];
    out[len] = '\0';
    return out;
}

char *fs_linux_to_win(linux_backend_t *backend, const char *linux_path,
                      const char *distro_name) {
    if (!linux_path) return NULL;

    /* /mnt/c/foo -> C:\foo */
    if (strncmp(linux_path, "/mnt/", 5) == 0 && linux_path[5] != '\0'
        && (linux_path[6] == '/' || linux_path[6] == '\0')) {

        char drive = (char)toupper((unsigned char)linux_path[5]);
        const char *rest = linux_path[6] == '/' ? linux_path + 6 : "";
        size_t len = strlen(rest);
        char *out = (char *)malloc(len + 4);
        if (!out) return NULL;

        out[0] = drive; out[1] = ':';
        size_t pos = 2;
        for (size_t i = 0; i < len; i++)
            out[pos++] = (rest[i] == '/') ? '\\' : rest[i];
        out[pos] = '\0';
        return out;
    }

    /* For WSL2: /home/user/foo -> \\wsl$\distro\home\user\foo */
    if (backend && backend->type == LINUX_BACKEND_WSL2 && distro_name) {
        size_t dlen = strlen(distro_name);
        size_t plen = strlen(linux_path);
        /* \\wsl$\ + distro + path with backslashes */
        char *out = (char *)malloc(7 + dlen + plen + 2);
        if (!out) return NULL;

        snprintf(out, 7 + dlen + plen + 2, "\\\\wsl$\\%s", distro_name);
        size_t pos = 7 + dlen;
        for (size_t i = 0; i < plen; i++)
            out[pos++] = (linux_path[i] == '/') ? '\\' : linux_path[i];
        out[pos] = '\0';
        return out;
    }

    return NULL;
}

linux_error_t fs_write_file(linux_backend_t *backend,
                            const char *linux_path,
                            const char *content) {
    if (!backend || !linux_path || !content)
        return LINUX_ERR_INVALID_ARG;

    /* Use base64 encoding to avoid heredoc injection issues —
     * content is arbitrary and could contain any delimiter string. */
    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return LINUX_ERR_OUT_OF_MEMORY;
    size_t content_len = strlen(content);
    size_t b64_len;
    char *b64 = base64_encode((const unsigned char *)content, content_len, &b64_len);
    if (!b64) { free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }
    size_t cmd_len = strlen(esc_path) + b64_len + 64;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_path); free(b64); return LINUX_ERR_OUT_OF_MEMORY; }

    snprintf(cmd, cmd_len,
             "printf '%%s' '%s' | base64 -d > %s",
             b64, esc_path);
    free(esc_path);
    free(b64);

    int exit_code = -1;
    linux_error_t rc = backend->exec(backend, cmd, NULL, NULL, &exit_code);
    free(cmd);

    if (rc != LINUX_OK) return rc;
    return (exit_code == 0) ? LINUX_OK : LINUX_ERR_EXEC_FAILED;
}

linux_error_t fs_read_file(linux_backend_t *backend,
                           const char *linux_path,
                           char **content) {
    if (!backend || !linux_path || !content)
        return LINUX_ERR_INVALID_ARG;
    *content = NULL;

    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return LINUX_ERR_OUT_OF_MEMORY;
    size_t cmd_len = strlen(esc_path) + 16;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }

    snprintf(cmd, cmd_len, "cat %s", esc_path);
    free(esc_path);

    int exit_code = -1;
    linux_error_t rc = backend->exec(backend, cmd, content, NULL, &exit_code);
    free(cmd);

    if (rc != LINUX_OK) return rc;
    return (exit_code == 0) ? LINUX_OK : LINUX_ERR_EXEC_FAILED;
}

int fs_exists(linux_backend_t *backend, const char *linux_path) {
    if (!backend || !linux_path) return 0;

    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return 0;
    size_t cmd_len = strlen(esc_path) + 32;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_path); return 0; }

    snprintf(cmd, cmd_len, "test -e %s", esc_path);
    free(esc_path);

    int exit_code = -1;
    backend->exec(backend, cmd, NULL, NULL, &exit_code);
    free(cmd);

    return (exit_code == 0) ? 1 : 0;
}

linux_error_t fs_mkdir(linux_backend_t *backend, const char *linux_path) {
    if (!backend || !linux_path) return LINUX_ERR_INVALID_ARG;

    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return LINUX_ERR_OUT_OF_MEMORY;
    size_t cmd_len = strlen(esc_path) + 16;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }

    snprintf(cmd, cmd_len, "mkdir -p %s", esc_path);
    free(esc_path);

    int exit_code = -1;
    linux_error_t rc = backend->exec(backend, cmd, NULL, NULL, &exit_code);
    free(cmd);

    if (rc != LINUX_OK) return rc;
    return (exit_code == 0) ? LINUX_OK : LINUX_ERR_EXEC_FAILED;
}

/* --------------------------------------------------------------------------
 * Base64 encoding table (for fs_upload)
 * -------------------------------------------------------------------------- */
static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const unsigned char *data, size_t len, size_t *out_len) {
    size_t olen = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(olen + 1);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i + 2 < len; i += 3) {
        out[j++] = b64_table[(data[i] >> 2) & 0x3F];
        out[j++] = b64_table[((data[i] & 0x03) << 4) | (data[i+1] >> 4)];
        out[j++] = b64_table[((data[i+1] & 0x0F) << 2) | (data[i+2] >> 6)];
        out[j++] = b64_table[data[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = b64_table[(data[i] >> 2) & 0x3F];
        if (i + 1 < len) {
            out[j++] = b64_table[((data[i] & 0x03) << 4) | (data[i+1] >> 4)];
            out[j++] = b64_table[(data[i+1] & 0x0F) << 2];
        } else {
            out[j++] = b64_table[(data[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static unsigned char *base64_decode(const char *data, size_t len,
                                    size_t *out_len) {
    if (len % 4 != 0) return NULL;
    size_t olen = len / 4 * 3;
    if (len > 0 && data[len-1] == '=') olen--;
    if (len > 1 && data[len-2] == '=') olen--;

    unsigned char *out = (unsigned char *)malloc(olen + 1);
    if (!out) return NULL;

    size_t i, j;
    for (i = 0, j = 0; i < len; i += 4) {
        int a = b64_decode_char(data[i]);
        int b = b64_decode_char(data[i+1]);
        int c = (data[i+2] == '=') ? 0 : b64_decode_char(data[i+2]);
        int d = (data[i+3] == '=') ? 0 : b64_decode_char(data[i+3]);
        if (a < 0 || b < 0) { free(out); return NULL; }

        out[j++] = (unsigned char)((a << 2) | (b >> 4));
        if (data[i+2] != '=')
            out[j++] = (unsigned char)(((b & 0x0F) << 4) | (c >> 2));
        if (data[i+3] != '=')
            out[j++] = (unsigned char)(((c & 0x03) << 6) | d);
    }
    out[j] = '\0';
    if (out_len) *out_len = j;
    return out;
}

/* --------------------------------------------------------------------------
 * Upload: host file → Linux environment (works with all backends)
 * -------------------------------------------------------------------------- */
linux_error_t fs_upload(linux_backend_t *backend,
                        const char *host_path,
                        const char *linux_path) {
    if (!backend || !host_path || !linux_path)
        return LINUX_ERR_INVALID_ARG;

    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return LINUX_ERR_OUT_OF_MEMORY;

    /* Read host file */
    FILE *f = fopen(host_path, "rb");
    if (!f) { free(esc_path); return LINUX_ERR_EXEC_FAILED; }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size < 0 || file_size > 100 * 1024 * 1024) {
        fclose(f); free(esc_path);
        return LINUX_ERR_INVALID_ARG;  /* Limit: 100MB */
    }

    unsigned char *data = (unsigned char *)malloc((size_t)file_size);
    if (!data) { fclose(f); free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }
    size_t nread = fread(data, 1, (size_t)file_size, f);
    fclose(f);

    /* Base64 encode — use actual bytes read, not file_size */
    size_t b64_len;
    char *b64 = base64_encode(data, nread, &b64_len);
    free(data);
    if (!b64) { free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }

    /* Send via exec: echo <base64> | base64 -d > <path>
     * Split into chunks to avoid command line length limits */
    size_t chunk_size = 65536;
    linux_error_t result = LINUX_OK;
    size_t esc_len = strlen(esc_path);

    /* First chunk creates the file */
    size_t offset = 0;
    while (offset < b64_len && result == LINUX_OK) {
        size_t this_chunk = b64_len - offset;
        if (this_chunk > chunk_size) this_chunk = chunk_size;

        if (offset == 0 && offset + this_chunk >= b64_len) {
            /* Single chunk — fits in one command */
            size_t full_len = this_chunk + esc_len + 64;
            char *full = (char *)malloc(full_len);
            if (!full) { result = LINUX_ERR_OUT_OF_MEMORY; break; }
            snprintf(full, full_len,
                     "printf '%%s' '%.*s' | base64 -d > %s",
                     (int)this_chunk, b64 + offset, esc_path);
            int exit_code = -1;
            result = backend->exec(backend, full, NULL, NULL, &exit_code);
            free(full);
            if (result == LINUX_OK && exit_code != 0)
                result = LINUX_ERR_EXEC_FAILED;
        } else {
            /* Multi-chunk: append to temp file in /tmp/linux_template/ */
            const char *redir = (offset == 0) ? ">" : ">>";
            size_t full_len = this_chunk + esc_len + 160;
            char *full = (char *)malloc(full_len);
            if (!full) { result = LINUX_ERR_OUT_OF_MEMORY; break; }
            snprintf(full, full_len,
                     "mkdir -p /tmp/linux_template; "
                     "printf '%%s' '%.*s' %s /tmp/linux_template/_upload.b64tmp",
                     (int)this_chunk, b64 + offset, redir);
            int exit_code = -1;
            result = backend->exec(backend, full, NULL, NULL, &exit_code);
            free(full);
            if (result == LINUX_OK && exit_code != 0)
                result = LINUX_ERR_EXEC_FAILED;
        }
        offset += this_chunk;
    }

    /* If we used multi-chunk, decode the assembled base64 temp file */
    if (result == LINUX_OK && b64_len > chunk_size) {
        size_t decode_len = esc_len + 128;
        char *decode_cmd = (char *)malloc(decode_len);
        if (!decode_cmd) { free(b64); free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }
        snprintf(decode_cmd, decode_len,
                 "base64 -d < /tmp/linux_template/_upload.b64tmp > %s && "
                 "rm -f /tmp/linux_template/_upload.b64tmp",
                 esc_path);
        int exit_code = -1;
        result = backend->exec(backend, decode_cmd, NULL, NULL, &exit_code);
        free(decode_cmd);
        if (result == LINUX_OK && exit_code != 0)
            result = LINUX_ERR_EXEC_FAILED;
    }

    free(b64);
    free(esc_path);
    return result;
}

/* --------------------------------------------------------------------------
 * Download: Linux file → host (works with all backends)
 * -------------------------------------------------------------------------- */
linux_error_t fs_download(linux_backend_t *backend,
                          const char *linux_path,
                          const char *host_path) {
    if (!backend || !linux_path || !host_path)
        return LINUX_ERR_INVALID_ARG;

    /* Read file as base64 from Linux */
    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return LINUX_ERR_OUT_OF_MEMORY;

    /* Check remote file size first to avoid memory exhaustion */
    {
        char size_cmd[512];
        snprintf(size_cmd, sizeof(size_cmd), "stat -c '%%s' %s 2>/dev/null", esc_path);
        char *size_out = NULL;
        backend->exec(backend, size_cmd, &size_out, NULL, NULL);
        if (size_out) {
            long long fsize = atoll(size_out);
            free(size_out);
            if (fsize > 100LL * 1024 * 1024) {
                free(esc_path);
                return LINUX_ERR_INVALID_ARG;  /* > 100MB */
            }
        }
    }

    size_t cmd_len = strlen(esc_path) + 32;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }
    snprintf(cmd, cmd_len, "base64 %s", esc_path);
    free(esc_path);

    char *b64_output = NULL;
    int exit_code = -1;
    linux_error_t rc = backend->exec(backend, cmd, &b64_output, NULL, &exit_code);
    free(cmd);

    if (rc != LINUX_OK) { free(b64_output); return rc; }
    if (exit_code != 0) { free(b64_output); return LINUX_ERR_EXEC_FAILED; }
    if (!b64_output) return LINUX_ERR_EXEC_FAILED;

    /* Strip whitespace from base64 output */
    size_t clean_len = 0;
    for (size_t i = 0; b64_output[i]; i++) {
        if (b64_output[i] != '\n' && b64_output[i] != '\r' &&
            b64_output[i] != ' ')
            b64_output[clean_len++] = b64_output[i];
    }
    b64_output[clean_len] = '\0';

    /* Decode base64 */
    size_t data_len;
    unsigned char *data = base64_decode(b64_output, clean_len, &data_len);
    free(b64_output);
    if (!data) return LINUX_ERR_EXEC_FAILED;

    /* Write to host file */
    FILE *f = fopen(host_path, "wb");
    if (!f) { free(data); return LINUX_ERR_EXEC_FAILED; }
    size_t written = fwrite(data, 1, data_len, f);
    fclose(f);
    free(data);

    return (written == data_len) ? LINUX_OK : LINUX_ERR_EXEC_FAILED;
}

/* --------------------------------------------------------------------------
 * List directory contents
 * -------------------------------------------------------------------------- */
linux_error_t fs_list_dir(linux_backend_t *backend,
                          const char *linux_path,
                          char **listing) {
    if (!backend || !linux_path || !listing)
        return LINUX_ERR_INVALID_ARG;
    *listing = NULL;

    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return LINUX_ERR_OUT_OF_MEMORY;
    size_t cmd_len = strlen(esc_path) + 64;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }

    snprintf(cmd, cmd_len,
             "ls -la %s 2>/dev/null || echo '(empty or not found)'",
             esc_path);
    free(esc_path);

    int exit_code = -1;
    linux_error_t rc = backend->exec(backend, cmd, listing, NULL, &exit_code);
    free(cmd);
    return rc;
}

/* --------------------------------------------------------------------------
 * File stat
 * -------------------------------------------------------------------------- */
linux_error_t fs_stat(linux_backend_t *backend,
                      const char *linux_path,
                      char **info) {
    if (!backend || !linux_path || !info)
        return LINUX_ERR_INVALID_ARG;
    *info = NULL;

    char *esc_path = shell_escape(linux_path);
    if (!esc_path) return LINUX_ERR_OUT_OF_MEMORY;
    size_t cmd_len = strlen(esc_path) * 2 + 128;
    char *cmd = (char *)malloc(cmd_len);
    if (!cmd) { free(esc_path); return LINUX_ERR_OUT_OF_MEMORY; }

    snprintf(cmd, cmd_len,
             "stat -c '%%s %%F %%A %%U:%%G' %s 2>/dev/null || "
             "stat -f '%%z %%HT %%Sp %%Su:%%Sg' %s 2>/dev/null || "
             "echo 'not found'",
             esc_path, esc_path);
    free(esc_path);

    int exit_code = -1;
    linux_error_t rc = backend->exec(backend, cmd, info, NULL, &exit_code);
    free(cmd);
    return rc;
}
