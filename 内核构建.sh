echo "删除前一次构建信息"
# 【编译速度优化】改为增量编译:make 会自动检测 .config/源码变更并只重编受影响文件
# 需要全量重建时手动执行: make clean && rm -rf out/*
# make clean&&rm -rf out/*
# ================= 1. 基础变量配置 =================
export USE_CCACHE=1
export CCACHE_DIR="/home/wcoom/桌面/oplus13/.ccache"
export CCACHE_MAXSIZE="5G"
export CCACHE_HARDLINK="true"
# 【编译速度优化】关闭 ccache 日志:避免每次编译产生数 GB 日志 IO
# export CCACHE_LOGFILE="$HOME/ccache_debug.log"

export KBUILD_BUILD_TIMESTAMP="Mon May 12 09:09:59 UTC 2025"
# AFDO 优化已取消（原: export AFDO_PROFILE="/home/wcoom/桌面/oplus13/android_kernel_common_oneplus_sm8750/android/gki/aarch64/afdo/kernel.afdo"）

# ================= 2. 关键：正确配置 PATH =================

# 第一步：先将真实的 Clang-19 加入 PATH
export PATH="/home/wcoom/桌面/oplus13/clang-19/bin:$PATH"

# 第二步：准备 ccache 伪装目录
mkdir -p $HOME/.ccache_bin
# 确保 ccache 指向系统安装的 ccache
ln -sf /usr/bin/ccache $HOME/.ccache_bin/clang
ln -sf /usr/bin/ccache $HOME/.ccache_bin/clang++

# 第三步：将伪装目录放到 PATH 的【最前面】，盖过 Clang-19
export PATH="$HOME/.ccache_bin:$PATH"

# ================= 3. 验证环节 (非常重要) =================
echo "正在检查编译器路径..."
WHICH_CLANG=$(which clang)
echo "当前使用的 clang 是: $WHICH_CLANG"

if [[ "$WHICH_CLANG" != *".ccache_bin/clang"* ]]; then
    echo "错误：ccache 未生效！检测到的 clang 路径不是伪装路径。"
    echo "请检查 PATH 设置。"
    exit 1
else
    echo "成功：ccache 已接管编译器。"
fi

# ================= 4. 编译命令 =================

# 【编译速度优化】定义优化参数
# 已取消: -mllvm -polly (Polly 循环优化器编译开销大 ~20-50%，对内核收益小)
#          -fauto-profile / -flto (AFDO / LTO)
# 新增:   -pipe (编译中间结果走管道,减少磁盘 IO)
# 保留:   -O2 -mcpu=oryon-1 (Oryon 微架构优化)
export CUSTOM_FLAGS="-O2 -mcpu=oryon-1 -Wno-error -pipe"

# 【编译速度优化】并行度:12 线程对 11G 内存过多,clang 编译峰值内存约 1.5-2G/任务,
# 满开会导致 swap 反而变慢,降到 8 更优;内存更大可调高
export JOBS=8

# 【固定版本名】置空 LOCALVERSION 环境变量，抑制 setlocalversion 追加 "+"
# （gki_defconfig 的 CONFIG_LOCALVERSION 已是完整后缀且 AUTO 已关闭，
#   脚本仅对"未设置 LOCALVERSION 环境变量"的情况追加 +；置空可精确还原版本串）
export LOCALVERSION=""

# 进入内核源码目录（脚本自包含，不依赖调用方 cwd）
cd /home/wcoom/桌面/oplus13/android_kernel_common_oneplus_sm8750 || exit 1

# 执行 Make
# 注意：这里去掉了 CC="ccache clang"，因为 PATH 已经搞定了
make -j$JOBS \
    LLVM=1 \
    ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- \
    PAHOLE=/usr/bin/pahole \
    LD=ld.lld \
    HOSTLD=ld.lld \
    O=out \
    KCFLAGS+="$CUSTOM_FLAGS" \
    gki_defconfig all
echo "========================================"
echo "编译完成！正在检查 Ccache 统计数据："
echo "当前缓存目录: $CCACHE_DIR"
ccache -s
echo "========================================"