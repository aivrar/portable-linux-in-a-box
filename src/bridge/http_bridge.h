#ifndef LINUX_HTTP_BRIDGE_H
#define LINUX_HTTP_BRIDGE_H

#include "../linux/backend_types.h"

/* Simple HTTP response */
typedef struct {
    int    status_code;
    char  *body;        /* Heap-allocated, caller frees with free() */
    size_t body_len;
} http_response_t;

/* Perform an HTTP GET request.
 * Returns LINUX_OK on success (even if status != 200).
 * Caller must call http_response_free() on the response. */
linux_error_t http_get(const char *url, http_response_t *response);

/* Perform an HTTP POST request with a body.
 * content_type: e.g. "application/json"
 * Caller must call http_response_free() on the response. */
linux_error_t http_post(const char *url,
                        const char *content_type,
                        const char *body,
                        size_t body_len,
                        http_response_t *response);

/* Free response resources. */
void http_response_free(http_response_t *response);

#endif /* LINUX_HTTP_BRIDGE_H */
