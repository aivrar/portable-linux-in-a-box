#!/bin/bash
# Download a pre-built Linux kernel suitable for the linux-template backends.
#
# Usage:
#   ./build_kernel.sh <outdir> [arch]
#   arch: x86_64 (default) or riscv
#
# For x86_64: downloads Alpine Linux virt kernel (small, optimized for VMs)
# For riscv: downloads pre-built RISC-V kernel from known sources

set -e

OUTDIR="${1:-.}"
ARCH="${2:-x86_64}"

mkdir -p "$OUTDIR"

case "$ARCH" in
    x86_64|x86)
        echo "Downloading x86_64 kernel..."

        # Try Alpine virt kernel (from Alpine repos)
        ALPINE_VER="3.20"
        ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VER}/main/x86_64"
        KERNEL_PKG="linux-virt"

        # Get the latest virt kernel package name
        echo "Fetching package index from Alpine v${ALPINE_VER}..."
        APKINDEX_URL="${ALPINE_MIRROR}/APKINDEX.tar.gz"

        # Direct download approach: find and download the kernel package
        # Try a direct URL pattern for the kernel
        if command -v curl >/dev/null 2>&1; then
            FETCHER="curl -fsSL -o"
        elif command -v wget >/dev/null 2>&1; then
            FETCHER="wget -q -O"
        else
            echo "ERROR: curl or wget required"
            exit 1
        fi

        # Download a known minimal kernel — use Debian's cloud kernel
        # as an alternative if Alpine method fails
        KERNEL_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VER}/releases/x86_64/netboot/vmlinuz-virt"
        echo "Downloading: $KERNEL_URL"
        $FETCHER "$OUTDIR/bzImage" "$KERNEL_URL" || {
            echo "Alpine download failed. Trying alternative..."
            # Fallback: download from kernel.org (latest stable)
            echo "You can manually place a bzImage at: $OUTDIR/bzImage"
            echo ""
            echo "Quick options:"
            echo "  1. From Alpine: apk fetch linux-virt && extract"
            echo "  2. From Debian: apt download linux-image-cloud-amd64"
            echo "  3. Build from source: make defconfig && make bzImage"
            exit 1
        }

        echo "Kernel saved: $OUTDIR/bzImage"
        ls -lh "$OUTDIR/bzImage"
        ;;

    riscv|riscv64)
        echo "Downloading RISC-V kernel for TinyEMU..."

        if command -v curl >/dev/null 2>&1; then
            FETCHER="curl -fsSL -o"
        elif command -v wget >/dev/null 2>&1; then
            FETCHER="wget -q -O"
        else
            echo "ERROR: curl or wget required"
            exit 1
        fi

        # TinyEMU's pre-built RISC-V images
        RISCV_KERNEL_URL="https://bellard.org/tinyemu/diskimage-linux-riscv-2018-09-23.tar.gz"
        TMPFILE="$OUTDIR/_riscv_images.tar.gz"

        echo "Downloading: $RISCV_KERNEL_URL"
        $FETCHER "$TMPFILE" "$RISCV_KERNEL_URL" || {
            echo "Download failed. Place bbl64.bin manually in: $OUTDIR/"
            echo "Source: https://bellard.org/tinyemu/"
            exit 1
        }

        # Extract kernel (bbl64.bin)
        tar xzf "$TMPFILE" -C "$OUTDIR/" --strip-components=1 \
            "*/bbl64.bin" 2>/dev/null || \
        tar xzf "$TMPFILE" -C "$OUTDIR/" "bbl64.bin" 2>/dev/null || {
            echo "Extraction failed — check archive format"
            rm -f "$TMPFILE"
            exit 1
        }

        rm -f "$TMPFILE"
        echo "RISC-V kernel saved: $OUTDIR/bbl64.bin"
        ls -lh "$OUTDIR/bbl64.bin" 2>/dev/null || echo "(file not found)"
        ;;

    *)
        echo "Unknown architecture: $ARCH"
        echo "Supported: x86_64, riscv"
        exit 1
        ;;
esac

echo "Done."
