echo "########## 确认身处容器（不是宿主） ##########"
echo "  PID1 = $(cat /proc/1/comm)   uptime = $(cut -d' ' -f1 /proc/uptime)s"
echo "  /etc/os-release: $(grep -m1 PRETTY_NAME /etc/os-release | cut -d'"' -f2)"
echo
echo "########## 容器健康 ##########"
df -h / | sed 's/^/  /'
free -m | sed 's/^/  /'
echo
echo "########## §1 残留复查（应为 0） ##########"
r=0
[ -f /etc/profile.d/99-ds-path.sh ] && { echo "  !! 99-ds-path.sh 仍在"; r=$((r+1)); }
[ -f /etc/security/limits.d/99-ds-dev.conf ] && { echo "  !! limits.d 仍在"; r=$((r+1)); }
[ -f /etc/systemd/system.conf.d/99-ds-dev.conf ] && { echo "  !! system.conf.d 仍在"; r=$((r+1)); }
[ -f /etc/systemd/journald.conf.d/99-ds-dev.conf ] && { echo "  !! journald.conf.d 仍在"; r=$((r+1)); }
[ -d /mnt/data/ds-build ] && { echo "  !! ds-build 仍在"; r=$((r+1)); }
n=$(wc -l < /etc/environment 2>/dev/null || echo 0)
[ "$n" != "1" ] && { echo "  !! /etc/environment 有 $n 行（应为 1）"; r=$((r+1)); } || echo "  ✓ /etc/environment 恰 1 行"
c=0; for t in gcc g++ cc clang clang++ gcc-15 c++ aarch64-linux-gnu-gcc; do [ -e /usr/local/bin/$t ] && c=$((c+1)); done
[ "$c" != "0" ] && { echo "  !! /usr/local/bin 下仍有 $c 个 ccache shim"; r=$((r+1)); } || echo "  ✓ /usr/local/bin 无 ccache shim"
grep -c "ds-dev-ulimit\|ds-dev-ccache" /etc/bash.bashrc | grep -q "^0$" && echo "  ✓ bash.bashrc 无残留追加" || { echo "  !! bash.bashrc 仍有追加"; r=$((r+1)); }
echo "  ⇒ 残留项合计 = $r"
echo
echo "########## 保留项确认（应存在） ##########"
for f in /root/.ds-opt/pre-revert-20260930 /etc/systemd/system/fstrim.timer.d/override.conf; do
  [ -e "$f" ] && echo "  ✓ $f" || echo "  ✗ $f 缺失"
done
printf "  ✓ /usr/local/bin 链接: "; for t in bun bunx claude codex; do [ -e /usr/local/bin/$t ] && printf '%s ' $t; done; echo
echo "  ✓ ccache 缓存: $(du -sh /mnt/data/.ccache 2>/dev/null | cut -f1)"
echo "  ✓ 时区: $(cat /etc/timezone 2>/dev/null)"
echo
echo "########## 工具链（应为 22/22） ##########"
n=0; miss=""
for t in make cmake ninja pkg-config clang clang++ lld ld.lld ccache rustc cargo gcc g++ node npm python3 pip3 autoconf automake libtool gdb git; do
  command -v $t >/dev/null 2>&1 && n=$((n+1)) || miss="$miss $t"
done
echo "  $n / 22${miss:+   缺失:$miss}"
echo
echo "########## 容器内 cgroup / 环境变量 ##########"
echo "  /proc/self/cgroup: $(tr '\n' ' ' < /proc/self/cgroup)"
echo "  调优类环境变量残留: $(env | grep -cE '^(TMPDIR|CCACHE_|CARGO_TARGET_DIR|GOCACHE|GOMODCACHE|npm_config_cache|BUN_INSTALL_CACHE_DIR|CMAKE_GENERATOR)=') 个（应为 0）"
