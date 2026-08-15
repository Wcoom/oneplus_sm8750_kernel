#!/system/bin/sh
# 99-sched-tune.sh — walt 参数开机应用/回退（KernelSU service.d）
# 定案项：zone_max_util_pct（目标 /data/adb/sched-tune，缺省 90；基线 /data/adb/sched-tune-baseline）
# 浸泡项：hispeed_load（目标 /data/adb/sched-tune-hispeed，缺省 70；基线 /data/adb/sched-tune-hispeed-baseline）
# 用法：service.d 自动执行（apply）；手动回退：sh 99-sched-tune.sh restore

MODE="${1:-apply}"

ZONE_TARGET=90
[ -f /data/adb/sched-tune ] && ZONE_TARGET="$(cat /data/adb/sched-tune 2>/dev/null)"
HISPEED_TARGET=70
[ -f /data/adb/sched-tune-hispeed ] && HISPEED_TARGET="$(cat /data/adb/sched-tune-hispeed 2>/dev/null)"

for p in /sys/devices/system/cpu/cpufreq/policy*/walt; do
	pol="$(basename "$(dirname "$p")")"

	# zone_max_util_pct（定案项）
	f="$p/zone_max_util_pct"
	if [ -f "$f" ]; then
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
	fi

	# hispeed_load（浸泡项）
	g="$p/hispeed_load"
	if [ -f "$g" ]; then
		case "$MODE" in
		restore)
			v="$(awk -v p="$pol" '$1 == p { print $2 }' /data/adb/sched-tune-hispeed-baseline 2>/dev/null)"
			[ -n "$v" ] && echo "$v" > "$g" 2>/dev/null
			;;
		*)
			echo "${HISPEED_TARGET:-70}" > "$g" 2>/dev/null
			;;
		esac
	fi
done
