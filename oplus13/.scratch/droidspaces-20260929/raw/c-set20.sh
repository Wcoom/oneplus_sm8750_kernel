echo 20 > /proc/sys/vm/compaction_proactiveness
echo "  容器内写入 20，回读 = $(cat /proc/sys/vm/compaction_proactiveness)"
echo "  容器内 /proc/uptime = $(cut -d' ' -f1 /proc/uptime)"
