#include "http_bridge.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void http_response_free(http_response_t *response) {
    if (!response) return;
    free(response->body);
    response->body = NULL;
    response->body_len = 0;
    response->status_code = 0;
}

/* ======================================================================
 * Windows implementation using WinHTTP
 * ====================================================================== */
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

/* Parse a URL into components for WinHTTP */
typedef struct {
    wchar_t host[256];
    wchar_t path[1024];
    INTERNET_PORT port;
    BOOL   secure;
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out) {
    /* Convert URL to wide string */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, url, -1, NULL, 0);
    wchar_t *wurl = (wchar_t *)malloc(wlen * sizeof(wchar_t));
    if (!wurl) return -1;
    MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl, wlen);

    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = out->host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = out->path;
    uc.dwUrlPathLength = 1024;

    BOOL ok = WinHttpCrackUrl(wurl, 0, 0, &uc);
    free(wurl);
    if (!ok) return -1;

    out->port = uc.nPort;
    out->secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    return 0;
}

static linux_error_t winhttp_request(const char *method_a,
                                     const char *url,
                                     const char *content_type,
                                     const char *req_body,
                                     size_t req_body_len,
                                     http_response_t *response) {
    memset(response, 0, sizeof(*response));

    parsed_url_t pu;
    if (parse_url(url, &pu) < 0) return LINUX_ERR_INVALID_ARG;

    /* Convert method to wide */
    wchar_t method[16];
    MultiByteToWideChar(CP_UTF8, 0, method_a, -1, method, 16);

    linux_error_t result = LINUX_ERR_INTERNAL;
    HINTERNET hSession = NULL, hConnect = NULL, hRequest = NULL;

    hSession = WinHttpOpen(L"linux-template/1.0",
                           WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                           WINHTTP_NO_PROXY_NAME,
                           WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto cleanup;

    hConnect = WinHttpConnect(hSession, pu.host, pu.port, 0);
    if (!hConnect) goto cleanup;

    DWORD flags = pu.secure ? WINHTTP_FLAG_SECURE : 0;
    hRequest = WinHttpOpenRequest(hConnect, method, pu.path,
                                  NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) goto cleanup;

    /* Add content-type header if provided */
    if (content_type) {
        wchar_t hdr[512];
        swprintf(hdr, 512, L"Content-Type: %hs", content_type);
        WinHttpAddRequestHeaders(hRequest, hdr, (ULONG)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD);
    }

    /* Send request */
    BOOL sent = WinHttpSendRequest(hRequest,
                                   WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                   (LPVOID)req_body,
                                   (DWORD)req_body_len,
                                   (DWORD)req_body_len, 0);
    if (!sent) goto cleanup;

    if (!WinHttpReceiveResponse(hRequest, NULL)) goto cleanup;

    /* Get status code */
    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    response->status_code = (int)status;

    /* Read response body */
    growbuf_t gb;
    growbuf_init(&gb, 4096);

    DWORD avail = 0, read = 0;
    char buf[4096];
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
        DWORD to_read = avail < sizeof(buf) ? avail : sizeof(buf);
        if (WinHttpReadData(hRequest, buf, to_read, &read) && read > 0)
            growbuf_append(&gb, buf, read);
    }

    size_t body_len = gb.len;
    response->body = growbuf_finish(&gb);
    response->body_len = response->body ? body_len : 0;
    result = LINUX_OK;

cleanup:
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);
    return result;
}

linux_error_t http_get(const char *url, http_response_t *response) {
    return winhttp_request("GET", url, NULL, NULL, 0, response);
}

linux_error_t http_post(const char *url, const char *content_type,
                        const char *body, size_t body_len,
                        http_response_t *response) {
    return winhttp_request("POST", url, content_type, body, body_len, response);
}

/* ======================================================================
 * Linux implementation using POSIX sockets
 * ====================================================================== */
#elif defined(__linux__) || defined(__unix__)

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>

static linux_error_t socket_request(const char *method,
                                    const char *url,
                                    const char *content_type,
                                    const char *req_body,
                                    size_t req_body_len,
                                    http_response_t *response) {
    memset(response, 0, sizeof(*response));

    /* Parse URL: http://host:port/path */
    if (strncmp(url, "http://", 7) != 0)
        return LINUX_ERR_INVALID_ARG;  /* Only HTTP for now */

    const char *hoststart = url + 7;
    const char *pathstart = strchr(hoststart, '/');
    const char *path = pathstart ? pathstart : "/";

    /* Extract host and port */
    char host[256];
    char port_str[8] = "80";
    size_t hostlen;

    const char *colon = memchr(hoststart, ':', pathstart ? (size_t)(pathstart - hoststart) : strlen(hoststart));
    if (colon) {
        hostlen = (size_t)(colon - hoststart);
        size_t portlen = pathstart ? (size_t)(pathstart - colon - 1) : strlen(colon + 1);
        if (portlen >= sizeof(port_str)) return LINUX_ERR_INVALID_ARG;
        memcpy(port_str, colon + 1, portlen);
        port_str[portlen] = '\0';
    } else {
        hostlen = pathstart ? (size_t)(pathstart - hoststart) : strlen(hoststart);
    }
    if (hostlen >= sizeof(host)) return LINUX_ERR_INVALID_ARG;
    memcpy(host, hoststart, hostlen);
    host[hostlen] = '\0';

    /* Resolve and connect */
    struct addrinfo hints = {0}, *res;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return LINUX_ERR_EXEC_FAILED;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return LINUX_ERR_EXEC_FAILED; }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd); freeaddrinfo(res); return LINUX_ERR_EXEC_FAILED;
    }
    freeaddrinfo(res);

    /* Build HTTP request */
    growbuf_t req;
    growbuf_init(&req, 1024);
    char line[512];
    int n;

    n = snprintf(line, sizeof(line), "%s %s HTTP/1.1\r\n", method, path);
    growbuf_append(&req, line, (size_t)n);

    n = snprintf(line, sizeof(line), "Host: %s\r\n", host);
    growbuf_append(&req, line, (size_t)n);

    growbuf_append(&req, "Connection: close\r\n", 19);

    if (content_type) {
        n = snprintf(line, sizeof(line), "Content-Type: %s\r\n", content_type);
        growbuf_append(&req, line, (size_t)n);
    }
    if (req_body && req_body_len > 0) {
        n = snprintf(line, sizeof(line), "Content-Length: %zu\r\n", req_body_len);
        growbuf_append(&req, line, (size_t)n);
    }
    growbuf_append(&req, "\r\n", 2);
    if (req_body && req_body_len > 0)
        growbuf_append(&req, req_body, req_body_len);

    /* Send — save length before finish since growbuf_finish resets len.
     * Using strlen() would truncate binary POST bodies at the first NUL. */
    size_t reqlen = req.len;
    char *reqdata = growbuf_finish(&req);
    ssize_t sent = 0;

    const char *p = reqdata;
    size_t remaining = reqlen;
    while (remaining > 0) {
        sent = write(fd, p, remaining);
        if (sent <= 0) { free(reqdata); close(fd); return LINUX_ERR_EXEC_FAILED; }
        p += sent;
        remaining -= (size_t)sent;
    }
    free(reqdata);

    /* Read response */
    growbuf_t resp_buf;
    growbuf_init(&resp_buf, 4096);
    char buf[4096];
    ssize_t rd;
    while ((rd = read(fd, buf, sizeof(buf))) > 0)
        growbuf_append(&resp_buf, buf, (size_t)rd);
    close(fd);

    size_t raw_len = resp_buf.len;
    char *raw = growbuf_finish(&resp_buf);
    if (!raw) return LINUX_ERR_OUT_OF_MEMORY;

    /* Parse status line: HTTP/1.1 200 OK\r\n */
    char *status_start = strchr(raw, ' ');
    if (status_start) response->status_code = atoi(status_start + 1);

    /* Find body (after \r\n\r\n) — use tracked length, not strlen,
     * to correctly handle binary response bodies with NUL bytes. */
    char *body_start = strstr(raw, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        response->body_len = raw_len - (size_t)(body_start - raw);
        response->body = (char *)malloc(response->body_len + 1);
        if (response->body) {
            memcpy(response->body, body_start, response->body_len);
            response->body[response->body_len] = '\0';
        }
    }
    free(raw);

    return LINUX_OK;
}

linux_error_t http_get(const char *url, http_response_t *response) {
    return socket_request("GET", url, NULL, NULL, 0, response);
}

linux_error_t http_post(const char *url, const char *content_type,
                        const char *body, size_t body_len,
                        http_response_t *response) {
    return socket_request("POST", url, content_type, body, body_len, response);
}

#endif
