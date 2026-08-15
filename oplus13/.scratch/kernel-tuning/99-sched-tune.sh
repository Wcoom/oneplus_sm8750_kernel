#!/system/bin/sh
# 99-sched-tune.sh — walt zone_max_util_pct 开机应用/回退（KernelSU service.d）
# 目标值：/data/adb/sched-tune（缺省 90）；基线：/data/adb/sched-tune-baseline
# 用法：service.d 自动执行（apply）；手动回退：sh 99-sched-tune.sh restore

MODE="${1:-apply}"
TARGET=90
TARGET_FILE=/data/adb/sched-tune
BASELINE_FILE=/data/adb/sched-tune-baseline

[ -f "$TARGET_FILE" ] && TARGET="$(cat "$TARGET_FILE" 2>/dev/null)"

for p in /sys/devices/system/cpu/cpufreq/policy*/walt; do
	f="$p/zone_max_util_pct"
	[ -f "$f" ] || continue
	case "$MODE" in
	restore)
		v="$(awk -v pol="$(basename "$(dirname "$p")")" '$1 == pol { sub(/^[^ ]+ /, ""); print }' "$BASELINE_FILE" 2>/dev/null)"
		[ -n "$v" ] && printf '%s\n' "$v" > "$f" 2>/dev/null
		;;
	*)
		v="$(cat "$f" 2>/dev/null)"
		printf '%s\n' "$v" | awk -v t="$TARGET" '{ for (i=2; i<=NF; i+=2) $i=t; print }' > "$f" 2>/dev/null
		;;
	esac
done
