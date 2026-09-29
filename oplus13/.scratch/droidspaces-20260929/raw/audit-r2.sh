echo "############ 1. 出处裁决：dpkg 是否认领这些文件 ############"
for f in /etc/environment /etc/bash.bashrc /etc/profile.d/99-ds-path.sh \
         /etc/profile.d/droidspaces_env.sh /etc/security/limits.d/99-ds-dev.conf \
         /etc/systemd/system.conf.d/99-ds-dev.conf /etc/systemd/journald.conf.d/99-ds-dev.conf \
         /etc/systemd/system/fstrim.timer.d/override.conf \
         /etc/systemd/system/fstrim.service.d/override.conf \
         /etc/systemd/system/systemd-udev-trigger.service.d/override.conf \
         /etc/systemd/system/systemd-networkd-wait-online.service \
         /etc/systemd/system/systemd-journald-audit.socket \
         /etc/localtime /etc/timezone; do
  pkg=$(dpkg -S "$f" 2>/dev/null | cut -d: -f1)
  if [ -n "$pkg" ]; then own="dpkg:$pkg"; else own="未认领(→外部创建)"; fi
  printf "  %-62s %-28s mtime=%s\n" "$f" "$own" "$(stat -c%y "$f" 2>/dev/null | cut -d. -f1)"
done
echo
echo "  [udev-trigger override 内容]"
cat /etc/systemd/system/systemd-udev-trigger.service.d/override.conf 2>/dev/null | sed 's/^/      /'
echo
echo "############ 2. 各改动文件全文（判断去留用） ############"
for f in /etc/profile.d/99-ds-path.sh /etc/security/limits.d/99-ds-dev.conf \
         /etc/systemd/system.conf.d/99-ds-dev.conf /etc/systemd/journald.conf.d/99-ds-dev.conf; do
  echo "--- $f ---"; cat "$f" | sed 's/^/    /'
done
echo "--- /etc/bash.bashrc 第 75-95 行 ---"
sed -n '75,95p' /etc/bash.bashrc | cat -A | sed 's/\$$//' | sed 's/^/    /'
echo
echo "--- /root/.ds-opt/environment.orig（真正的原始值）---"
cat /root/.ds-opt/environment.orig | sed 's/^/    /'
echo "--- /root/.ds-opt/99-ds-path.sh.orig ---"
cat /root/.ds-opt/99-ds-path.sh.orig | sed 's/^/    /'
echo "--- /root/.ds-opt/ccache-shims.list ---"
cat /root/.ds-opt/ccache-shims.list | sed 's/^/    /'
echo
echo "############ 3. ccache 现状（为何 show-config 为空） ############"
echo "  which ccache : $(command -v ccache || echo '不在 PATH')"
echo "  /usr/bin/ccache 存在? $([ -x /usr/bin/ccache ] && echo yes || echo no)"
echo "  /usr/lib/ccache 存在? $([ -d /usr/lib/ccache ] && echo yes || echo no)"
/usr/bin/ccache --version 2>&1 | head -2 | sed 's/^/    /'
echo "  --- 直调 /usr/bin/ccache --show-config ---"
/usr/bin/ccache --show-config 2>&1 | grep -E "cache_dir|max_size|compression" | sed 's/^/    /'
echo "  --- 配置文件位置探测 ---"
for p in /etc/ccache.conf /root/.ccache.conf /root/.config/ccache/ccache.conf /mnt/data/ds-build/ccache/ccache.conf; do
  [ -f "$p" ] && { echo "    [有] $p"; cat "$p" | sed 's/^/         /'; } || echo "    [无] $p"
done
echo
echo "############ 4. 上一轮装的软件包（B 组） ############"
echo "  apt-install.log 中被显式安装的包："
grep -oP '^Setting up \K[^ ]+' /root/.ds-opt/apt-install.log 2>/dev/null | sort -u | tr '\n' ' ' | fold -w 100 | sed 's/^/    /'
echo
echo "  apt 标记为手动安装且在上轮时间窗内安装的包："
grep -E "^(Start-Date|Commandline)" /var/log/apt/history.log 2>/dev/null | tail -8 | sed 's/^/    /'
echo
echo "############ 5. benchmark / 临时产物残留 ############"
for d in /mnt/data/.b3 /tmp/b3 /mnt/data/.bench /root/.b3 /mnt/data/tmp; do
  if [ -e "$d" ]; then echo "    [残留] $d  $(du -sh "$d" 2>/dev/null | cut -f1)"; ls "$d" 2>/dev/null | head -5 | sed 's/^/           /'; fi
done
echo "    /tmp 非 systemd-private 项：$(ls /tmp | grep -v systemd-private | tr '\n' ' ')"
echo "    /mnt/data 顶层：$(ls -A /mnt/data | tr '\n' ' ')"
echo
echo "############ 6. host 侧脚本残留（容器可见的 /mnt/data/local/tmp） ############"
ls -la /mnt/data/local/tmp/ 2>/dev/null | sed 's/^/    /'
