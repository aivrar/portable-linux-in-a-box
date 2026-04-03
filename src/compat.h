#ifndef LINUX_TEMPLATE_COMPAT_H
#define LINUX_TEMPLATE_COMPAT_H

/*
 * Portability macros for threading, timing, and string ops.
 * Allows the same code to compile on Windows (MSVC) and POSIX (macOS/Linux).
 */

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* Threading */
#define THREAD_FUNC_DECL    DWORD WINAPI
#define THREAD_FUNC_RET     return 0
#define THREAD_PARAM        LPVOID

static inline void compat_thread_launch(DWORD (WINAPI *fn)(LPVOID), void *arg) {
    HANDLE h = CreateThread(NULL, 0, fn, arg, 0, NULL);
    if (h) CloseHandle(h);
}

/* Timing */
#define COMPAT_TICK_MS()    GetTickCount()
#define COMPAT_SLEEP_MS(ms) Sleep(ms)

/* String */
#define COMPAT_STRTOK(str, delim, ctx)  strtok_s(str, delim, ctx)

#else /* POSIX (macOS, Linux) */

#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

/* Threading */
#define THREAD_FUNC_DECL    void *
#define THREAD_FUNC_RET     return NULL
#define THREAD_PARAM        void *

static inline void compat_thread_launch(void *(*fn)(void *), void *arg) {
    pthread_t t;
    if (pthread_create(&t, NULL, fn, arg) == 0)
        pthread_detach(t);
}

/* Timing */
static inline unsigned long compat_tick_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
#define COMPAT_TICK_MS()    compat_tick_ms()
#define COMPAT_SLEEP_MS(ms) usleep((unsigned)(ms) * 1000)

/* String */
#define COMPAT_STRTOK(str, delim, ctx)  strtok_r(str, delim, ctx)

#endif /* _WIN32 */

#endif /* LINUX_TEMPLATE_COMPAT_H */
