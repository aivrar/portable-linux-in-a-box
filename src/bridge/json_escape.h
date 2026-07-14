#ifndef LINUX_JSON_ESCAPE_H
#define LINUX_JSON_ESCAPE_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Escape a string for safe embedding inside a JSON double-quoted string.
 *
 * Handles: \\ \" \n \r \t \b \f and control chars < 0x20 as \uXXXX.
 * Returns a heap-allocated string (without surrounding quotes).
 * Caller must free(). Returns strdup("") on NULL input or allocation failure.
 */
static inline char *json_escape(const char *s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    if (len > SIZE_MAX / 6) return NULL;
    char *out = (char *)malloc(len * 6 + 1);
    if (!out) return strdup("");
    char *p = out;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '\\': *p++ = '\\'; *p++ = '\\'; break;
        case '"':  *p++ = '\\'; *p++ = '"';  break;
        case '\n': *p++ = '\\'; *p++ = 'n';  break;
        case '\r': *p++ = '\\'; *p++ = 'r';  break;
        case '\t': *p++ = '\\'; *p++ = 't';  break;
        case '\b': *p++ = '\\'; *p++ = 'b';  break;
        case '\f': *p++ = '\\'; *p++ = 'f';  break;
        default:
            if (c < 0x20) p += snprintf(p, 7, "\\u%04x", c);
            else *p++ = (char)c;
        }
    }
    *p = '\0';
    return out;
}

#endif /* LINUX_JSON_ESCAPE_H */
