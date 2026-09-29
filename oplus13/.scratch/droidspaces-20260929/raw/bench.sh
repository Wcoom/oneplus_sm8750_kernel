#!/bin/sh
# Droidspaces Ubuntu 性能基准套件（只读式：仅创建/删除自己的临时文件）
# 用法: sh bench.sh <标签>
TAG="${1:-run}"
ms() { date +%s%3N; }
B=/mnt/data/.dsbench
mkdir -p $B 2>/dev/null
echo "===== BENCHMARK [$TAG] $(date -u +%FT%TZ) ====="
echo "kernel: $(uname -r)"
echo "loadavg(宿主全局): $(cat /proc/loadavg)"
echo "memfree_kb: $(awk '/MemFree/{print $2}' /proc/meminfo) memavail_kb: $(awk '/MemAvailable/{print $2}' /proc/meminfo)"
echo "zram_used_kb: $(awk '/^SwapTotal/{t=$2}/^SwapFree/{f=$2}END{print t-f}' /proc/meminfo)"
echo

echo "--- B1 单核 CPU 吞吐 (openssl sha256, 2s) ---"
openssl speed -seconds 2 sha256 2>/dev/null | awk '/^sha256/{printf "B1_sha256_16k_kps=%s\n", $6}'

echo "--- B2 8 核并行 CPU 吞吐 (8x openssl sha256, 2s) ---"
for i in 1 2 3 4 5 6 7 8; do
  openssl speed -seconds 2 sha256 2>/dev/null | awk '/^sha256/{print $6}' > $B/cpu.$i &
done
wait
awk '{s+=$1; n++} END{printf "B2_8core_sum_16k_kps=%.0f (n=%d)\n", s, n}' $B/cpu.*
rm -f $B/cpu.*

echo "--- B3 进程创建吞吐 (1000x /bin/true) ---"
S=$(ms); i=0; while [ $i -lt 1000 ]; do /bin/true; i=$((i+1)); done; E=$(ms)
echo "B3_fork_exec_1000_ms=$((E-S))"

echo "--- B4 顺序写+fsync 256MB ---"
for pair in "/tmp:tmpfs" "/:rootfs_ext4_loop" "/mnt/data:f2fs_direct"; do
  d=${pair%%:*}; lbl=${pair##*:}
  w="$d/.dsbench_w.tmp"
  S=$(ms); dd if=/dev/zero of=$w bs=1M count=256 conv=fsync 2>/dev/null; E=$(ms)
  echo "B4_write256M_fsync_$lbl""_ms=$((E-S))"
  rm -f $w
done

echo "--- B5 小文件创建/删除 (2000 x 空文件) ---"
for pair in "/mnt/data:f2fs_direct" "/tmp:tmpfs" "/:rootfs_ext4_loop"; do
  d=${pair%%:*}; lbl=${pair##*:}
  DD="$d/.dsbench_small"
  rm -rf $DD; mkdir -p $DD
  S=$(ms); i=0; while [ $i -lt 2000 ]; do : > $DD/f$i; i=$((i+1)); done; E=$(ms)
  C=$((E-S))
  S=$(ms); rm -rf $DD; E=$(ms)
  echo "B5_create2000_$lbl""_ms=$C  B5_delete2000_$lbl""_ms=$((E-S))"
done

echo "--- B6 gcc -O2 编译吞吐（同一 TU 编译 5 次）---"
python3 - <<'PYEOF' > $B/big.c
import math
print('#include <stdio.h>')
print('#include <math.h>')
for i in range(400):
    print(f'static double f{i}(double x){{ double r=0; for(int j=0;j<40;j++) r+=sin(x*{i+1}+j)*cos(x*j+{i}); return r; }}')
print('int main(void){ double s=0;')
for i in range(400):
    print(f'  s+=f{i}((double){i});')
print('  printf("%f\\n", s); return 0; }')
PYEOF
wc -l < $B/big.c | awk '{printf "B6_source_lines=%s\n", $1}'
S=$(ms); i=0; while [ $i -lt 5 ]; do gcc -O2 -c -o $B/big.o $B/big.c; i=$((i+1)); done; E=$(ms)
echo "B6_gcc5_ms=$((E-S))"
rm -f $B/big.c $B/big.o

echo "--- B7 Python 解释器启动 (100x) ---"
S=$(ms); i=0; while [ $i -lt 100 ]; do python3 -c pass; i=$((i+1)); done; E=$(ms)
echo "B7_python_start_100_ms=$((E-S))"

echo "--- B8 内存分配+触碰吞吐 (分配并写 512MB) ---"
S=$(ms)
python3 -c "
b=bytearray(512*1024*1024)
for i in range(0,len(b),4096): b[i]=1
" 2>/dev/null
E=$(ms)
echo "B8_alloc_touch_512M_ms=$((E-S))"

echo "--- B9 内存压力指标增量（对比基线）---"
grep -E "^(allocstall_normal|allocstall_movable|pgscan_direct|pgsteal_direct|pgmajfault|pswpin|pswpout|compact_stall|compact_fail)" /proc/vmstat
echo "PSI_cpu: $(cat /proc/pressure/cpu | tr '\n' ' ')"
echo "PSI_mem: $(cat /proc/pressure/memory | tr '\n' ' ')"
echo "PSI_io : $(cat /proc/pressure/io | tr '\n' ' ')"
echo
echo "===== END [$TAG] ====="
rm -rf $B /tmp/.dsbench_small
