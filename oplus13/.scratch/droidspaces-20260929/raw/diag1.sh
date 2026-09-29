#!/bin/sh
# Droidspaces 容器环境诊断 —— 第一阶段：系统 / 容器形态 / CPU / cgroup（纯只读）
sec() { echo; echo "########## $* ##########"; }

sec "1. 系统信息"
echo "[uname -a]"; uname -a
echo "[uname -r]"; uname -r
echo "[uname -m]"; uname -m
echo "[os-release]"; cat /etc/os-release 2>/dev/null | head -6
echo "[getconf LONG_BIT]"; getconf LONG_BIT 2>/dev/null
echo "[nproc]"; nproc
echo "[hostname]"; hostname
echo "[uptime]"; uptime

sec "2. 容器形态判定"
echo "[PID 1]"; cat /proc/1/comm 2>/dev/null; head -1 /proc/1/status 2>/dev/null
echo "[PID 1 cmdline]"; tr '\0' ' ' < /proc/1/cmdline 2>/dev/null; echo
echo "[self cgroup]"; cat /proc/self/cgroup 2>/dev/null
echo "[PID1 ns]"; ls -l /proc/1/ns/ 2>/dev/null
echo "[self ns]"; ls -l /proc/self/ns/ 2>/dev/null
echo "[mountinfo 前 12 行]"; head -12 /proc/self/mountinfo 2>/dev/null
echo "[文件系统类型统计]"; findmnt -rno FSTYPE 2>/dev/null | sort | uniq -c | sort -rn | head -15
echo "[PRoot/ptrace 迹象]"; grep -iE "proot|ptrace|qemu" /proc/self/status /proc/1/status 2>/dev/null | head -5
echo "[systemd-detect-virt]"; systemd-detect-virt 2>&1 | head -2
echo "[systemd-detect-virt --container]"; systemd-detect-virt --container 2>&1 | head -2
echo "[systemd 版本]"; systemctl --version 2>/dev/null | head -1

sec "3. CPU topology"
echo "[online]"; cat /sys/devices/system/cpu/online 2>/dev/null
echo "[possible]"; cat /sys/devices/system/cpu/possible 2>/dev/null
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  n=$(basename "$c")
  [ -f "$c/topology/core_id" ] || continue
  printf '%s core=%s pkg=%s cluster=%s\n' "$n" \
    "$(cat $c/topology/core_id 2>/dev/null)" \
    "$(cat $c/topology/physical_package_id 2>/dev/null)" \
    "$(cat $c/topology/cluster_id 2>/dev/null)"
done
echo "[cpu_capacity]"; for c in /sys/devices/system/cpu/cpu[0-9]*; do printf '%s=%s ' "$(basename $c)" "$(cat $c/cpu_capacity 2>/dev/null)"; done; echo
echo "[cpufreq policy 布局]"; ls -d /sys/devices/system/cpu/cpufreq/policy* 2>/dev/null
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  echo "--- $(basename $p): cpus=$(cat $p/affected_cpus 2>/dev/null) gov=$(cat $p/scaling_governor 2>/dev/null) min=$(cat $p/scaling_min_freq 2>/dev/null) max=$(cat $p/scaling_max_freq 2>/dev/null) hwmax=$(cat $p/cpuinfo_max_freq 2>/dev/null) cur=$(cat $p/scaling_cur_freq 2>/dev/null)"
  echo "    avail_gov=$(cat $p/scaling_available_governors 2>/dev/null)"
  echo "    avail_freq=$(cat $p/scaling_available_frequencies 2>/dev/null | head -c 300)"
done

sec "4. cgroup"
echo "[cgroup 挂载]"; mount 2>/dev/null | grep -i cgroup | head -5
echo "[cgroup.controllers (根)]"; cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null
echo "[cgroup.subtree_control (根)]"; cat /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null
echo "[cgroup v2 根下条目]"; ls /sys/fs/cgroup/ 2>/dev/null | head -25
echo "[cpu.max / cpu.weight]"; cat /sys/fs/cgroup/cpu.max 2>/dev/null; cat /sys/fs/cgroup/cpu.weight 2>/dev/null
echo "[uclamp]"; ls /sys/fs/cgroup/cpu.uclamp.* 2>/dev/null; cat /sys/fs/cgroup/cpu.uclamp.min 2>/dev/null; cat /sys/fs/cgroup/cpu.uclamp.max 2>/dev/null
echo "[上游 cpuset]"; ls /sys/fs/cgroup/cpuset.cpus* 2>/dev/null

sec "5. 内存"
echo "[free -h]"; free -h 2>/dev/null
echo "[meminfo 关键项]"; grep -E "MemTotal|MemFree|MemAvailable|Buffers|^Cached|SwapTotal|SwapFree|Dirty|Writeback|AnonPages|Mapped|Slab|SReclaimable|Committed_AS|CommitLimit|PageTables|KernelStack" /proc/meminfo 2>/dev/null
echo "[MGLRU enabled]"; cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null
echo "[MGLRU min_ttl]"; cat /sys/kernel/mm/lru_gen/min_ttl_ms 2>/dev/null
echo "[PSI cpu]"; cat /proc/pressure/cpu 2>/dev/null
echo "[PSI memory]"; cat /proc/pressure/memory 2>/dev/null
echo "[PSI io]"; cat /proc/pressure/io 2>/dev/null
echo "[vmstat 快照]"; vmstat 1 2 2>/dev/null | tail -3

sec "6. swap / zram"
echo "[swapon --show]"; swapon --show 2>/dev/null
echo "[/proc/swaps]"; cat /proc/swaps 2>/dev/null
echo "[zramctl]"; zramctl 2>/dev/null
for z in /sys/block/zram*; do
  [ -d "$z" ] || continue
  echo "--- $(basename $z): disksize=$(cat $z/disksize 2>/dev/null) algo=$(cat $z/comp_algorithm 2>/dev/null)"
done
echo "[zram mm_stat]" ; for z in /sys/block/zram*/mm_stat; do [ -f "$z" ] && echo "$z: $(cat $z)"; done

sec "7. VM sysctl"
for k in vm.swappiness vm.watermark_scale_factor vm.watermark_boost_factor vm.dirty_ratio vm.dirty_background_ratio vm.dirty_bytes vm.dirty_background_bytes vm.vfs_cache_pressure vm.page-cluster vm.min_free_kbytes vm.extra_free_kbytes vm.overcommit_memory vm.max_map_count; do
  printf '%s = %s\n' "$k" "$(sysctl -n $k 2>/dev/null)"
done

sec "8. 限制"
echo "[ulimit -a]"; ulimit -a 2>/dev/null
echo "[/proc/sys/kernel/pid_max]"; cat /proc/sys/kernel/pid_max 2>/dev/null
echo "[threads-max]"; cat /proc/sys/kernel/threads-max 2>/dev/null
echo "[file-max]"; cat /proc/sys/fs/file-max 2>/dev/null
echo "[file-nr]"; cat /proc/sys/fs/file-nr 2>/dev/null
echo "[cgroup pids.max]"; cat /sys/fs/cgroup/pids.max 2>/dev/null
echo "[cgroup memory.max]"; cat /sys/fs/cgroup/memory.max 2>/dev/null

sec "9. thermal"
echo "[/sys/class/thermal zones]"; for t in /sys/class/thermal/thermal_zone*; do [ -d "$t" ] || continue; printf '%s type=%s temp=%s\n' "$(basename $t)" "$(cat $t/type 2>/dev/null)" "$(cat $t/temp 2>/dev/null)"; done | head -20
echo "[thermal pressure]"; cat /proc/pressure/thermal 2>/dev/null || echo "(无 /proc/pressure/thermal)"
echo "[cpu cooling devices]"; ls /sys/class/thermal/cooling_device*/type 2>/dev/null | head -5
echo "[sched 调优节点抽样]"; ls /sys/kernel/ 2>/dev/null | head -20

sec "10. 存储"
echo "[df -hT]"; df -hT 2>/dev/null | head -20
echo "[findmnt / ]"; findmnt -no SOURCE,FSTYPE,OPTIONS / 2>/dev/null
echo "[/etc/fstab]"; cat /etc/fstab 2>/dev/null | grep -v '^#' | head -10
echo "[挂载点概览]"; findmnt -rno TARGET,FSTYPE,SOURCE 2>/dev/null | head -25
echo "[lsblk]"; lsblk 2>/dev/null | head -20
echo "[/dev 块设备]"; ls -l /dev/ | grep -E "^b" | head -15
echo "[某块设备 queue 参数]"; for b in $(lsblk -rno NAME 2>/dev/null | head -6); do [ -f "/sys/block/$b/queue/scheduler" ] && echo "$b: sched=$(cat /sys/block/$b/queue/scheduler 2>/dev/null) ra=$(cat /sys/block/$b/queue/read_ahead_kb 2>/dev/null) nr=$(cat /sys/block/$b/queue/nr_requests 2>/dev/null)"; done

echo
echo "########## 诊断结束 ##########"
