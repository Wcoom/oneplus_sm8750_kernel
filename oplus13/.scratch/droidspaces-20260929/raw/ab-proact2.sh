KPID=""
for p in /proc/[0-9]*; do
  [ "$(cat $p/comm 2>/dev/null)" = "kcompactd0" ] && { KPID=$(basename $p); break; }
done

snap() {
  grep -E "^compact_daemon_wake|^compact_daemon_migrate_scanned|^compact_daemon_free_scanned|^compact_stall|^compact_fail|^compact_success|^pgscan_direct |^thp_fault_alloc |^thp_fault_fallback " /proc/vmstat | awk '{print $1, $2}'
  [ -n "$KPID" ] && awk -v k="$KPID" '{print "kcompactd_cpu_jiffies", $14+$15}' /proc/$KPID/stat
  awk '{for(i=5;i<=NF;i++) t[i-4]+=$i} END{for(o=0;o<=10;o++) printf "freepages_order%d %d\n", o, t[o+1]}' /proc/buddyinfo
}

echo "===== 长窗口：p=100，连续两个 60 秒子窗口 ====="
echo "  观察点：第 2 个窗口的扫描量是否显著低于第 1 个（衰减=收敛，持平=持续收税）"
echo
echo 100 > /proc/sys/vm/compaction_proactiveness
echo "  实读: $(cat /proc/sys/vm/compaction_proactiveness)"
echo
snap > /data/local/tmp/l_t0.txt
echo "--- 子窗口 1（0-60s）---"
sleep 60
snap > /data/local/tmp/l_t1.txt
awk 'NR==FNR{a[$1]=$2; next} {printf "  %-32s %+d\n", $1, $2-a[$1]}' /data/local/tmp/l_t0.txt /data/local/tmp/l_t1.txt
echo
echo "--- 子窗口 2（60-120s）---"
sleep 60
snap > /data/local/tmp/l_t2.txt
awk 'NR==FNR{a[$1]=$2; next} {printf "  %-32s %+d\n", $1, $2-a[$1]}' /data/local/tmp/l_t1.txt /data/local/tmp/l_t2.txt
echo
echo "--- 全窗口小结（0-120s）---"
awk 'NR==FNR{a[$1]=$2; next} {printf "  %-32s %+d\n", $1, $2-a[$1]}' /data/local/tmp/l_t0.txt /data/local/tmp/l_t2.txt
echo
echo "===== 恢复 p=0 ====="
cat /data/local/tmp/proact.orig > /proc/sys/vm/compaction_proactiveness
echo "  已恢复为: $(cat /proc/sys/vm/compaction_proactiveness)"
echo
echo "===== 宿主侧最终确认 ====="
grep -E "^compact_" /proc/vmstat | sed 's/^/  /'
