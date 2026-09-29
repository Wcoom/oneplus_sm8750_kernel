D=/data/local/Droidspaces/Containers/ubuntu
echo "########## container.config（当前生效，530B，mtime 09-30 00:14） ##########"
cat $D/container.config | sed 's/^/  /'
echo
echo "########## container.config.bak-cgv1（09-28 21:20 备份） ##########"
cat $D/container.config.bak-cgv1 | sed 's/^/  /'
echo
echo "########## 两者差异（< 当前  > 备份） ##########"
diff $D/container.config $D/container.config.bak-cgv1 | sed 's/^/  /'
[ $? -eq 0 ] && echo "  (完全相同)"
echo
echo "########## 宿主 cgroup2 上真正启用的控制器 ##########"
echo "  cgroup.controllers = [$(cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null)]"
echo "  cgroup.subtree_control = [$(cat /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null)]"
echo "  /sys/fs/cgroup/apps 内: $(ls /sys/fs/cgroup/apps 2>/dev/null | head -20 | tr '\n' ' ')"
echo
echo "########## v1 控制器清单（/dev/memcg 等） ##########"
for c in memory cpu cpuset blkio freezer devices; do
  t=$(findmnt -no TARGET -t cgroup /dev/$c 2>/dev/null)
  [ -z "$t" ] && t=$(findmnt -no TARGET -t cgroup /dev/${c} 2>/dev/null)
  printf "  v1 %-9s mount=%s\n" "$c" "${t:-未挂载}"
done
echo "  /proc/mounts 里的 cgroup 行:"
grep " cgroup" /proc/mounts | sed 's/^/    /'
echo
echo "########## droidspaces 的 cgroup 相关能力 ##########"
strings /data/local/Droidspaces/bin/droidspaces 2>/dev/null | grep -iE "^--?(cgroup|force-cg|no-cg)" | sort -u | sed 's/^/    /'
/data/local/Droidspaces/bin/droidspaces --help 2>&1 | grep -iE "cgroup|hw-access|rootfs" | sed 's/^/    /'
