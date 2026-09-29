KPID=""
for p in /proc/[0-9]*; do
  [ "$(cat $p/comm 2>/dev/null)" = "kcompactd0" ] && { KPID=$(basename $p); break; }
done

snap() {
  grep -E "^compact_daemon_migrate_scanned|^compact_daemon_free_scanned|^compact_stall|^compact_fail|^compact_success|^pgscan_direct " /proc/vmstat
  [ -n "$KPID" ] && awk '{print "kcompactd_cpu_jiffies", $14+$15}' /proc/$KPID/stat
  awk '/^MemFree/{print "MemFree_kB", $2}' /proc/meminfo
  awk '{for(i=5;i<=NF;i++) t[i-4]+=$i} END{for(o=7;o<=10;o++) printf "hi_order%d %d\n", o, t[o+1]}' /proc/buddyinfo
}

phase() {
  p=$1; lbl=$2
  echo "$p" > /proc/sys/vm/compaction_proactiveness
  snap > /data/local/tmp/p_t0.txt
  sleep 60
  snap > /data/local/tmp/p_t1.txt
  echo "--- $lbl (p=$p) ---"
  awk 'NR==FNR{a[$1]=$2; next} {printf "  %-32s %+d\n", $1, $2-a[$1]}' /data/local/tmp/p_t0.txt /data/local/tmp/p_t1.txt
  echo
}

echo "配对对照：A(0) -> B(100) -> A(0)，各 60 秒，看高阶空闲块的变化速率"
echo "判读：若仅 B 窗口出现明显高阶净增、两个 A 窗口都没有，则聚并可归因于主动压缩"
echo
phase 0   "对照 A1"
phase 100 "处理 B"
phase 0   "对照 A2"

echo "--- 恢复 p=0 并确认 ---"
cat /data/local/tmp/proact.orig > /proc/sys/vm/compaction_proactiveness
echo "  compaction_proactiveness = $(cat /proc/sys/vm/compaction_proactiveness)"
echo "  （原始值来自 /data/local/tmp/proact.orig = $(cat /data/local/tmp/proact.orig)）"
