#!/bin/bash
# Build a minimal Linux root filesystem for the linux-template backends.
#
# Usage:
#   ./build_rootfs.sh <outdir> [arch]
#   arch: x86_64 (default) or riscv64
#
# For x86_64: builds an initramfs from Alpine minirootfs (~5MB)
#   - Includes: busybox, sh, basic utils
#   - Auto-configured: serial console, auto-login, /proc /sys /dev
#   - Output: initramfs.cpio.gz (for WHPX) + rootfs.ext2 (for QEMU)
#
# For riscv64: downloads pre-built TinyEMU rootfs
#
# Requires: tar, gzip, cpio (or runs inside WSL/Linux)

set -euo pipefail

OUTDIR="${1:-.}"
ARCH="${2:-x86_64}"
ALPINE_VER="3.20"
STAGING="_rootfs_staging"

mkdir -p "$OUTDIR"

case "$ARCH" in
    x86_64|x86)
        echo "=== Building x86_64 initramfs ==="

        for tool in tar gzip cpio find; do
            if ! command -v "$tool" >/dev/null 2>&1; then
                echo "Missing required build tool: $tool" >&2
                echo "Install it before rebuilding the x86 initramfs." >&2
                exit 1
            fi
        done

        ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VER}/releases/x86_64/alpine-minirootfs-${ALPINE_VER}.0-x86_64.tar.gz"
        TARBALL="$OUTDIR/_alpine-minirootfs.tar.gz"

        if command -v curl >/dev/null 2>&1; then
            FETCH="curl -fsSL -o"
        elif command -v wget >/dev/null 2>&1; then
            FETCH="wget -q -O"
        else
            echo "ERROR: curl or wget required"
            exit 1
        fi

        # Download Alpine minirootfs
        if [ ! -f "$TARBALL" ]; then
            echo "Downloading Alpine minirootfs..."
            $FETCH "$TARBALL" "$ALPINE_URL" || {
                echo "Download failed. URL: $ALPINE_URL"
                exit 1
            }
        fi

        # Extract into staging directory
        rm -rf "$STAGING"
        mkdir -p "$STAGING"
        echo "Extracting..."
        tar xzf "$TARBALL" -C "$STAGING"

        # Apply our overlay: init script, serial console, auto-login
        echo "Applying overlay..."

        # Create init script that sets up the environment
        cat > "$STAGING/init" << 'INITEOF'
#!/bin/sh
# linux-template init — minimal boot to shell on serial console

# Mount essential filesystems
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev 2>/dev/null || {
    mkdir -p /dev
    mknod -m 666 /dev/null c 1 3
    mknod -m 666 /dev/zero c 1 5
    mknod -m 666 /dev/tty c 5 0
    mknod -m 666 /dev/console c 5 1
    mknod -m 666 /dev/ttyS0 c 4 64
    mknod -m 666 /dev/random c 1 8
    mknod -m 666 /dev/urandom c 1 9
}
mkdir -p /dev/pts /dev/shm /tmp /run
mount -t devpts devpts /dev/pts 2>/dev/null
mount -t tmpfs tmpfs /tmp
mount -t tmpfs tmpfs /run

# Set up basic networking (loopback)
ip link set lo up 2>/dev/null

# Set hostname
hostname linux-template

# Set PATH
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root
export TERM=linux

# Create basic files
echo "root:x:0:0:root:/root:/bin/sh" > /etc/passwd
echo "root:x:0:" > /etc/group
mkdir -p /root

# Print banner
echo ""
echo "=== linux-template ==="
echo "Alpine Linux $(cat /etc/alpine-release 2>/dev/null || echo 'minimal')"
echo "Type commands to execute."
echo ""

# Launch shell on the console
exec /bin/sh -l
INITEOF
        chmod +x "$STAGING/init"

        # Create a .profile for the shell
        mkdir -p "$STAGING/root"
        cat > "$STAGING/root/.profile" << 'PROFEOF'
export PS1='# '
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOME=/root
PROFEOF

        # Build cpio initramfs
        echo "Creating initramfs.cpio.gz..."
        (cd "$STAGING" && find . | cpio -o -H newc 2>/dev/null | gzip -9) \
            > "$OUTDIR/initramfs.cpio.gz"

        # Clean up
        rm -rf "$STAGING"
        rm -f "$TARBALL"

        echo "Output: $OUTDIR/initramfs.cpio.gz"
        ls -lh "$OUTDIR/initramfs.cpio.gz"
        echo ""
        echo "Use with WHPX backend:"
        echo "  linux_template --kernel linux/bzImage --rootfs linux/initramfs.cpio.gz"
        ;;

    riscv64|riscv)
        echo "=== Downloading RISC-V rootfs for TinyEMU ==="

        if command -v curl >/dev/null 2>&1; then
            FETCH="curl -fsSL -o"
        elif command -v wget >/dev/null 2>&1; then
            FETCH="wget -q -O"
        else
            echo "ERROR: curl or wget required"
            exit 1
        fi

        # TinyEMU's pre-built images include a rootfs
        RISCV_URL="https://bellard.org/tinyemu/diskimage-linux-riscv-2018-09-23.tar.gz"
        TMPFILE="$OUTDIR/_riscv_images.tar.gz"

        if [ ! -f "$TMPFILE" ]; then
            echo "Downloading RISC-V images..."
            $FETCH "$TMPFILE" "$RISCV_URL" || {
                echo "Download failed."
                exit 1
            }
        fi

        tar xzf "$TMPFILE" -C "$OUTDIR/" --strip-components=1 \
            "*/root-riscv64.bin" 2>/dev/null || \
        tar xzf "$TMPFILE" -C "$OUTDIR/" "root-riscv64.bin" 2>/dev/null || {
            echo "Extraction failed"
            rm -f "$TMPFILE"
            exit 1
        }

        # Rename to our convention
        if [ -f "$OUTDIR/root-riscv64.bin" ]; then
            mv "$OUTDIR/root-riscv64.bin" "$OUTDIR/rootfs-riscv64.ext2"
        fi

        rm -f "$TMPFILE"
        echo "Output: $OUTDIR/rootfs-riscv64.ext2"
        ls -lh "$OUTDIR/rootfs-riscv64.ext2" 2>/dev/null
        ;;

    *)
        echo "Unknown architecture: $ARCH"
        echo "Supported: x86_64, riscv64"
        exit 1
        ;;
esac

echo "Done."
