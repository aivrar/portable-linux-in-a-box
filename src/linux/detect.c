#include "detect.h"
#include <stdio.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* --------------------------------------------------------------------------
 * Error code to string
 * -------------------------------------------------------------------------- */
const char *linux_error_string(linux_error_t err) {
    switch (err) {
    case LINUX_OK:                return "OK";
    case LINUX_ERR_NOT_AVAILABLE: return "Backend not available";
    case LINUX_ERR_ALREADY_RUNNING: return "Already running";
    case LINUX_ERR_NOT_RUNNING:   return "Not running";
    case LINUX_ERR_START_FAILED:  return "Start failed";
    case LINUX_ERR_EXEC_FAILED:   return "Execution failed";
    case LINUX_ERR_PIPE_FAILED:   return "Pipe failed";
    case LINUX_ERR_TIMEOUT:       return "Timeout";
    case LINUX_ERR_OUT_OF_MEMORY: return "Out of memory";
    case LINUX_ERR_INVALID_ARG:   return "Invalid argument";
    case LINUX_ERR_INTERNAL:      return "Internal error";
    default:                      return "Unknown error";
    }
}

/* --------------------------------------------------------------------------
 * Detection — picks the best available backend
 *
 * Priority:
 *   1. Native (running on Linux)
 *   2. WSL2  (wslapi.dll present)
 *   3. QEMU  (qemu-system-x86_64 found — auto-uses WHPX if available)
 *   4. Stub  (nothing available)
 * -------------------------------------------------------------------------- */

#ifdef _WIN32
static int probe_qemu(void) {
    char found[MAX_PATH];
    if (SearchPathA(NULL, "qemu-system-x86_64.exe", NULL, MAX_PATH, found, NULL))
        return 1;

    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *s = strrchr(exe_dir, '\\');
    if (s) *s = '\0';

    char bundled[MAX_PATH];
    snprintf(bundled, sizeof(bundled), "%s\\qemu\\qemu-system-x86_64.exe", exe_dir);
    if (GetFileAttributesA(bundled) != INVALID_FILE_ATTRIBUTES)
        return 1;

    return 0;
}

static int probe_whpx(void) {
    HMODULE h = LoadLibraryExW(L"WinHvPlatform.dll", NULL,
                                LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) return 0;

    typedef HRESULT (WINAPI *PFN_Cap)(int, void*, UINT32, UINT32*);
    PFN_Cap pCap = (PFN_Cap)GetProcAddress(h, "WHvGetCapability");
    int available = 0;
    if (pCap) {
        BOOL present = FALSE;
        UINT32 written = 0;
        HRESULT hr = pCap(0, &present, sizeof(present), &written);
        if (SUCCEEDED(hr) && present)
            available = 1;
    }
    FreeLibrary(h);
    return available;
}

static int probe_tinyemu(void) {
    char found[MAX_PATH];
    if (SearchPathA(NULL, "temu.exe", NULL, MAX_PATH, found, NULL))
        return 1;

    char exe_dir[MAX_PATH];
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *s = strrchr(exe_dir, '\\');
    if (s) *s = '\0';

    char bundled[MAX_PATH];
    snprintf(bundled, sizeof(bundled), "%s\\temu\\temu.exe", exe_dir);
    if (GetFileAttributesA(bundled) != INVALID_FILE_ATTRIBUTES)
        return 1;

    snprintf(bundled, sizeof(bundled), "%s\\temu.exe", exe_dir);
    if (GetFileAttributesA(bundled) != INVALID_FILE_ATTRIBUTES)
        return 1;

    return 0;
}
#endif

/* --------------------------------------------------------------------------
 * Detection cascade (highest to lowest priority):
 *   1. Native  — running on Linux, zero overhead
 *   2. WSL2    — best Windows option (full kernel, GPU-PV)
 *   3. WHPX    — hardware-accel VM, no WSL dependency
 *   4. QEMU    — full VM (auto-uses WHPX if available)
 *   5. TinyEMU — software emulation fallback (always works)
 *   6. None
 * -------------------------------------------------------------------------- */
linux_backend_type_t linux_detect_best_type(void) {
#if defined(__linux__)
    return LINUX_BACKEND_NATIVE;
#elif defined(__APPLE__)
    /* macOS: QEMU via Homebrew is the primary option */
    return LINUX_BACKEND_QEMU;
#elif defined(_WIN32)
    /* Best: WSL2 */
    HMODULE h = LoadLibraryExW(L"wslapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (h) {
        FreeLibrary(h);
        return LINUX_BACKEND_WSL2;
    }

    /* WHPX: hardware-accelerated VM without WSL */
    if (probe_whpx())
        return LINUX_BACKEND_WHPX;

    /* QEMU: full system emulation (with optional WHPX accel) */
    if (probe_qemu())
        return LINUX_BACKEND_QEMU;

    /* TinyEMU: software emulation fallback */
    if (probe_tinyemu())
        return LINUX_BACKEND_TINYEMU;

    return LINUX_BACKEND_NONE;
#else
    return LINUX_BACKEND_NONE;
#endif
}

linux_backend_t *linux_detect_backend(const linux_config_t *config) {
    linux_backend_type_t best = linux_detect_best_type();
    linux_backend_t *b = NULL;

    LINUX_LOG(config, "Detected best backend type: %d", (int)best);

    switch (best) {
#if defined(__linux__)
    case LINUX_BACKEND_NATIVE:
        b = linux_backend_create_native();
        break;
#endif
#if defined(__APPLE__) || (defined(__unix__) && !defined(__linux__))
    case LINUX_BACKEND_QEMU:
        b = linux_backend_create_qemu_posix();
        break;
#endif
#ifdef _WIN32
    case LINUX_BACKEND_WSL2:
        b = linux_backend_create_wsl();
        break;
    case LINUX_BACKEND_WHPX:
#ifdef HAVE_WHPX
        b = linux_backend_create_whpx();
#else
        /* WHPX backend not compiled in — fall through to QEMU/TinyEMU */
        if (probe_qemu()) { b = linux_backend_create_qemu(); break; }
        if (probe_tinyemu()) { b = linux_backend_create_tinyemu(); break; }
        LINUX_LOG(config, "WHPX detected but backend not compiled in, "
                  "no QEMU/TinyEMU fallback available");
#endif
        break;
    case LINUX_BACKEND_QEMU:
        b = linux_backend_create_qemu();
        break;
    case LINUX_BACKEND_TINYEMU:
        b = linux_backend_create_tinyemu();
        break;
#endif
    default:
        break;
    }

    if (b)
        LINUX_LOG(config, "Created backend: %s", b->name);

    return b;
}
