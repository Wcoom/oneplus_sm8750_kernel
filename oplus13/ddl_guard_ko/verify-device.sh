#!/system/bin/sh
# verify-device.sh - 驱动 droidspaces 容器启停，观察 ddl_guard 的状态迁移
#
# 用法: sh verify-device.sh [循环次数，默认 2]
# 前置: 先跑 install.sh 把模块加载好（dry 或 real 都行）
#
# 判定标准：
#   dry  模式：state 的 ns 应随容器 1↔0 迁移，而 ddl / DDL 节点全程不变（未被写入）
#   real 模式：容器运行中 ns>=1 且 DDL=0；停止后 linger_ms 到期 ns=0 且 DDL=1
# 附加检查：容器运行中多次采样，ns 必须保持 >=1（证明 put_pid_ns 兜底没有误摘）

D=/data/local/Droidspaces/bin/droidspaces
NAME=ubuntu
CONF=/data/local/Droidspaces/Containers/$NAME/container.config
CYCLES="${1:-2}"
STATE=/sys/module/ddl_guard/parameters/state
DDL=/proc/oplus_scheduler/sched_assist/sched_ddl_enabled

[ -d /sys/module/ddl_guard ] || { echo "ddl_guard 未加载，先跑 install.sh"; exit 1; }

getpid() { "$D" --name=$NAME pid 2>/dev/null | tr -d '\r\n' | grep -oE '[0-9]+' | head -1; }
snap() { printf '    状态: %s\n' "$(cat $STATE 2>/dev/null | tr -d '\r\n')"; printf '    DDL : %s\n' "$(cat $DDL 2>/dev/null | tr -d '\r\n')"; }

echo "===== 起始状态 ====="
snap
printf '  容器 PID（应为空）: '; getpid; echo

i=1
while [ $i -le $CYCLES ]; do
	echo
	echo "########## 第 $i/$CYCLES 轮 ##########"
	echo "--- 启动容器 ---"
	"$D" --name=$NAME -C "$CONF" start > /data/local/tmp/ddlg-start-$i.log 2>&1 &
	n=0; P=""
	while [ $n -lt 40 ]; do
		P=$(getpid); [ -n "$P" ] && break
		sleep 2; n=$((n+1))
	done
	echo "  容器 PID='$P'（等待 $((n*2))s）"

	sleep 3
	echo "  [运行中 +3s]"; snap
	sleep 6
	echo "  [运行中 +9s，复查 put_pid_ns 兜底]"; snap

	echo "--- 停止容器 ---"
	"$D" --name=$NAME stop 2>&1 | tail -3
	n=0
	while [ $n -lt 30 ]; do
		P=$(getpid); [ -z "$P" ] && break
		sleep 2; n=$((n+1))
	done
	echo "  已停止（等待 $((n*2))s）"
	printf '  [停止瞬间] 状态: '; cat $STATE | tr -d '\r\n'; echo
	sleep 8
	echo "  [停止后 +8s，linger 应已到期]"; snap

	i=$((i+1))
done

echo
echo "===== 终态 ====="
snap
printf '  容器 PID（应为空）: '; getpid; echo
echo "===== dmesg（ddl_guard）====="
dmesg | grep -a ddl_guard | tail -30
echo
echo "===== 残余探针内核侧核对（模块自己注册的探针不在这里）====="
printf '  kprobe_events 行数（应为 0）: '; grep -c . /sys/kernel/tracing/kprobe_events 2>/dev/null
echo "VERIFY_DONE"
