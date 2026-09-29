#!/bin/sh
# 补 ccache 到交互式 shell + ccache 收益 A/B 实测
B=/mnt/data/.ccbench
mkdir -p $B
ms() { date +%s%3N; }

# ---- 1. /etc/bash.bashrc 补 ccache（交互式非登录 shell 也能命中）----
if ! grep -q "ds-dev-ccache" /etc/bash.bashrc 2>/dev/null; then
  cat >> /etc/bash.bashrc <<'EOF'

# ds-dev-ccache 编译器缓存前置（回滚：删除本段）
if [ -d /usr/lib/ccache ]; then
  case ":$PATH:" in
    *":/usr/lib/ccache:"*) ;;
    *) PATH="/usr/lib/ccache:$PATH"; export PATH ;;
  esac
fi
EOF
  echo "[bash.bashrc] 已补 ds-dev-ccache 段"
else
  echo "[bash.bashrc] ds-dev-ccache 已存在"
fi

# ---- 2. 生成一个中等规模 C 文件（模拟真实 TU）----
cat > $B/gen.py <<'PYEOF'
import sys
n = int(sys.argv[1])
out = ['#include <stdio.h>', '#include <math.h>', '#include <string.h>']
for i in range(n):
    out.append(f"static double f{i}(double x) {{ return sin(x*{i+1}) + cos(x/({i+1}.0)) * {i%7+1}; }}")
out.append("double total(double x){ double s=0;")
for i in range(n):
    out.append(f"  s += f{i}(x);")
out.append("  return s; }")
out.append("int main(void){ printf(\"%f\\n\", total(1.5)); return 0; }")
open(sys.argv[2],'w').write("\n".join(out))
PYEOF
python3 $B/gen.py 800 $B/big.c
echo "[生成] big.c 行数=$(wc -l < $B/big.c) 大小=$(wc -c < $B/big.c) 字节"

# ---- 3. 基线：直接用 /usr/bin/gcc（不经 ccache）----
echo
echo "===== 基线：/usr/bin/gcc 直调（无缓存）====="
i=0
while [ $i -lt 3 ]; do
  S=$(ms); /usr/bin/gcc -O2 -c -o $B/direct.o $B/big.c; E=$(ms)
  echo "  gcc_direct_run$((i+1))_ms=$((E-S))"
  i=$((i+1))
done

# ---- 4. ccache：显式调用 ccache gcc，冷缓存 → 热缓存 ----
echo
echo "===== ccache：冷缓存 → 热缓存 ====="
ccache -C >/dev/null 2>&1
ccache -z >/dev/null 2>&1
S=$(ms); ccache gcc -O2 -c -o $B/cc1.o $B/big.c; E=$(ms)
echo "  ccache_cold_miss_ms=$((E-S))"
i=0
while [ $i -lt 3 ]; do
  S=$(ms); ccache gcc -O2 -c -o $B/cc2.o $B/big.c; E=$(ms)
  echo "  ccache_hit_run$((i+1))_ms=$((E-S))"
  i=$((i+1))
done
echo "  --- ccache -s ---"
ccache -s 2>/dev/null | grep -iE "cacheable|cache hit|cache miss|hit rate|local storage|cache size" | sed 's/^/    /'

# ---- 5. 验证产物一致（ccache 不能改变语义）----
echo
echo "===== 产物一致性 ====="
if cmp -s $B/direct.o $B/cc1.o; then echo "  OK  direct.o 与 ccache 产物逐字节一致"; else echo "  !! 产物不一致（可能是 __TIME__ 类宏或构建路径差异）"; fi

# ---- 6. PATH 前置后 gcc 实际解析 ----
echo
echo "===== PATH 解析 ====="
echo "  登录 shell gcc = $(/bin/bash -lc 'command -v gcc')"
echo "  登录 shell ccache = $(/bin/bash -lc 'command -v ccache')"
echo "  裸 PATH gcc = $(command -v gcc)（预期 /usr/bin/gcc，bench 用此路径，不影响 before/after 对比）"

rm -rf $B
echo "=== 完成 ==="
