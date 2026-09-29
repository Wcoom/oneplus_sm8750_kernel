echo "############ 1. droidspaces_env.sh 真身 ############"
ls -la /etc/profile.d/droidspaces_env.sh
echo "  readlink: $(readlink /etc/profile.d/droidspaces_env.sh)"
echo "  /run/droidspaces.env 内容:"; cat /run/droidspaces.env 2>/dev/null | sed 's/^/    /'
echo "  /root/.ds-opt/droidspaces_env.sh.orig:"; ls -la /root/.ds-opt/droidspaces_env.sh.orig
echo
echo "############ 2. 若无上轮 override，限额默认值是多少 ############"
echo "  --- systemd 编译默认 (DefaultLimitNOFILE/MEMLOCK) ---"
grep -rE "DefaultLimit(NOFILE|MEMLOCK)" /etc/systemd/system.conf /usr/lib/systemd/system.conf 2>/dev/null | sed 's/^/    /'
echo "  --- systemd 当前实际生效 ---"
systemctl show -p DefaultLimitNOFILE -p DefaultLimitMEMLOCK 2>/dev/null | sed 's/^/    /'
echo "  --- login 默认 limits.conf ---"
grep -vE "^\s*#|^\s*$" /etc/security/limits.conf 2>/dev/null | sed 's/^/    /'
echo "  --- 本进程实际限额（当前）---"
echo "    nofile=$(ulimit -Sn)/$(ulimit -Hn)  memlock=$(ulimit -Sl)kB  nproc=$(ulimit -Su)"
echo
echo "############ 3. /mnt/data 顶层可疑目录归属 ############"
for d in .b3 .dsb2 .recycle .thp ds-build; do
  p=/mnt/data/$d
  if [ -e "$p" ]; then
    printf "  %-14s mtime=%s  %s\n" "$d" "$(stat -c%y "$p" 2>/dev/null | cut -d. -f1)" "$(du -sh "$p" 2>/dev/null | cut -f1)"
    ls -A "$p" 2>/dev/null | head -6 | sed 's/^/                   /'
  fi
done
echo
echo "############ 4. /etc/environment 与环境变量实际注入面 ############"
echo "  --- /etc/environment 中 DS_ 标记行 ---"
grep -n "DS_\|CCACHE\|TMPDIR\|CARGO_TARGET\|GOCACHE\|GOMODCACHE\|npm_config\|BUN_INSTALL\|CMAKE_GEN" /etc/environment | sed 's/^/    /'
echo
echo "  --- 当前 shell 实际继承（非登录 run 路径）---"
for v in CCACHE_DIR CCACHE_MAXSIZE TMPDIR CARGO_TARGET_DIR GOCACHE GOMODCACHE npm_config_cache BUN_INSTALL_CACHE_DIR CMAKE_GENERATOR; do
  eval "val=\$$v"; printf "    %-24s = %s\n" "$v" "${val:-<未设置>}"
done
