#!/system/bin/sh

# 主 KO 列表，每行一个文件名
MAIN_KOS="
oplus_chg_v2.ko
"

# 副 KO 列表，每行一个文件名
AUXILIARY_KOS="
ufcs_class.ko
"

# 允许 CRC 白名单修补的 KO 列表，每行一个文件名
CRC_PATCHABLE_KOS="
oplus_chg_v2.ko
"

command -v ui_print >/dev/null 2>&1 || ui_print() { echo "$1"; }

kot_abort() {
	ui_print "$1"
	exit 1
}

kot_md5() {
	"$BB" md5sum "$1" 2>/dev/null | "$BB" awk '{print $1}'
}

jz_install_finish() {
	ui_print "- 清理临时文件"
	rm -r $MODPATH/bin/dtc
	rm -r $MODPATH/bin/mkdtimg
}

kot_print_log_file() {
	log_file="$1"
	log_prefix="$2"

	[ -s "$log_file" ] || return 0

	while IFS= read -r line; do
		if [ -n "$log_prefix" ]; then
			ui_print "$log_prefix$line"
		else
			ui_print "$line"
		fi
	done <"$log_file"
}

kot_require_file() {
	[ -f "$1" ] || kot_abort "! 缺少文件: $1"
}

kot_prepare_env() {
	[ -n "$MODPATH" ] || MODPATH=${0%/*}
	[ -n "$MODPATH" ] || kot_abort "! MODPATH 不存在"

	BB="$MODPATH/bin/busybox"
	CP_TOOL="/system/bin/cp"
	MKFS_EROFS="$MODPATH/bin/mkfs.erofs"
	LPADD="$MODPATH/bin/lpadd_auto"
	LPDUMP="$MODPATH/bin/lpdump"
	KO_GUARD="$MODPATH/bin/ko_guard"
	KO_CRC_PATCHER="$MODPATH/bin/ko_crc_patcher"
	KO_CRC_WHITELIST="$MODPATH/ko_crc_whitelist.txt"
	AVBCTL="$MODPATH/bin/avbctl"
	MBOOT="$MODPATH/bin/mboot"
	SOC_MODEL_MAP_FILE="$MODPATH/soc_model.map"
	KO_MD5_FILE="$MODPATH/vendor_dlkm_ko.md5"
	KO_MD5_TMP="$KO_MD5_FILE.tmp"
	NEED_REBOOT_FILE="/tmp/need_reboot"

	kot_require_file "$BB"
	kot_require_file "$CP_TOOL"
	kot_require_file "$MKFS_EROFS"
	kot_require_file "$LPADD"
	kot_require_file "$LPDUMP"
	kot_require_file "$KO_GUARD"
	kot_require_file "$KO_CRC_PATCHER"
	kot_require_file "$KO_CRC_WHITELIST"
	kot_require_file "$AVBCTL"
	kot_require_file "$MBOOT"
	kot_require_file "$SOC_MODEL_MAP_FILE"

	chmod 0755 "$BB" "$MKFS_EROFS" "$LPADD" "$LPDUMP" "$KO_GUARD" "$KO_CRC_PATCHER" "$AVBCTL" "$MBOOT" 2>/dev/null
	export PATH="$MODPATH/bin:$PATH"
}

kot_check_need_reboot() {
	[ -e "$NEED_REBOOT_FILE" ] || return
	kot_abort "! 检测到重启标志，请重启后再刷入"
}

kot_mark_need_reboot() {
	mkdir -p "${NEED_REBOOT_FILE%/*}" 2>/dev/null
	touch "$NEED_REBOOT_FILE" 2>/dev/null || ui_print "! 写入重启标记失败: $NEED_REBOOT_FILE"
}

kot_check_cccv_conflict() {
	[ ! -e /proc/sys/cccv ] || kot_abort "! 本机内核与模块冲突，请更换内核重启后重新刷入"
}

kot_check_super_writable() {
	super_part="/dev/block/by-name/super"
	[ -e "$super_part" ] || kot_abort "! 未找到 super 分区: $super_part"

	super_ro=$(blockdev --getro "$super_part" 2>/dev/null)
	[ "$super_ro" = "0" ] || kot_abort "! 可能有防格机模块，请关闭后重启重试"
}

kot_run_ko_guard() {
	ko_guard_ko="$1"
	ko_guard_with_ko="$2"

	ui_print "- 正在执行内核模块符号对齐检查: ${ko_guard_ko##*/}"
	if [ -n "$ko_guard_with_ko" ]; then
		ko_guard_output=$("$KO_GUARD" --modules-dir "$WORKDIR/d" --with-ko "$ko_guard_with_ko" "$ko_guard_ko" 2>&1)
	else
		ko_guard_output=$("$KO_GUARD" --modules-dir "$WORKDIR/d" "$ko_guard_ko" 2>&1)
	fi
	ko_guard_code=$?

	printf '%s\n' "$ko_guard_output" | while IFS= read -r ko_guard_line; do
		[ -n "$ko_guard_line" ] || continue
		ui_print "- $ko_guard_line"
	done

	[ "$ko_guard_code" -eq 0 ] || return 1

	ui_print "- 内核模块符号对齐检查通过"
	return 0
}

kot_is_crc_patchable() {
	crc_patch_target="$1"

	for crc_patch_name in $CRC_PATCHABLE_KOS; do
		[ "$crc_patch_name" = "$crc_patch_target" ] && return 0
	done
	return 1
}

kot_patch_and_verify_ko() {
	ko_patch_source="$1"
	ko_patch_with_ko="$2"
	ko_patch_name=${ko_patch_source##*/}
	ko_patch_output_path="$WORKDIR/patched/$ko_patch_name"

	rm -f "$ko_patch_output_path"
	ui_print "- 正在修补内核模块 CRC: $ko_patch_name"
	if [ -n "$ko_patch_with_ko" ]; then
		ko_patch_output=$("$KO_CRC_PATCHER" --modules-dir "$WORKDIR/d" --with-ko "$ko_patch_with_ko" --whitelist "$KO_CRC_WHITELIST" --output "$ko_patch_output_path" "$ko_patch_source" 2>&1)
	else
		ko_patch_output=$("$KO_CRC_PATCHER" --modules-dir "$WORKDIR/d" --whitelist "$KO_CRC_WHITELIST" --output "$ko_patch_output_path" "$ko_patch_source" 2>&1)
	fi
	ko_patch_code=$?

	printf '%s\n' "$ko_patch_output" | while IFS= read -r ko_patch_line; do
		[ -n "$ko_patch_line" ] || continue
		ui_print "- $ko_patch_line"
	done

	if [ "$ko_patch_code" -ne 0 ] || [ ! -f "$ko_patch_output_path" ]; then
		rm -f "$ko_patch_output_path"
		return 1
	fi
	if ! kot_run_ko_guard "$ko_patch_output_path" "$ko_patch_with_ko"; then
		rm -f "$ko_patch_output_path"
		return 1
	fi

	ui_print "- 内核模块 CRC 修补并复检通过: $ko_patch_name"
	return 0
}

kot_detect_vendor_dlkm_group() {
	slot_suffix=$(getprop ro.boot.slot_suffix)
	target_part="vendor_dlkm${slot_suffix}"
	super_part="/dev/block/by-name/super"

	case "$slot_suffix" in
	_a)
		slot_index=0
		;;
	_b)
		slot_index=1
		;;
	"")
		slot_index=0
		;;
	*)
		kot_abort "! 未知槽位后缀: $slot_suffix"
		;;
	esac

	# ui_print "- lpdump slot_suffix: ${slot_suffix:-<empty>}"
	# ui_print "- lpdump slot_index: ${slot_index:-<empty>}"
	# ui_print "- lpdump target_part: ${target_part:-<empty>}"

	lpdump_text=$("$LPDUMP" "$super_part" -s "$slot_index" 2>&1)
	lpdump_code=$?
	if [ "$lpdump_code" -ne 0 ] || [ -z "$lpdump_text" ]; then
		ui_print "! lpdump 执行失败或输出为空，返回码: $lpdump_code"
		printf '%s\n' "$lpdump_text" | while IFS= read -r lpdump_line; do
			[ -n "$lpdump_line" ] || continue
			ui_print "! lpdump: $lpdump_line"
		done
		kot_abort "! 未找到 vendor_dlkm 动态分区组，请重启后再刷入"
	fi

	vendor_dlkm_block=$(printf '%s\n' "$lpdump_text" | "$BB" awk -v target="$target_part" '
        /^[[:space:]]*Name:/ {
            name = $0
            sub(/^[[:space:]]*Name:[[:space:]]*/, "", name)
            sub(/[[:space:]]*$/, "", name)
            in_target = (name == target)
        }
        in_target {
            print
        }
        in_target && /^-+$/ {
            exit
        }
    ')
	if [ -z "$vendor_dlkm_block" ]; then
		ui_print "! lpdump 未匹配到目标分区: $target_part"
		ui_print "! lpdump vendor_dlkm 候选 Name:"
		printf '%s\n' "$lpdump_text" | "$BB" awk '
            /^[[:space:]]*Name: vendor_dlkm/ {
                print
                count++
                if (count >= 12) {
                    exit
                }
            }
        ' | while IFS= read -r lpdump_line; do
			[ -n "$lpdump_line" ] || continue
			ui_print "! lpdump: $lpdump_line"
		done
		kot_abort "! 未找到 vendor_dlkm 分区信息，请重启后再刷入"
	fi

	vendor_dlkm_group=$(printf '%s\n' "$vendor_dlkm_block" | "$BB" awk '/^[[:space:]]*Group:/ {value = $0; sub(/^[[:space:]]*Group:[[:space:]]*/, "", value); sub(/[[:space:]]*$/, "", value); print value; exit}')

	# ui_print "- lpdump group: ${vendor_dlkm_group:-<empty>}"

	[ -n "$vendor_dlkm_group" ] || kot_abort "! 未找到 vendor_dlkm 动态分区组，请重启后再刷入"

	# ui_print "- lpdump final_group: ${vendor_dlkm_group:-<empty>}"
}

kot_resolve_soc_module_dir() {
	SOC_MODULE_DIR="$SOC_MODEL"
	soc_map_target=$("$BB" awk -F= -v model="$SOC_MODEL" '
		$1 == model {
			sub(/\r$/, "", $2)
			if (NF != 2 || $2 !~ /^[0-9]+$/) exit 1
			print $2
			exit
		}
	' "$SOC_MODEL_MAP_FILE") || kot_abort "! 处理器型号映射格式无效: $SOC_MODEL"
	[ -z "$soc_map_target" ] || SOC_MODULE_DIR="$soc_map_target"
}

kot_check_device() {
	[ -f /vendor_dlkm/lib/modules/oplus_chg_v2.ko ] || kot_abort "! 系统不支持"

	kernel_release=$(uname -r)
	SOC_MODEL=""
	for soc_prop in ro.product.oplus.cpuinfo ro.soc.model ro.vendor.qti.soc_model; do
		soc_value=$(getprop "$soc_prop")
		soc_value=$(printf '%s' "$soc_value" | "$BB" tr -d '[:space:]' | "$BB" tr '[:lower:]' '[:upper:]')
		case "$soc_value" in
		SM*)
			soc_value=${soc_value#SM}
			;;
		esac
		case "$soc_value" in
		"" | *[!0-9]*)
			continue
			;;
		esac
		SOC_MODEL="$soc_value"
		break
	done

	[ -n "$SOC_MODEL" ] || kot_abort "! 处理器型号读取失败"

	kot_resolve_soc_module_dir
}

kot_copy_vendor_dlkm_from_mount() {
	cp_log="$WORKDIR/cp_vendor_dlkm.log"
	cp_fallback_log="$WORKDIR/cp_vendor_dlkm_fallback.log"

	rm -rf "$WORKDIR/d"

	: >"$cp_log" || kot_abort "! 创建复制日志失败"
	if "$CP_TOOL" --preserve=all -ar /vendor_dlkm "$WORKDIR/d" >"$cp_log" 2>&1; then
		return 0
	fi

	rm -rf "$WORKDIR/d"
	: >"$cp_fallback_log" || kot_abort "! 创建复制回退日志失败"
	if "$CP_TOOL" -a /vendor_dlkm "$WORKDIR/d" >"$cp_fallback_log" 2>&1; then
		return 0
	fi

	kot_print_log_file "$cp_log" "! cp: "
	kot_print_log_file "$cp_fallback_log" "! cp: "
	rm -rf "$WORKDIR/d"
	return 1
}

kot_disable_avb() {
	avb_verity_log="$WORKDIR/avbctl_disable_verity.log"
	avb_verification_log="$WORKDIR/avbctl_disable_verification.log"

	ui_print "- 正在关闭 AVB 校验"
	: >"$avb_verity_log" || kot_abort "! 创建 avbctl 日志失败"
	"$AVBCTL" disable-verity --force >"$avb_verity_log" 2>&1 || {
		kot_print_log_file "$avb_verity_log" "! avbctl: "
		kot_abort "! avbctl disable-verity 执行失败"
	}
	: >"$avb_verification_log" || kot_abort "! 创建 avbctl 日志失败"
	"$AVBCTL" disable-verification --force >"$avb_verification_log" 2>&1 || {
		kot_print_log_file "$avb_verification_log" "! avbctl: "
		kot_abort "! avbctl disable-verification 执行失败"
	}

	if ! "$AVBCTL" get-verity 2>/dev/null | "$BB" grep -qi "disabled"; then
		kot_abort "! verity 未关闭，退出安装"
	fi
	if ! "$AVBCTL" get-verification 2>/dev/null | "$BB" grep -qi "disabled"; then
		kot_abort "! verification 未关闭，退出安装"
	fi
	ui_print "- AVB 校验已关闭"
}

kot_print_avb_state() {
	avb_verity_state=$("$AVBCTL" get-verity 2>/dev/null | "$BB" tr -d '\r' | "$BB" head -n 1)
	avb_verification_state=$("$AVBCTL" get-verification 2>/dev/null | "$BB" tr -d '\r' | "$BB" head -n 1)

	[ -n "$avb_verity_state" ] || avb_verity_state="unknown"
	[ -n "$avb_verification_state" ] || avb_verification_state="unknown"

	ui_print "- 当前 AVB verity: $avb_verity_state"
	ui_print "- 当前 AVB verification: $avb_verification_state"
}

kot_enable_avb() {
	avb_verity_log="$WORKDIR/avbctl_enable_verity.log"
	avb_verification_log="$WORKDIR/avbctl_enable_verification.log"

	ui_print "- 正在启用 AVB 校验"
	: >"$avb_verity_log" || kot_abort "! 创建 avbctl 日志失败"
	"$AVBCTL" enable-verity --force >"$avb_verity_log" 2>&1 || {
		kot_print_log_file "$avb_verity_log" "! avbctl: "
		kot_abort "! avbctl enable-verity 执行失败"
	}
	: >"$avb_verification_log" || kot_abort "! 创建 avbctl 日志失败"
	"$AVBCTL" enable-verification --force >"$avb_verification_log" 2>&1 || {
		kot_print_log_file "$avb_verification_log" "! avbctl: "
		kot_abort "! avbctl enable-verification 执行失败"
	}

	if ! "$AVBCTL" get-verity 2>/dev/null | "$BB" grep -qi "enabled"; then
		kot_abort "! verity 未启用，退出安装"
	fi
	if ! "$AVBCTL" get-verification 2>/dev/null | "$BB" grep -qi "enabled"; then
		kot_abort "! verification 未启用，退出安装"
	fi
	ui_print "- AVB 校验已启用"
}

kot_choose_avb_policy() {
	AVB_POLICY=""

	case "$SOC_MODEL" in
	8845 | 8850)
		ui_print ""
		ui_print "- 检测到 $SOC_MODEL，请选择你得root方式"
		ui_print "! 音量+：已经使用 efisp(伪回锁/免解锁/假回锁) "
		ui_print "! 音量-：未使用 efisp 免解，正常解锁"
		while :; do
			case "$(until_key)" in
			up)
				ui_print "- efisp 免解状态: 已启用"
				AVB_POLICY="enable"
				return 0
				;;
			down)
				ui_print "- efisp 免解状态: 未启用"
				ui_print "- 将尝试对 vendor_boot 进行补丁"
				AVB_POLICY="patch"
				return 0
				;;
			*)
				ui_print "! 请按音量键选择"
				;;
			esac
		done
		;;
	*)
		ui_print ""
		ui_print "- efisp 免解状态: 未启用"
		ui_print "- 将尝试对 vendor_boot 进行补丁"
		AVB_POLICY="patch"
		;;
	esac
}

kot_handle_avb_policy() {
	case "$AVB_POLICY" in
	enable)
		if (kot_prepare_vendor_boot_patch); then
			kot_flash_vendor_boot_patch "! vendor_boot处理失败，请还原官方 vendor_boot 后重试刷入"
			kot_enable_avb
		else
			ui_print "! 无法对 vendor_boot 补丁，强制关闭 AVB"
			AVB_POLICY="disable"
			kot_disable_avb
		fi
		;;
	patch)
		if (kot_prepare_vendor_boot_patch); then
			kot_flash_vendor_boot_patch "! vendor_boot 写入失败，请还原官方 vendor_boot 后重试"
		else
			ui_print "! 无法对 vendor_boot 补丁，强制关闭 AVB"
			AVB_POLICY="disable"
			kot_disable_avb
		fi
		;;
	disable)
		kot_disable_avb
		;;
	*)
		kot_abort "! 未知 AVB 处理策略: $AVB_POLICY"
		;;
	esac

	kot_print_avb_state
}

kot_patch_fstab_file() {
	fstab_file="$1"
	fstab_tmp="$fstab_file.tmp"

	[ -f "$fstab_file" ] || kot_abort "! 未找到 fstab.qcom: $fstab_file"

	if "$BB" awk '
        BEGIN {
            found = 0
            patched = 0
        }
        $1 == "vendor_dlkm" && $2 == "/vendor_dlkm" {
            found = 1
            before = $0
            gsub(/,avb=vbmeta,/, ",")
            gsub(/,avb=vbmeta$/, "")
            if ($0 != before) {
                patched = 1
            }
        }
        {
            print
        }
        END {
            if (found == 0) {
                exit 2
            }
            if (patched == 0) {
                exit 3
            }
        }
    ' "$fstab_file" >"$fstab_tmp"; then
		fstab_patch_ret=0
	else
		fstab_patch_ret=$?
	fi

	case "$fstab_patch_ret" in
	0)
		mv -f "$fstab_tmp" "$fstab_file" || kot_abort "! 写回 fstab.qcom 失败"
		;;
	2)
		rm -f "$fstab_tmp"
		kot_abort "! fstab.qcom 中未找到 vendor_dlkm /vendor_dlkm 行"
		;;
	3)
		rm -f "$fstab_tmp"
		;;
	*)
		rm -f "$fstab_tmp"
		kot_abort "! fstab.qcom 处理失败"
		;;
	esac
}

kot_prepare_vendor_boot_patch() {
	slot_suffix=$(getprop ro.boot.slot_suffix)
	vendor_boot_part="/dev/block/by-name/vendor_boot${slot_suffix}"
	vendor_boot_work="$WORKDIR/vendor_boot_patch"

	[ -e "$vendor_boot_part" ] || kot_abort "! 未找到 vendor_boot 分区: $vendor_boot_part"
	rm -rf "$vendor_boot_work"
	mkdir -p "$vendor_boot_work" || kot_abort "! 创建 vendor_boot 工作目录失败"

	cd "$vendor_boot_work" || kot_abort "! 进入 vendor_boot 工作目录失败"
	"$CP_TOOL" "$MBOOT" "$vendor_boot_work/mboot" || kot_abort "! 复制 mboot 失败"
	chmod 0755 "$vendor_boot_work/mboot" 2>/dev/null

	ui_print "- 正在提取 vendor_boot"
	vendor_boot_dd_log="$vendor_boot_work/dd_vendor_boot.log"
	: >"$vendor_boot_dd_log" || kot_abort "! 创建 vendor_boot 日志失败"
	dd if="$vendor_boot_part" of=vendor_boot.img >"$vendor_boot_dd_log" 2>&1 || {
		kot_print_log_file "$vendor_boot_dd_log" "! dd vendor_boot: "
		kot_abort "! 提取 vendor_boot 失败"
	}
	[ -f vendor_boot.img ] || kot_abort "! vendor_boot.img 不存在"

	ui_print "- 正在解包 vendor_boot"
	vendor_boot_unpack_log="$vendor_boot_work/mboot_unpack.log"
	: >"$vendor_boot_unpack_log" || kot_abort "! 创建 mboot 日志失败"
	if ! ./mboot unpack -h vendor_boot.img >"$vendor_boot_unpack_log" 2>&1; then
		kot_print_log_file "$vendor_boot_unpack_log" "! mboot unpack: "
	fi

	target_cpio="vendor_ramdisk/ramdisk.cpio"
	[ -f "$target_cpio" ] || kot_abort "! 未找到 vendor_boot ramdisk: $target_cpio"

	vendor_boot_extract_log="$vendor_boot_work/mboot_cpio_extract.log"
	: >"$vendor_boot_extract_log" || kot_abort "! 创建 mboot cpio 日志失败"
	./mboot cpio "$target_cpio" "extract first_stage_ramdisk/fstab.qcom fstab.qcom" >"$vendor_boot_extract_log" 2>&1 || {
		kot_print_log_file "$vendor_boot_extract_log" "! mboot cpio extract: "
		kot_abort "! 提取 first_stage_ramdisk/fstab.qcom 失败"
	}
	[ -f fstab.qcom ] || kot_abort "! fstab.qcom 提取失败"

	kot_patch_fstab_file fstab.qcom

	vendor_boot_add_log="$vendor_boot_work/mboot_cpio_add.log"
	: >"$vendor_boot_add_log" || kot_abort "! 创建 mboot cpio 日志失败"
	./mboot cpio "$target_cpio" "add 0644 first_stage_ramdisk/fstab.qcom fstab.qcom" >"$vendor_boot_add_log" 2>&1 || {
		kot_print_log_file "$vendor_boot_add_log" "! mboot cpio add: "
		kot_abort "! 写回 first_stage_ramdisk/fstab.qcom 失败"
	}

	ui_print "- 正在重新打包 vendor_boot"
	ui_print "- 此过程时间较长请耐心等待，请勿离开安装界面"
	vendor_boot_repack_log="$vendor_boot_work/mboot_repack.log"
	: >"$vendor_boot_repack_log" || kot_abort "! 创建 mboot repack 日志失败"
	./mboot repack vendor_boot.img new_vendor_boot.img >"$vendor_boot_repack_log" 2>&1 || {
		kot_print_log_file "$vendor_boot_repack_log" "! mboot repack: "
		kot_abort "! 重新打包 vendor_boot 失败"
	}
	[ -s new_vendor_boot.img ] || kot_abort "! new_vendor_boot.img 不存在或为空"

	vendor_boot_image_size=$("$BB" stat -c %s new_vendor_boot.img 2>/dev/null)
	vendor_boot_part_size=$(blockdev --getsize64 "$vendor_boot_part" 2>/dev/null)
	case "$vendor_boot_image_size" in
	"" | *[!0-9]* | 0)
		kot_abort "! vendor_boot 镜像或分区大小读取失败"
		;;
	esac
	case "$vendor_boot_part_size" in
	"" | *[!0-9]* | 0)
		kot_abort "! vendor_boot 镜像或分区大小读取失败"
		;;
	esac
	[ "$vendor_boot_image_size" -le "$vendor_boot_part_size" ] || kot_abort "! 新 vendor_boot 镜像大于分区容量"

	ui_print "- 正在校验 vendor_boot 打包结果"
	vendor_boot_verify_work="$vendor_boot_work/repack_verify"
	vendor_boot_verify_log="$vendor_boot_work/mboot_verify.log"
	rm -rf "$vendor_boot_verify_work"
	mkdir -p "$vendor_boot_verify_work" || kot_abort "! 创建 vendor_boot 校验目录失败"
	cd "$vendor_boot_verify_work" || kot_abort "! 进入 vendor_boot 校验目录失败"
	: >"$vendor_boot_verify_log" || kot_abort "! 创建 vendor_boot 校验日志失败"
	../mboot unpack -h ../new_vendor_boot.img >"$vendor_boot_verify_log" 2>&1 || true
	[ -s vendor_ramdisk/ramdisk.cpio ] || {
		kot_print_log_file "$vendor_boot_verify_log" "! mboot verify: "
		kot_abort "! vendor_boot 重打包镜像校验失败"
	}
	vendor_boot_verify_fstab_log="$vendor_boot_work/mboot_verify_fstab.log"
	: >"$vendor_boot_verify_fstab_log" || kot_abort "! 创建 vendor_boot fstab 校验日志失败"
	../mboot cpio vendor_ramdisk/ramdisk.cpio "extract first_stage_ramdisk/fstab.qcom fstab.qcom" >"$vendor_boot_verify_fstab_log" 2>&1 || {
		kot_print_log_file "$vendor_boot_verify_fstab_log" "! mboot verify fstab: "
		kot_abort "! 重打包镜像中的 fstab.qcom 提取失败"
	}
	patched_fstab_md5=$(kot_md5 ../fstab.qcom)
	verified_fstab_md5=$(kot_md5 fstab.qcom)
	[ -n "$patched_fstab_md5" ] || kot_abort "! 补丁 fstab.qcom MD5 读取失败"
	[ "$patched_fstab_md5" = "$verified_fstab_md5" ] || kot_abort "! vendor_boot 重打包后 fstab.qcom 校验不一致"
	cd "$vendor_boot_work" || kot_abort "! 返回 vendor_boot 工作目录失败"
	ui_print "- vendor_boot 补丁镜像准备完成"
}

kot_flash_vendor_boot_patch() {
	vendor_boot_failure_message="$1"
	slot_suffix=$(getprop ro.boot.slot_suffix)
	vendor_boot_part="/dev/block/by-name/vendor_boot$slot_suffix"
	vendor_boot_work="$WORKDIR/vendor_boot_patch"
	vendor_boot_image="$vendor_boot_work/new_vendor_boot.img"

	[ -n "$vendor_boot_failure_message" ] || vendor_boot_failure_message="! vendor_boot 写入失败，请还原官方 vendor_boot 后重试"
	[ -e "$vendor_boot_part" ] || {
		ui_print "! 未找到 vendor_boot 分区: $vendor_boot_part"
		kot_abort "$vendor_boot_failure_message"
	}
	[ -s "$vendor_boot_image" ] || {
		ui_print "! new_vendor_boot.img 不存在或为空"
		kot_abort "$vendor_boot_failure_message"
	}

	vendor_boot_image_size=$("$BB" stat -c %s "$vendor_boot_image" 2>/dev/null)
	case "$vendor_boot_image_size" in
	"" | *[!0-9]* | 0)
		ui_print "! vendor_boot 镜像大小读取失败"
		kot_abort "$vendor_boot_failure_message"
		;;
	esac

	cd "$vendor_boot_work" || kot_abort "$vendor_boot_failure_message"
	ui_print "- 正在写入 vendor_boot"
	vendor_boot_flash_log="$vendor_boot_work/dd_new_vendor_boot.log"
	: >"$vendor_boot_flash_log" || kot_abort "$vendor_boot_failure_message"
	dd if=new_vendor_boot.img of="$vendor_boot_part" conv=fsync >"$vendor_boot_flash_log" 2>&1 || {
		kot_print_log_file "$vendor_boot_flash_log" "! dd new_vendor_boot: "
		kot_abort "$vendor_boot_failure_message"
	}

	ui_print "- 正在校验 vendor_boot 写入结果"
	vendor_boot_image_md5=$(kot_md5 new_vendor_boot.img)
	vendor_boot_readback_log="$vendor_boot_work/vendor_boot_readback.log"
	: >"$vendor_boot_readback_log" || kot_abort "$vendor_boot_failure_message"
	vendor_boot_readback_md5=$("$BB" head -c "$vendor_boot_image_size" "$vendor_boot_part" 2>"$vendor_boot_readback_log" | "$BB" md5sum | "$BB" awk '{print $1}')
	[ -n "$vendor_boot_image_md5" ] || {
		ui_print "! 新 vendor_boot 镜像 MD5 读取失败"
		kot_abort "$vendor_boot_failure_message"
	}
	[ -n "$vendor_boot_readback_md5" ] || {
		kot_print_log_file "$vendor_boot_readback_log" "! vendor_boot readback: "
		kot_abort "$vendor_boot_failure_message"
	}
	[ "$vendor_boot_image_md5" = "$vendor_boot_readback_md5" ] || {
		ui_print "! vendor_boot 写入后校验不一致，停止刷入 vendor_dlkm"
		kot_abort "$vendor_boot_failure_message"
	}
	cd "$WORKDIR" || kot_abort "! 返回工作目录失败"
	ui_print "- vendor_boot 补丁写入并校验成功"
}

kot_prepare_selinux_retry() {
	SELINUX_STATE_ORIG=$(getenforce 2>/dev/null)
	SELINUX_CHANGED=0
	case "$SELINUX_STATE_ORIG" in
	Enforcing | Permissive | Disabled) ;;
	*)
		kot_abort "! SELinux 状态读取失败: $SELINUX_STATE_ORIG"
		;;
	esac
	ui_print "- 当前 SELinux 状态: $SELINUX_STATE_ORIG"
}

kot_export_vendor_dlkm_image() {
	export_img="$WORKDIR/vendor_dlkm_new_erofs.img"
	sdcard_img="/sdcard/vendor_dlkm_opbatt.img"
	export_log="$WORKDIR/export_vendor_dlkm_image.log"

	[ -f "$export_img" ] || kot_abort "! 导出镜像失败：未找到 vendor_dlkm_new_erofs.img"
	[ -d "/sdcard" ] || kot_abort "! 导出镜像失败：未找到 /sdcard"

	: >"$export_log" || kot_abort "! 创建导出日志失败"
	"$CP_TOOL" "$export_img" "$sdcard_img" >"$export_log" 2>&1 || {
		kot_print_log_file "$export_log" "! 导出镜像: "
		kot_abort "! 导出镜像失败：无法写入 /sdcard/vendor_dlkm_opbatt.img"
	}

	ui_print "系统下刷入vendor_dlkm失败"
	ui_print "========================"
	ui_print "你可能启动了防格机、bbg等功能！！！！！"
	ui_print "你可能启动了防格机、bbg等功能！！！！！"
	ui_print "你可能启动了防格机、bbg等功能！！！！！"
	ui_print "========================"
	ui_print "请停用后重新刷入后者使用以下方法刷入"
	ui_print "内置存储已准备好刷入镜像：vendor_dlkm_opbatt.img。"
	ui_print "方法1：使用“爱玩机工具箱”将此镜像刷入vendor_dlkm分区。"
	ui_print "方法2：请你尝试使用电脑或者另一台手机fastboot工具包刷入。"
	ui_print "刷入方法：关机后按住音量减号和电源键进入fastboot。"
	ui_print "连接电脑或手机输入\"fastboot reboot fastboot\"不带引号。"
	ui_print "使用fastboot flash vendor_dlkm vendor_dlkm_opbatt.img 刷入。"
	ui_print "如果不会命令行，请自行搜索支持fastbootd模式的工具箱或使用方法1。"
	exit 1
}

kot_restore_selinux_state() {
	[ "$SELINUX_CHANGED" = "1" ] || return 0

	setenforce_restore_log="$WORKDIR/setenforce_restore.log"
	ui_print "- 正在恢复 SELinux 状态: Enforcing"
	: >"$setenforce_restore_log" || kot_abort "! 创建 SELinux 恢复日志失败"
	setenforce 1 >"$setenforce_restore_log" 2>&1 || {
		kot_print_log_file "$setenforce_restore_log" "! setenforce: "
		kot_abort "! 恢复 SELinux 状态失败: Enforcing"
	}
}

kot_prepare_workdir() {
	TMP_BASE="${TMPDIR:-/tmp}"
	[ -d "$TMP_BASE" ] || mkdir -p "$TMP_BASE" || kot_abort "! 创建临时目录失败: $TMP_BASE"
	WORKDIR="$TMP_BASE/kotools.$$"
	FDATE=$(date +%Y%m%d%H%M%S)

	case "$WORKDIR" in
	/*/kotools.[0-9]*) ;;
	*)
		kot_abort "! 工作目录异常: $WORKDIR"
		;;
	esac
	rm -rf "$WORKDIR"
	mkdir -p "$WORKDIR" || kot_abort "! 创建工作目录失败: $WORKDIR"

	"$CP_TOOL" "$MKFS_EROFS" "$WORKDIR/mkfs.erofs" || kot_abort "! 复制 mkfs.erofs 失败"
	"$CP_TOOL" "$LPADD" "$WORKDIR/lpadd_auto" || kot_abort "! 复制 lpadd_auto 失败"
	chmod 0755 "$WORKDIR/mkfs.erofs" "$WORKDIR/lpadd_auto" 2>/dev/null
}

kot_prepare_vendor_dlkm_tree() {
	if ! kot_copy_vendor_dlkm_from_mount; then
		kot_abort "! /vendor_dlkm 处理失败，请重启后再刷入，工作目录: $WORKDIR"
	fi
	mkdir -p "$WORKDIR/d/etc" || kot_abort "! 创建 vendor_dlkm 标记目录失败"
	for ver_file in "$WORKDIR"/d/etc/*.ver; do
		[ -e "$ver_file" ] || continue
		rm -f "$ver_file" || kot_abort "! 删除旧 ver 标记失败"
	done
	touch "$WORKDIR/d/etc/${FDATE}.ver" || kot_abort "! 写入 ver 标记失败"

	[ -d "$WORKDIR/d/lib/modules" ] || kot_abort "! vendor_dlkm 中缺少 lib/modules 目录"
}

kot_has_ko_files() {
	ko_dir="$1"

	for ko_file in "$ko_dir"/*.ko; do
		[ -f "$ko_file" ] || continue
		return 0
	done
	return 1
}

kot_candidate_version_number() {
	candidate_path="$1"
	candidate_name=${candidate_path##*/}

	case "$candidate_name" in
	v[0-9]*)
		candidate_number=${candidate_name#v}
		case "$candidate_number" in
		"" | *[!0-9]*)
			return 1
			;;
		esac
		printf '%s\n' "$candidate_number"
		;;
	esac
}

kot_check_candidate_ko_files() {
	candidate_dir="$1"

	for required_ko_name in $MAIN_KOS $AUXILIARY_KOS; do
		[ -f "$candidate_dir/$required_ko_name" ] || {
			ui_print "! 候选目录中缺少 ko: $required_ko_name"
			return 1
		}
		[ -f "$WORKDIR/d/lib/modules/$required_ko_name" ] || {
			ui_print "! vendor_dlkm 中缺少待替换 ko: $required_ko_name"
			return 1
		}
	done
	return 0
}

kot_build_candidate_main_paths() {
	candidate_dir="$1"
	candidate_main_ko_paths=""

	for candidate_main_name in $MAIN_KOS; do
		candidate_main_path="$candidate_dir/$candidate_main_name"
		if [ -n "$candidate_main_ko_paths" ]; then
			candidate_main_ko_paths="$candidate_main_ko_paths,$candidate_main_path"
		else
			candidate_main_ko_paths="$candidate_main_path"
		fi
	done
}

kot_copy_and_verify_ko() {
	copy_ko_source="$1"
	copy_ko_name="$2"
	copy_ko_target="$WORKDIR/d/lib/modules/$copy_ko_name"

	[ -f "$copy_ko_source" ] || {
		ui_print "! 待复制 ko 不存在: $copy_ko_name"
		return 1
	}
	[ -f "$copy_ko_target" ] || {
		ui_print "! vendor_dlkm 中缺少待替换 ko: $copy_ko_name"
		return 1
	}
	"$CP_TOOL" "$copy_ko_source" "$copy_ko_target" || {
		ui_print "! 复制 $copy_ko_name 失败"
		return 1
	}

	copy_ko_source_md5=$(kot_md5 "$copy_ko_source")
	copy_ko_target_md5=$(kot_md5 "$copy_ko_target")
	[ -n "$copy_ko_source_md5" ] || {
		ui_print "! 源 ko MD5 读取失败: $copy_ko_name"
		return 1
	}
	[ -n "$copy_ko_target_md5" ] || {
		ui_print "! 目标 ko MD5 读取失败: $copy_ko_name"
		return 1
	}
	[ "$copy_ko_source_md5" = "$copy_ko_target_md5" ] || {
		ui_print "! ko MD5 不一致: $copy_ko_name"
		return 1
	}
	printf '%s  %s\n' "$copy_ko_target_md5" "$copy_ko_name" >>"$KO_MD5_TMP" || {
		ui_print "! 写入校验失败"
		return 1
	}
	return 0
}

kot_check_candidate_modules() {
	candidate_dir="$1"
	rm -f "$KO_MD5_TMP"
	: >"$KO_MD5_TMP" || kot_abort "! 创建校验失败"
	rm -rf "$WORKDIR/patched"
	mkdir -p "$WORKDIR/patched" || kot_abort "! 创建 ko 补丁目录失败"

	kot_check_candidate_ko_files "$candidate_dir" || return 1
	kot_build_candidate_main_paths "$candidate_dir"

	for candidate_aux_name in $AUXILIARY_KOS; do
		candidate_aux_source="$candidate_dir/$candidate_aux_name"
		if kot_run_ko_guard "$candidate_aux_source" "$candidate_main_ko_paths"; then
			continue
		fi
		kot_is_crc_patchable "$candidate_aux_name" || return 1
		ui_print "- 副 ko 首检未通过，尝试 CRC 白名单修补: $candidate_aux_name"
		if ! kot_patch_and_verify_ko "$candidate_aux_source" "$candidate_main_ko_paths"; then
			return 1
		fi
	done

	for candidate_aux_name in $AUXILIARY_KOS; do
		candidate_selected_source="$candidate_dir/$candidate_aux_name"
		[ ! -f "$WORKDIR/patched/$candidate_aux_name" ] || candidate_selected_source="$WORKDIR/patched/$candidate_aux_name"
		kot_copy_and_verify_ko "$candidate_selected_source" "$candidate_aux_name" || return 1
	done

	for candidate_main_name in $MAIN_KOS; do
		candidate_main_source="$candidate_dir/$candidate_main_name"
		candidate_selected_source="$candidate_main_source"
		if ! kot_run_ko_guard "$candidate_main_source" ""; then
			kot_is_crc_patchable "$candidate_main_name" || return 1
			ui_print "- 主 ko 首检未通过，尝试 CRC 白名单修补: $candidate_main_name"
			if ! kot_patch_and_verify_ko "$candidate_main_source" ""; then
				return 1
			fi
			candidate_selected_source="$WORKDIR/patched/$candidate_main_name"
		fi
		kot_copy_and_verify_ko "$candidate_selected_source" "$candidate_main_name" || return 1
	done

	[ -s "$KO_MD5_TMP" ] || {
		ui_print "! 校验为空"
		return 1
	}
	return 0
}

kot_select_and_verify_modules() {
	ui_print "- 内核版本 $kernel_release"
	ui_print "- 处理器型号 $SOC_MODEL"
	soc_module_note=""
	if [ "$SOC_MODULE_DIR" != "$SOC_MODEL" ]; then
		ui_print "- 模块目录映射: $SOC_MODEL -> $SOC_MODULE_DIR"
		soc_module_note=" (处理器 $SOC_MODEL)"
	fi
	found_soc_dir=0
	found_ko_dir=0

	for version_number in $(for version_dir in "$MODPATH"/modules/v[0-9]*; do
		[ -d "$version_dir" ] || continue
		kot_candidate_version_number "$version_dir"
	done | "$BB" sort -n); do
		version_dir="$MODPATH/modules/v${version_number}"
		candidate_dir="$version_dir/$SOC_MODULE_DIR"

		if [ ! -d "$candidate_dir" ]; then
			ui_print "- 跳过 v${version_number}: 缺少 $SOC_MODULE_DIR$soc_module_note"
			continue
		fi
		found_soc_dir=1
		if ! kot_has_ko_files "$candidate_dir"; then
			ui_print "- 跳过 v${version_number}: 没有 ko 文件"
			continue
		fi
		found_ko_dir=1

		kot_prepare_vendor_dlkm_tree
		if ! kot_check_candidate_modules "$candidate_dir"; then
			ui_print "! v${version_number}/$SOC_MODULE_DIR$soc_module_note 系统版本检测不通过，尝试下一个版本"
			rm -f "$KO_MD5_TMP"
			continue
		fi

		ui_print "- 适配版本: v${version_number}"
		return 0
	done

	[ "$found_soc_dir" -eq 1 ] || kot_abort "! 缺少处理器模块目录: $SOC_MODULE_DIR$soc_module_note"
	[ "$found_ko_dir" -eq 1 ] || kot_abort "! 处理器模块目录内没有 ko 文件: $SOC_MODULE_DIR$soc_module_note"
	kot_abort "! 系统版本检测不通过"
}

kot_pack_vendor_dlkm() {
	mkfs_log="$WORKDIR/mkfs_erofs.log"
	cd "$WORKDIR" || kot_abort "! 进入工作目录失败"
	ui_print "- 正在打包 vendor_dlkm"
	: >"$mkfs_log" || kot_abort "! 创建 mkfs.erofs 日志失败"
	./mkfs.erofs -zlz4hc -d2 --all-root -C 65536 --preserve-mtime vendor_dlkm_new_erofs.img d/ >"$mkfs_log" 2>&1 || {
		kot_print_log_file "$mkfs_log" "! mkfs.erofs: "
		kot_abort "! mkfs.erofs 打包失败"
	}
	[ -f "$WORKDIR/vendor_dlkm_new_erofs.img" ] || kot_abort "! vendor_dlkm 新镜像不存在"
}

kot_run_lpadd_once() {
	lpadd_log="$WORKDIR/lpadd.log"
	: >"$lpadd_log" || kot_abort "! 创建 lpadd 日志失败"

	# BBG 放行方案（2026-08-30）：不写 super 分区表（BBG 保护对象），
	# 改为读取当前 slot 的 dm-linear 表，创建同名映射的镜像 dm 设备
	# （镜像名取非当前 slot 后缀，复用 BBG 的 vendor_dlkm allowlist），
	# 直写镜像设备后删除；重启后 init 按 LP 元数据重建分区即生效。
	slot_suffix=$(getprop ro.boot.slot_suffix)
	case "$slot_suffix" in
	_a) mirror_name="vendor_dlkm_b" ;;
	*) mirror_name="vendor_dlkm_a" ;;
	esac
	dm_name="vendor_dlkm${slot_suffix}"

	dmctl table "$dm_name" >"$lpadd_log" 2>&1 || {
		echo "! dmctl table $dm_name 失败" >>"$lpadd_log"
		return 1
	}

	dm_args=""
	while IFS= read -r line; do
		case "$line" in
		*-*:*"linear, "*)
			seg="${line%%: linear*}"
			start="${seg%%-*}"
			end="${seg##*-}"
			dev="${line##*linear, }"
			dm_args="$dm_args linear $start $((end - start)) $dev"
			;;
		esac
	done <"$lpadd_log"
	[ -n "$dm_args" ] || {
		echo "! 解析 $dm_name 的 dm 表失败" >"$lpadd_log"
		return 1
	}

	dmctl delete "$mirror_name" >/dev/null 2>&1
	if ! dmctl create "$mirror_name" $dm_args >>"$lpadd_log" 2>&1; then
		echo "! 创建镜像 dm 设备失败" >>"$lpadd_log"
		return 1
	fi

	if ! dd if=vendor_dlkm_new_erofs.img of="/dev/block/mapper/$mirror_name" bs=4M conv=fsync >>"$lpadd_log" 2>&1; then
		dmctl delete "$mirror_name" >/dev/null 2>&1
		echo "! 写入镜像设备失败" >>"$lpadd_log"
		return 1
	fi

	dmctl delete "$mirror_name" >/dev/null 2>&1
	return 0
}

kot_flash_vendor_dlkm() {
	slot_suffix=$(getprop ro.boot.slot_suffix)
	super_part="/dev/block/by-name/super"
	target_part="vendor_dlkm${slot_suffix}"

	ui_print "- 目标分区: $target_part"
	ui_print "- 动态分区组: $vendor_dlkm_group"
	ui_print "- 正在写入 vendor_dlkm，请勿关机"

	kot_prepare_selinux_retry

	if ! kot_run_lpadd_once; then
		kot_print_log_file "$WORKDIR/lpadd.log" "! lpadd: "
		case "$SELINUX_STATE_ORIG" in
		Disabled | Permissive)
			kot_abort "! lpadd 写入失败，请重启后再刷入"
			;;
		Enforcing)
			setenforce_retry_log="$WORKDIR/setenforce_retry.log"
			ui_print "- 检测到分区写入失败，尝试关闭 SELinux 后重试"
			: >"$setenforce_retry_log" || kot_abort "! 创建 SELinux 重试日志失败"
			setenforce 0 >"$setenforce_retry_log" 2>&1 || {
				kot_print_log_file "$setenforce_retry_log" "! setenforce: "
				kot_abort "! 关闭 SELinux 失败"
			}
			SELINUX_CHANGED=1
			if ! kot_run_lpadd_once; then
				kot_print_log_file "$WORKDIR/lpadd.log" "! lpadd: "
				kot_restore_selinux_state
				kot_export_vendor_dlkm_image
			fi
			;;
		esac
	fi

	kot_mark_need_reboot
	kot_restore_selinux_state
	[ -s "$KO_MD5_TMP" ] || kot_abort "! 保存校验失败，请重启后再刷入"
	mv -f "$KO_MD5_TMP" "$KO_MD5_FILE" || kot_abort "! 保存校验失败，请重启后再刷入"
	ui_print "- vendor_dlkm 写入成功"
}

kot_finish_install() {
	[ -f "$MODPATH/module.prop" ] && {
		"$CP_TOOL" "$MODPATH/module.prop" "$MODPATH/module.prop.bak" 2>/dev/null
	}

	[ -d "$MODPATH/modules" ] && rm -rf $MODPATH/modules
	ui_print ""
	ui_print "- 安装完成，请重启设备"
}

kotools_main() {
	ui_print "- OPlus 电池工具包"
	kot_prepare_env
	kot_check_need_reboot
	kot_check_cccv_conflict
	kot_check_device
	kot_choose_avb_policy
	kot_check_super_writable
	kot_detect_vendor_dlkm_group
	kot_prepare_workdir
	kot_select_and_verify_modules
	kot_pack_vendor_dlkm
	kot_handle_avb_policy
	kot_flash_vendor_dlkm
	kot_finish_install
	jz_install_finish
}
