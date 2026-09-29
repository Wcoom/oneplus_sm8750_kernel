echo "########## 1. droidspaces 配置：hw-access 是否开启 ##########"
for f in /data/local/Droidspaces/Containers/ubuntu/config /data/local/Droidspaces/Containers/ubuntu/*.conf \
         /data/local/Droidspaces/Containers/ubuntu/config.json /data/local/Droidspaces/config; do
  [ -f "$f" ] && { echo "  --- $f ---"; cat "$f" | sed 's/^/    /'; }
done
echo "  --- 容器目录内容 ---"
ls -la /data/local/Droidspaces/Containers/ubuntu/ 2>/dev/null | sed 's/^/    /'
echo
echo "########## 2. 运行中实例的真实开关（从进程 cmdline） ##########"
for p in 114219 114220; do
  [ -d /proc/$p ] || continue
  echo "  PID $p cmdline: $(tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)"
done
echo "  --- droidspaces 主进程 ---"
ps -A -o pid,args 2>/dev/null | grep -E "droidspaces" | grep -v grep | sed 's/^/    /'
echo
echo "########## 3. loop50 的 dio 值（§11 双层页缓存的关键） ##########"
for f in dio autoclear partscan offset sizelimit; do
  printf "  loop50/%s = %s\n" "$f" "$(cat /sys/block/loop50/loop/$f 2>/dev/null)"
done
echo "  解读: dio=0 → 走 buffered I/O → 同一份数据在 ext4 与 f2fs 各缓存一次"
echo
echo "########## 4. 宿主 /apps 的 v1 详细（LMKD 视角） ##########"
for f in memory.usage_in_bytes memory.max_usage_in_bytes memory.limit_in_bytes memory.soft_limit_in_bytes memory.failcnt; do
  printf "  /apps/%-30s = %s\n" "$f" "$(cat /dev/memcg/apps/$f 2>/dev/null)"
done
echo "  --- lmkd 的 PSI 阈值 ---"
getprop 2>/dev/null | grep -iE "lmkd.*(psi|thrash|critical|threshold)" | sed 's/^/    /'
echo
echo "########## 5. 容器进程是否真在 /apps（逐个核对） ##########"
for p in $(ps -A -o pid 2>/dev/null | head -400); do
  [ -r /proc/$p/cgroup ] || continue
  c=$(awk -F: '$2=="memory"{print $3}' /proc/$p/cgroup 2>/dev/null)
  case "$c" in /droidspaces*|/apps/droidspaces*) echo "    PID $p 在独立 cgroup: $c  ($(cat /proc/$p/comm 2>/dev/null))";; esac
done
echo "    (无输出 = 没有任何进程被放进 droidspaces 专属 memory cgroup)"
echo
echo "########## 6. /sys/fs/cgroup 在宿主上的真相 ##########"
echo "  host /sys/fs/cgroup 内容: $(ls /sys/fs/cgroup 2>/dev/null | head -8 | tr '\n' ' ')"
echo "  host v1 memory 控制器: $(findmnt -no TARGET,FSTYPE /dev/memcg 2>/dev/null)"
echo "  v2 是否真挂在宿主上: $(grep -c cgroup2 /proc/mounts) 处"
grep cgroup2 /proc/mounts | sed 's/^/    /'
