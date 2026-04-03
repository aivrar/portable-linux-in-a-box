# Linux Image Build System
#
# Builds minimal Linux kernel + rootfs images for the linux-template backends.
# Run from WSL, Linux, or MSYS2 with standard tools installed.
#
# Targets:
#   make kernel-x86     - Download/build x86_64 kernel for WHPX/QEMU
#   make rootfs-x86     - Build x86_64 Alpine initramfs for WHPX/QEMU
#   make kernel-riscv   - Download RISC-V kernel for TinyEMU
#   make rootfs-riscv   - Download RISC-V rootfs for TinyEMU
#   make all-x86        - Build both x86_64 kernel + rootfs
#   make all-riscv      - Build both RISC-V kernel + rootfs
#   make all             - Build everything
#   make clean          - Remove all built images
#
# Output directory: ./out/

OUTDIR     := out
ALPINE_VER := 3.20
ALPINE_ARCH := x86_64
ALPINE_URL := https://dl-cdn.alpinelinux.org/alpine/v$(ALPINE_VER)/releases/$(ALPINE_ARCH)
ALPINE_MINI := alpine-minirootfs-$(ALPINE_VER).0-$(ALPINE_ARCH).tar.gz

# Kernel version for direct download (pre-built from Alpine)
KERNEL_VER := 6.6.32-0-virt
KERNEL_PKG := linux-virt-$(KERNEL_VER).apk

# TinyEMU pre-built images from Bellard's site
TINYEMU_URL := https://bellard.org/tinyemu

.PHONY: all all-x86 all-riscv kernel-x86 rootfs-x86 kernel-riscv rootfs-riscv clean

all: all-x86 all-riscv

all-x86: kernel-x86 rootfs-x86

all-riscv: kernel-riscv rootfs-riscv

$(OUTDIR):
	mkdir -p $(OUTDIR)

# ============================================================
# x86_64 kernel (for WHPX and QEMU backends)
# ============================================================

kernel-x86: $(OUTDIR)/bzImage
$(OUTDIR)/bzImage: | $(OUTDIR)
	@echo "=== Downloading Alpine virt kernel ==="
	./build_kernel.sh "$(OUTDIR)"

# ============================================================
# x86_64 initramfs (for WHPX backend — no disk needed)
# ============================================================

rootfs-x86: $(OUTDIR)/initramfs.cpio.gz $(OUTDIR)/rootfs.qcow2
$(OUTDIR)/initramfs.cpio.gz: | $(OUTDIR)
	@echo "=== Building x86_64 initramfs ==="
	./build_rootfs.sh "$(OUTDIR)" x86_64

# Also create a QEMU-compatible disk image
$(OUTDIR)/rootfs.qcow2: $(OUTDIR)/initramfs.cpio.gz
	@echo "=== Creating QEMU disk image ==="
	@if command -v qemu-img >/dev/null 2>&1; then \
		qemu-img create -f qcow2 "$(OUTDIR)/rootfs.qcow2" 2G; \
		echo "Created empty qcow2 (install OS via QEMU for full disk)"; \
	else \
		echo "qemu-img not found, skipping qcow2 creation"; \
		touch "$(OUTDIR)/rootfs.qcow2"; \
	fi

# ============================================================
# RISC-V kernel + rootfs (for TinyEMU backend)
# ============================================================

kernel-riscv: $(OUTDIR)/bbl64.bin
$(OUTDIR)/bbl64.bin: | $(OUTDIR)
	@echo "=== Downloading RISC-V kernel for TinyEMU ==="
	./build_kernel.sh "$(OUTDIR)" riscv

rootfs-riscv: $(OUTDIR)/rootfs-riscv64.ext2
$(OUTDIR)/rootfs-riscv64.ext2: | $(OUTDIR)
	@echo "=== Building RISC-V rootfs ==="
	./build_rootfs.sh "$(OUTDIR)" riscv64

# ============================================================
# Install: copy built images to the template linux/ directory
# ============================================================

.PHONY: install
install: all-x86
	@echo "=== Installing images ==="
	@test -d "../linux" || mkdir -p "../linux"
	cp -v $(OUTDIR)/bzImage ../linux/ 2>/dev/null || true
	cp -v $(OUTDIR)/initramfs.cpio.gz ../linux/ 2>/dev/null || true
	cp -v $(OUTDIR)/rootfs.qcow2 ../linux/ 2>/dev/null || true
	@echo "Images installed to ../linux/"

.PHONY: install-riscv
install-riscv: all-riscv
	@test -d "../linux" || mkdir -p "../linux"
	cp -v $(OUTDIR)/bbl64.bin ../linux/ 2>/dev/null || true
	cp -v $(OUTDIR)/rootfs-riscv64.ext2 ../linux/ 2>/dev/null || true

clean:
	rm -rf $(OUTDIR) _rootfs_staging
	@echo "Cleaned."
