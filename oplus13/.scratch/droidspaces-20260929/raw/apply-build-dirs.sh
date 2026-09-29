set -e
ENVF=/etc/environment
BK=/root/.ds-opt/environment.bak-pre-dsbuild

echo "===== 1. 备份当前 /etc/environment ====="
mkdir -p /root/.ds-opt
cp -a "$ENVF" "$BK"
echo "  备份 -> $BK  ($(wc -l < $BK) 行)"

echo
echo "===== 2. 幂等检查：是否已存在 DS_DEV_BUILD 块 ====="
if grep -q "DS_DEV_BUILD" "$ENVF"; then
  echo "  已存在，先移除旧块"
  sed -i '/# DS_DEV_BUILD/,/^TMPDIR=/d' "$ENVF"
fi

echo
echo "===== 3. 创建 /mnt/data/ds-build 目录树 ====="
for d in cargo-target go-build go-mod npm bun tmp; do
  mkdir -p "/mnt/data/ds-build/$d"
  printf '  %-42s %s\n' "/mnt/data/ds-build/$d" "$(stat -c '%U:%G %a' /mnt/data/ds-build/$d)"
done

echo
echo "===== 4. 追加环境变量块 ====="
cat >> "$ENVF" <<'ENVEOF'
# DS_DEV_BUILD —— 构建输出/缓存指向 /mnt/data（f2fs 直通）
# 依据：实测大文件写 f2fs 1169 MB/s vs rootfs ext4-on-loop 533 MB/s（快 2.19×）
# 还原：cp /root/.ds-opt/environment.bak-pre-dsbuild /etc/environment
CARGO_TARGET_DIR=/mnt/data/ds-build/cargo-target
GOCACHE=/mnt/data/ds-build/go-build
GOMODCACHE=/mnt/data/ds-build/go-mod
npm_config_cache=/mnt/data/ds-build/npm
BUN_INSTALL_CACHE_DIR=/mnt/data/ds-build/bun
TMPDIR=/tmp
ENVEOF
echo "  追加完成，当前文件："
sed 's/^/    /' "$ENVF"

echo
echo "===== 5. 可写性冒烟测试 ====="
for d in cargo-target go-build go-mod npm bun tmp; do
  f="/mnt/data/ds-build/$d/.wtest"
  if echo ok > "$f" 2>/dev/null && [ "$(cat $f)" = ok ]; then rm -f "$f"; echo "  $d 可写 ✓"; else echo "  $d 不可写 ✗"; fi
done
