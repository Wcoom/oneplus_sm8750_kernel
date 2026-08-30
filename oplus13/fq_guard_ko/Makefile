# fq_guard_ko - 给上游 whitewhale v3.5 prebuilt 内核用的 fq 守护模块
# 构建(vermagic 需与目标内核一致,见 build.sh):
#   make LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#        O=<kernel>/out M=$(PWD) modules
obj-m += fq_guard_ko.o
