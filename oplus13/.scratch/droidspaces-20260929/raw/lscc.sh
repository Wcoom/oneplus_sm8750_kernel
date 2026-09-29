echo "--- /usr/lib/ccache shim 清单 ---"
ls -1 /usr/lib/ccache/ | tr '\n' ' '; echo
echo "  共 $(ls -1 /usr/lib/ccache/ | wc -l) 个"
echo "--- 抽查两个实体 ---"
ls -l /usr/lib/ccache/gcc /usr/lib/ccache/clang 2>&1
