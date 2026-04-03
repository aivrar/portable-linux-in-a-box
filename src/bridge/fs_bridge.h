#ifndef LINUX_FS_BRIDGE_H
#define LINUX_FS_BRIDGE_H

#include "../linux/backend.h"

/* Convert a Windows path to a Linux path visible inside the backend.
 * e.g. "C:\Users\chris\file.txt" -> "/mnt/c/Users/chris/file.txt" (WSL2)
 * Returns heap-allocated string. Caller frees. Returns NULL on failure. */
char *fs_win_to_linux(linux_backend_t *backend, const char *win_path);

/* Convert a Linux path to a Windows-accessible path.
 * e.g. "/home/user/file.txt" -> "\\wsl$\Ubuntu\home\user\file.txt" (WSL2)
 * Returns heap-allocated string. Caller frees. Returns NULL on failure. */
char *fs_linux_to_win(linux_backend_t *backend, const char *linux_path,
                      const char *distro_name);

/* Write a string to a file inside the Linux environment.
 * Uses the backend's exec to write via shell. */
linux_error_t fs_write_file(linux_backend_t *backend,
                            const char *linux_path,
                            const char *content);

/* Read a file from the Linux environment.
 * Returns heap-allocated content. Caller frees. */
linux_error_t fs_read_file(linux_backend_t *backend,
                           const char *linux_path,
                           char **content);

/* Check if a file/directory exists in Linux environment. */
int fs_exists(linux_backend_t *backend, const char *linux_path);

/* Create a directory (and parents) in the Linux environment. */
linux_error_t fs_mkdir(linux_backend_t *backend, const char *linux_path);

/* Upload a file from the Windows host into the Linux environment.
 * Works with ALL backends (WSL, QEMU, WHPX, TinyEMU) by encoding
 * the file as base64 and decoding inside Linux.
 * Handles both text and binary files. */
linux_error_t fs_upload(linux_backend_t *backend,
                        const char *host_path,
                        const char *linux_path);

/* Download a file from the Linux environment to the Windows host.
 * Reads via base64 encoding over the exec pipe. */
linux_error_t fs_download(linux_backend_t *backend,
                          const char *linux_path,
                          const char *host_path);

/* List directory contents. Returns heap-allocated newline-separated list.
 * Caller frees. */
linux_error_t fs_list_dir(linux_backend_t *backend,
                          const char *linux_path,
                          char **listing);

/* Get file info (size, type, permissions).
 * Returns heap-allocated string in "size type perms" format. Caller frees. */
linux_error_t fs_stat(linux_backend_t *backend,
                      const char *linux_path,
                      char **info);

#endif /* LINUX_FS_BRIDGE_H */
