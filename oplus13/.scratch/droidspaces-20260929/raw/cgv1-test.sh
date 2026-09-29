#!/bin/sh
# 在 Android 宿主侧运行（非容器内）
D=/data/local/Droidspaces/bin/droidspaces
CONF=/data/local/Droidspaces/Containers/ubuntu/container.config
BK=/data/local/Droidspaces/Containers/ubuntu/container.config.bak-cgv1

echo "===== 1. 备份配置 ====="
cp -a "$CONF" "$BK" && echo "  已备份 -> $BK"
ls -l "$BK"

echo
echo "===== 2. 停止容器 ====="
$D --name=ubuntu stop 2>&1 | tail -5
sleep 2
echo "  剩余进程: $(pgrep -f 'Containers/ubuntu' | wc -l)"

echo
echo "===== 3. 改 force_cgroupv1=0 -> 1 ====="
sed -i 's/^force_cgroupv1=0$/force_cgroupv1=1/' "$CONF"
grep -n 'force_cgroupv1' "$CONF"

echo
echo "===== 4. 启动容器 ====="
$D -C "$CONF" start 2>&1 | tail -8
sleep 6
PID=$($D pid --name=ubuntu 2>/dev/null || pgrep -f 'Containers/ubuntu' | head -1)
echo "  容器 PID = $PID"
if [ -z "$PID" ]; then
  echo "  !! 容器未起来"
else
  echo "  进程存活检查: $(ls -d /proc/$PID >/dev/null 2>&1 && echo OK || echo DEAD)"
fi

echo
echo "===== 5. 起容器后 dmesg 尾部（看有无报错）====="
dmesg 2>/dev/null | tail -15
