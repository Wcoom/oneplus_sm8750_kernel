echo "########## loop50/loop/dio 的权限（决定能否零补丁修好 §11） ##########"
ls -l /sys/block/loop50/loop/ | sed 's/^/  /'
echo
echo "  --- 逐项权限 ---"
for f in /sys/block/loop50/loop/*; do
  printf "  %-40s %s\n" "$f" "$(ls -l $f 2>/dev/null | awk '{print $1}')"
done
echo
echo "########## 内核是否支持 LOOP_SET_DIRECT_IO（运行时 ioctl 路径） ##########"
echo "  LOOP_SET_DIRECT_IO = 0x4C08 (19464)，查内核是否实现:"
grep -c "loop_set_dio\|LOOP_SET_DIRECT_IO" /proc/kallsyms 2>/dev/null
echo "  losetup 是否可用: $(command -v losetup || echo '不在宿主 PATH')"
echo "  容器内 losetup: $(ls -l /dev/loop50 2>/dev/null || echo '容器内无 /dev/loop50')"
echo
echo "########## 确认 dio 改变必须发生在挂载之前（安全性判断） ##########"
echo "  loop50 当前使用者:"
grep loop50 /proc/mounts | sed 's/^/    /'
echo "  ← 已挂载 ⇒ 运行中改 dio 有数据一致性风险"
echo
echo "########## droidspaces 是否暴露 loop/dio 相关设置 ##########"
/data/local/Droidspaces/bin/droidspaces --help 2>&1 | grep -iE "dio|direct|loop|image|sparse" | sed 's/^/  /'
