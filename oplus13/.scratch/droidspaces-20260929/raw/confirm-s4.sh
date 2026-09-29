echo "=== 1. 新 run 环境 ==="
echo "  env 里的 CCACHE*: $(env | grep '^CCACHE' | tr '\n' ' ')"
echo
echo "=== 2. 配置来源（此时环境已干净，max_size 应来自配置文件）==="
ccache --show-config 2>/dev/null | grep -E "^\(.*\) (cache_dir|max_size|compression|compression_level)" | sed 's/^/  /'
echo "  origin 统计:"; ccache --show-config 2>/dev/null | grep -oE '^\([^)]*\)' | sort | uniq -c | sed 's/^/    /'

echo
echo "=== 3. 决定性测试：ccache 直调，两次编译 ==="
cd /tmp && rm -rf ccf && mkdir ccf && cd ccf
printf '#include <stdio.h>\nint main(void){printf("ok\\n");return 0;}\n' > t.c
b=$(find /mnt/data/ds-build/ccache -type f | wc -l)
ccache -z >/dev/null 2>&1
ccache gcc -c -o t.o t.c
ccache gcc -c -o t.o t.c
ccache -s 2>/dev/null | grep -E "Cacheable calls|Hits:|Misses:|Compression" | sed 's/^/    /'
a=$(find /mnt/data/ds-build/ccache -type f | wc -l)
echo "    缓存文件数: $b -> $a"
cd /tmp && rm -rf ccf

echo
echo "=== 4. ccache 在裸 run 中缺席的根因 ==="
echo "  裸 run PATH : $PATH"
echo "  登录 PATH   : $(/bin/bash -lc 'echo $PATH')"
echo "  裸 run 下 gcc 实体 : $(ls -l $(command -v gcc) | awk '{print $NF}')"
echo "  登录下 gcc 实体    : $(/bin/bash -lc 'ls -l $(command -v gcc) | awk "{print \\$NF}"')"
echo "  /usr/local/bin 现有内容: $(ls /usr/local/bin 2>/dev/null | tr '\n' ' ' | cut -c1-200)"

echo
echo "=== 5. S6 旁证：宿主 Android 时区未受影响（容器内看不到，另行检查）==="
echo "  容器 /etc/localtime -> $(readlink /etc/localtime)"
