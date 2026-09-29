#!/bin/sh
D=/data/local/Droidspaces/bin/droidspaces
CONF=/data/local/Droidspaces/Containers/ubuntu/container.config

echo "===== 0. 先抓失败线索（容器日志/README）====="
for p in /data/local/Droidspaces/logs /data/local/Droidspaces/Containers/ubuntu/logs \
         /data/local/Droidspaces/Containers/ubuntu; do
  [ -d "$p" ] && { echo "[$p]"; ls -la "$p" 2>/dev/null | head -12; }
done
echo "[dmesg 中的 cgroup 相关]"
dmesg 2>/dev/null | grep -iE "cgroup|droidspace" | tail -8

echo
echo "===== 1. 回滚 force_cgroupv1=1 -> 0 ====="
sed -i 's/^force_cgroupv1=1$/force_cgroupv1=0/' "$CONF"
grep -n force_cgroupv1 "$CONF"

echo
echo "===== 2. 重启容器 ====="
$D -C "$CONF" start 2>&1 | tail -6
sleep 8

echo
echo "===== 3. 健康检查 ====="
$D show 2>&1 | head -10
PID=$($D --name=ubuntu pid 2>&1 | tr -d '\r')
echo "  PID = $PID"
echo "  挂载: $(mount 2>/dev/null | grep -c droidspace) 条"

echo
echo "===== 4. 容器内自检（确认可执行）====="
$D --name=ubuntu run /bin/sh -c 'echo alive' 2>&1 | tail -3
