#!/system/bin/sh
#
# ddl_guard 开机自启（KernelSU service.d；root）
#
# 语义：先把 DDL 写成 0（安全缺省，与 op_mods/deploy/99-oplus-sched-ddl-guard.sh 相同），
#       再加载 ddl_guard.ko 交给它按容器生命周期动态管理：
#         有容器 → DDL 关；无容器 → DDL 开（厂商默认运行态）。
#       模块加载失败（例如换内核后 vermagic 不匹配）时**保持 0**，即退回旧的恒关行为。
#
# 依赖文件：
#   /data/adb/ddl_guard/ddl_guard.ko   模块（每次重刷内核后必须重新构建并替换）
#   /data/adb/ddl_guard/boot.log       本脚本日志
#
# 想退回"恒关 DDL、不加载模块"：把 POLICY 改成 0（或直接删掉本脚本、改用
# op_mods/deploy/99-oplus-sched-ddl-guard.sh）。

DDL_NODE=/proc/oplus_scheduler/sched_assist/sched_ddl_enabled
KO=/data/adb/ddl_guard/ddl_guard.ko
LOG=/data/adb/ddl_guard/boot.log

# 1 = 容器作用域（有容器才关，默认）；0 = 恒关（只关不开，等同旧的保护脚本）
POLICY=1
# 容器停止后延迟多少毫秒恢复 DDL
LINGER_MS=3000

log() {
	printf '[%s] %s\n' "$(date '+%m-%d %H:%M:%S')" "$*" >>"$LOG" 2>/dev/null
}

log "---- 启动，policy=$POLICY linger_ms=$LINGER_MS ----"

# 已经加载过（例如手动 insmod 后重启了 zygote 而非整机）就不再重复
if [ -d /sys/module/ddl_guard ]; then
	log "模块已在，跳过"
	exit 0
fi

# --- 阶段 1：安全缺省 ---------------------------------------------------
# 节点最多等 90 秒（vendor 模块 oplus_bsp_sched_assist 初始化后才会出现）。
# 这一步必须成功，否则后面就算模块加载了，也可能在"容器启动前"的窗口里留着 1。
attempt=0
while [ "$attempt" -lt 90 ]; do
	if [ -r "$DDL_NODE" ] && [ -w "$DDL_NODE" ]; then
		[ "$(cat "$DDL_NODE" 2>/dev/null)" != "0" ] &&
			printf '0\n' >"$DDL_NODE" 2>/dev/null
		[ "$(cat "$DDL_NODE" 2>/dev/null)" = "0" ] && break
	fi
	attempt=$((attempt + 1))
	sleep 1
done

if [ "$(cat "$DDL_NODE" 2>/dev/null)" != "0" ]; then
	log "DDL 节点 ${attempt}s 内未就绪，放弃（保持系统当前值）"
	exit 0
fi
log "已写 0（安全缺省就位，等待 ${attempt}s）"

# --- 阶段 2：加载守护模块 ----------------------------------------------
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
		log "模块已加载：$(cat /sys/module/ddl_guard/parameters/state 2>/dev/null)"
		exit 0
	fi
	attempt=$((attempt + 1))
	log "insmod 失败（第 $attempt 次），5s 后重试"
	sleep 5
done

log "模块加载失败，已放弃；DDL 保持 0（旧的安全行为）"
dmesg | grep -a ddl_guard | tail -5 >>"$LOG" 2>/dev/null
exit 0
