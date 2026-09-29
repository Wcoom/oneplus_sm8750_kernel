#!/bin/sh
D=/data/local/Droidspaces/bin/droidspaces
echo "===== droidspaces show ====="
$D show 2>&1 | head -20
echo
echo "===== droidspaces pid ====="
$D --name=ubuntu pid 2>&1 | head -3
echo
echo "===== droidspaces info ====="
$D --name=ubuntu info 2>&1 | head -30
echo
echo "===== ps 找容器进程 ====="
ps -eo pid,ppid,comm 2>/dev/null | grep -iE "droidspace|systemd" | head -10
echo
echo "===== 挂载点 ====="
mount 2>/dev/null | grep -i droidspace | head -5
echo
echo "===== 配置现值 ====="
grep force_cgroupv1 /data/local/Droidspaces/Containers/ubuntu/container.config
