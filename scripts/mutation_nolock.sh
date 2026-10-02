#!/usr/bin/env bash
# Sanity check for the stress test: delete the driver's mutex in a COPY of the project
# and confirm the stress test now fails. Does not touch the real sources.
set -uo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)
COPY=/home/$(id -un)/work/mut

rm -rf "$COPY"; mkdir -p "$COPY"
cp -r "$PROJ/driver" "$PROJ/tests" "$PROJ/scripts" "$COPY/"
rm -f "$COPY"/driver/*.ko "$COPY"/driver/*.o
sed -i '/mutex_lock(&edu->lock);/d;/mutex_unlock(&edu->lock);/d' "$COPY/driver/edu_driver.c"
echo "mutex calls left in copy: $(grep -c 'mutex_lock' "$COPY/driver/edu_driver.c")"

if bash "$COPY/scripts/test.sh"; then
  echo "MUTATION SURVIVED: stress test did not detect the missing lock"; exit 1
else
  echo "MUTATION KILLED: stress test detected the missing lock"
fi
