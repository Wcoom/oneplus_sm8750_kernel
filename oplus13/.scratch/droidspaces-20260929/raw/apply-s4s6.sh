ENVF=/etc/environment
OLD=/root/.cache/ccache
NEW=/mnt/data/ds-build/ccache

echo "########## S4：ccache 迁移到 /mnt/data ##########"
echo "=== 1. 备份与迁移已有缓存 ==="
mkdir -p /root/.ds-opt
cp -a "$ENVF" /root/.ds-opt/environment.bak-pre-s4s6
echo "  /etc/environment 备份 -> /root/.ds-opt/environment.bak-pre-s4s6"

mkdir -p "$NEW"
if [ -d "$OLD" ] && [ -n "$(ls -A $OLD 2>/dev/null)" ]; then
  cp -a "$OLD/." "$NEW/"
  o=$(find "$OLD" -type f 2>/dev/null | wc -l)
  n=$(find "$NEW" -type f 2>/dev/null | wc -l)
  echo "  迁移条目: 旧 $o 文件 -> 新 $n 文件"
  [ "$o" = "$n" ] || { echo "  ✗ 条目数不一致，中止删除旧目录"; }
else
  o=0; n=0; echo "  旧缓存为空，无需迁移"
fi

echo
echo "=== 2. 写入缓存级配置文件（ccache 真正会读的位置）==="
cat > "$NEW/ccache.conf" <<'CONF'
# ccache 缓存级配置（$CCACHE_DIR/ccache.conf —— ccache 官方支持的位置）
# 注意：~/.ccache.conf 不被 ccache 读取（2026-09-30 实测证实），配置必须放这里
max_size = 20G
compression = true
compression_level = 6
CONF
echo "  已写: $NEW/ccache.conf"; sed 's/^/    /' "$NEW/ccache.conf"

echo
echo "=== 3. 更新 /etc/environment 的 CCACHE_DIR ==="
sed -i "s|^CCACHE_DIR=.*|CCACHE_DIR=$NEW|" "$ENVF"
grep -n "CCACHE" "$ENVF" | sed 's/^/  /'

echo
echo "=== 4. 停用从未生效的 /root/.ccache.conf ==="
if [ -f /root/.ccache.conf ]; then
  mv /root/.ccache.conf /root/.ds-opt/ccache.conf.DEAD-never-read-by-ccache
  echo "  已移走 -> /root/.ds-opt/ccache.conf.DEAD-never-read-by-ccache（留作证据）"
fi

echo
echo "=== 5. 条目数一致则删除旧缓存目录 ==="
if [ "$o" = "$n" ]; then
  rm -rf "$OLD"
  echo "  已删除 $OLD（内容已迁移；ccache 缓存可再生）"
  [ -e "$OLD" ] && echo "  ✗ 仍存在" || echo "  ✓ 确认已移除"
fi
echo "  rootfs 释放: $(df -h / | tail -1 | awk '{print $4}') 可用"

echo
echo "########## S6：时区设置 ##########"
echo "=== 1. 备份当前时区设置 ==="
cp -a /etc/localtime /root/.ds-opt/localtime.orig 2>/dev/null
echo "  原 /etc/localtime -> $(readlink /etc/localtime)"
[ -e /etc/timezone ] && cp -a /etc/timezone /root/.ds-opt/timezone.orig && echo "  原 /etc/timezone 已备份" || echo "  原 /etc/timezone 不存在（无需备份）"

echo
echo "=== 2. 设置 Asia/Shanghai ==="
ln -sf /usr/share/zoneinfo/Asia/Shanghai /etc/localtime
echo "Asia/Shanghai" > /etc/timezone
echo "  /etc/localtime -> $(readlink /etc/localtime)"
echo "  /etc/timezone   -> $(cat /etc/timezone)"

echo
echo "=== 3. 生效验证 ==="
echo "  date            : $(date)"
echo "  date -u         : $(date -u)"
echo "  UTC 与本地时差  : $(awk 'BEGIN{print (strftime("%z")==""?"n/a":"")}' 2>/dev/null; date +%z)"
echo "  timedatectl     : $(timedatectl 2>&1 | grep -E 'Time zone|Local time' | tr '\n' ' ')"
