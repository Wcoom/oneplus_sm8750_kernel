echo "########## S3 侦察：fstrim 定时 ##########"
echo "=== fstrim 单元是否存在 ==="
ls -l /usr/lib/systemd/system/fstrim.* 2>&1 | sed 's/^/  /'
echo "=== fstrim.timer 状态 ==="
systemctl status fstrim.timer --no-pager 2>&1 | head -10 | sed 's/^/  /'
echo "=== fstrim.service（关注 ConditionVirtualization）==="
cat /usr/lib/systemd/system/fstrim.service 2>&1 | sed 's/^/  /'
echo "=== fstrim.timer（关注 Persistent/OnCalendar）==="
cat /usr/lib/systemd/system/fstrim.timer 2>&1 | sed 's/^/  /'
echo "=== 已注册的 timer ==="
systemctl list-timers --all --no-pager 2>&1 | head -12 | sed 's/^/  /'
echo "=== 根挂载参数（是否 discard）==="
findmnt -no SOURCE,FSTYPE,OPTIONS / 2>&1 | sed 's/^/  /'
echo "=== 容器已运行 ==="
uptime -p
echo "=== fstrim 可用性 ==="
echo "  路径: $(command -v fstrim)"; fstrim --version 2>&1 | head -1 | sed 's/^/  /'

echo
echo "########## S5 侦察：/root/.codex/packages ##########"
echo "=== codex 版本 ==="
echo "  $(codex --version 2>&1 | head -1)"
echo "=== /root/.codex 整体占用 ==="
du -sh /root/.codex 2>/dev/null | sed 's/^/  /'
echo "=== packages 明细（按大小）==="
du -sh /root/.codex/packages/* 2>/dev/null | sort -h | sed 's/^/  /'
echo "=== 顶层结构 ==="
ls -la /root/.codex/packages/ 2>&1 | head -20 | sed 's/^/  /'

echo
echo "########## S7 侦察：/usr/local/bin 冲突检查 ##########"
echo "=== 现有内容 ==="
ls -la /usr/local/bin/ | sed 's/^/  /'
echo "=== 待建 shim 名冲突检查 ==="
c=0
for n in cc c++ gcc g++ gcc-15 g++-15 clang clang++ clang-21 clang++-21 c89-gcc c99-gcc aarch64-linux-gnu-gcc aarch64-linux-gnu-g++ aarch64-linux-gnu-gcc-15 aarch64-linux-gnu-g++-15; do
  if [ -e "/usr/local/bin/$n" ] || [ -L "/usr/local/bin/$n" ]; then echo "  ⚠ 冲突: $n"; c=$((c+1)); fi
done
[ $c = 0 ] && echo "  ✓ 16 个名字全部无冲突"
echo "=== ccache 本体 ==="
ls -l /usr/bin/ccache | sed 's/^/  /'
echo "  $(ccache --version | head -1)"
