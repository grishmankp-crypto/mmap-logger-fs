#!/bin/bash
# Run this on YOUR machine (not in a sandbox) — mounts tsfs, hammers it with
# 8 concurrent writer processes, and checks the result for data loss.
set -e
cd "$(dirname "$0")"
rm -rf mnt && mkdir mnt
./tsfs -f mnt > /tmp/tsfs.log 2>&1 &
FUSE_PID=$!
sleep 1

echo "--- normal write/read ---"
echo "10.01,10.02,10.03" > mnt/sensor.log
cat mnt/sensor.log
ls -l mnt/sensor.log

echo "--- concurrent multi-writer test: 8 processes appending to same file ---"
: > mnt/multi.log
PIDS=()
for i in $(seq 1 8); do
  ( for j in $(seq 1 200); do printf "t%d-%d\n" "$i" "$j"; done >> mnt/multi.log ) &
  PIDS+=($!)
done
for p in "${PIDS[@]}"; do wait "$p"; done
sleep 0.3
echo "actual lines: $(wc -l < mnt/multi.log)  (expected: 1600)"

fusermount3 -u mnt
wait "$FUSE_PID" 2>/dev/null || true
echo "test complete"
