#!/usr/bin/env bash
# Sanity check for test_dma: in a COPY of the project, skip the device->RAM transfer and
# confirm the DMA test now fails. Does not touch the real sources.
set -uo pipefail

PROJ=$(cd "$(dirname "$0")/.." && pwd)
COPY=/home/$(id -un)/work/mut

rm -rf "$COPY"; mkdir -p "$COPY"
cp -r "$PROJ/driver" "$PROJ/tests" "$PROJ/scripts" "$COPY/"
rm -f "$COPY"/driver/*.ko "$COPY"/driver/*.o
sed -i 's/ret = edu_dma_run(edu, EDU_DEV_BUF, edu->rx_bus, req->len, EDU_DMA_TO_RAM);/ret = 0;/' "$COPY/driver/edu_driver.c"
echo "return-DMA calls left in copy: $(grep -c 'EDU_DEV_BUF, edu->rx_bus' "$COPY/driver/edu_driver.c")"

if bash "$COPY/scripts/test.sh"; then
  echo "MUTATION SURVIVED: DMA test did not notice the missing transfer"; exit 1
else
  echo "MUTATION KILLED: DMA test detected the missing transfer"
fi
