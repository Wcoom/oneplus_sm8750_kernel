# ddl_guard_ko - 容器作用域的 DDL 动态守护（可加载模块）
#
# 构建必须走 build.sh（它负责 PATH / O= / 取消签名 / 版本串校验）：
#   bash /home/wcoom/桌面/oplus13/ddl_guard_ko/build.sh
#
# 手工构建等价命令：
#   make LLVM=1 ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
#        PAHOLE=/usr/bin/pahole LD=ld.lld HOSTLD=ld.lld \
#        O=<kernel>/out CONFIG_MODULE_SIG_ALL= M=$(PWD) modules
obj-m += ddl_guard.o
