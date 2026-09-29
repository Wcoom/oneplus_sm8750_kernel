echo "=== 内核 ==="
uname -r
echo
echo "=== 相关 sysctl 当前值 ==="
for k in compaction_proactiveness extfrag_threshold compact_unevictable_allowed min_free_kbytes watermark_scale_factor; do
  v=$(cat /proc/sys/vm/$k 2>/dev/null)
  printf '  vm.%-28s = %s\n' "$k" "${v:-<无此节点>}"
done
echo "  可写? $([ -w /proc/sys/vm/compaction_proactiveness ] && echo 是 || echo 否)"
echo
echo "=== 内存 ==="
grep -E "MemTotal|MemFree|MemAvailable|AnonHugePages|Committed_AS|SwapTotal|SwapFree" /proc/meminfo | sed 's/^/  /'
echo
echo "=== zone 水位 ==="
awk '/Node/{n=$0} /protection|watermark/{print "  "n" "$0}' /proc/zoneinfo 2>/dev/null | head -8
awk '/^  low|^  high|^  min/{print "  "$0}' /proc/zoneinfo 2>/dev/null | head -12
echo
echo "=== kcompactd 线程 ==="
ps -A 2>/dev/null | grep -i compact | sed 's/^/  /'
echo
echo "=== 压缩/回收计数器 T0 ==="
grep -E "compact_stall|compact_fail|compact_success|compact_daemon|pgscan_direct|pgsteal_direct|thp_" /proc/vmstat | sed 's/^/  /'
echo
echo "=== PSI T0 ==="
sed 's/^/  /' /proc/pressure/memory
echo
echo "===== 静置 60s，观察自然活动 ====="
sleep 60
echo "=== 压缩/回收计数器 T1（与 T0 相减即 60s 内的自然增量）==="
grep -E "compact_stall|compact_fail|compact_success|compact_daemon|pgscan_direct|pgsteal_direct|thp_" /proc/vmstat | sed 's/^/  /'
echo
echo "=== PSI T1 ==="
sed 's/^/  /' /proc/pressure/memory
echo
echo "=== 内存 T1 ==="
grep -E "MemFree|MemAvailable|AnonHugePages" /proc/meminfo | sed 's/^/  /'
