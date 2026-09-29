#!/bin/sh
echo "########## 容器健康 ##########"
echo "PID1: $(cat /proc/1/comm)  uptime: $(cut -d' ' -f1 /proc/uptime)s"
echo "根分区: $(df -h / | tail -1)"
echo "内存: $(free -m | awk '/Mem:/{print $2" MB total, "$7" MB available"}')"

echo
echo "########## A 组配置是否在重启后保留 ##########"
echo "-- 符号链接（H3 修复）--"
for t in bun bunx claude codex; do
  p=$(command -v $t 2>/dev/null)
  printf '  %-8s %s\n' "$t" "${p:-MISS}"
done
echo "-- profile.d/99-ds-path.sh --"
[ -f /etc/profile.d/99-ds-path.sh ] && echo "  存在" || echo "  丢失"
echo "-- droidspaces_env.sh 应为空 --"
[ -s /etc/profile.d/droidspaces_env.sh ] && echo "  非空(异常)" || echo "  空(正常)"
echo "-- bash.bashrc 两段 --"
grep -c "ds-dev-ulimit\|ds-dev-ccache" /etc/bash.bashrc
echo "-- 登录 shell 实测 --"
/bin/bash -lc 'echo "  nofile=$(ulimit -Sn)/$(ulimit -Hn) memlock=$(ulimit -Sl)kB"; echo "  ccache在PATH: $(command -v gcc)"'
echo "-- ccache 配置 --"
ccache --show-config 2>/dev/null | grep -E "max_size|compression =|cache_dir" | sed 's/^/  /'

echo
echo "########## B 组工具链是否完整 ##########"
n=0; miss=""
for t in make cmake ninja pkg-config clang clang++ lld ld.lld ccache rustc cargo \
         gcc g++ node npm python3 pip3 autoconf automake libtool gdb git; do
  if command -v $t >/dev/null 2>&1; then n=$((n+1)); else miss="$miss $t"; fi
done
echo "  可用 $n / 22"
[ -n "$miss" ] && echo "  缺失:$miss"

echo
echo "########## fstrim 结果是否保留 ##########"
df -h / | tail -1

echo
echo "########## 基线状态（与报告一致？）##########"
echo "  内核: $(uname -r)"
echo "  sched_util_clamp_min: $(cat /proc/sys/kernel/sched_util_clamp_min 2>/dev/null)"
echo "  THP: $(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "  MGLRU: $(cat /sys/kernel/mm/lru_gen/enabled 2>/dev/null)"
echo "  TZ: $(cat /etc/timezone 2>/dev/null)"
echo "  cgroup controllers: [$(cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null)]"
