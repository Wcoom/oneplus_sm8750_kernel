echo "===== A. 全新进程能否看到变量（证明 /etc/environment 注入）====="
for v in CARGO_TARGET_DIR GOCACHE GOMODCACHE npm_config_cache BUN_INSTALL_CACHE_DIR TMPDIR; do
  eval "val=\$$v"; printf '  %-22s = %s\n' "$v" "${val:-<未注入>}"
done

echo
echo "===== B. 各工具实际采纳情况 ====="
echo "-- go（go env 直接读环境变量）--"
go env GOCACHE GOMODCACHE TMPDIR 2>/dev/null | sed 's/^/    /'
echo "-- npm --"
echo "    cache = $(npm config get cache 2>/dev/null)"
echo "-- cargo（真实编译，看 target 落在哪）--"
cd /tmp && rm -rf cargotest && cargo new --bin --vcs none cargotest -q 2>&1 | tail -2
cd /tmp/cargotest && cargo build -q 2>&1 | tail -3
if [ -d /mnt/data/ds-build/cargo-target/debug ]; then
  echo "    ✓ target 落在 /mnt/data/ds-build/cargo-target/debug"
  ls /mnt/data/ds-build/cargo-target/debug/ | head -5 | sed 's/^/        /'
else
  echo "    ✗ target 未落在预期位置；本地 target 目录: $(ls -d /tmp/cargotest/target 2>/dev/null || echo 无)"
fi
echo "-- bun（建临时工程后查缓存路径）--"
mkdir -p /tmp/buntest && cd /tmp/buntest && echo '{"name":"t","version":"1.0.0"}' > package.json
echo "    bun pm cache = $(bun pm cache 2>&1 | head -1)"

echo
echo "===== C. 旧默认目录是否仍在被使用（应为空/未增长）====="
for d in /root/.cache/go-build /root/go/pkg/mod /root/.npm /root/.bun/install/cache; do
  if [ -e "$d" ]; then printf '    %-30s %s\n' "$d" "$(du -sh $d 2>/dev/null | cut -f1)";

  else printf '    %-30s %s\n' "$d" "<不存在>"; fi
done

echo
echo "===== D. 登录 shell 路径是否同样可见 ====="
/bin/bash -lc 'echo "    TMPDIR=$TMPDIR  CARGO_TARGET_DIR=$CARGO_TARGET_DIR  GOCACHE=$GOCACHE"'

echo
echo "===== E. 新目录占用 ====="
du -sh /mnt/data/ds-build 2>/dev/null | sed 's/^/    /'
df -h /mnt/data | tail -1 | sed 's/^/    /'
