echo "=== standalone 下的版本 ==="
ls -la /root/.codex/packages/standalone/ | sed 's/^/  /'
echo "=== app-server-daemon 下的版本 ==="
ls -la /root/.codex/packages/app-server-daemon/ | sed 's/^/  /'
echo "=== 各版本占用 ==="
du -sh /root/.codex/packages/standalone/*/ 2>/dev/null | sed 's/^/  /'
du -sh /root/.codex/packages/app-server-daemon/*/ 2>/dev/null | sed 's/^/  /'
echo "=== 正在运行的 codex 进程 ==="
ps -eo pid,etime,cmd | grep -i "[c]odex" | head -10 | sed 's/^/  /'
echo "=== codex 可执行文件指向 ==="
echo "  command -v: $(command -v codex)"
echo "  实际指向  : $(readlink -f $(command -v codex) 2>&1)"
echo "=== /root/.codex 一级占用 ==="
du -sh /root/.codex/* 2>/dev/null | sort -h | sed 's/^/  /'
echo "=== 宿主侧：codex 进程 ==="
