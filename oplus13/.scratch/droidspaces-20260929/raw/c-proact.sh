F=/proc/sys/vm/compaction_proactiveness
echo "===== 容器内视角 ====="
echo "  文件: $F"
echo "  ls: $(ls -l $F 2>&1)"
echo "  挂载类型: $(grep -E ' /proc/sys| /proc ' /proc/self/mountinfo | head -3)"
echo "  当前读值: $(cat $F 2>&1)"
echo "  /proc/sys 是否 ro: $(awk '$5=="/proc/sys"{print $6}' /proc/self/mountinfo)"
echo
echo "===== 容器内写入往返测试（改的是宿主全局值，测完恢复）====="
if echo 20 > $F 2>/dev/null; then
  echo "  写入 20: 成功"
  echo "  容器内回读: $(cat $F)"
  echo "  >>> 若宿主侧同时读到 20，则该 sysctl 未做命名空间隔离，容器可影响宿主"
  sleep 2
  echo 0 > $F
  echo "  已恢复 0，容器内回读: $(cat $F)"
else
  echo "  写入失败（只读挂载或权限不足）: $(echo 20 > $F 2>&1)"
  echo "  >>> 容器无法影响该宿主全局值"
fi
echo
echo "===== 宿主侧应同步显示（证明是同一条全局链）====="
