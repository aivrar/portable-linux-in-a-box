#ifndef LINUX_DETECT_H
#define LINUX_DETECT_H

#include "backend.h"

/* Probe the system and return the best available backend type. */
linux_backend_type_t linux_detect_best_type(void);

/* Probe and construct the best available backend.
 * Returns NULL if no backend could be created (should not happen once
 * TinyEMU is implemented — the stub always succeeds).
 * The caller owns the returned pointer and must call ->destroy(). */
linux_backend_t *linux_detect_backend(const linux_config_t *config);

#endif /* LINUX_DETECT_H */
