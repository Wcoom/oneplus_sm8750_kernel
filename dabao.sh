#!/bin/bash
# 打包内核 Image 为 AnyKernel3 刷机包
# 命名规则: AnyKernel3-<内核版本号>-<提交哈希前5位>.zip

set -e

KERNEL_DIR="/home/wcoom/桌面/oplus13/android_kernel_common_oneplus_sm8750"
SOURCE_FILE="$KERNEL_DIR/out/arch/arm64/boot/Image"
TARGET_DIR="/home/wcoom/桌面/oplus13/AnyKernel3-6.6.112-NOKSU-OnePlus8Elite"

if [ ! -f "$SOURCE_FILE" ]; then
    echo "错误: 未找到 Image，请先编译内核: $SOURCE_FILE"
    exit 1
fi

# 内核版本号: 取纯版本号(不含 -g<sha>/-dirty 后缀，避免与下面的哈希重复)
KVER=$(make -s -C "$KERNEL_DIR" kernelversion 2>/dev/null | tr -d '[:space:]')

# 补上 LOCALVERSION(如 -4k)，与实际内核版本保持一致
LOCALVER=$(sed -n 's/^CONFIG_LOCALVERSION="\(.*\)"$/\1/p' "$KERNEL_DIR/out/.config" 2>/dev/null)
KVER="${KVER}${LOCALVER}"

if [ -z "$KVER" ]; then
    echo "错误: 无法获取内核版本号"
    exit 1
fi

# zip 命名使用 Image 镜像的修改时间（年月日时分），不再用提交哈希
ZIP_NAME="AnyKernel3-$(date -r "$SOURCE_FILE" +%Y%m%d-%H%M).zip"

echo "内核版本: $KVER"
echo "输出文件: $ZIP_NAME"

# Android GKI 的 Image 内可能包含内核自身编译进来的 empty_root.dtb。
# 这不是可以单独替换的外置 DTB；若把完整 Image 命名为 Image，AnyKernel
# 会把它复制到 split_img/kernel，同时保留原 boot 镜像的 kernel_dtb，最终
# 形成“新 Image + 旧尾块”的错误启动镜像。使用 *-dtb 命名可以触发
# AnyKernel 清理旧 kernel_dtb，再将完整 Image 原样重打包。
rm -f "$TARGET_DIR/Image" "$TARGET_DIR/Image-dtb"
cp -f "$SOURCE_FILE" "$TARGET_DIR/Image-dtb"

# 生成前检查产物确实是 ARM64 Linux Image，而不是空文件或误拿压缩镜像。
if ! grep -a -q -m1 "Linux version " "$SOURCE_FILE"; then
    echo "错误: Image 中未找到 Linux 版本标识，拒绝打包: $SOURCE_FILE"
    exit 1
fi

cd "$TARGET_DIR"

# 同名文件先删除，避免 zip 增量更新残留旧内容
rm -f "$ZIP_NAME"

zip -r "$ZIP_NAME" META-INF tools anykernel.sh Image-dtb LICENSE .gitignore \
    Zram_WebUI-v0.1-21-f3ff59f-release.zip

echo "打包完成: $TARGET_DIR/$ZIP_NAME"
ls -lh "$TARGET_DIR/$ZIP_NAME"
