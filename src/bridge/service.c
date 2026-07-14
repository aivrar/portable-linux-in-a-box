#include "service.h"
#include "http_bridge.h"
#include "json_escape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void svc_init(service_manager_t *mgr, linux_backend_t *backend) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->backend = backend;
    COMPAT_MUTEX_INIT(mgr->lock);
}

int svc_register(service_manager_t *mgr,
                 const char *name,
                 const char *command,
                 const char *health_url,
                 int port) {
    COMPAT_MUTEX_LOCK(mgr->lock);

    if (mgr->count >= MAX_SERVICES) {
        COMPAT_MUTEX_UNLOCK(mgr->lock);
        return -1;
    }

    int idx = mgr->count++;
    linux_service_t *s = &mgr->services[idx];
    memset(s, 0, sizeof(*s));

    /* Sanitize name to [a-zA-Z0-9_-] — it's interpolated into shell
     * commands as a log filename, so metacharacters must be stripped. */
    {
        int j = 0;
        for (int i = 0; name[i] && j < (int)sizeof(s->name) - 1; i++) {
            char c = name[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_' || c == '-')
                s->name[j++] = c;
        }
        s->name[j] = '\0';
        if (!s->name[0]) snprintf(s->name, sizeof(s->name), "svc%d", idx);
    }
    snprintf(s->command, sizeof(s->command), "%s", command);
    if (health_url)
        snprintf(s->health_url, sizeof(s->health_url), "%s", health_url);
    s->port = port;
    s->state = SVC_STOPPED;

    COMPAT_MUTEX_UNLOCK(mgr->lock);
    return idx;
}

linux_error_t svc_start(service_manager_t *mgr, int index) {
    if (index < 0 || index >= mgr->count)
        return LINUX_ERR_INVALID_ARG;

    COMPAT_MUTEX_LOCK(mgr->lock);
    linux_service_t *s = &mgr->services[index];
    s->state = SVC_STARTING;
    s->error[0] = '\0';

    /* Build background launch command:
     * nohup <command> > /tmp/linux_template/svc_<name>.log 2>&1 &
     * echo $!  (prints the PID) */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p /tmp/linux_template; "
             "nohup %s > /tmp/linux_template/svc_%s.log 2>&1 & echo $!",
             s->command, s->name);

    char *out = NULL;
    char *err = NULL;
    int exit_code = -1;

    linux_error_t rc = mgr->backend->exec(mgr->backend, cmd, &out, &err, &exit_code);
    if (rc != LINUX_OK || exit_code != 0) {
        s->state = SVC_FAILED;
        snprintf(s->error, sizeof(s->error), "Failed to start: %s",
                 err ? err : linux_error_string(rc));
        free(out); free(err);
        COMPAT_MUTEX_UNLOCK(mgr->lock);
        return rc != LINUX_OK ? rc : LINUX_ERR_EXEC_FAILED;
    }

    /* Store the PID for targeted kill/check later */
    s->pid = (out && out[0]) ? atoi(out) : 0;
    if (s->pid <= 0) s->pid = 0;  /* Invalid PID, discard */

    free(out);
    free(err);

    s->state = SVC_RUNNING;
    COMPAT_MUTEX_UNLOCK(mgr->lock);
    return LINUX_OK;
}

linux_error_t svc_stop(service_manager_t *mgr, int index) {
    if (index < 0 || index >= mgr->count)
        return LINUX_ERR_INVALID_ARG;

    COMPAT_MUTEX_LOCK(mgr->lock);
    linux_service_t *s = &mgr->services[index];

    /* Kill by PID (and its children) if known.
     * SIGTERM first with grace period, then SIGKILL. Guard against PID <= 1.
     * kill -- -PID sends to the process group (catches child processes like
     * VLLM::EngineCore and multiprocessing.resource_tracker). Do not use
     * bare port-based kill here; a different process may have grabbed the
     * port after this service died. */
    char cmd[512];
    if (s->pid > 1 && s->port > 0) {
        snprintf(cmd, sizeof(cmd),
                 "kill -- -%d 2>/dev/null; kill %d 2>/dev/null; sleep 2; "
                 "kill -0 %d 2>/dev/null && "
                 "(kill -9 -- -%d 2>/dev/null; kill -9 %d 2>/dev/null); "
                 "echo done",
                 s->pid, s->pid, s->pid, s->pid, s->pid);
    } else if (s->pid > 1) {
        snprintf(cmd, sizeof(cmd),
                 "kill -- -%d 2>/dev/null; kill %d 2>/dev/null; sleep 2; "
                 "kill -0 %d 2>/dev/null && "
                 "(kill -9 -- -%d 2>/dev/null; kill -9 %d 2>/dev/null); "
                 "echo done",
                 s->pid, s->pid, s->pid, s->pid, s->pid);
    } else if (s->port > 0) {
        snprintf(cmd, sizeof(cmd), "echo no-pid-for-port-%d", s->port);
    } else {
        snprintf(cmd, sizeof(cmd), "echo done");
    }
    COMPAT_MUTEX_UNLOCK(mgr->lock);

    mgr->backend->exec(mgr->backend, cmd, NULL, NULL, NULL);

    COMPAT_MUTEX_LOCK(mgr->lock);
    s->state = SVC_STOPPED;
    COMPAT_MUTEX_UNLOCK(mgr->lock);
    return LINUX_OK;
}

service_state_t svc_check(service_manager_t *mgr, int index) {
    if (index < 0 || index >= mgr->count)
        return SVC_STOPPED;

    COMPAT_MUTEX_LOCK(mgr->lock);
    linux_service_t *s = &mgr->services[index];

    /* If we have a health URL, try HTTP GET.
     * 503 means alive but still loading (SVC_STARTING).
     * Other responses mean fully running. */
    if (s->health_url[0]) {
        http_response_t resp;
        linux_error_t rc = http_get(s->health_url, &resp);
        if (rc == LINUX_OK && resp.status_code > 0) {
            s->state = (resp.status_code == 503) ? SVC_STARTING : SVC_RUNNING;
            http_response_free(&resp);
            service_state_t result = s->state;
            COMPAT_MUTEX_UNLOCK(mgr->lock);
            return result;
        }
        http_response_free(&resp);
    }

    /* Check if process is listening on port.
     * Use bash /dev/tcp probe — works without iproute2/ss. */
    if (s->port > 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "(echo > /dev/tcp/127.0.0.1/%d) 2>/dev/null && echo UP || echo DOWN", s->port);

        char *out = NULL;
        int exit_code = -1;
        COMPAT_MUTEX_UNLOCK(mgr->lock);
        mgr->backend->exec(mgr->backend, cmd, &out, NULL, &exit_code);
        COMPAT_MUTEX_LOCK(mgr->lock);

        if (out && strncmp(out, "UP", 2) == 0) {
            s->state = SVC_RUNNING;
            free(out);
            service_state_t result = s->state;
            COMPAT_MUTEX_UNLOCK(mgr->lock);
            return result;
        }
        free(out);
    }

    /* If we got here and state was RUNNING, it might have died */
    if (s->pid > 1 && (s->state == SVC_RUNNING || s->state == SVC_STARTING)) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "kill -0 %d 2>/dev/null && echo UP || echo DOWN",
                 s->pid);
        char *out = NULL;
        COMPAT_MUTEX_UNLOCK(mgr->lock);
        mgr->backend->exec(mgr->backend, cmd, &out, NULL, NULL);
        COMPAT_MUTEX_LOCK(mgr->lock);
        if (out && strncmp(out, "UP", 2) == 0) {
            s->state = SVC_RUNNING;
        } else {
            s->state = SVC_FAILED;
            snprintf(s->error, sizeof(s->error), "Process exited unexpectedly");
        }
        free(out);
    }

    service_state_t result = s->state;
    COMPAT_MUTEX_UNLOCK(mgr->lock);
    return result;
}

void svc_stop_all(service_manager_t *mgr) {
    COMPAT_MUTEX_LOCK(mgr->lock);
    int count = mgr->count;
    int to_stop[MAX_SERVICES];
    int n_stop = 0;
    for (int i = 0; i < count; i++) {
        if (mgr->services[i].state == SVC_RUNNING ||
            mgr->services[i].state == SVC_STARTING) {
            to_stop[n_stop++] = i;
        }
    }
    COMPAT_MUTEX_UNLOCK(mgr->lock);
    for (int i = 0; i < n_stop; i++) {
        svc_stop(mgr, to_stop[i]);
    }
}

int svc_find(service_manager_t *mgr, const char *name) {
    for (int i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->services[i].name, name) == 0)
            return i;
    }
    return -1;
}

char *svc_status_json(service_manager_t *mgr) {
    /* Returns a heap-allocated JSON array string. Caller must free() the result. */
    COMPAT_MUTEX_LOCK(mgr->lock);
    growbuf_t gb;
    growbuf_init(&gb, 1024);
    growbuf_append(&gb, "[", 1);

    for (int i = 0; i < mgr->count; i++) {
        linux_service_t *s = &mgr->services[i];
        const char *state_str;
        switch (s->state) {
        case SVC_STOPPED:  state_str = "stopped";  break;
        case SVC_STARTING: state_str = "starting"; break;
        case SVC_RUNNING:  state_str = "running";  break;
        case SVC_FAILED:   state_str = "failed";   break;
        default:               state_str = "unknown";  break;
        }

        char *name_esc = json_escape(s->name);
        char *err_esc = json_escape(s->error);
        char entry[512];
        int n = snprintf(entry, sizeof(entry),
                 "%s{\"name\":\"%s\",\"state\":\"%s\",\"port\":%d,\"error\":\"%s\"}",
                 i > 0 ? "," : "", name_esc, state_str, s->port, err_esc);
        free(name_esc);
        free(err_esc);
        growbuf_append(&gb, entry, (size_t)n);
    }

    growbuf_append(&gb, "]", 1);
    COMPAT_MUTEX_UNLOCK(mgr->lock);
    return growbuf_finish(&gb);
}
