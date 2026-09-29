ENVF=/etc/environment
NEW=/mnt/data/ds-build/ccache

echo "########## S4 验证 ##########"
echo "=== 1. 移除 CCACHE_MAXSIZE（让配置文件成为唯一事实源）==="
sed -i 's|^CCACHE_MAXSIZE=20G$|# CCACHE_MAXSIZE 已移入 $CCACHE_DIR/ccache.conf（2026-09-30）：环境变量优先级高于缓存配置，留着会遮蔽配置文件|' "$ENVF"
grep -n "CCACHE" "$ENVF" | sed 's/^/  /'

echo
echo "=== 2. 裸 run 路径下的环境变量 ==="
echo "  CCACHE_DIR=${CCACHE_DIR:-<未设置>}"

echo
echo "=== 3. 配置来源（origin 标记）==="
ccache --show-config 2>/dev/null | grep -E "^\(.*\) (cache_dir|max_size|compression|compression_level)" | sed 's/^/  /'
echo "  --- origin 统计 ---"
ccache --show-config 2>/dev/null | grep -oE '^\([^)]*\)' | sort | uniq -c | sed 's/^/  /'

echo
echo "=== 4. 决定性测试：真实编译两次 ==="
cd /tmp && rm -rf ccachetest && mkdir ccachetest && cd ccachetest
printf '#include <stdio.h>\nint main(void){printf("hi\\n");return 0;}\n' > t.c
ccache -z >/dev/null 2>&1
echo "  第1次编译(应 miss): $(gcc -c -o t.o t.c 2>&1)"
echo "  第2次编译(应 hit) : $(gcc -c -o t.o t.c 2>&1)"
echo "  统计:"
ccache -s 2>/dev/null | grep -E "Hits|Misses|Cache size|Cache directory|Compression" | sed 's/^/    /'
echo "  新目录文件数 : $(find $NEW -type f 2>/dev/null | wc -l)  (含 ccache.conf)"
echo "  旧目录       : $([ -e /root/.cache/ccache ] && echo '仍存在(异常)' || echo '已移除(正常)')"
cd /tmp && rm -rf ccachetest

echo
echo "########## S6 验证 ##########"
echo "  全新进程读到的时区："
echo "    date         : $(date)"
echo "    date +%Z%z   : $(date +%Z%z)"
echo "    date -u      : $(date -u +%Z%z)"
echo "    /etc/localtime -> $(readlink /etc/localtime)"
echo "    /etc/timezone  : $(cat /etc/timezone)"
echo "    TZ 环境变量    : ${TZ:-<未设置>}"
echo "    python tzname  : $(python3 -c 'import time;print(time.tzname)' 2>&1)"
echo "    c 库 localtime : $(date +%s | xargs -I{} date -d @{} +%Z%z 2>/dev/null)"
echo "    timedatectl    : $(timedatectl 2>&1 | grep -E 'Time zone' | tr -s ' ')"
echo "    时间命名空间   : $(readlink /proc/1/ns/time) @PID1"
