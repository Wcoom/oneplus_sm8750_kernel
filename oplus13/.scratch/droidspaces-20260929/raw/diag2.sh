#!/bin/sh
# Droidspaces 容器环境诊断 —— 第二阶段：内核配置 / 工具链 / IO / 进程 / 边界（纯只读）
sec() { echo; echo "########## $* ##########"; }

sec "A. 内核配置"
if [ -r /proc/config.gz ]; then
  echo "[来源] /proc/config.gz"
  KC="zcat /proc/config.gz"
elif [ -r /boot/config-$(uname -r) ]; then
  echo "[来源] /boot/config-$(uname -r)"
  KC="cat /boot/config-$(uname -r)"
else
  echo "[来源] 无（IKCONFIG 未启用或不可读）"
  KC=""
fi
if [ -n "$KC" ]; then
  echo "[内核版本行]"; $KC 2>/dev/null | grep -E "^CONFIG_LOCALVERSION|^# Linux" | head -3
  echo "[cgroup / ns]"; $KC 2>/dev/null | grep -E "^CONFIG_CGROUP|^CONFIG_NAMESPACES|^CONFIG_(PID|NET|UTS|IPC|USER|MNT)_NS|^CONFIG_MEMCG|^CONFIG_BLK_CGROUP" | head -30
  echo "[内存管理]"; $KC 2>/dev/null | grep -E "^CONFIG_LRU_GEN|^CONFIG_ZRAM|^CONFIG_ZSMALLOC|^CONFIG_ZSWAP|^CONFIG_KSM|^CONFIG_TRANSPARENT_HUGEPAGE|^CONFIG_COMPACTION|^CONFIG_PSI|^CONFIG_SWAP|^CONFIG_MEMCG_SWAP|^CONFIG_DAMON" | head -25
  echo "[调度]"; $KC 2>/dev/null | grep -E "^CONFIG_SCHED_(CLASS|AUTOGROUP|CORE|SMT|MC|RT)|^CONFIG_UCLAMP|^CONFIG_FAIR_GROUP|^CONFIG_PREEMPT|^CONFIG_HZ|^CONFIG_NR_CPUS|^CONFIG_SCHED_WALT" | head -25
  echo "[IO / 块层]"; $KC 2>/dev/null | grep -E "^CONFIG_BLK_|^CONFIG_IOSCHED|^CONFIG_MQ_IOSCHED|^CONFIG_IO_URING|^CONFIG_AIO|^CONFIG_F2FS|^CONFIG_EXT4|^CONFIG_BLK_DEV_LOOP|^CONFIG_DM_" | head -25
  echo "[文件系统/其他]"; $KC 2>/dev/null | grep -E "^CONFIG_FUSE|^CONFIG_OVERLAY_FS|^CONFIG_TMPFS|^CONFIG_KALLSYMS|^CONFIG_MODULES|^CONFIG_BPF_SYSCALL|^CONFIG_NTSYNC" | head -15
  echo "[安全]"; $KC 2>/dev/null | grep -E "^CONFIG_SECURITY_SELINUX|^CONFIG_SECURITY_LOCKDOWN|^CONFIG_SECURITY_YAMA|^CONFIG_HARDENED_USERCOPY|^CONFIG_STACKPROTECTOR" | head -10
else
  echo "(内核配置不可读)"
fi

sec "B. 工具链"
for t in gcc g++ clang clang++ rustc cargo node npm pnpm yarn python3 pip3 go git make ninja cmake ccache distcc sccache mold lld ld.gold gdb strace perf iostat; do
  p=$(command -v $t 2>/dev/null)
  if [ -n "$p" ]; then
    v=$($t --version 2>&1 | head -1)
    printf '%-10s %-32s %s\n' "$t" "$p" "$v"
  else
    printf '%-10s %s\n' "$t" "（未安装）"
  fi
done
echo "[nproc / 编译并行度默认值]"
echo "nproc=$(nproc)"
echo "MAKEFLAGS=${MAKEFLAGS:-(未设置)}"
echo "npm_config_jobs=${npm_config_jobs:-(未设置)}"
echo "CARGO_BUILD_JOBS=${CARGO_BUILD_JOBS:-(未设置)}"

sec "C. 网络（shared netns = 宿主网络）"
echo "[ip addr]"; ip -o addr show 2>/dev/null | head -12
echo "[默认路由]"; ip route show default 2>/dev/null | head -3
echo "[resolv.conf]"; cat /etc/resolv.conf 2>/dev/null | head -5
echo "[TCP sysctl]"
for k in net.core.rmem_max net.core.wmem_max net.core.rmem_default net.core.wmem_default net.ipv4.tcp_rmem net.ipv4.tcp_wmem net.ipv4.tcp_congestion_control net.core.somaxconn net.ipv4.tcp_max_syn_backlog net.core.netdev_max_backlog net.ipv4.tcp_fastopen net.ipv4.tcp_slow_start_after_idle net.ipv4.tcp_mtu_probing net.ipv4.ip_local_port_range net.ipv4.tcp_tw_reuse net.core.bpf_jit_enable; do
  printf '  %-42s = %s\n' "$k" "$(sysctl -n $k 2>/dev/null)"
done
echo "[MTU]"; ip -o link show 2>/dev/null | grep -oE '^[0-9]+: [a-z0-9]+|mtu [0-9]+' | paste - - 2>/dev/null | head -8

sec "D. I/O / 块设备详情"
for b in loop50 dm-61 sda sdf; do
  q=/sys/block/$b/queue
  [ -d "$q" ] || continue
  echo "--- $b"
  echo "    scheduler=[$(cat $q/scheduler 2>/dev/null)]"
  echo "    nr_requests=$(cat $q/nr_requests 2>/dev/null) read_ahead_kb=$(cat $q/read_ahead_kb 2>/dev/null)"
  echo "    rotational=$(cat $q/rotational 2>/dev/null) wbt_lat_usec=$(cat $q/wbt_lat_usec 2>/dev/null) nomerges=$(cat $q/nomerges 2>/dev/null)"
  echo "    max_sectors_kb=$(cat $q/max_sectors_kb 2>/dev/null) max_hw_sectors_kb=$(cat $q/max_hw_sectors_kb 2>/dev/null)"
  echo "    iostats=$(cat $q/iostats 2>/dev/null) add_random=$(cat $q/add_random 2>/dev/null)"
  echo "    discard_max_bytes=$(cat $q/discard_max_bytes 2>/dev/null) write_cache=$(cat $q/write_cache 2>/dev/null)"
done
echo "[/sys/block/loop50 直接确认]"
ls /sys/block/loop50/ 2>/dev/null | head -5
echo "[loop50 后端文件]"
cat /sys/block/loop50/loop/backing_file 2>/dev/null || echo "(不可读)"
echo "[磁盘统计 iostat -x 1 2]"
iostat -x 1 2 2>/dev/null | tail -20 || echo "(iostat 未安装)"

sec "E. CPU 亲和性与调度"
echo "[self status 关键行]"; grep -E "Cpus_allowed|Mems_allowed|Seccomp|CapEff" /proc/self/status 2>/dev/null
echo "[任务亲和性]"; taskset -pc $$ 2>/dev/null || echo "(taskset 未安装)"
echo "[sched_rt_runtime_us]"; cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null
echo "[sched_child_runs_first]"; cat /proc/sys/kernel/sched_child_runs_first 2>/dev/null
echo "[per-cpu 频率现状]"
for c in 0 3 6 7; do
  printf '  cpu%s cur=%s min=%s max=%s\n' "$c" \
    "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq 2>/dev/null)" \
    "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_min_freq 2>/dev/null)" \
    "$(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq 2>/dev/null)"
done
echo "[cpu.shares / cpu.weight (上游 v1 视图)]"
cat /sys/fs/cgroup/cpu/cpu.shares 2>/dev/null || echo "(容器内无 v1 cpu 挂载)"
echo "[cpuidle 状态 (cpu0)]"; ls /sys/devices/system/cpu/cpu0/cpuidle/ 2>/dev/null | head -5

sec "F. Droidspaces vproc 机制（负载/uptime 来源）"
echo "[vproc 挂载内容]"; ls -la /run/droidspaces/vproc/ 2>/dev/null
echo "[容器内 /proc/loadavg]"; cat /proc/loadavg
echo "[容器内 /proc/uptime]"; cat /proc/uptime
echo "[vproc 里有无 loadavg 副本]"; find /run/droidspaces -maxdepth 3 2>/dev/null | head -20
echo "[/proc/uptime 挂载来源]"; findmnt -no SOURCE,FSTYPE,OPTIONS /proc/uptime 2>/dev/null
echo "[宿主真实 uptime]"; cat /proc/1/../uptime 2>/dev/null; echo
echo "[开机时长来源对比]"; cut -d. -f1 /proc/uptime 2>/dev/null

sec "G. systemd 服务与进程"
echo "[运行中的 unit]"; systemctl list-units --type=service --state=running --no-pager --no-legend 2>/dev/null | head -30
echo "[unit 总数]"; systemctl list-units --type=service --state=running --no-pager --no-legend 2>/dev/null | wc -l
echo "[failed unit]"; systemctl list-units --state=failed --no-pager --no-legend 2>/dev/null | head -10
echo "[进程总数]"; ps -e --no-headers 2>/dev/null | wc -l
echo "[内存 TOP 12]"; ps aux --sort=-rss 2>/dev/null | head -13
echo "[CPU TOP 12]"; ps aux --sort=-%cpu 2>/dev/null | head -13

sec "H. 内存细节"
echo "[/proc/meminfo 全量]"; cat /proc/meminfo 2>/dev/null
echo "[slab top]"; head -20 /proc/slabinfo 2>/dev/null
echo "[buddyinfo]"; cat /proc/buddyinfo 2>/dev/null
echo "[zoneinfo]"; grep -E "Node|zone|managed|free|min|low|high|present" /proc/zoneinfo 2>/dev/null | head -30
echo "[vmstat -s 摘要]"; vmstat -s 2>/dev/null | head -25
echo "[容器 cgroup 内存限制]"
for f in memory.max memory.high memory.current memory.swap.max memory.swap.current memory.stat; do
  printf '  %-22s = %s\n' "$f" "$(head -1 /sys/fs/cgroup/$f 2>/dev/null)"
done
echo "[memory.stat 关键项]"; grep -E "^(anon|file|slab|kernel|sock|shmem|zswap|swapcached|pgfault|pgmajfault) " /sys/fs/cgroup/memory.stat 2>/dev/null | head -12

sec "I. 挂载参数与 tmpfs 容量"
echo "[/tmp]"; findmnt -no SOURCE,FSTYPE,OPTIONS,SIZE /tmp 2>/dev/null
echo "[/dev/shm]"; findmnt -no SOURCE,FSTYPE,OPTIONS,SIZE /dev/shm 2>/dev/null
echo "[/run]"; findmnt -no SOURCE,FSTYPE,OPTIONS,SIZE /run 2>/dev/null
echo "[全部 tmpfs]"; findmnt -t tmpfs -rno TARGET,SIZE,OPTIONS 2>/dev/null

sec "J. 已安装包与磁盘占用"
echo "[dpkg 包数]"; dpkg -l 2>/dev/null | grep -c '^ii'
echo "[根目录占用 TOP]"; du -sh /usr /var /opt /root /home /tmp 2>/dev/null
echo "[/usr 占用 TOP 10]"; du -sh /usr/* 2>/dev/null | sort -rh | head -10
echo "[/var/log 大小]"; du -sh /var/log 2>/dev/null
echo "[journal 占用]"; journalctl --disk-usage 2>/dev/null

sec "K. 时间与杂项"
echo "[date]"; date
echo "[timedatectl]"; timedatectl 2>/dev/null | head -8
echo "[locale]"; locale 2>/dev/null | head -3
echo "[swap 使用 TOP]"; for f in /proc/*/status; do awk '/^Name:/{n=$2}/^VmSwap:/{if($2>10000) print $2" kB  "n}' $f 2>/dev/null; done | sort -rn | head -12

echo
echo "########## 第二阶段诊断结束 ##########"
