echo "===== H0. 身份与内核 ====="
echo "uptime: $(cut -d' ' -f1 /proc/uptime)s   now: $(date '+%F %T %Z')"
echo "kernel: $(uname -r)  $(uname -m)   nproc: $(nproc)"
echo
echo "===== H1. 内存总览 ====="
free -m | sed 's/^/  /'
echo "  --- meminfo 关键项 ---"
grep -E "^(MemTotal|MemFree|MemAvailable|Buffers|Cached|SwapTotal|SwapFree|SwapCached|Dirty|Writeback|AnonPages|Mapped|Shmem|Slab|SReclaimable|SUnreclaim|KReclaimable|PageTables|KernelStack|Committed_AS|CommitLimit|VmallocUsed|Percpu):" /proc/meminfo | sed 's/^/    /'
echo
echo "===== H2. 水位：为什么 MemFree 长期低于 high ====="
echo "  --- 全局 min 水位 ---"
grep -E "^(Node|  zone|        )" /proc/zoneinfo 2>/dev/null | head -0
awk '/^Node/{node=$2} /zone /{z=$2} /protection:/{print "  node"node" "z" : "$2" "$3" "$4" "$5} /managed/{m=$2} /^  free/{print "      free="$2" (managed="m")"}' /proc/zoneinfo | head -60
echo "  --- watermark_scale_factor / min_free_kbytes / boost ---"
for k in watermark_scale_factor watermark_boost_factor min_free_kbytes; do printf "    %-28s = %s\n" "$k" "$(cat /proc/sys/vm/$k 2>/dev/null)"; done
echo
echo "===== H3. 回收压力事实 ====="
echo "  --- PSI ---"
for f in cpu memory io; do printf "    %-7s %s\n" "$f" "$(cat /proc/pressure/$f)"; done
echo "  --- vmstat 累计 ---"
grep -E "^(pgscan|pgsteal|pgactivate|pgdeactivate|pgrefill|pgdemote|workingset_refault|workingset_activate|workingset_restore|allocstall|compact_migrate_scanned|compact_free_scanned|compact_stall|compact_fail|compact_success|pgmajfault|pswpin|pswpout|nr_free_pages|nr_zone_inactive_anon|nr_zone_active_anon|nr_zone_inactive_file|nr_zone_active_file|nr_slab_reclaimable|nr_slab_unreclaimable)" /proc/vmstat | sed 's/^/    /'
echo
echo "===== H4. MGLRU ====="
echo "    /sys/kernel/mm/lru_gen/enabled = $(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null || echo '(不存在)')"
echo "    /sys/kernel/mm/lru_gen/min_ttl_ms = $(cat /sys/kernel/mm/lru_gen/min_ttl_ms 2>/dev/null || echo '(不存在)')"
ls /sys/kernel/mm/lru_gen/ 2>/dev/null | sed 's/^/      /'
echo
echo "===== H5. THP / compaction / 其他 vm 旋钮现状 ====="
for p in /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/shmem_enabled \
         /proc/sys/vm/compaction_proactiveness /proc/sys/vm/swappiness /proc/sys/vm/page-cluster \
         /proc/sys/vm/vfs_cache_pressure /proc/sys/vm/dirty_ratio /proc/sys/vm/dirty_background_ratio \
         /proc/sys/vm/dirty_bytes /proc/sys/vm/dirty_background_bytes /proc/sys/vm/dirty_expire_centisecs \
         /proc/sys/vm/dirty_writeback_centisecs /proc/sys/vm/watermark_scale_factor \
         /proc/sys/vm/watermark_boost_factor /proc/sys/vm/min_free_kbytes /proc/sys/vm/extfrag_threshold \
         /proc/sys/vm/stat_interval /proc/sys/kernel/sched_util_clamp_min /proc/sys/kernel/sched_util_clamp_max; do
  [ -e "$p" ] && printf "    %-56s = %s\n" "$(basename $p)" "$(cat $p)"
done
echo
echo "===== H6. zram 现状 ====="
cat /proc/swaps | sed 's/^/    /'
for z in /sys/block/zram*; do
  [ -d "$z" ] || continue
  echo "  --- $(basename $z) ---"
  printf "    disksize=%s  comp_algorithm=%s\n" "$(cat $z/disksize 2>/dev/null)" "$(cat $z/comp_algorithm 2>/dev/null)"
  cat $z/mm_stat 2>/dev/null | awk '{printf "    orig=%s MB  compr=%s MB  压缩率=%.2fx  mem_used=%s MB  same_pages=%s  huge_pages=%s\n",$1/1048576,$2/1048576,$1/$2,$3/1048576,$7,$8}'
done
echo
echo "===== H7. 命名空间（§3 架构模型） ====="
echo "  --- 容器 init(1) 的 ns ---"
for n in /proc/1/ns/*; do :; done
echo "  --- 宿主 init(1) ---"
printf "    pid1 comm = %s\n" "$(cat /proc/1/comm)"
echo "    /proc/1/cgroup = $(cat /proc/1/cgroup)"
echo "    /proc/self/cgroup = $(cat /proc/self/cgroup)"
echo "  --- 宿主侧的 droidspaces 进程 ---"
ps -A -o pid,ppid,comm 2>/dev/null | grep -iE "droidspaces|containerd|dsp" | head -10 | sed 's/^/    /'
echo
echo "===== H8. debugfs 是否残留挂载（上轮越界项复核） ====="
grep -c debugfs /proc/mounts | sed 's/^/    含 debugfs 的挂载行数 = /'
grep debugfs /proc/mounts | sed 's/^/    /'
ls -la /sys/kernel/debug 2>/dev/null | head -3 | sed 's/^/    /'
echo
echo "===== H9. 上轮宿主脚本清单（用于清理） ====="
ls -la /data/local/tmp/*.sh /data/local/tmp/*.py /data/local/tmp/*.txt /data/local/tmp/*.orig 2>/dev/null | awk '{print "    "$5"\t"$9}'
