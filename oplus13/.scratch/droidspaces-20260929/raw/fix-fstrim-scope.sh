echo "=== 容器内可见的挂载点（fstrim 默认会遍历这些）==="
findmnt -rno TARGET,SOURCE,FSTYPE 2>/dev/null | grep -vE "^(/proc|/sys|/dev|/run)" | sed 's/^/  /'

echo
echo "=== 收紧 ExecStart：只 TRIM 容器 rootfs，不碰宿主挂载 ==="
cat > /etc/systemd/system/fstrim.service.d/override.conf <<'EOF'
[Unit]
ConditionVirtualization=

[Service]
# 2026-09-30：Ubuntu 默认的
#   ExecStart=/sbin/fstrim --listed-in /etc/fstab:/proc/self/mountinfo
# 会遍历「所有」挂载点。本容器 enable_android_storage=1 把宿主 /data 及其子分区
# 挂进了容器，实测该默认行为会对宿主分区发 TRIM（journal: /dev/block/dm-61、
# /dev/block/sdf3）。容器内的定时任务不应外溢到宿主存储，故改为只 TRIM 容器
# 自己的 rootfs。宿主分区由 Android 自身的维护机制负责。
ExecStart=
ExecStart=/usr/sbin/fstrim --verbose /
EOF
systemctl daemon-reload
echo "  已写入："; sed 's/^/    /' /etc/systemd/system/fstrim.service.d/override.conf

echo
echo "=== 再实测一次（确认只 TRIM 容器 rootfs）==="
systemctl start fstrim.service
echo "  start 退出码=$?"
echo "  journal:"; journalctl -u fstrim.service -n 8 --no-pager 2>&1 | tail -8 | sed 's/^/    /'
echo
echo "=== timer 仍正常 ==="
systemctl status fstrim.timer --no-pager 2>&1 | grep -E "Active:|Trigger:" | sed 's/^/  /'
echo
echo "=== 容器内 rootfs 用量 ==="
df -h / | tail -1 | sed 's/^/  /'
