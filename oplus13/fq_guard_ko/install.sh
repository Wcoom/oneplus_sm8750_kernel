#!/system/bin/sh
# install.sh - 设备端加载 fq_guard_ko(需 root)
# 用法: sh install.sh [ko路径, 默认 /data/local/tmp/fq_guard_ko.ko]
#
# 流程:
#  1) 放开 kptr_restrict(否则 /proc/kallsyms 地址全 0);
#  2) 用户态解析 fq_qdisc_ops 地址(oplus 禁止内核态读 kallsyms,
#     地址只能用户态拿,经 fq_ops_addr 模块参数传入);
#  3) insmod。KASLR 每次开机地址都会变,重启后需重新执行本脚本。

KO="${1:-/data/local/tmp/fq_guard_ko.ko}"

# 已加载则先卸载,保证可重复执行
if ls /sys/module/fq_guard_ko >/dev/null 2>&1; then
    rmmod fq_guard_ko 2>/dev/null || echo "rmmod 失败(继续尝试重载)"
fi

echo 0 > /proc/sys/kernel/kptr_restrict

addr=$(cat /proc/kallsyms | grep -w fq_qdisc_ops | awk '{print $1}')
if [ -z "$addr" ]; then
    echo "解析 fq_qdisc_ops 地址失败"
    exit 1
fi
echo "fq_qdisc_ops @ $addr"

insmod "$KO" fq_ops_addr=0x$addr
rc=$?
echo "insmod exit=$rc"
[ $rc -ne 0 ] && dmesg | tail -5
exit $rc
