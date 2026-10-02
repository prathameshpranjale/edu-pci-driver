#!/usr/bin/env bash
# Reproduce CI locally: test a FRESH CLONE of the committed state (not your working tree),
# which is what a CI runner sees. Catches things like missing executable bits or files
# that are not committed. Reuses the kernel already built under $WORK.
set -euo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-/home/$(id -un)/work}
CLONE=$WORK/ci-clone

rm -rf "$CLONE"
git clone -q "$PROJ" "$CLONE"
cd "$CLONE"

echo "committed script modes:"
git ls-files -s scripts | awk '{print $1, $4}'

# CI invokes scripts directly, so each must be executable in the clone
for f in scripts/*.sh; do
  [ -x "$f" ] || { echo "NOT EXECUTABLE in clone: $f"; exit 126; }
done

WORK="$WORK" scripts/test.sh
