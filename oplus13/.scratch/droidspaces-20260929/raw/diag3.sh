#!/bin/sh
# Droidspaces 容器环境诊断 —— 第三阶段：工具链实位 / 缓存 / THP / 宿主对照（纯只读）
sec() { echo; echo "########## $* ##########"; }

sec "A. 环境变量与 PATH"
echo "PATH=$PATH"
echo "HOME=$HOME"
echo "SHELL=$SHELL"
echo "USER=$(id -un) UID=$(id -u)"
echo "[env 全量]"; env 2>/dev/null | sort | head -40

sec "B. /root 占用 2.5G 明细"
du -sh /root/* /root/.[a-zA-Z]* 2>/dev/null | sort -rh | head -25

sec "C. 工具链实位搜索（含非 PATH 位置）"
for d in /usr/local/bin /usr/local/lib /opt /root/.cargo/bin /root/.rustup /root/.nvm /root/.local/bin /root/.bun/bin /snap/bin /usr/lib/node_modules; do
  [ -e "$d" ] && echo "[存在] $d" && ls "$d" 2>/dev/null | head -12 && echo "---"
done
echo "[find 搜索可执行文件（限深度）]"
find /root /opt /usr/local /usr/lib/node_modules -maxdepth 4 -type f \
  \( -name node -o -name npm -o -name pnpm -o -name yarn -o -name bun -o -name cargo -o -name rustc -o -name clang -o -name ccache -o -name ninja -o -name make -o -name cmake \) \
  -perm -u+x 2>/dev/null | head -30
echo "[node_modules 全局树]"
find / -maxdepth 5 -type d -name node_modules 2>/dev/null | head -10

sec "D. THP / KSM / MGLRU 运行时状态"
echo "[THP enabled]"; cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null
echo "[THP defrag]"; cat /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null
echo "[THP shmem_enabled]"; cat /sys/kernel/mm/transparent_hugepage/shmem_enabled 2>/dev/null
echo "[THP hpage_pmd_size]"; cat /sys/kernel/mm/transparent_hugepage/hpage_pmd_size 2>/dev/null
echo "[KSM run]"; cat /sys/kernel/mm/ksm/run 2>/dev/null
echo "[KSM pages_to_scan]"; cat /sys/kernel/mm/ksm/pages_to_scan 2>/dev/null
echo "[KSM sleep_millisecs]"; cat /sys/kernel/mm/ksm/sleep_millisecs 2>/dev/null
echo "[KSM full_scans]"; cat /sys/kernel/mm/ksm/full_scans 2>/dev/null
echo "[MGLRU enabled=0x$(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null)]"
echo "[MGLRU min_ttl_ms]"; cat /sys/kernel/mm/lru_gen/min_ttl_ms 2>/dev/null
echo "[MGLRU max_seq / 代际]"; for f in /sys/kernel/mm/lru_gen/*; do [ -f "$f" ] && echo "  $(basename $f) = $(head -c 200 $f 2>/dev/null | tr '\n' ' ')"; done
echo "[compaction]"
for f in /proc/sys/vm/compact_memory /proc/sys/vm/compaction_proactiveness /proc/sys/vm/extfrag_threshold; do
  [ -r "$f" ] && echo "  $f = $(cat $f 2>/dev/null)"
done
echo "[其他 vm 可调]"
for k in vm.zone_reclaim_mode vm.laptop_mode vm.stat_interval vm.numa_balancing vm.dirty_writeback_centisecs vm.dirty_expire_centisecs vm.percpu_pagelist_high_fraction vm.min_slab_ratio vm.admin_reserve_kbytes vm.user_reserve_kbytes; do
  printf '  %-38s = %s\n' "$k" "$(sysctl -n $k 2>/dev/null)"
done

sec "E. CPU 限频溯源"
echo "[各 policy 频率上限]"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
  [ -d "$p" ] || continue
  echo "--- $(basename $p): cpus=$(cat $p/affected_cpus 2>/dev/null)"
  echo "    scaling_max=$(cat $p/scaling_max_freq 2>/dev/null) cpuinfo_max=$(cat $p/cpuinfo_max_freq 2>/dev/null)"
  echo "    scaling_min=$(cat $p/scaling_min_freq 2>/dev/null) cpuinfo_min=$(cat $p/cpuinfo_min_freq 2>/dev/null)"
  echo "    cur=$(cat $p/scaling_cur_freq 2>/dev/null) gov=$(cat $p/scaling_governor 2>/dev/null)"
  echo "    boost=$(cat $p/scaling_boost_frequencies 2>/dev/null | head -c 150)"
done
echo "[thermal cooling device 状态]"
for cd in /sys/class/thermal/cooling_device*; do
  [ -d "$cd" ] || continue
  t=$(cat $cd/type 2>/dev/null); s=$(cat $cd/cur_state 2>/dev/null); m=$(cat $cd/max_state 2>/dev/null)
  case "$t" in *cpu*|*cpufreq*|*limit*) echo "  $(basename $cd) type=$t cur=$s max=$m";; esac
done
echo "[cpu 压力 stall 计数]"
grep -E "^(cpu|memory|io|thermal|irq|workingset)" /proc/pressure/* 2>/dev/null | head -30
echo "[schedstat 每核]"; cat /proc/schedstat 2>/dev/null | tail -10 | cut -c1-160

sec "F. 内存水位与回收压力"
echo "[vmstat 采样 5 次]"; vmstat 1 5 2>/dev/null
echo "[pgsteal / pgscan 累计]"; grep -E "^(pgsteal|pgscan|nr_)" /proc/vmstat 2>/dev/null | head -25
echo "[direct reclaim 计数]"; grep -E "allocstall|pgscan_direct|pgsteal_direct|compact_stall|compact_fail|thp_fault_alloc|thp_fault_fallback|thp_collapse" /proc/vmstat 2>/dev/null
echo "[swap 活动]"; grep -E "pswpin|pswpout|swap_ra" /proc/vmstat 2>/dev/null

sec "G. 容器与宿主边界"
echo "[/run/droidspaces/version]"; cat /run/droidspaces/version 2>/dev/null
echo "[/run/droidspaces/mount]"; head -30 /run/droidspaces/mount 2>/dev/null
echo "[/run/droidspaces/name]"; cat /run/droidspaces/name 2>/dev/null
echo "[container.config]"; cat /run/droidspaces/container.config 2>/dev/null | head -60
echo "[/proc/1/environ]"; tr '\0' '\n' < /proc/1/environ 2>/dev/null | head -20

sec "H. 网络连通性（只读探测）"
echo "[ping 网关/外网]"; ping -c 2 -W 2 1.1.1.1 2>&1 | tail -3
echo "[DNS 解析]"; getent hosts github.com 2>&1 | head -3
echo "[HTTPS 探测]"; curl -s -o /dev/null -w "github.com -> HTTP %{http_code}, dns=%{time_namelookup}s connect=%{time_connect}s tls=%{time_appconnect}s total=%{time_total}s\n" --max-time 12 https://github.com 2>&1
echo "[策略路由表]"; ip route show table all 2>/dev/null | grep -E "^default|^[0-9]+:" | head -12

sec "I. 磁盘镜像与稀疏情况"
echo "[rootfs.img 宿主侧 stat]"
stat -c 'size=%s blocks=%b blksize=%B' /mnt/data/local/Droidspaces/Containers/ubuntu/rootfs.img 2>/dev/null
echo "[容器内已用]"; df -h / 2>/dev/null | tail -1
echo "[宿主 /data 剩余]"; df -h /mnt/data 2>/dev/null | tail -1

sec "J. 应用侧存在性（无 PATH 依赖）"
for t in node nodejs deno bun rustup cargo clang clang-19 gcc-15 g++-15 make gmake cmake ninja-build ninja pnpm npm python3.13 python3.14 uv pipx; do
  for d in /usr/bin /usr/local/bin /bin /opt; do
    [ -x "$d/$t" ] && echo "  命中 $d/$t"
  done
done
echo "[apt 相关包名]"; dpkg -l 2>/dev/null | awk '$2 ~ /^(build-essential|make|cmake|ninja|clang|rustc|cargo|nodejs|npm|python3-pip|curl|wget|git)$/ {print "  "$2" "$3}'

echo
echo "########## 第三阶段诊断结束 ##########"
