#!/usr/bin/env bash
# Build the guest kernel (with KASAN and lockdep). Run inside Linux/WSL.
# The source tree lives on the Linux filesystem (fast), not on /mnt/c.
#   KVER=6.6.87  WORK=/path  NO_DEBUG_INFO=1 (smaller build, used by CI)
set -euo pipefail

KVER=${KVER:-6.6.87}
WORK=${WORK:-/home/$(id -un)/work}   # not $HOME: it is mangled when WSL is launched from Windows
KSRC=${KSRC:-$WORK/linux-$KVER}

if [ ! -d "$KSRC" ]; then
  mkdir -p "$WORK"
  curl -fsSL "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz" | tar -xJ -C "$WORK"
fi

cd "$KSRC"
make defconfig kvm_guest.config

# What the driver needs: modules, initramfs, devtmpfs, serial console, debugfs.
./scripts/config \
  -e MODULES -e MODULE_UNLOAD \
  -e BLK_DEV_INITRD \
  -e DEVTMPFS -e DEVTMPFS_MOUNT \
  -e SERIAL_8250 -e SERIAL_8250_CONSOLE \
  -e PCI \
  -e DEBUG_FS -e FTRACE \
  -e DEBUG_KERNEL

# Bug finders: memory errors (KASAN) and locking mistakes (lockdep).
./scripts/config \
  -e KASAN -e KASAN_GENERIC -e KASAN_INLINE \
  -e PROVE_LOCKING \
  -e DEBUG_ATOMIC_SLEEP \
  -e DEBUG_MUTEXES -e DEBUG_SPINLOCK \
  -e DEBUG_LIST

if [ -n "${NO_DEBUG_INFO:-}" ]; then
  ./scripts/config -e DEBUG_INFO_NONE
else
  ./scripts/config -e DEBUG_INFO_DWARF5 -d DEBUG_INFO_NONE   # for GDB
fi
make olddefconfig

make -j"$(nproc)" bzImage modules
echo "built: $KSRC/arch/x86/boot/bzImage"
