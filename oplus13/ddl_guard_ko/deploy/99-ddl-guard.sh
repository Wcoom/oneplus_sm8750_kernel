#!/system/bin/sh
#
# ddl_guard 开机自启（KernelSU service.d；root）
#
# 按优先级自动服务两种形态：
#
#   A) 内置（CONFIG_DDL_GUARD=y，**当前部署形态**）
#      守护代码在 vmlinux 里，开机自动注册探针，无需 insmod。厂商模块由用户态
#      init 从 vendor_boot ramdisk 加载，晚于所有 initcall，所以内置守护要等它
#      出现后按名字解析 global_sched_ddl_enabled（最多重试 60 次 × 2s）。
#      因此本脚本退化为**看门狗**：确认解析成功；若守护最终没拿到符号，就把 DDL
#      写 0，退回旧的恒关行为（宁可少一层调度优化，也不要带着越界写崩溃风险跑）。
#
#   B) 可加载（ddl_guard.ko，开发/回退形态）
#      先写 0 兜底，再 insmod 交给它按容器生命周期动态管理；加载失败保持 0。
#
# 判定依据是 /sys/module/ddl_guard/parameters/state 里的 resolved=/tries= 字段：
#   resolved=1       已拿到厂商符号，守护生效
#   tries>60         重试耗尽、已放弃（本脚本随即写 0 兜底）
#
# ⚠️ 绝对不要把本脚本和 op_mods/deploy/99-oplus-sched-ddl-guard.sh（开机写 0 的旧
#    保护脚本）同时部署：守护只在容器事件/参数变更时重估，旧脚本开机写下的 0 会一直
#    留到下一次容器停止才被纠正。恒关请用 `policy=0`，不要用"再写一个 0"的方式。
#
# 依赖文件：
#   /data/adb/ddl_guard/ddl_guard.ko   .ko 形态才需要（内置形态可不存在）
#   /data/adb/ddl_guard/boot.log       本脚本日志
#
# 调试用环境变量：DDLG_FORCE_KO=1 跳过内置分支，强制走 .ko 分支

DDL_NODE=/proc/oplus_scheduler/sched_assist/sched_ddl_enabled
PARAM=/sys/module/ddl_guard/parameters
STATE=$PARAM/state
KO=/data/adb/ddl_guard/ddl_guard.ko
LOG=/data/adb/ddl_guard/boot.log

# .ko 形态专用：1 = 容器作用域（有容器才关）；0 = 恒关（只关不开）
POLICY=1
# .ko 形态专用：容器停止后延迟多少毫秒恢复 DDL
LINGER_MS=3000
# 内置形态：等守护解析厂商符号的最长时间（守护自己重试 60×2s，这里给足余量）
WAIT_S=170

log() {
	printf '[%s] %s\n' "$(date '+%m-%d %H:%M:%S')" "$*" >>"$LOG" 2>/dev/null
}

# 从单行 state 输出里取 key= 的值（Android 的 mksh 没有可靠的分词，手写解析）
field() {
	for kv in $(cat "$STATE" 2>/dev/null); do
		case "$kv" in
		"$1"=*) printf '%s' "${kv#*=}"; return 0 ;;
		esac
	done
	printf ''
}

# 把 DDL 写 0（安全缺省）。节点最多等 90 秒：厂商模块初始化后它才出现。
# 返回 0 表示已确认为 0。
write_zero() {
	i=0
	while [ "$i" -lt 90 ]; do
		if [ -w "$DDL_NODE" ]; then
			[ "$(cat "$DDL_NODE" 2>/dev/null)" != "0" ] &&
				printf '0\n' >"$DDL_NODE" 2>/dev/null
			[ "$(cat "$DDL_NODE" 2>/dev/null)" = "0" ] && return 0
		fi
		i=$((i + 1))
		sleep 1
	done
	return 1
}

log "---- 启动（内置看门狗 + .ko 回退；DDLG_FORCE_KO=${DDLG_FORCE_KO:-}）----"

# ---------- 形态 A：内置守护 ----------
if [ -z "$DDLG_FORCE_KO" ] && [ -r "$STATE" ]; then
	log "检测到内置 ddl_guard：$(cat "$STATE" 2>/dev/null)"

	waited=0
	while [ "$waited" -lt "$WAIT_S" ]; do
		[ "$(field resolved)" = "1" ] && break
		# tries 超过上限 = 守护已放弃重试，不必再等
		tr=$(field tries)
		if [ -n "$tr" ] && [ "$tr" -gt 60 ] 2>/dev/null; then
			log "守护已放弃重试（tries=$tr），不再等待"
			break
		fi
		sleep 5
		waited=$((waited + 5))
	done

	if [ "$(field resolved)" = "1" ]; then
		log "内置守护已就绪（等待 ${waited}s）：$(cat "$STATE" 2>/dev/null)"
		exit 0
	fi

	log "内置守护未拿到厂商符号（等待 ${waited}s）：$(cat "$STATE" 2>/dev/null)"
	if write_zero; then
		log "已把 DDL 写 0 兜底（退回旧的恒关行为）"
	else
		log "DDL 节点 90s 内不可写，放弃（保持系统当前值）"
	fi
	dmesg | grep -a ddl_guard | tail -5 >>"$LOG" 2>/dev/null
	exit 0
fi

# ---------- 形态 B：可加载模块 ----------
log "未检测到内置 ddl_guard，走 .ko 形态"

# 阶段 1：安全缺省。必须先落实，否则模块加载前的窗口里会留着 1。
if ! write_zero; then
	log "DDL 节点 90s 内未就绪，放弃（保持系统当前值）"
	exit 0
fi
log "已写 0（安全缺省就位，等待 ${i}s）"

if [ "$POLICY" = "0" ]; then
	log "POLICY=0，不加载模块，保持恒关"
	exit 0
fi

[ -f "$KO" ] || { log "$KO 不存在，保持恒关"; exit 0; }

# insmod 需要 vendor 模块已加载（global_sched_ddl_enabled 才能被内核解析），
# 节点就绪是它的充分条件；仍留几次重试兜住竞态。
attempt=0
while [ "$attempt" -lt 5 ]; do
	if insmod "$KO" policy=1 linger_ms="$LINGER_MS" 2>>"$LOG"; then
		sleep 1
		log "模块已加载：$(cat "$STATE" 2>/dev/null)"
		exit 0
	fi
	attempt=$((attempt + 1))
	log "insmod 失败（第 $attempt 次），5s 后重试"
	sleep 5
done

log "模块加载失败，已放弃；DDL 保持 0（旧的安全行为）"
dmesg | grep -a ddl_guard | tail -5 >>"$LOG" 2>/dev/null
exit 0
