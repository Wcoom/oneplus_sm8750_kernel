########## S7：ccache shim 链入 /usr/local/bin ##########
echo "########## S7 ##########"
echo "=== 执行前 ==="
echo "  command -v gcc = $(command -v gcc)"
mkdir -p /root/.ds-opt
ls -1 /usr/lib/ccache/ > /root/.ds-opt/ccache-shims.list
n=0
while read -r s; do
  [ -n "$s" ] || continue
  ln -sfn "/usr/lib/ccache/$s" "/usr/local/bin/$s" && n=$((n+1))
done < /root/.ds-opt/ccache-shims.list
echo "  已建 shim: $n 个（清单存 /root/.ds-opt/ccache-shims.list）"
echo "=== 原有工具未受影响？ ==="
for t in bun bunx claude claude-go claude-native codex; do
  printf '  %-14s -> %s\n' "$t" "$(readlink /usr/local/bin/$t 2>&1)"
done

echo
echo "=== 验证：裸 run 下解析 ==="
echo "  gcc   = $(command -v gcc)   | $(gcc --version 2>&1 | head -1)"
echo "  cc    = $(command -v cc)    | $(cc --version 2>&1 | head -1)"
echo "  clang = $(command -v clang) | $(clang --version 2>&1 | head -1)"

echo
echo "=== 决定性测试：裸 gcc 是否被 ccache 接管 ==="
cd /tmp && rm -rf s7t && mkdir s7t && cd s7t
printf '#include <stdio.h>\nint main(void){printf("s7-ok\\n");return 0;}\n' > t.c
ccache -z >/dev/null 2>&1
gcc -c -o t.o t.c
gcc -c -o t.o t.c
ccache -s 2>/dev/null | grep -E "Cacheable calls|Hits:|Misses:" | sort -u | sed 's/^/  /'
echo "  运行验证: $(gcc t.c -o t && ./t)"
cd /tmp && rm -rf s7t

echo
echo "########## S3 ##########"
echo "=== 执行前 timer ==="
systemctl status fstrim.timer --no-pager 2>&1 | grep -E "Active:|Condition:" | sed 's/^/  /'
mkdir -p /etc/systemd/system/fstrim.timer.d /etc/systemd/system/fstrim.service.d
cat > /etc/systemd/system/fstrim.timer.d/override.conf <<'EOF'
# 2026-09-30：systemd 原生带 ConditionVirtualization=!container，本容器内每周被
# 静默跳过（journal 实测 "skipped, unmet condition"）。rootfs 为 ext4-on-loop-on-f2fs
# 且未挂 discard，必须靠定期 TRIM 回收稀疏镜像膨胀（上次手动 trim 回收约 9.8 GiB）。
[Unit]
ConditionVirtualization=
EOF
printf '[Unit]\nConditionVirtualization=\n' > /etc/systemd/system/fstrim.service.d/override.conf
systemctl daemon-reload
systemctl restart fstrim.timer
echo "=== 执行后 timer ==="
systemctl status fstrim.timer --no-pager 2>&1 | grep -E "Active:|Trigger:|Condition:" | sed 's/^/  /'
systemctl list-timers fstrim.timer --no-pager 2>&1 | head -3 | sed 's/^/  /'

echo
echo "=== 实测触发一次 ==="
t0=$(date +%s)
systemctl start fstrim.service
rc=$?
echo "  start 退出码=$rc  耗时 $(( $(date +%s) - t0 ))s"
systemctl status fstrim.service --no-pager 2>&1 | head -7 | sed 's/^/    /'
echo "  journal:"; journalctl -u fstrim.service -n 12 --no-pager 2>&1 | tail -12 | sed 's/^/    /'

echo
echo "########## S5 ##########"
KEEP=0.158.0-aarch64-unknown-linux-musl
RD=/root/.codex/packages/standalone/releases
CUR=$(basename "$(readlink /root/.codex/packages/standalone/current)")
echo "=== 安全检查 ==="
echo "  current 指向: $CUR   保留: $KEEP"
[ "$CUR" = "$KEEP" ] || { echo "  ✗ current 不符，中止"; exit 1; }
case "$(readlink -f "$(command -v codex)")" in *"$KEEP"*) echo "  ✓ codex 可执行位于保留版本内";; *) echo "  ✗ 中止"; exit 1;; esac
echo "  ✓ 运行中的 codex 进程: $(ps -eo cmd | grep -c '[c]odex') 个"
echo "=== 执行前 ==="
echo "  /root/.codex = $(du -sh /root/.codex 2>/dev/null | cut -f1)   根分区可用 = $(df -h / | tail -1 | awk '{print $4}')"
echo "=== 删除非 current 版本 ==="
for d in "$RD"/*/; do
  v=$(basename "$d")
  [ "$v" = "$KEEP" ] && { echo "  保留 $v"; continue; }
  sz=$(du -sh "$d" | cut -f1); rm -rf "$d" && echo "  已删 $v ($sz)"
done
echo "=== 执行后 ==="
echo "  releases 剩余: $(ls -1 $RD | tr '\n' ' ')"
echo "  current 完好 : $([ -L /root/.codex/packages/standalone/current ] && echo 是 || echo 否)"
echo "  codex 可运行 : $(codex --version 2>&1 | head -1)"
echo "  /root/.codex = $(du -sh /root/.codex 2>/dev/null | cut -f1)   根分区可用 = $(df -h / | tail -1 | awk '{print $4}')"
