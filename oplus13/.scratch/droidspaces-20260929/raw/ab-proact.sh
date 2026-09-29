KPID=""
for p in /proc/[0-9]*; do
  [ "$(cat $p/comm 2>/dev/null)" = "kcompactd0" ] && { KPID=$(basename $p); break; }
done
echo "kcompactd0 PID = ${KPID:-未找到}"
echo

snap() {
  grep -E "^compact_stall|^compact_fail|^compact_success|^compact_daemon_wake|^compact_daemon_migrate_scanned|^compact_daemon_free_scanned|^pgscan_direct |^pgsteal_direct |^thp_fault_alloc |^thp_fault_fallback " /proc/vmstat
  [ -n "$KPID" ] && awk '{print "kcompactd_cpu_jiffies " $14+$15}' /proc/$KPID/stat
  awk '/^some/{t=$4; gsub("total=","",t); print "psi_some_total_us " t}' /proc/pressure/memory
  awk '/^MemFree/{print "MemFree_kB " $2}' /proc/meminfo
}

run_phase() {
  p=$1; label=$2
  echo "$p" > /proc/sys/vm/compaction_proactiveness
  echo "=========== 阶段 $label：proactiveness=$p（实读 $(cat /proc/sys/vm/compaction_proactiveness)）==========="
  snap > /data/local/tmp/s_t0.txt
  sleep 60
  snap > /data/local/tmp/s_t1.txt
  awk 'NR==FNR{a[$1]=$2; next} {d=$2-a[$1]; printf "  %-32s %+d\n", $1, d}' /data/local/tmp/s_t0.txt /data/local/tmp/s_t1.txt
  echo
}

echo "原始值备份: $(cat /proc/sys/vm/compaction_proactiveness) -> /data/local/tmp/proact.orig"
cat /proc/sys/vm/compaction_proactiveness > /data/local/tmp/proact.orig
echo

run_phase 0   "A 基线"
run_phase 20  "B 内核默认"
run_phase 100 "C 最激进"

echo "=========== 恢复原值 ==========="
cat /data/local/tmp/proact.orig > /proc/sys/vm/compaction_proactiveness
echo "  已恢复为: $(cat /proc/sys/vm/compaction_proactiveness)"
echo
echo "=========== 期间是否有 kcompactd0 被唤醒过的旁证 ==========="
grep -E "^compact_daemon" /proc/vmstat | sed 's/^/  /'
