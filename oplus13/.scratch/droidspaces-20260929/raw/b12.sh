#!/bin/sh
echo "loadavg: $(cat /proc/loadavg)"
echo "--- B1 单核 ---"
for r in 1 2 3; do
  /usr/bin/openssl speed -seconds 3 sha256 2>/dev/null | awk '/^sha256/{printf "  run%s: 8192B列=%s  16384B列=%s\n", '"$r"', $6, $7}'
done
echo "--- B2 8 核并行（8 进程同时跑，各列求和）---"
for r in 1 2; do
  for i in 1 2 3 4 5 6 7 8; do
    /usr/bin/openssl speed -seconds 3 sha256 2>/dev/null | awk '/^sha256/{print $6, $7}' > /tmp/.c$i &
  done
  wait
  cat /tmp/.c1 /tmp/.c2 /tmp/.c3 /tmp/.c4 /tmp/.c5 /tmp/.c6 /tmp/.c7 /tmp/.c8 | \
    awk '{a+=$1; b+=$2} END{printf "  run%s: 8核总和 8192B列=%.0f k/s (%.2f GB/s)  16384B列=%.0f k/s (%.2f GB/s)\n", '"$r"', a, a/1e6*8, b, b/1e6*16}'
  rm -f /tmp/.c*
done
