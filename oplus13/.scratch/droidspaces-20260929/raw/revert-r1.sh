set -u
BK=/root/.ds-opt/pre-revert-20260930
mkdir -p "$BK"
echo "===== 0. 先备份当前状态（使还原本身可逆） ====="
cp -a /etc/environment "$BK/environment.pre-revert"
cp -a /etc/bash.bashrc "$BK/bash.bashrc.pre-revert"
cp -a /etc/localtime  "$BK/localtime.pre-revert" 2>/dev/null || true
[ -f /etc/security/limits.d/99-ds-dev.conf ] && cp -a /etc/security/limits.d/99-ds-dev.conf "$BK/"
[ -d /etc/systemd/system.conf.d ] && cp -a /etc/systemd/system.conf.d/99-ds-dev.conf "$BK/" 2>/dev/null
[ -d /etc/systemd/journald.conf.d ] && cp -a /etc/systemd/journald.conf.d/99-ds-dev.conf "$BK/" 2>/dev/null
cp -a /etc/profile.d/99-ds-path.sh "$BK/" 2>/dev/null || true
ls -la "$BK" | sed 's/^/    /'
echo
echo "===== 1. 还原 /etc/environment（→ 仅剩 PATH 一行） ====="
cp -a /root/.ds-opt/environment.orig /etc/environment
echo "  还原后全文:"; cat /etc/environment | sed 's/^/    /'
echo
echo "===== 2. 还原 /etc/bash.bashrc ====="
cp -a /root/.ds-opt/bash.bashrc.orig /etc/bash.bashrc
echo "  残留标记行数（应为0）: $(grep -c 'ds-dev-ulimit\|ds-dev-ccache' /etc/bash.bashrc)"
echo
echo "===== 3. 删除 shell profile 层 ====="
rm -fv /etc/profile.d/99-ds-path.sh | sed 's/^/    /'
echo
echo "===== 4. 删除限额层 ====="
rm -fv /etc/security/limits.d/99-ds-dev.conf | sed 's/^/    /'
rm -fv /etc/systemd/system.conf.d/99-ds-dev.conf | sed 's/^/    /'
rmdir /etc/systemd/system.conf.d 2>/dev/null && echo "    已删空目录 /etc/systemd/system.conf.d"
rm -fv /etc/systemd/journald.conf.d/99-ds-dev.conf | sed 's/^/    /'
rmdir /etc/systemd/journald.conf.d 2>/dev/null && echo "    已删空目录 /etc/systemd/journald.conf.d"
echo
echo "===== 5. 拆除 ccache 注入（16 个 shim） ====="
n=0
while read -r t; do
  [ -n "$t" ] || continue
  if [ -L "/usr/local/bin/$t" ]; then rm -fv "/usr/local/bin/$t" >/dev/null; n=$((n+1)); fi
done < /root/.ds-opt/ccache-shims.list
echo "    已删 ccache shim: $n 个"
echo "    /usr/local/bin 剩余:"; ls -la /usr/local/bin/ | sed 's/^/      /'
echo
echo "===== 6. 清构建缓存目录（S1/S4 产物） ====="
du -sh /mnt/data/ds-build 2>/dev/null | sed 's/^/    删前 /'
rm -rf /mnt/data/ds-build
echo "    /mnt/data/ds-build 存在? $([ -e /mnt/data/ds-build ] && echo 是 || echo 否)"
rm -f /root/.ccache.conf 2>/dev/null; echo "    /root/.ccache.conf 存在? $([ -e /root/.ccache.conf ] && echo 是 || echo 否)"
echo
echo "===== 7. 清 benchmark 残留 ====="
for d in /mnt/data/.b3 /mnt/data/.thp /mnt/data/.dsb2; do
  if [ -e "$d" ]; then du -sh "$d" 2>/dev/null | sed 's/^/    删前 /'; rm -rf "$d"; echo "    已删 $d"; fi
done
echo "    /mnt/data 顶层 .b3/.thp/.dsb2 残留? $(ls -d /mnt/data/.b3 /mnt/data/.thp /mnt/data/.dsb2 2>/dev/null | wc -l) 个"
echo
echo "===== 8. 保留项复查（这些「不动」，列出来供核对） ====="
echo "  -- 时区（判定：非性能调优，且宿主同为 CST，保留） --"
echo "     /etc/localtime -> $(readlink /etc/localtime)   /etc/timezone=$(cat /etc/timezone 2>/dev/null)"
echo "  -- 应用 PATH 链接（判定：PATH 管路非调优，保留） --"
for t in bun bunx claude codex claude-go claude-native; do
  [ -e "/usr/local/bin/$t" ] && printf "     %-14s -> %s\n" "$t" "$(readlink /usr/local/bin/$t)"
done
echo "  -- fstrim override（判定：待 §8 分析后裁决，暂留） --"
ls /etc/systemd/system/fstrim.timer.d/ /etc/systemd/system/fstrim.service.d/ 2>/dev/null | sed 's/^/     /'
echo
echo "===== 9. 还原后环境变量实况（非登录 run 路径） ====="
for v in CCACHE_DIR CCACHE_MAXSIZE TMPDIR CMAKE_GENERATOR CARGO_TARGET_DIR GOCACHE GOMODCACHE npm_config_cache BUN_INSTALL_CACHE_DIR; do
  eval "val=\$$v"; printf "    %-24s = %s\n" "$v" "${val:-<已清除>}"
done
echo "    PATH = $PATH"
echo "    which gcc = $(command -v gcc)"
echo
echo "===== 10. 残留调优复扫（§1 收尾核查） ====="
r=0
for f in /etc/profile.d/99-ds-path.sh /etc/security/limits.d/99-ds-dev.conf \
         /etc/systemd/system.conf.d/99-ds-dev.conf /etc/systemd/journald.conf.d/99-ds-dev.conf \
         /root/.ccache.conf /etc/ccache.conf; do
  [ -e "$f" ] && { echo "    ★残留 $f"; r=$((r+1)); }
done
echo "    bash.bashrc 标记: $(grep -c 'ds-dev-' /etc/bash.bashrc)"
echo "    /usr/local/bin 中指向 /usr/lib/ccache 的链接: $(ls -la /usr/local/bin/ | grep -c '/usr/lib/ccache')"
echo "    /etc/environment 含 CCACHE/TMPDIR/CARGO 行: $(grep -cE 'CCACHE|TMPDIR|CARGO|GOCACHE|npm_config|BUN_INSTALL|CMAKE' /etc/environment)"
echo "    ── 合计残留项: $r ──"
