echo "===== S4-1. ccache 配置来源（括号内为来源标记）====="
ccache --show-config 2>/dev/null | grep -E "cache_dir|max_size|compression|hard_link|file_clone" | sed 's/^/  /'
echo
echo "  --- 完整来源分布（统计各来源条目数）---"
ccache --show-config 2>/dev/null | grep -oE '^\([^)]*\)' | sort | uniq -c | sort -rn | sed 's/^/    /'
echo
echo "  --- /root/.ccache.conf 的内容与是否被引用 ---"
if [ -f /root/.ccache.conf ]; then echo "    文件存在:"; sed 's/^/      /' /root/.ccache.conf; else echo "    文件不存在"; fi
echo "    ccache 会读的候选路径是否可访问："
for p in /etc/ccache.conf /root/.config/ccache/ccache.conf /root/.ccache.conf "$CCACHE_DIR/ccache.conf"; do
  if [ -f "$p" ]; then echo "      [有] $p"; else echo "      [无] $p"; fi
done
echo "    （ccache 官方：$CCACHE_DIR/ccache.conf 为缓存级配置，/etc/ccache.conf 为系统级；~/.ccache.conf 不在列表）"
echo
echo "===== S4-2. ccache 现状 ====="
echo "  cache_dir = $CCACHE_DIR"
du -sh /root/.cache/ccache 2>/dev/null | sed 's/^/  占用: /'
ccache --show-stats 2>/dev/null | grep -E "Cache size|Max cache size|Hits|Misses|Cacheable" | sed 's/^/  /'

echo
echo "===== S6-1. 时区现状 ====="
echo "  /etc/timezone: $(cat /etc/timezone 2>/dev/null || echo '<不存在>')"
echo "  /etc/localtime: $(ls -l /etc/localtime 2>/dev/null | sed 's/.*-> //')  ($( [ -L /etc/localtime ] && echo 符号链接 || echo 普通文件 ))"
echo "  TZ 环境变量: ${TZ:-<未设置>}"
echo "  date 输出: $(date)"
echo "  date -u:   $(date -u)"
echo
echo "===== S6-2. 时区数据可用性 ====="
for z in Asia/Shanghai Etc/UTC; do
  p="/usr/share/zoneinfo/$z"
  if [ -f "$p" ]; then echo "  [有] $p  ($(stat -c %s $p) 字节)"; else echo "  [无] $p  ← 需装 tzdata"; fi
done
echo "  tzdata 包: $(dpkg -l tzdata 2>/dev/null | awk '/^ii/{print $2" "$3}')"
echo "  timedatectl: $(timedatectl 2>&1 | head -4 | tr '\n' ' | ')"

echo
echo "===== 时间命名空间（确认改时区只影响显示，不影响时钟）====="
echo "  time ns: $(readlink /proc/self/ns/time 2>/dev/null)  ← 与宿主对比: $(readlink /proc/1/ns/time 2>/dev/null)"
