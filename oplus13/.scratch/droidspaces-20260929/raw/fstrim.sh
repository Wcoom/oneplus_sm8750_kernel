#!/bin/sh
echo "===== fstrim 前 ====="
echo "[容器内 / 用量]"
df -h / | tail -1
echo "[容器内 / 的 discard 能力]"
lsblk -o NAME,ROTA,DISC-GRAN,DISC-MAX,MOUNTPOINT 2>/dev/null | grep -E "loop|NAME" || echo "  (lsblk 不可用)"
echo "[挂载选项]"
grep ' / ' /proc/mounts
echo
echo "===== 执行 fstrim -v / ====="
fstrim -v / 2>&1
echo "  rc=$?"
echo
echo "===== fstrim 后 ====="
df -h / | tail -1
echo
echo "===== 容器内 /mnt/data 状态（f2fs 已自动 discard，仅查看）====="
grep ' /mnt/data ' /proc/mounts
