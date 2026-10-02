#!/usr/bin/env bash
# Sanity check for the kernel-health gate: plant a 1-byte heap overflow in a COPY of the
# driver and confirm KASAN reports it and test.sh fails. Does not touch the real sources.
set -uo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)
COPY=/home/$(id -un)/work/mut

rm -rf "$COPY"; mkdir -p "$COPY"
cp -r "$PROJ/driver" "$PROJ/tests" "$PROJ/scripts" "$COPY/"
rm -f "$COPY"/driver/*.ko "$COPY"/driver/*.o
sed -i 's|^\tswitch (cmd) {|\t{ volatile char *k = kmalloc(8, GFP_KERNEL); k[8] = 1; kfree((void *)k); }\n\tswitch (cmd) {|' \
  "$COPY/driver/edu_driver.c"
echo "planted overflow lines in copy: $(grep -c 'k\[8\] = 1' "$COPY/driver/edu_driver.c")"

if bash "$COPY/scripts/test.sh"; then
  echo "MUTATION SURVIVED: KASAN did not flag the overflow"; exit 1
else
  echo "MUTATION KILLED: the health gate caught the overflow"
fi
