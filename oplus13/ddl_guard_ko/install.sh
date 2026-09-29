#!/system/bin/sh
# install.sh - 设备端加载 ddl_guard.ko（需 root）
#
# 用法: sh install.sh [ko路径] [mode]
#   ko路径  默认 /data/local/tmp/ddl_guard.ko
#   mode    dry（默认，只记录不写开关）| real（真的动态开关 DDL）
#
# 注意：real 模式下、且当前无容器时，模块会把 DDL 打开（= 厂商默认运行态）。
#       想先看行为再用 real，直接省略第二个参数。

KO="${1:-/data/local/tmp/ddl_guard.ko}"
MODE="${2:-dry}"

case "$MODE" in
	dry)  DRY=1 ;;
	real) DRY=0 ;;
	*) echo "mode 只能是 dry 或 real（收到 '$MODE'）"; exit 2 ;;
esac

[ -f "$KO" ] || { echo "找不到 $KO"; exit 1; }

echo "=== 0. 前置检查 ==="
printf 'uname -r      : '; uname -r
printf 'DDL 当前值    : '; cat /proc/oplus_scheduler/sched_assist/sched_ddl_enabled 2>&1
if grep -qw global_sched_ddl_enabled /proc/kallsyms 2>/dev/null; then
	echo "符号          : global_sched_ddl_enabled 在 kallsyms 中"
else
	echo "符号          : 未在 /proc/kallsyms 找到（厂商模块可能未加载 → insmod 会失败）"
fi
if [ -d /sys/module/ddl_guard ]; then
	echo "残留模块      : 先卸载"
	rmmod ddl_guard || { echo "rmmod 失败"; exit 1; }
fi

echo
echo "=== 1. insmod $KO dry_run=$DRY ==="
insmod "$KO" dry_run=$DRY
rc=$?
if [ $rc -ne 0 ]; then
	echo "insmod 失败 rc=$rc"
	echo "（模块未加载 = DDL 保持当前值不变，这是设计上的安全侧）"
	dmesg | tail -5
	exit $rc
fi

sleep 1
echo
echo "=== 2. 加载后状态 ==="
printf 'state : '; cat /sys/module/ddl_guard/parameters/state 2>&1
printf 'DDL   : '; cat /proc/oplus_scheduler/sched_assist/sched_ddl_enabled 2>&1
echo "--- dmesg（ddl_guard）---"
dmesg | grep -a ddl_guard | tail -10
echo
echo "MODE=$MODE 就绪。下一步可用 verify-device.sh 驱动容器启停观察状态迁移。"
