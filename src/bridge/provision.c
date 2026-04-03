#include "provision.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

linux_error_t provision_run(linux_backend_t *backend,
                            const provision_recipe_t *recipe,
                            provision_progress_fn progress,
                            void *user) {
    if (!backend || !recipe) return LINUX_ERR_INVALID_ARG;

    for (int i = 0; i < recipe->step_count; i++) {
        const provision_step_t *step = &recipe->steps[i];

        if (progress)
            progress(i + 1, recipe->step_count, step->description, user);

        /* Check if step is already done */
        if (step->check) {
            int exit_code = -1;
            backend->exec(backend, step->check, NULL, NULL, &exit_code);
            if (exit_code == 0) {
                if (progress) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "%s (already done)", step->description);
                    progress(i + 1, recipe->step_count, msg, user);
                }
                continue;  /* Skip — already provisioned */
            }
        }

        /* Run the step */
        char *out = NULL, *err = NULL;
        int exit_code = -1;
        linux_error_t rc = backend->exec(backend, step->command,
                                         &out, &err, &exit_code);
        free(out);

        if (rc != LINUX_OK || exit_code != 0) {
            if (progress) {
                char msg[512];
                snprintf(msg, sizeof(msg), "FAILED: %s — %s",
                         step->description, err ? err : "unknown error");
                progress(i + 1, recipe->step_count, msg, user);
            }
            free(err);
            return rc != LINUX_OK ? rc : LINUX_ERR_EXEC_FAILED;
        }
        free(err);
    }
    return LINUX_OK;
}

/* --------------------------------------------------------------------------
 * Built-in recipes
 * -------------------------------------------------------------------------- */

static provision_step_t base_steps[] = {
    { "Updating package lists",
      "sudo apt-get update -y -qq",
      NULL },
    { "Installing build essentials",
      "sudo apt-get install -y -qq build-essential curl wget git",
      "which gcc && which curl && which git" },
};

provision_recipe_t provision_recipe_base(void) {
    provision_recipe_t r = { "Base Development Tools", base_steps, 2 };
    return r;
}

static provision_step_t python_steps[] = {
    { "Installing Python 3 + pip",
      "sudo apt-get install -y -qq python3 python3-pip python3-venv",
      "python3 --version && pip3 --version" },
};

provision_recipe_t provision_recipe_python(void) {
    provision_recipe_t r = { "Python 3", python_steps, 1 };
    return r;
}

static provision_step_t node_steps[] = {
    { "Installing Node.js + npm",
      "curl -fsSL https://deb.nodesource.com/setup_lts.x | sudo -E bash - && sudo apt-get install -y -qq nodejs",
      "node --version && npm --version" },
};

provision_recipe_t provision_recipe_node(void) {
    provision_recipe_t r = { "Node.js", node_steps, 1 };
    return r;
}

static provision_step_t vllm_steps[] = {
    { "Updating package lists",
      "sudo apt-get update -y -qq",
      NULL },
    { "Installing Python 3 + pip",
      "sudo apt-get install -y -qq python3 python3-pip python3-venv",
      "python3 --version" },
    { "Creating vLLM virtual environment",
      "python3 -m venv ~/vllm-env",
      "test -d ~/vllm-env" },
    { "Installing vLLM (this may take a while)",
      "~/vllm-env/bin/pip install vllm",
      "~/vllm-env/bin/python -c 'import vllm'" },
};

provision_recipe_t provision_recipe_vllm(void) {
    provision_recipe_t r = { "vLLM", vllm_steps, 4 };
    return r;
}

static provision_step_t llamacpp_steps[] = {
    { "Installing build dependencies",
      "sudo apt-get update -y -qq && "
      "sudo apt-get install -y -qq build-essential cmake git curl "
      "|| sudo apk add build-base cmake git curl",
      "which cmake && which g++" },
    { "Installing CUDA toolkit (if GPU available)",
      "if nvidia-smi >/dev/null 2>&1; then "
      "  sudo apt-get install -y -qq nvidia-cuda-toolkit 2>/dev/null || "
      "  (mkdir -p /tmp/linux_template && "
      "   curl -fsSL https://developer.download.nvidia.com/compute/cuda/repos/wsl-ubuntu/x86_64/cuda-keyring_1.1-1_all.deb -o /tmp/linux_template/cuda-keyring.deb && "
      "   sudo dpkg -i /tmp/linux_template/cuda-keyring.deb && sudo apt-get update -y -qq && "
      "   sudo apt-get install -y -qq cuda-toolkit 2>/dev/null) || true; "
      "  echo 'CUDA setup attempted'; "
      "else echo 'No GPU detected, will build CPU-only'; fi",
      NULL },
    { "Cloning llama.cpp",
      "git clone --depth 1 https://github.com/ggerganov/llama.cpp.git ~/llama.cpp",
      "test -d ~/llama.cpp/.git" },
    { "Building llama.cpp (auto-detects GPU)",
      "cd ~/llama.cpp && "
      "if which nvcc >/dev/null 2>&1; then "
      "  echo 'Building with CUDA (GPU)' && "
      "  cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON; "
      "else "
      "  echo 'Building CPU-only' && "
      "  cmake -B build -DCMAKE_BUILD_TYPE=Release; "
      "fi && "
      "cmake --build build --config Release -j$(nproc)",
      "test -x ~/llama.cpp/build/bin/llama-server" },
    { "Downloading a small test model (TinyLlama 1.1B Q4)",
      "mkdir -p ~/models && "
      "curl -fsSL -o ~/models/tinyllama-1.1b-q4_0.gguf "
      "'https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_0.gguf'",
      "test -f ~/models/tinyllama-1.1b-q4_0.gguf" },
};

provision_recipe_t provision_recipe_llamacpp(void) {
    provision_recipe_t r = { "llama.cpp", llamacpp_steps, 5 };
    return r;
}

static provision_step_t update_steps[] = {
    { "Updating system packages (apt/apk)",
      "sudo apt-get update -y -qq 2>/dev/null && sudo apt-get upgrade -y -qq 2>/dev/null || "
      "sudo apk update 2>/dev/null && sudo apk upgrade 2>/dev/null || true",
      NULL },
    { "Upgrading Python packages (pip)",
      "pip3 list --outdated --format=columns 2>/dev/null | tail -n +3 | "
      "awk '{print $1}' | xargs -r pip3 install --upgrade 2>/dev/null; "
      "test -d ~/vllm-env && ~/vllm-env/bin/pip install --upgrade vllm 2>/dev/null; "
      "echo done",
      "which pip3" },
    { "Upgrading Node.js packages (npm)",
      "npm update -g 2>/dev/null; echo done",
      "which npm" },
    { "Upgrading Rust toolchain (rustup)",
      "rustup update 2>/dev/null; "
      "cargo install-update -a 2>/dev/null || echo 'cargo-update not installed, skipping crate updates'; "
      "echo done",
      "which rustup" },
    { "Upgrading Go packages",
      "go install golang.org/dl/gotip@latest 2>/dev/null; "
      "go get -u all 2>/dev/null || true; echo done",
      "which go" },
    { "Upgrading Ruby gems",
      "gem update --system 2>/dev/null; gem update 2>/dev/null; echo done",
      "which gem" },
    { "Updating llama.cpp",
      "cd ~/llama.cpp && git pull && "
      "cmake --build build --config Release -j$(nproc) 2>/dev/null; echo done",
      "test -d ~/llama.cpp/.git" },
    { "Pulling latest Docker images",
      "docker images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | "
      "grep -v '<none>' | xargs -r -I{} docker pull {} 2>/dev/null; echo done",
      "which docker" },
    { "Cleaning up",
      "sudo apt-get autoremove -y -qq 2>/dev/null; sudo apt-get clean 2>/dev/null; "
      "docker system prune -f 2>/dev/null; echo done",
      NULL },
};

provision_recipe_t provision_recipe_update(void) {
    provision_recipe_t r = { "Update All", update_steps, 9 };
    return r;
}
