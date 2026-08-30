#!/system/bin/sh
MODDIR=${0%/*}

if [ -f "$MODDIR"/.boot ]; then
    touch "$MODDIR"/disable
    rm "$MODDIR"/.boot
else
    touch "$MODDIR"/.boot

    # fq_guard_ko: 守护数据接口 root qdisc 为 fq(BBRv3 公平性配套)。
    # fq_qdisc_ops 是内核 static 符号,oplus 禁止内核态读 kallsyms,
    # 只能用户态解析地址后经 fq_ops_addr 参数传入(KASLR 每次开机变化)
    if [ -f "$MODDIR"/fq_guard_ko.ko ]; then
        if [ -d /sys/module/fq_guard_ko ]; then
            rmmod fq_guard_ko 2>/dev/null
        fi
        echo 0 > /proc/sys/kernel/kptr_restrict 2>/dev/null
        FG_ADDR=$(cat /proc/kallsyms 2>/dev/null | grep -w fq_qdisc_ops | awk '{print $1}')
        if [ -n "$FG_ADDR" ]; then
            insmod "$MODDIR"/fq_guard_ko.ko fq_ops_addr=0x$FG_ADDR >/dev/null 2>&1 \
                || insmod "$MODDIR"/fq_guard_ko.ko >/dev/null 2>&1
        else
            insmod "$MODDIR"/fq_guard_ko.ko >/dev/null 2>&1
        fi
    fi

    for mod in "$MODDIR"/rekernel_x-*.ko; do
        if [ -f "$mod" ]; then
            if insmod "$mod" >/dev/null 2>&1; then
                break
            fi
        fi
    done

    rm "$MODDIR"/.boot
fi
