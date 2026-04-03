#ifndef LINUX_SERVICE_H
#define LINUX_SERVICE_H

#include "../linux/backend.h"

/* Maximum number of managed services */
#define MAX_SERVICES 16

/* Service state */
typedef enum {
    SERVICE_STOPPED = 0,
    SERVICE_STARTING,
    SERVICE_RUNNING,
    SERVICE_FAILED
} service_state_t;

/* A managed background Linux service */
typedef struct {
    char    name[64];          /* Human-readable name, e.g. "vllm" */
    char    command[1024];     /* Shell command to start */
    char    health_url[256];   /* HTTP URL to check if ready (optional) */
    int     port;              /* Port it listens on (0 if unknown) */
    int     pid;               /* Remote PID (from echo $!) */
    service_state_t state;
    char    error[256];        /* Last error message */
} linux_service_t;

/* Service manager */
typedef struct {
    linux_backend_t *backend;
    linux_service_t  services[MAX_SERVICES];
    int              count;
} service_manager_t;

/* Initialize the service manager */
void svc_init(service_manager_t *mgr, linux_backend_t *backend);

/* Register a service (does not start it).
 * Returns the service index, or -1 on failure. */
int svc_register(service_manager_t *mgr,
                 const char *name,
                 const char *command,
                 const char *health_url,
                 int port);

/* Start a registered service (runs command in background via nohup). */
linux_error_t svc_start(service_manager_t *mgr, int index);

/* Stop a service (kills the process). */
linux_error_t svc_stop(service_manager_t *mgr, int index);

/* Check if a service is responding (via health_url or port check). */
service_state_t svc_check(service_manager_t *mgr, int index);

/* Stop all running services. */
void svc_stop_all(service_manager_t *mgr);

/* Get service by name. Returns index or -1. */
int svc_find(service_manager_t *mgr, const char *name);

/* Get service info as JSON string (heap-allocated, caller frees). */
char *svc_status_json(service_manager_t *mgr);

#endif /* LINUX_SERVICE_H */
