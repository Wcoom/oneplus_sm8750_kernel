echo "===== Z1. /proc/zoneinfo 原始（逐 zone 水位） ====="
awk '
/^Node/ {node=$2; zone=$4; print "--- "node" "zone" ---"; next}
/^  pages free/ {printf "   pages free = %s\n", $3}
/^        min/ {printf "   min        = %s\n", $2}
/^        low/ {printf "   low        = %s\n", $2}
/^        high/ {printf "   high       = %s\n", $2}
/^        protection/ {printf "   protection = %s %s %s %s\n", $2,$3,$4,$5}
/^        spanned/ {printf "   spanned    = %s\n", $2}
/^        present/ {printf "   present    = %s\n", $2}
/^        managed/ {printf "   managed    = %s\n", $2}
' /proc/zoneinfo
echo
echo "===== Z2. 各 zone 的 free 与水位比较（判定是否低于 high） ====="
awk '
/^Node/ {node=$2; zone=$4; free=-1; min=-1; low=-1; high=-1; managed=-1; next}
/^  pages free/ {free=$3}
$1=="min" && free>=0 {min=$2}
$1=="low" && free>=0 {low=$2}
$1=="high" && free>=0 {high=$2}
$1=="managed" && free>=0 {
  managed=$2;
  verdict = (free < high) ? "★ free < high  ← 低于高水位" : ((free < low) ? "☆ free < low" : "  free >= high（正常）");
  printf "  %-8s %-8s free=%-8s low=%-8s high=%-8s managed=%-9s %s\n", node, zone, free, low, high, managed, verdict;
  printf "           free占比=%.2f%%  距high差=%s页(%.1fMB)\n", free*100/managed, high-free, (high-free)*4/1024;
  free=-1
}
' /proc/zoneinfo
echo
echo "===== Z3. 直接回收是否仍在发生（两次采样 20s 间隔） ====="
snap() { grep -E "^(pgscan_direct|pgsteal_direct|allocstall_movable|allocstall_normal|pgscan_kswapd|pgsteal_kswapd|workingset_refault_file|workingset_refault_anon|pswpin|pswpout|pgmajfault|nr_free_pages|compact_stall)" /proc/vmstat; }
snap > /tmp/z_a.txt
cat /proc/pressure/memory > /tmp/z_pa.txt
sleep 20
snap > /tmp/z_b.txt
cat /proc/pressure/memory > /tmp/z_pb.txt
echo "  --- 20 秒内的增量 ---"
join /tmp/z_a.txt /tmp/z_b.txt | awk '{d=$3-$2; if(d!=0) printf "    %-26s +%s\n", $1, d; else printf "    %-26s +0\n", $1}'
echo "  --- PSI memory before ---"; cat /tmp/z_pa.txt | sed 's/^/    /'
echo "  --- PSI memory after  ---"; cat /tmp/z_pb.txt | sed 's/^/    /'
echo
echo "===== Z4. MemAvailable 与 MemFree 的差（可直接回收量） ====="
awk '/^MemTotal/{t=$2}/^MemFree/{f=$2}/^MemAvailable/{a=$2}/^Cached/{c=$2}/^SwapCached/{sc=$2}
END{printf "  MemTotal=%.0fMB MemFree=%.0fMB MemAvailable=%.0fMB Cached=%.0fMB SwapCached=%.0fMB\n  可回收差=%.0fMB\n",t/1024,f/1024,a/1024,c/1024,sc/1024,(a-f)/1024}' /proc/meminfo
echo
echo "===== Z5. 容器进程落在哪个 cgroup（宿主视角） ====="
echo "  --- 容器内 PID1 的宿主 PID 与 cgroup ---"
echo "  --- 找 droidspaces 进程 ---"
ps -A -o pid,ppid,rss,comm 2>/dev/null | grep -iE "droidsp|systemd|ubuntu" | head -10 | sed 's/^/    /'
echo "  --- 所有 memory cgroup 及其用量 ---"
for d in /sys/fs/cgroup/memory* /dev/memcg/*; do
  [ -d "$d" ] || continue
  [ -f "$d/memory.current" ] && echo "    [v2] $d current=$(cat $d/memory.current 2>/dev/null)"
done 2>/dev/null | head -20
ls /dev/memcg/ 2>/dev/null | head -20 | sed 's/^/    /'
ls /sys/fs/cgroup/ 2>/dev/null | head -5 | sed 's/^/    /'
echo
echo "===== Z6. 内存消耗 TOP10（宿主全进程） ====="
ps -A -o pid,rss,comm --sort=-rss 2>/dev/null | head -12 | awk '{printf "    %-8s %8.1f MB  %s\n", $1, $2/1024, $3}'
