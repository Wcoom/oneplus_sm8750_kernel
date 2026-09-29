echo "=== standalone/releases 内容 ==="
ls -la /root/.codex/packages/standalone/releases/ | sed 's/^/  /'
echo "=== 各 release 目录独立占用（逐个 du，不用通配符）==="
for d in /root/.codex/packages/standalone/releases/*/; do
  echo "  $(du -sh "$d" 2>/dev/null | cut -f1)  $d"
done
echo "=== app-server-daemon/releases 内容 ==="
ls -la /root/.codex/packages/app-server-daemon/releases/ | sed 's/^/  /'
echo "=== 版本标记文件 ==="
echo "  standalone/auto-update-version : $(cat /root/.codex/packages/standalone/auto-update-version 2>&1)"
echo "  app-server-daemon/auto-update-version : $(cat /root/.codex/packages/app-server-daemon/auto-update-version 2>&1)"
echo "  /root/.codex/version.json : $(head -c 200 /root/.codex/version.json 2>&1)"
echo "=== 两个 current 指向 ==="
echo "  standalone: $(readlink /root/.codex/packages/standalone/current)"
echo "  app-server: $(readlink /root/.codex/packages/app-server-daemon/current)"
echo "=== packages 真实总占用（-x 不跨文件系统，逐个独立算）==="
echo "  standalone releases 全部: $(du -sh --exclude=current /root/.codex/packages/standalone/releases 2>/dev/null | cut -f1)"
