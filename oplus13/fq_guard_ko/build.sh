#!/bin/bash
# build.sh - 构建 fq_guard_ko.ko(目标:上游 whitewhale v3.5 prebuilt 内核)
#
# 要点:
#  1) vermagic 必须与目标内核一致:临时覆盖 out/include/generated/utsrelease.h
#     为目标版本串(abogki20260808-4k),构建后恢复;
#  2) 目标内核 CONFIG_MODULE_SIG_PROTECT=y:本地密钥签名的模块会验签失败
#     (fatal),因此命令行 CONFIG_MODULE_SIG_ALL= 覆盖为不签名;
#  3) 引用符号的 CRC 走本地 out/Module.symvers(与上游同源,预期一致,
#     若 insmod 报 version magic / symbol version 不符再另行处理)。
set -e

KERNEL_ROOT="/home/wcoom/oplus13/android_kernel_common_oneplus_sm8750"
KO_DIR="$(cd "$(dirname "$0")" && pwd)"
OUT="$KERNEL_ROOT/out"
TARGET_RELEASE="6.6.118-android15-8-gf4dc45704e54-abogki20260808-4k"

export PATH="/home/wcoom/oplus13/clang-19/bin:$PATH"
export KBUILD_BUILD_TIMESTAMP="Mon May 12 09:09:59 UTC 2025"

UTS="$OUT/include/generated/utsrelease.h"
[ -f "$UTS" ] || { echo "缺少 $UTS(先跑过 内核构建.sh)"; exit 1; }
cp "$UTS" "$UTS.bak"
echo "覆盖 UTS_RELEASE -> $TARGET_RELEASE"
echo "#define UTS_RELEASE \"$TARGET_RELEASE\"" > "$UTS"

trap 'cp "$UTS.bak" "$UTS"; rm -f "$UTS.bak"' EXIT

cd "$KERNEL_ROOT"
make LLVM=1 \
     ARCH=arm64 \
     CROSS_COMPILE=aarch64-linux-gnu- \
     PAHOLE=/usr/bin/pahole \
     LD=ld.lld \
     HOSTLD=ld.lld \
     O=out \
     CONFIG_MODULE_SIG_ALL= \
     M="$KO_DIR" \
     modules

echo "========================================"
ls -la "$KO_DIR"/*.ko
echo "vermagic:"
strings "$KO_DIR"/fq_guard_ko.ko | grep -E "^6\.6\.118" || true
echo "签名检查(应无输出):"
tail -c 512 "$KO_DIR"/fq_guard_ko.ko | strings | grep "Module signature" || echo "未签名 OK"
echo "========================================"
