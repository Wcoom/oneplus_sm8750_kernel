echo "=== compact_memory 全量压缩：前后全量计数器对比 ==="
echo "--- 写之前 ---"
grep -E "^compact_" /proc/vmstat | sed 's/^/  /'
echo
echo "--- 写入 1 触发全量压缩 ---"
echo 1 > /proc/sys/vm/compact_memory && echo "  写入成功"
sleep 2
echo
echo "--- 写之后 ---"
grep -E "^compact_" /proc/vmstat | sed 's/^/  /'
