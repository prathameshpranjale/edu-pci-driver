#!/usr/bin/env bash
# Build everything and run the test in QEMU. Exit status is the test result.
set -euo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)

TESTS="test_edu test_dma test_stress"

for t in $TESTS; do
  gcc -static -O2 -Wall -o "$PROJ/tests/$t" "$PROJ/tests/$t.c"
done
make -C "$PROJ/driver" >/dev/null
"$PROJ/scripts/build_initramfs.sh" >/dev/null

out=$(timeout 300 "$PROJ/scripts/run_qemu.sh" 2>&1 || true)
echo "$out" | grep -aE 'edu_driver 0000|test_[a-z]+:|KERNEL-'
echo "$out" | grep -aq 'KERNEL-CLEAN' || {
  echo "kernel reported problems (KASAN / lockdep / BUG / WARNING):"
  echo "$out" | sed -n '/=== kernel health ===/,$p' | head -60
  exit 1
}
for t in $TESTS; do
  echo "$out" | grep -aq "$t: PASS" || { echo "missing PASS for $t"; exit 1; }
done
