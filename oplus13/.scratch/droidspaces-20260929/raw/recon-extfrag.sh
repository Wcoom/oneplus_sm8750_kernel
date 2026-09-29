echo "=== uptime ==="; cat /proc/uptime
echo
echo "=== THP 当前状态 ==="
echo "  enabled       = $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "  defrag        = $(cat /sys/kernel/mm/transparent_hugepage/defrag)"
echo "  shmem_enabled = $(cat /sys/kernel/mm/transparent_hugepage/shmem_enabled 2>/dev/null)"
echo
echo "=== debugfs 是否可用 ==="
if [ -d /sys/kernel/debug/extfrag ]; then
  echo "  已挂载，extfrag 目录存在"
else
  echo "  未挂载，尝试挂载…"
  mount -t debugfs none /sys/kernel/debug 2>&1 && echo "  挂载成功" || echo "  挂载失败"
fi
echo
if [ -d /sys/kernel/debug/extfrag ]; then
  echo "=== 碎片指数 extfrag_index（每阶一行：-1000=完全碎片, 1000=无碎片）==="
  echo "--- DMA32 ---"; sed 's/^/  /' /sys/kernel/debug/extfrag/extfrag_index 2>/dev/null | head -12
  echo "--- 完整文件行数: $(wc -l < /sys/kernel/debug/extfrag/extfrag_index 2>/dev/null) ---"
  echo
  echo "=== 不可移动页占比 unreserve_highatomic / unusable_index ==="
  head -12 /sys/kernel/debug/extfrag/unusable_index 2>/dev/null | sed 's/^/  /'
fi
echo
echo "=== kcompactd CPU 时间（/proc/83/stat 的 utime/stime，单位 jiffies）==="
awk '{print "  pid="$1" comm="$2" utime="$14" stime="$15" 合计="$14+$15}' /proc/83/stat 2>/dev/null
echo "  CLK_TCK=$(getconf CLK_TCK 2>/dev/null || echo 100)"
echo
echo "=== 容器内看到的同一 sysctl（验证是否全局共享）==="
echo "  （下一步在容器内单独读）"
echo
echo "=== 直接压缩能力基线：compact_memory 一次性全量压缩 ==="
grep -E "compact_stall|compact_fail|compact_success|compact_daemon" /proc/vmstat | sed 's/^/  前 /'
echo 1 > /proc/sys/vm/compact_memory
sleep 3
grep -E "compact_stall|compact_fail|compact_success|compact_daemon" /proc/vmstat | sed 's/^/  后 /'
echo
echo "=== 压缩后 extfrag（若可用）==="
head -6 /sys/kernel/debug/extfrag/extfrag_index 2>/dev/null | sed 's/^/  /'
