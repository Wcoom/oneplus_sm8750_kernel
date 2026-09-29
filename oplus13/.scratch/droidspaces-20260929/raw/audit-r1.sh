echo "############ A. 备份区：上一轮改动的原始值凭证 ############"
echo "--- /root/.ds-opt/ 全部内容 ---"
ls -la /root/.ds-opt/ 2>/dev/null | sed 's/^/  /'
echo
echo "############ B. 环境变量与 shell 层 ############"
for f in /etc/environment /etc/profile.d/99-ds-path.sh /etc/profile.d/droidspaces_env.sh \
         /etc/bash.bashrc /etc/security/limits.d/99-ds-dev.conf \
         /etc/systemd/system.conf.d/99-ds-dev.conf /etc/systemd/journald.conf.d/99-ds-dev.conf; do
  if [ -e "$f" ]; then
    printf "  [存在] %-52s %s 字节\n" "$f" "$(stat -c%s "$f")"
  else
    printf "  [缺失] %s\n" "$f"
  fi
done
echo
echo "--- /etc/environment 全文 ---"
cat /etc/environment 2>/dev/null | sed 's/^/    /'
echo
echo "--- /etc/environment 与两个备份的差异 ---"
for b in environment.orig environment.bak-pre-dsbuild environment.bak-pre-s4s6; do
  if [ -f /root/.ds-opt/$b ]; then
    n=$(diff /root/.ds-opt/$b /etc/environment 2>/dev/null | grep -c '^[<>]')
    printf "    vs %-28s 差异行数 %s\n" "$b" "$n"
  fi
done
echo
echo "--- profile.d 目录全部文件 ---"
ls -la /etc/profile.d/ 2>/dev/null | sed 's/^/    /'
echo
echo "--- bash.bashrc 中带标记的段 ---"
grep -n "ds-dev-ulimit\|ds-dev-ccache\|DS_DEV\|ds-opt" /etc/bash.bashrc 2>/dev/null | sed 's/^/    /'
echo
echo "############ C. systemd unit / override / timer ############"
echo "--- 所有 override.conf ---"
find /etc/systemd/system -name "override.conf" 2>/dev/null | sed 's/^/    /'
echo "--- 上一轮创建的 drop-in 目录内容 ---"
for d in /etc/systemd/system/fstrim.timer.d /etc/systemd/system/fstrim.service.d; do
  [ -d "$d" ] && { echo "  [$d]"; cat $d/*.conf 2>/dev/null | sed 's/^/      /'; }
done
echo "--- fstrim timer/service 当前状态 ---"
systemctl is-enabled fstrim.timer 2>/dev/null | sed 's/^/    enabled: /'
systemctl is-active  fstrim.timer 2>/dev/null | sed 's/^/    active : /'
echo "--- fstrim.timer 下次触发 ---"
systemctl list-timers fstrim.timer --no-pager 2>/dev/null | sed 's/^/    /'
echo
echo "--- 是否存在非发行版自带的 unit ---"
for u in $(systemctl list-unit-files --no-pager --no-legend 2>/dev/null | awk '{print $1}'); do
  p=$(systemctl show -p FragmentPath --value "$u" 2>/dev/null)
  case "$p" in /etc/systemd/system/*|/usr/local/*) echo "    [$u] $p";; esac
done
echo
echo "############ D. cron / timer / service 残留 ############"
echo "--- crontab ---"
crontab -l 2>/dev/null | sed 's/^/    /' || echo "    (无)"
ls -la /etc/cron.d/ 2>/dev/null | sed 's/^/    /'
echo "--- 用户自建 timer ---"
systemctl list-timers --all --no-pager 2>/dev/null | head -12 | sed 's/^/    /'
echo
echo "############ E. 用户空间调优产物 ############"
echo "--- build-performance-* 脚本（任务书点名）---"
ls -la ~/bin/build-performance-* /usr/local/bin/build-performance-* 2>/dev/null | sed 's/^/    /' || echo "    (不存在)"
echo "--- ~/bin 目录 ---"
ls -la ~/bin/ 2>/dev/null | sed 's/^/    /' || echo "    (不存在)"
echo "--- uclamp/cpuset 脚本 ---"
find /root /usr/local/bin /etc -maxdepth 3 \( -name "*uclamp*" -o -name "*cpuset*" -o -name "*performance-on*" -o -name "*build-perf*" \) 2>/dev/null | sed 's/^/    /'
echo
echo "############ F. ccache / 构建缓存 ############"
echo "--- /usr/local/bin 中的 shim 与链接 ---"
ls -la /usr/local/bin/ 2>/dev/null | sed 's/^/    /'
echo "--- /root/.ccache.conf（A4 死文件）---"
[ -f /root/.ccache.conf ] && cat /root/.ccache.conf | sed 's/^/    /' || echo "    (不存在)"
echo "--- /etc/ccache.conf ---"
[ -f /etc/ccache.conf ] && cat /etc/ccache.conf | sed 's/^/    /' || echo "    (不存在)"
echo "--- ccache 当前生效配置与 origin ---"
ccache --show-config 2>/dev/null | grep -E "^(cache_dir|max_size|compression|compression_level)" | sed 's/^/    /'
echo "--- /mnt/data/ds-build 占用 ---"
du -sh /mnt/data/ds-build/* 2>/dev/null | sed 's/^/    /'
echo
echo "############ G. 时区 ############"
echo "    /etc/localtime -> $(readlink /etc/localtime)"
echo "    /etc/timezone   = $(cat /etc/timezone 2>/dev/null)"
echo "    备份 localtime.orig: $(readlink /root/.ds-opt/localtime.orig 2>/dev/null || echo '非链接')"
echo
echo "############ H. benchmark / 临时测试文件 ############"
echo "--- 容器内 /tmp ---"
ls -la /tmp/ 2>/dev/null | head -20 | sed 's/^/    /'
echo "--- 遗留测试目录 ---"
for d in /tmp/b3 /tmp/bench* /mnt/data/.b3 /mnt/data/.bench* /root/.b3; do
  [ -e "$d" ] && echo "    [存在] $d" || true
done
echo "    (以上为空则无残留)"
