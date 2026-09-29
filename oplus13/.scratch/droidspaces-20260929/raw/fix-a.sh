#!/bin/sh
# A 组修正版：修 H3（run 路径 PATH）、去 PATH 重复、补 ulimit、补 ccache 配置
# 全部可回滚，备份在 /root/.ds-opt/
B=/root/.ds-opt
mkdir -p $B
echo "=== 开始 $(date -u +%FT%TZ) ==="

# ---------- A1' 用户工具符号链接进 /usr/local/bin（真正修 H3）----------
# 裸 PATH 含 /usr/local/bin，故 run/enter/login/cron/systemd 全部可见
echo "--- A1' 符号链接 ---"
for d in /root/.local/bin /root/.bun/bin /usr/local/go/bin; do
  [ -d "$d" ] || continue
  for f in "$d"/*; do
    [ -x "$f" ] || continue
    n=$(basename "$f")
    case "$n" in
      *.js|*.ts|*.map|*.json|*.md|*README*) continue ;;
    esac
    # 已被系统包提供的名字不覆盖（如 apt 装的 node/npm）
    tgt=$(command -v "$n" 2>/dev/null)
    if [ -n "$tgt" ] && [ "$tgt" != "/usr/local/bin/$n" ]; then
      echo "  跳过 $n（系统已有 $tgt）"
      continue
    fi
    ln -sfn "$f" "/usr/local/bin/$n"
    echo "  链接 $n -> $f"
  done
done

# ---------- A2' profile.d：只补 ccache + ulimit，去掉重复项 ----------
echo "--- A2' profile.d ---"
[ -f /etc/profile.d/99-ds-path.sh ] && cp -a /etc/profile.d/99-ds-path.sh $B/99-ds-path.sh.orig
cat > /etc/profile.d/99-ds-path.sh <<'EOF'
# 开发环境：ccache 前置 + 提高资源限额
# 说明：~/.local/bin 与 ~/.bun/bin 已由 /root/.profile 与 /root/.bashrc 提供，此处不重复添加
# 回滚：rm /etc/profile.d/99-ds-path.sh
if [ -d /usr/lib/ccache ]; then
  case ":$PATH:" in
    *":/usr/lib/ccache:"*) ;;
    *) PATH="/usr/lib/ccache:$PATH"; export PATH ;;
  esac
fi
ulimit -n 524288 2>/dev/null || true
ulimit -l 8388608 2>/dev/null || true
EOF
chmod 0644 /etc/profile.d/99-ds-path.sh

# Droidspaces 预留的 env 注入点：恢复为空（内容已并入 99-ds-path.sh）
[ -f /etc/profile.d/droidspaces_env.sh ] && cp -a /etc/profile.d/droidspaces_env.sh $B/droidspaces_env.sh.orig
: > /etc/profile.d/droidspaces_env.sh
echo "  droidspaces_env.sh 已恢复为空（内容并入 99-ds-path.sh）"

# ---------- A3' bash.bashrc：交互式非登录 shell 的 ulimit ----------
echo "--- A3' /etc/bash.bashrc ---"
if ! grep -q "ds-dev-ulimit" /etc/bash.bashrc 2>/dev/null; then
  cp -a /etc/bash.bashrc $B/bash.bashrc.orig
  cat >> /etc/bash.bashrc <<'EOF'

# ds-dev-ulimit 开发限额（回滚：删除本段）
ulimit -n 524288 2>/dev/null || true
ulimit -l 8388608 2>/dev/null || true
EOF
  echo "  已追加 ds-dev-ulimit 段"
else
  echo "  已存在，跳过"
fi

# ---------- A4 ccache 配置（本次补做）----------
echo "--- A4 ccache ---"
[ -f /root/.ccache.conf ] && cp -a /root/.ccache.conf $B/ccache.conf.orig
cat > /root/.ccache.conf <<'EOF'
# ccache 配置（Droidspaces Ubuntu 开发容器）
# 注意：hash_dir 保持默认 true，不做跨目录命中优化（避免相对包含路径歧义）
max_size = 20G
compression = true
compression_level = 6
EOF
mkdir -p /root/.cache/ccache
echo "  写入 /root/.ccache.conf"
ccache --show-config 2>/dev/null | grep -E "^(max_size|compression|compression_level|cache_dir|version)" | sed 's/^/    /'

echo "=== 完成 $(date -u +%FT%TZ) ==="
echo
echo "########## 立即复验 ##########"
echo "--- run 路径（非登录非交互，裸 PATH）---"
for t in bun bunx claude codex node npm python3 cargo rustc ccache ninja cmake go; do
  p=$(command -v $t 2>/dev/null)
  [ -n "$p" ] && printf '  OK   %-8s %s\n' "$t" "$p" || printf '  MISS %-8s\n' "$t"
done
echo "  PATH=$PATH"
echo
echo "--- 登录 shell 路径 ---"
/bin/bash -lc 'echo "  PATH=$PATH"; echo "  nofile=$(ulimit -Sn)/$(ulimit -Hn)  memlock=$(ulimit -Sl) kB"'
