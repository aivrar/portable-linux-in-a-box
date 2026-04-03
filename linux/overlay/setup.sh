#!/bin/sh
# Auto-setup script for the linux-template Linux environment.
# Runs inside the Linux guest to install dependencies.
#
# Usage:
#   ./setup.sh              — install base dev tools
#   ./setup.sh python       — install Python 3 + pip
#   ./setup.sh node         — install Node.js + npm
#   ./setup.sh vllm         — install vLLM (requires GPU for inference)
#   ./setup.sh llamacpp     — build llama.cpp + download small model
#   ./setup.sh all          — install everything
#   ./setup.sh update       — update all installed packages + tools

set -e

install_base() {
    echo "=== Installing base development tools ==="
    if command -v apk >/dev/null 2>&1; then
        # Alpine
        apk update
        apk add build-base curl wget git bash openssh
    elif command -v apt-get >/dev/null 2>&1; then
        # Debian/Ubuntu
        apt-get update -y -qq
        apt-get install -y -qq build-essential curl wget git
    fi
    echo "Base tools installed."
}

install_python() {
    echo "=== Installing Python 3 + pip ==="
    if command -v apk >/dev/null 2>&1; then
        apk add python3 py3-pip python3-dev
    elif command -v apt-get >/dev/null 2>&1; then
        apt-get install -y -qq python3 python3-pip python3-venv
    fi
    python3 --version
    echo "Python installed."
}

install_node() {
    echo "=== Installing Node.js + npm ==="
    if command -v apk >/dev/null 2>&1; then
        apk add nodejs npm
    elif command -v apt-get >/dev/null 2>&1; then
        curl -fsSL https://deb.nodesource.com/setup_lts.x | bash -
        apt-get install -y -qq nodejs
    fi
    node --version
    npm --version
    echo "Node.js installed."
}

install_vllm() {
    echo "=== Installing vLLM ==="
    install_python
    python3 -m venv ~/vllm-env
    ~/vllm-env/bin/pip install vllm
    echo "vLLM installed in ~/vllm-env"
    echo "Start with: ~/vllm-env/bin/python -m vllm.entrypoints.openai.api_server --model <model>"
}

update_all() {
    echo "=== Updating system packages ==="
    if command -v apk >/dev/null 2>&1; then
        apk update && apk upgrade
    elif command -v apt-get >/dev/null 2>&1; then
        apt-get update -y -qq && apt-get upgrade -y -qq
        apt-get autoremove -y -qq && apt-get clean
    fi

    if command -v pip3 >/dev/null 2>&1; then
        echo "=== Updating Python packages (pip) ==="
        pip3 list --outdated --format=columns 2>/dev/null | tail -n +3 | awk '{print $1}' | xargs -r pip3 install --upgrade 2>/dev/null || true
        if [ -d ~/vllm-env ]; then
            echo "=== Updating vLLM ==="
            ~/vllm-env/bin/pip install --upgrade vllm 2>/dev/null || true
        fi
    fi

    if command -v npm >/dev/null 2>&1; then
        echo "=== Updating Node.js packages (npm) ==="
        npm update -g 2>/dev/null || true
    fi

    if command -v rustup >/dev/null 2>&1; then
        echo "=== Updating Rust toolchain ==="
        rustup update 2>/dev/null || true
    fi

    if command -v go >/dev/null 2>&1; then
        echo "=== Updating Go ==="
        go get -u all 2>/dev/null || true
    fi

    if command -v gem >/dev/null 2>&1; then
        echo "=== Updating Ruby gems ==="
        gem update 2>/dev/null || true
    fi

    if [ -d ~/llama.cpp/.git ]; then
        echo "=== Updating llama.cpp ==="
        cd ~/llama.cpp && git pull
        cmake --build build --config Release -j$(nproc) 2>/dev/null || true
    fi

    if command -v docker >/dev/null 2>&1; then
        echo "=== Pulling latest Docker images ==="
        docker images --format '{{.Repository}}:{{.Tag}}' 2>/dev/null | grep -v '<none>' | xargs -r -I{} docker pull {} 2>/dev/null || true
        docker system prune -f 2>/dev/null || true
    fi

    echo "All updates complete."
}

case "${1:-base}" in
    base)   install_base ;;
    python) install_python ;;
    node)   install_node ;;
    vllm)   install_vllm ;;
    llamacpp)
        install_base
        # Install CUDA if GPU is available
        if nvidia-smi >/dev/null 2>&1; then
            echo "=== GPU detected — installing CUDA toolkit ==="
            sudo apt-get install -y -qq nvidia-cuda-toolkit 2>/dev/null || {
                curl -fsSL https://developer.download.nvidia.com/compute/cuda/repos/wsl-ubuntu/x86_64/cuda-keyring_1.1-1_all.deb -o /tmp/cuda-keyring.deb
                sudo dpkg -i /tmp/cuda-keyring.deb
                sudo apt-get update -y -qq
                sudo apt-get install -y -qq cuda-toolkit 2>/dev/null || true
            }
        else
            echo "No GPU detected — will build CPU-only"
        fi
        echo "=== Cloning llama.cpp ==="
        if [ ! -d ~/llama.cpp ]; then
            git clone --depth 1 https://github.com/ggerganov/llama.cpp.git ~/llama.cpp
        fi
        cd ~/llama.cpp
        echo "=== Building llama.cpp ==="
        if which nvcc >/dev/null 2>&1; then
            echo "Building with CUDA (GPU acceleration)"
            cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=ON
        else
            echo "Building CPU-only"
            cmake -B build -DCMAKE_BUILD_TYPE=Release
        fi
        cmake --build build --config Release -j$(nproc)
        echo "=== Downloading TinyLlama 1.1B Q4 model ==="
        mkdir -p ~/models
        if [ ! -f ~/models/tinyllama-1.1b-q4_0.gguf ]; then
            curl -fsSL -o ~/models/tinyllama-1.1b-q4_0.gguf \
                'https://huggingface.co/TheBloke/TinyLlama-1.1B-Chat-v1.0-GGUF/resolve/main/tinyllama-1.1b-chat-v1.0.Q4_0.gguf'
        fi
        echo ""
        echo "llama.cpp installed."
        if which nvcc >/dev/null 2>&1; then
            echo "  Built with: CUDA (GPU acceleration enabled)"
        else
            echo "  Built with: CPU-only"
        fi
        echo "  Run: ~/llama.cpp/build/bin/llama-server -m ~/models/tinyllama-1.1b-q4_0.gguf --port 8080"
        ;;
    all)    install_base; install_python; install_node ;;
    update) update_all ;;
    *)      echo "Unknown target: $1"; exit 1 ;;
esac
