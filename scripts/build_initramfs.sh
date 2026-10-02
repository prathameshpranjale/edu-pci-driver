#!/usr/bin/env bash
# Pack busybox + the driver (+ test program if built) into a tiny initramfs.
set -euo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/home/$(id -un)/work}
ROOT=$WORK/rootfs
OUT=$WORK/initramfs.cpio.gz

rm -rf "$ROOT"
mkdir -p "$ROOT"/{bin,proc,sys,dev,tmp}

cp "$(command -v busybox)" "$ROOT/bin/busybox"
for app in sh mount insmod rmmod dmesg poweroff cat ls sleep grep; do
  ln -s busybox "$ROOT/bin/$app"
done

cp "$PROJ/driver/edu_driver.ko" "$ROOT/"
for t in test_edu test_dma test_stress; do
  [ -f "$PROJ/tests/$t" ] && cp "$PROJ/tests/$t" "$ROOT/bin/"
done

cat > "$ROOT/init" <<'EOF'
#!/bin/sh
mount -t proc none /proc
mount -t sysfs none /sys
mount -t devtmpfs none /dev
echo "=== insmod edu_driver.ko ==="
insmod /edu_driver.ko
for t in /bin/test_*; do [ -x "$t" ] && "$t"; done
echo "=== rmmod ==="
rmmod edu_driver
echo "=== kernel health ==="
if dmesg | grep -aE 'BUG:|WARNING:|KASAN|possible (circular|recursive) locking|inconsistent lock|Call Trace'; then
  echo "KERNEL-ISSUES"
else
  echo "KERNEL-CLEAN"
fi
poweroff -f
EOF
chmod +x "$ROOT/init"

( cd "$ROOT" && find . | cpio -o -H newc --quiet | gzip ) > "$OUT"
echo "built: $OUT"
