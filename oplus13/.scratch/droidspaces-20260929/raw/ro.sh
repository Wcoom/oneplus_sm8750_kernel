echo "##### 调度器与扩展 #####"
zcat /proc/config.gz 2>/dev/null | grep -E "^CONFIG_SCHED_CLASS_EXT|^CONFIG_SCHED_EXT|^CONFIG_SCHED_WALT|^CONFIG_SCHED_BORE|^CONFIG_ENERGY_MODEL|^CONFIG_SCHED_DEBUG" 
echo "[sched_ext sysfs]"; ls /sys/kernel/sched_ext/ 2>&1 | head -5
echo "[scx 二进制]"; command -v scx_rusty scx_lavd 2>/dev/null || echo "(无)"
echo "##### 内存压缩/IO 关键项 #####"
zcat /proc/config.gz 2>/dev/null | grep -E "^CONFIG_ZRAM|^CONFIG_ZSWAP|^CONFIG_ZPOOL|^CONFIG_CRYPTO_ZSTD|^CONFIG_ZSTD|^CONFIG_LZ4|^CONFIG_MMC|^CONFIG_SCSI_UFS|^CONFIG_BLK_DEV_NVME" | head -20
echo "##### uclamp 实际可用性 #####"
echo "[sched_util_clamp_min]"; cat /proc/sys/kernel/sched_util_clamp_min 2>/dev/null
echo "[sched_util_clamp_max]"; cat /proc/sys/kernel/sched_util_clamp_max 2>/dev/null
echo "[sched_util_clamp_min_rt_default]"; cat /proc/sys/kernel/sched_util_clamp_min_rt_default 2>/dev/null
echo "##### 各核当前负载快照 #####"
cat /proc/loadavg; echo; head -1 /proc/stat
echo "##### zram 全量 #####"
for z in /sys/block/zram0; do
  echo "disksize=$(cat $z/disksize)"; echo "mem_limit=$(cat $z/mem_limit 2>/dev/null)"
  echo "mm_stat=$(cat $z/mm_stat 2>/dev/null)"
  echo "io_stat=$(cat $z/io_stat 2>/dev/null)"
done
echo "##### 稀疏镜像可回收量 #####"
echo "宿主 rootfs.img: $(stat -c '%s' /mnt/data/local/Droidspaces/Containers/ubuntu/rootfs.img) bytes 虚拟"
echo "实际占用 blocks: $(stat -c '%b' /mnt/data/local/Droidspaces/Containers/ubuntu/rootfs.img) x 512 = $(( $(stat -c '%b' /mnt/data/local/Droidspaces/Containers/ubuntu/rootfs.img) * 512 )) bytes"
