sec(){ echo; echo "##### $* #####"; }
sec "Droidspaces 环境注入点"
echo "[/etc/profile.d/droidspaces_env.sh]"; cat /etc/profile.d/droidspaces_env.sh 2>/dev/null
echo "[/etc/profile.d/01-locale-fix.sh]"; cat /etc/profile.d/01-locale-fix.sh 2>/dev/null
sec "apt 源与网络"
echo "[sources.list]"; cat /etc/apt/sources.list 2>/dev/null | grep -v '^#' | head
echo "[sources.list.d]"; ls /etc/apt/sources.list.d/ 2>/dev/null; for f in /etc/apt/sources.list.d/*.sources /etc/apt/sources.list.d/*.list; do [ -f "$f" ] && echo "--- $f" && grep -vE '^\s*#' "$f" | head -12; done
echo "[apt 代理配置]"; cat /etc/apt/apt.conf.d/*proxy* 2>/dev/null; grep -rn "Proxy" /etc/apt/apt.conf.d/ 2>/dev/null | head -5
sec "apt 可达性（只读探测）"
timeout 25 apt-get -s install --no-download make 2>&1 | tail -8
sec "已装/可装的关键包"
for p in build-essential make cmake ninja-build clang lld rustc cargo ccache nodejs npm pnpm python3-pip pkg-config autoconf automake libtool gdb; do
  st=$(dpkg-query -W -f='${Status}' "$p" 2>/dev/null)
  case "$st" in *"install ok installed"*) echo "  已装   $p";; *) echo "  未装   $p";; esac
done
sec "apt 缓存与归档占用"
du -sh /var/cache/apt /var/lib/apt/lists 2>/dev/null
sec "时间同步"
echo "[timesyncd 状态]"; systemctl is-enabled systemd-timesyncd 2>&1; systemctl is-active systemd-timesyncd 2>&1
echo "[chrony/ntp]"; command -v chronyd ntpd 2>/dev/null || echo "(无)"
echo "[当前时间 vs 宿主]"; date -u; cat /proc/uptime
sec "ulimit 可调性"
echo "[hard limits]"; ulimit -Hn; ulimit -Hl; ulimit -Hs; ulimit -Hu
echo "[当前]"; ulimit -Sn; ulimit -Sl; ulimit -Ss; ulimit -Su
echo "[/etc/security/limits.conf 有效行]"; grep -vE '^\s*#|^\s*$' /etc/security/limits.conf 2>/dev/null | head
echo "[/etc/security/limits.d]"; ls /etc/security/limits.d/ 2>/dev/null; cat /etc/security/limits.d/*.conf 2>/dev/null | grep -vE '^\s*#' | head
echo "[systemd DefaultLimitNOFILE]"; grep -rn "DefaultLimit" /etc/systemd/system.conf 2>/dev/null | head
sec "bun / claude / codex 实况"
echo "[bun]"; /root/.bun/bin/bun --version 2>&1 | head -2
echo "[claude]"; /root/.local/bin/claude --version 2>&1 | head -2
echo "[codex]"; /root/.local/bin/codex --version 2>&1 | head -2
echo "[/root/code]"; ls /root/code 2>/dev/null | head
echo "[codex packages 内容]"; ls /root/.codex/packages 2>/dev/null | head -10; du -sh /root/.codex/packages/* 2>/dev/null | sort -rh | head -8
sec "内核可调项在容器内的可见性（只读）"
for f in /proc/sys/vm/swappiness /proc/sys/vm/dirty_ratio /proc/sys/vm/overcommit_memory /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/lru_gen/min_ttl_ms; do
  printf '  %-50s w=%s r=%s\n' "$f" "$([ -w $f ] && echo yes || echo no)" "$(cat $f 2>/dev/null | head -1)"
done
echo
echo "##### 结束 #####"
