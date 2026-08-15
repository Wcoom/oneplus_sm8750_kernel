#!/system/bin/sh
# 99-sched-tune.sh — walt 参数开机应用/回退（KernelSU service.d）
# 定案项：zone_max_util_pct（目标 /data/adb/sched-tune，缺省 90；基线 /data/adb/sched-tune-baseline）
# 用法：service.d 自动执行（apply）；彻底回退出厂：sh 99-sched-tune.sh restore

MODE="${1:-apply}"

ZONE_TARGET=90
[ -f /data/adb/sched-tune ] && ZONE_TARGET="$(cat /data/adb/sched-tune 2>/dev/null)"

for p in /sys/devices/system/cpu/cpufreq/policy*/walt; do
	f="$p/zone_max_util_pct"
	[ -f "$f" ] || continue
	pol="$(basename "$(dirname "$p")")"
	case "$MODE" in
	restore)
		v="$(awk -v p="$pol" '$1 == p { sub(/^[^ ]+ /, ""); print }' /data/adb/sched-tune-baseline 2>/dev/null)"
		[ -n "$v" ] && printf '%s\n' "$v" > "$f" 2>/dev/null
		;;
	*)
		v="$(cat "$f" 2>/dev/null)"
		printf '%s\n' "$v" | awk -v t="$ZONE_TARGET" '{ for (i=2; i<=NF; i+=2) $i=t; print }' > "$f" 2>/dev/null
		;;
	esac
done
