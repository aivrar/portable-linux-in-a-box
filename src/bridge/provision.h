#ifndef LINUX_PROVISION_H
#define LINUX_PROVISION_H

#include "../linux/backend.h"

/* Callback for provisioning progress updates.
 * step: current step number (1-based)
 * total: total steps
 * message: human-readable status */
typedef void (*provision_progress_fn)(int step, int total,
                                      const char *message, void *user);

/* A provisioning step */
typedef struct {
    const char *description;  /* e.g. "Installing Python 3" */
    const char *command;      /* Shell command to run */
    const char *check;        /* Shell command to verify (exit 0 = already done) */
} provision_step_t;

/* A provisioning recipe — list of steps to set up a Linux environment */
typedef struct {
    const char       *name;
    provision_step_t *steps;
    int               step_count;
} provision_recipe_t;

/* Run a provisioning recipe. Skips steps whose check command succeeds.
 * Returns LINUX_OK if all steps complete. */
linux_error_t provision_run(linux_backend_t *backend,
                            const provision_recipe_t *recipe,
                            provision_progress_fn progress,
                            void *user);

/* Built-in recipes */
provision_recipe_t provision_recipe_base(void);       /* Basic dev tools */
provision_recipe_t provision_recipe_python(void);     /* Python + pip */
provision_recipe_t provision_recipe_node(void);       /* Node.js + npm */
provision_recipe_t provision_recipe_vllm(void);       /* vLLM + deps */
provision_recipe_t provision_recipe_llamacpp(void);   /* llama.cpp + small model */
provision_recipe_t provision_recipe_update(void);     /* Update all packages + tools */

#endif /* LINUX_PROVISION_H */
