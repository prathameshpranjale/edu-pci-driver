#!/usr/bin/env bash
# Boot the guest kernel with the emulated edu device. Extra args are passed to QEMU (e.g. -s -S for GDB).
set -euo pipefail

WORK=${WORK:-/home/$(id -un)/work}
KVER=${KVER:-6.6.87}
KERNEL=$WORK/linux-$KVER/arch/x86/boot/bzImage

ACCEL=()
[ -w /dev/kvm ] && [ -z "${NOKVM:-}" ] && ACCEL=(-enable-kvm -cpu host)

exec qemu-system-x86_64 "${ACCEL[@]}" \
  -kernel "$KERNEL" \
  -initrd "$WORK/initramfs.cpio.gz" \
  -append "console=ttyS0 panic=-1 quiet loglevel=4" \
  -device edu \
  -smp 4 -m 1G -nographic -no-reboot "$@"
