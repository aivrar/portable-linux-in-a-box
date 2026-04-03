#include "service.h"
#include "http_bridge.h"
#include "json_escape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void svc_init(service_manager_t *mgr, linux_backend_t *backend) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->backend = backend;
}

int svc_register(service_manager_t *mgr,
                 const char *name,
                 const char *command,
                 const char *health_url,
                 int port) {
    if (mgr->count >= MAX_SERVICES) return -1;

    int idx = mgr->count++;
    linux_service_t *s = &mgr->services[idx];
    memset(s, 0, sizeof(*s));

    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->command, sizeof(s->command), "%s", command);
    if (health_url)
        snprintf(s->health_url, sizeof(s->health_url), "%s", health_url);
    s->port = port;
    s->state = SERVICE_STOPPED;

    return idx;
}

linux_error_t svc_start(service_manager_t *mgr, int index) {
    if (index < 0 || index >= mgr->count)
        return LINUX_ERR_INVALID_ARG;

    linux_service_t *s = &mgr->services[index];
    s->state = SERVICE_STARTING;
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
        s->state = SERVICE_FAILED;
        snprintf(s->error, sizeof(s->error), "Failed to start: %s",
                 err ? err : linux_error_string(rc));
        free(out); free(err);
        return rc != LINUX_OK ? rc : LINUX_ERR_EXEC_FAILED;
    }

    /* Store the PID for targeted kill/check later */
    s->pid = (out && out[0]) ? atoi(out) : 0;

    free(out);
    free(err);

    /* Give it a moment, then check */
    s->state = SERVICE_RUNNING;
    return LINUX_OK;
}

linux_error_t svc_stop(service_manager_t *mgr, int index) {
    if (index < 0 || index >= mgr->count)
        return LINUX_ERR_INVALID_ARG;

    linux_service_t *s = &mgr->services[index];

    /* Kill by PID if known, fall back to port-based kill */
    char cmd[512];
    if (s->pid > 0) {
        snprintf(cmd, sizeof(cmd),
                 "kill %d 2>/dev/null; "
                 "kill -9 %d 2>/dev/null; "
                 "echo done", s->pid, s->pid);
    } else if (s->port > 0) {
        snprintf(cmd, sizeof(cmd),
                 "fuser -k %d/tcp 2>/dev/null; echo done", s->port);
    } else {
        snprintf(cmd, sizeof(cmd), "echo done");
    }

    mgr->backend->exec(mgr->backend, cmd, NULL, NULL, NULL);
    s->state = SERVICE_STOPPED;
    return LINUX_OK;
}

service_state_t svc_check(service_manager_t *mgr, int index) {
    if (index < 0 || index >= mgr->count)
        return SERVICE_STOPPED;

    linux_service_t *s = &mgr->services[index];

    /* If we have a health URL, try HTTP GET */
    if (s->health_url[0]) {
        http_response_t resp;
        linux_error_t rc = http_get(s->health_url, &resp);
        if (rc == LINUX_OK && resp.status_code >= 200 && resp.status_code < 500) {
            s->state = SERVICE_RUNNING;
            http_response_free(&resp);
            return SERVICE_RUNNING;
        }
        http_response_free(&resp);
    }

    /* Check if process is listening on port */
    if (s->port > 0) {
        char cmd[128];
        snprintf(cmd, sizeof(cmd),
                 "ss -tln | grep -q ':%d ' && echo UP || echo DOWN", s->port);

        char *out = NULL;
        int exit_code = -1;
        mgr->backend->exec(mgr->backend, cmd, &out, NULL, &exit_code);

        if (out && strncmp(out, "UP", 2) == 0) {
            s->state = SERVICE_RUNNING;
            free(out);
            return SERVICE_RUNNING;
        }
        free(out);
    }

    /* If we got here and state was RUNNING, it might have died */
    if (s->pid > 0 && (s->state == SERVICE_RUNNING || s->state == SERVICE_STARTING)) {
        /* Check if the exact PID is still alive */
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "kill -0 %d 2>/dev/null && echo UP || echo DOWN",
                 s->pid);
        char *out = NULL;
        mgr->backend->exec(mgr->backend, cmd, &out, NULL, NULL);
        if (out && strncmp(out, "UP", 2) == 0) {
            s->state = SERVICE_RUNNING;
        } else {
            s->state = SERVICE_FAILED;
            snprintf(s->error, sizeof(s->error), "Process exited unexpectedly");
        }
        free(out);
    }

    return s->state;
}

void svc_stop_all(service_manager_t *mgr) {
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->services[i].state == SERVICE_RUNNING ||
            mgr->services[i].state == SERVICE_STARTING) {
            svc_stop(mgr, i);
        }
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
    /* Build JSON array of service statuses */
    growbuf_t gb;
    growbuf_init(&gb, 1024);
    growbuf_append(&gb, "[", 1);

    for (int i = 0; i < mgr->count; i++) {
        linux_service_t *s = &mgr->services[i];
        const char *state_str;
        switch (s->state) {
        case SERVICE_STOPPED:  state_str = "stopped";  break;
        case SERVICE_STARTING: state_str = "starting"; break;
        case SERVICE_RUNNING:  state_str = "running";  break;
        case SERVICE_FAILED:   state_str = "failed";   break;
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
    return growbuf_finish(&gb);
}
