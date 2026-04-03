#ifndef LINUX_GUI_H
#define LINUX_GUI_H

#include "../linux/backend.h"
#include "../bridge/bridge.h"
#include "../bridge/service.h"

/* Launch the webview GUI.
 * This blocks until the window is closed.
 * The backend must already be started. */
int gui_run_webview(linux_backend_t *backend, const linux_config_t *config,
                    service_manager_t *svc_mgr);

#endif /* LINUX_GUI_H */
