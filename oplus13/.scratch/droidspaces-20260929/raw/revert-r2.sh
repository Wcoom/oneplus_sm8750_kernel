echo "===== 9. 还原后环境变量实况 ====="
echo "  --- 注：以下取自「当前运行中的」容器进程环境 ---"
for v in CCACHE_DIR CCACHE_MAXSIZE TMPDIR CMAKE_GENERATOR CARGO_TARGET_DIR GOCACHE GOMODCACHE npm_config_cache BUN_INSTALL_CACHE_DIR; do
  eval "val=\${$v:-}"; printf "    %-24s = %s\n" "$v" "${val:-<已清除>}"
done
echo "    PATH = $PATH"
echo "    which gcc = $(command -v gcc)"
echo "    which clang = $(command -v clang)"
echo
echo "===== 9b. 新起一个 droidspaces run 的真实注入面（这才是还原是否生效的判据） ====="
echo "    （本脚本自身就是新起的 run，上面若仍显示 CCACHE_DIR 说明 droidspaces 缓存了旧值）"
echo "    /etc/environment 现状 = $(cat /etc/environment)"
echo
echo "===== 10. 残留调优复扫（§1 收尾核查） ====="
r=0
for f in /etc/profile.d/99-ds-path.sh /etc/security/limits.d/99-ds-dev.conf \
         /etc/systemd/system.conf.d/99-ds-dev.conf /etc/systemd/journald.conf.d/99-ds-dev.conf \
         /root/.ccache.conf /etc/ccache.conf; do
  [ -e "$f" ] && { echo "    ★残留 $f"; r=$((r+1)); }
done
echo "    bash.bashrc 标记行数         : $(grep -c 'ds-dev-' /etc/bash.bashrc)"
echo "    /usr/local/bin 指向 ccache 的链接数: $(ls -la /usr/local/bin/ | grep -c '/usr/lib/ccache')"
echo "    /etc/environment 调优行数    : $(grep -cE 'CCACHE|TMPDIR|CARGO|GOCACHE|npm_config|BUN_INSTALL|CMAKE' /etc/environment)"
echo "    /mnt/data/ds-build 存在      : $([ -e /mnt/data/ds-build ] && echo 是 || echo 否)"
echo "    ── 文件级残留合计: $r ──"
echo
echo "===== 11. 受影响服务是否需重载 ====="
echo "    systemd 已重载? 需手动 daemon-reload 以清除 DefaultLimitNOFILE 缓存"
systemctl show -p DefaultLimitNOFILE -p DefaultLimitMEMLOCK 2>/dev/null | sed 's/^/      /'
echo "    systemd-journald 生效上限: $(journalctl --disk-usage 2>/dev/null | head -1)"
echo
echo "===== 12. 关键功能未被打断的验证 ====="
echo "    gcc 仍可用: $(gcc --version 2>/dev/null | head -1)"
echo "    clang 仍可用: $(clang --version 2>/dev/null | head -1)"
echo "    ninja: $(ninja --version 2>/dev/null)"
echo "    cmake: $(cmake --version 2>/dev/null | head -1)"
echo "    cargo: $(cargo --version 2>/dev/null)"
echo "    node : $(node --version 2>/dev/null)"
echo "    codex/claude/bun 可见性: $(command -v codex) / $(command -v claude) / $(command -v bun)"
