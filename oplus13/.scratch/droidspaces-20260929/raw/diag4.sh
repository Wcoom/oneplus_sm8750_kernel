sec() { echo; echo "##### $* #####"; }
sec "PATH 来源"
echo "[/root/.profile]"; cat /root/.profile 2>/dev/null
echo "[/root/.bashrc 中 PATH 相关]"; grep -nE "PATH|export" /root/.bashrc 2>/dev/null | head -20
echo "[/etc/environment]"; cat /etc/environment 2>/dev/null
echo "[/etc/profile.d 列表]"; ls /etc/profile.d/ 2>/dev/null
echo "[/etc/profile.d/*.sh 里的 PATH]"; grep -rnE "PATH" /etc/profile.d/ 2>/dev/null | head -10
sec "/root/.codex 明细"
du -sh /root/.codex/* 2>/dev/null | sort -rh | head -15
echo "[文件数]"; find /root/.codex -type f 2>/dev/null | wc -l
sec "基准测试工具可用性"
for t in openssl fio dd bc time hyperfine sysbench stress-ng hdparm; do
  p=$(command -v $t 2>/dev/null); printf '%-12s %s\n' "$t" "${p:-（无）}"
done
echo "[openssl 版本]"; openssl version 2>/dev/null
sec "THP / KSM 可读写性"
echo "THP enabled = $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>&1)"
ls -la /sys/kernel/mm/ 2>&1 | head -15
echo "[ksm 目录]"; ls -la /sys/kernel/mm/ksm/ 2>&1 | head -20
sec "镜像稀疏实况"
echo "[df /]"; df -h / | tail -1
echo "[df -i /]"; df -i / | tail -1
echo "[stat rootfs.img 实际占用]"; stat -c '%s bytes size, %b blocks x %B' /mnt/data/local/Droidspaces/Containers/ubuntu/rootfs.img 2>/dev/null
sec "各挂载点写性能快速探测（一次性 64MB 顺序写，随后删除）"
for d in /tmp / /mnt/data /dev/shm; do
  [ -d "$d" ] || continue
  t=$( { /usr/bin/time -f "%e" dd if=/dev/zero of=$d/.bench_tmp bs=1M count=64 conv=fsync 2>&1 1>/dev/null; } 2>&1 | tail -1 )
  echo "  $d : ${t}s (64MB 顺序写+fsync)"
  rm -f $d/.bench_tmp
done
sec "CPU 快速探测"
echo "[单核 openssl sha256 1s]"; openssl speed -seconds 1 sha256 2>/dev/null | tail -2
echo "[8 并行 sha256]"; for i in 1 2 3 4 5 6 7 8; do openssl speed -seconds 1 sha256 >/dev/null 2>&1 & done; wait; echo "done"
echo "[gcc 编译吞吐：编译一个中等 C 文件 10 次]"
cat > /tmp/bench.c <<'CEOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define N 200000
static double acc[N];
static void work(void){ for(int i=0;i<N;i++){ acc[i]=sqrt((double)i)+sin((double)i)*cos((double)i); } }
int main(void){ for(int r=0;r<20;r++) work(); double s=0; for(int i=0;i<N;i++) s+=acc[i]; printf("%f\n",s); return 0; }
CEOF
S=$(date +%s.%N); for i in $(seq 1 10); do gcc -O2 -o /tmp/bench.out /tmp/bench.c -lm; done; E=$(date +%s.%N)
echo "  10 次 gcc -O2 编译耗时: $(echo "$E - $S" | bc)s"
echo "[运行 5 次]"; S=$(date +%s.%N); for i in 1 2 3 4 5; do /tmp/bench.out >/dev/null; done; E=$(date +%s.%N)
echo "  5 次运行耗时: $(echo "$E - $S" | bc)s"
rm -f /tmp/bench.c /tmp/bench.out
echo
echo "##### 结束 #####"
