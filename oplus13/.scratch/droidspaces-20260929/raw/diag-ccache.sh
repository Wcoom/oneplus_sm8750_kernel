echo "=== A. CCACHE_MAXSIZE 到底从哪来 ==="
echo "  -- 进程环境里的 CCACHE* --"
env | grep -i "^CCACHE" | sed 's/^/    /'
echo "  -- 文件系统搜索 /etc /root（排除缓存数据）--"
for f in $(grep -rl "CCACHE_MAXSIZE" /etc /root /usr/local 2>/dev/null | grep -v "^/root/.cache"); do
  echo "    [$f]"; grep -n "CCACHE_MAXSIZE" "$f" | sed 's/^/      /'
done

echo
echo "=== B. gcc 是否被 ccache 接管 ==="
echo "  PATH            : $PATH"
echo "  command -v gcc  : $(command -v gcc)"
echo "  gcc 实体        : $(ls -l $(command -v gcc) 2>&1)"
echo "  command -v ccache: $(command -v ccache)"
echo "  /usr/lib/ccache : $([ -d /usr/lib/ccache ] && echo "存在，$(ls /usr/lib/ccache | wc -l) 个链接" || echo '不存在')"
echo "  bash -lc 下 gcc : $(/bin/bash -lc 'command -v gcc' 2>&1)"

echo
echo "=== C. 三种调用方式各自的 ccache 命中情况 ==="
cd /tmp && rm -rf cct && mkdir cct && cd cct
printf '#include <stdio.h>\nint main(void){printf("hi\\n");return 0;}\n' > t.c
before=$(find /mnt/data/ds-build/ccache -type f | wc -l)
ccache -z >/dev/null 2>&1
echo "  -- 方式1: 裸 gcc --"
gcc -c -o t1.o t.c 2>&1; echo "     st1=$(ccache -s 2>/dev/null | grep -c 'Cacheable calls')"
ccache -s 2>/dev/null | head -8 | sed 's/^/     /'
echo "  -- 方式2: /usr/lib/ccache/gcc --"
[ -x /usr/lib/ccache/gcc ] && { /usr/lib/ccache/gcc -c -o t2.o t.c 2>&1; ccache -s 2>/dev/null | head -8 | sed 's/^/     /'; } || echo "     不存在"
echo "  -- 方式3: ccache gcc --"
ccache gcc -c -o t3.o t.c 2>&1; ccache -s 2>/dev/null | head -8 | sed 's/^/     /'
after=$(find /mnt/data/ds-build/ccache -type f | wc -l)
echo "  缓存文件数: $before -> $after"
cd /tmp && rm -rf cct
