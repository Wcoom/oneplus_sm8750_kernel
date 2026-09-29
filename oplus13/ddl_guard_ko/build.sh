#!/bin/bash
# build.sh - 构建 ddl_guard.ko（目标：本工作区自建内核，即 out/ 里那一份配置）
#
# 与 fq_guard_ko/build.sh 的差异：本模块的目标内核就是本地 out/ 的构建产物
# （设备当前跑的就是它），所以**不覆盖** utsrelease.h，vermagic 天然一致。
#
# 要点：
#  1) 必须先在同一个 out/ 上跑过 内核构建.sh（需要 .config/Module.symvers/生成的头）；
#  2) CONFIG_MODULE_SIG_ALL=y 会拿本地密钥签名，命令行覆盖为不签名
#     （设备端 SIG_FORCE 未开，未签名模块可直接加载，签名反而可能验签失败）；
#  3) global_sched_ddl_enabled 由厂商模块 oplus_bsp_sched_assist 导出，本地
#     Module.symvers 里没有它 —— modpost 默认把未定义符号当**错误**（Android 树
#     的 LOG_ERROR 分支）。这里用 KBUILD_MODPOST_WARN=1 降级为警告，因为：
#       a) 该符号只能由运行内核在已加载模块的导出表里解析（解析不到 insmod 直接
#          失败 = 失败即安全），编不进 .ko；
#       b) 补 Module.symvers 需要写死 CRC，而厂商模块的真实 CRC 拿不到 ——
#          写死 0 会让内核打印 "disagrees about version of symbol"，
#          且一旦将来 revert 掉本地 check_version 放行补丁（84708f314ec5c）
#          就会加载失败；不写 CRC（当前做法）走 "no symbol version" 分支，
#          在任何内核上都能加载。
#     降级后严格性由本脚本自己补回：构建日志里只允许 global_sched_ddl_enabled
#     这一个未定义符号，出现任何其它未定义符号直接判构建失败。
#  4) 每次重刷内核后都必须重新构建并推送 .ko（vermagic/CRC 与内核绑定）。
set -e

KERNEL_ROOT="/home/wcoom/桌面/oplus13/android_kernel_common_oneplus_sm8750"
KO_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$KERNEL_ROOT/out"

export PATH="/home/wcoom/桌面/oplus13/clang-19/bin:$PATH"
export KBUILD_BUILD_TIMESTAMP="Mon May 12 09:09:59 UTC 2025"

[ -f "$OUT/.config" ] || { echo "缺少 $OUT/.config（先在 $KERNEL_ROOT 跑 内核构建.sh）"; exit 1; }
[ -f "$OUT/Module.symvers" ] || { echo "缺少 $OUT/Module.symvers（构建树不完整）"; exit 1; }

LOG="$KO_DIR/.build.log"
cd "$KERNEL_ROOT"
make LLVM=1 \
     ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     PAHOLE=/usr/bin/pahole \
     LD=ld.lld \
     HOSTLD=ld.lld \
     O=out \
     CONFIG_MODULE_SIG_ALL= \
     KBUILD_MODPOST_WARN=1 \
     M="$KO_DIR" \
     modules 2>&1 | tee "$LOG"

echo "--- 未定义符号审计（只允许 global_sched_ddl_enabled）---"
UNEXPECTED=$(grep -o '"[^"]*" \[.*\.ko\] undefined!' "$LOG" \
	     | sed 's/"\([^"]*\)".*/\1/' | sort -u | grep -v '^global_sched_ddl_enabled$' || true)
if [ -n "$UNEXPECTED" ]; then
	echo "构建失败：出现预期外的未定义符号："
	echo "$UNEXPECTED"
	exit 1
fi
grep -q 'global_sched_ddl_enabled.* undefined!' "$LOG" || echo "(未出现该警告：符号可能已被解析)"

echo "========================================"
ls -la "$KO_DIR"/ddl_guard.ko
echo "--- vermagic（必须与设备 uname -r 一致）---"
modinfo -F vermagic "$KO_DIR"/ddl_guard.ko 2>/dev/null || strings "$KO_DIR"/ddl_guard.ko | grep -m1 "6\.6\.118"
echo "--- out/ 的 UTS_RELEASE ---"
grep UTS_RELEASE "$OUT/include/generated/utsrelease.h"
echo "--- 签名检查（应无输出）---"
tail -c 512 "$KO_DIR"/ddl_guard.ko | strings | grep "Module signature" || echo "未签名 OK"
echo "--- 未解析符号（预期含 global_sched_ddl_enabled，运行时由内核解析）---"
nm "$KO_DIR"/ddl_guard.ko | grep " U " || true
echo "========================================"
