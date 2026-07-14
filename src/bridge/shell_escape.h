#ifndef LINUX_SHELL_ESCAPE_H
#define LINUX_SHELL_ESCAPE_H

#include <stdlib.h>
#include <string.h>

/*
 * Escape a string for safe use as a POSIX shell argument inside single quotes.
 *
 * Returns a heap-allocated string WITH surrounding single quotes.
 * Embedded single quotes are escaped as: '\''
 * (end single-quoted region, insert literal quote via backslash, restart region)
 *
 * Caller must free() the returned string.
 * Returns NULL on allocation failure or if s is NULL.
 *
 * Example: shell_escape("hello'world") -> "'hello'\\''world'"
 *          shell_escape("safe")        -> "'safe'"
 */
static inline char *shell_escape(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    if (len > (SIZE_MAX - 3) / 4) return NULL;
    /* Worst case: every char is a single quote -> 4x expansion + 2 outer quotes + NUL */
    char *out = (char *)malloc(len * 4 + 3);
    if (!out) return NULL;
    char *p = out;
    *p++ = '\'';
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\'') {
            *p++ = '\'';  /* end current single-quoted region */
            *p++ = '\\';  /* backslash-escape */
            *p++ = '\'';  /* the literal single quote */
            *p++ = '\'';  /* restart single-quoted region */
        } else {
            *p++ = s[i];
        }
    }
    *p++ = '\'';
    *p = '\0';
    return out;
}

#endif /* LINUX_SHELL_ESCAPE_H */
