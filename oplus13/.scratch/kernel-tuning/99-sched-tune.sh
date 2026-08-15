#!/system/bin/sh
# 99-sched-tune.sh — schedutil rate_limit_us 开机应用/回退（KernelSU service.d）
# 目标值：/data/adb/sched-tune（缺省 0）；基线：/data/adb/sched-tune-baseline
# 用法：service.d 自动执行（apply）；手动回退：sh 99-sched-tune.sh restore

MODE="${1:-apply}"
TARGET_FILE=/data/adb/sched-tune
BASELINE_FILE=/data/adb/sched-tune-baseline

for p in /sys/devices/system/cpu/cpufreq/policy*; do
	[ -f "$p/schedutil/rate_limit_us" ] || continue
	case "$MODE" in
	restore)
		v="$(awk -v p="$p" '$1 == p { print $2 }' "$BASELINE_FILE" 2>/dev/null)"
		[ -n "$v" ] && echo "$v" > "$p/schedutil/rate_limit_us" 2>/dev/null
		;;
	*)
		v=0
		[ -f "$TARGET_FILE" ] && v="$(cat "$TARGET_FILE" 2>/dev/null)"
		echo "${v:-0}" > "$p/schedutil/rate_limit_us" 2>/dev/null
		;;
	esac
done
