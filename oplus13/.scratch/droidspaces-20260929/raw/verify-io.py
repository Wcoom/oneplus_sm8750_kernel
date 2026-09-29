#!/usr/bin/env python3
"""交错采样：消除宿主 I/O 竞争随时间漂移带来的顺序偏差
对比 tmpfs / rootfs(ext4-on-loop) / f2fs-direct 三类落点
"""
import os, statistics, subprocess, time

TARGETS = [("/tmp", "tmpfs"), ("/", "ext4loop"), ("/mnt/data", "f2fs")]
REPS = 5


def w_large(d):
    """256MB 顺序写 + fsync"""
    p = os.path.join(d, ".v_large.tmp")
    buf = b"\0" * (1 << 20)
    t0 = time.monotonic()
    with open(p, "wb") as f:
        for _ in range(256):
            f.write(buf)
        f.flush(); os.fsync(f.fileno())
    dt = (time.monotonic() - t0) * 1000
    os.unlink(p)
    return dt


def w_tree(d):
    """500 个 8KB 文件（模拟源码树），返回 ms"""
    t0 = time.monotonic()
    payload = b"x" * 8192
    for i in range(500):
        with open(os.path.join(d, ".v_t%03d" % i), "wb") as f:
            f.write(payload)
    for i in range(500):
        os.unlink(os.path.join(d, ".v_t%03d" % i))
    return (time.monotonic() - t0) * 1000


def r_seq(d):
    """读回 256MB（先写一次再读，避免冷设备偏差）"""
    p = os.path.join(d, ".v_read.tmp")
    buf = b"y" * (1 << 20)
    with open(p, "wb") as f:
        for _ in range(256):
            f.write(buf)
        f.flush(); os.fsync(f.fileno())
    t0 = time.monotonic()
    with open(p, "rb") as f:
        while f.read(1 << 20):
            pass
    dt = (time.monotonic() - t0) * 1000
    os.unlink(p)
    return dt


res = {}
for name, fn in [("写256MB+fsync", w_large), ("500x8KB建+删", w_tree),
                 ("读256MB", r_seq)]:
    for d, lbl in TARGETS:
        res[(name, lbl)] = []
    for r in range(REPS):
        for d, lbl in TARGETS:          # 交错：每轮都遍历三个目标
            try:
                res[(name, lbl)].append(fn(d))
            except Exception as e:
                res[(name, lbl)].append(float("nan"))
                print("  ERR %s %s: %s" % (name, lbl, e))

print("===== 交错采样 [monotonic, %d reps] %s =====" %
      (REPS, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
print("loadavg: %s" % open("/proc/loadavg").read().strip())
print()

for name in ["写256MB+fsync", "500x8KB建+删", "读256MB"]:
    print("--- %s ---" % name)
    rows = []
    for d, lbl in TARGETS:
        xs = res[(name, lbl)]
        m = statistics.median(xs)
        rows.append((lbl, min(xs), m, xs))
    for lbl, lo, m, xs in rows:
        print("  %-12s min=%.0f  med=%.0f ms   raw=%s"
              % (lbl, lo, m, ["%.0f" % x for x in xs]))
    base = rows[-1][2]
    for lbl, lo, m, xs in rows:
        print("      %-12s 相对 f2fs: %.2fx" % (lbl, m / base))
    print()

# ---- fork+exec 用 /proc/uptime（单调、10ms 分辨率），shell 层测量 ----
print("--- fork+exec 1000 次（/proc/uptime 单调时钟，shell 层）---")
sh = r'''
u() { cut -d' ' -f1 /proc/uptime; }
S=$(u); i=0; while [ $i -lt 1000 ]; do /usr/bin/true; i=$((i+1)); done; E=$(u)
echo "  shell_fork_exec_1000_s=$(echo "$E - $S" | bc 2>/dev/null || python3 -c "print($E-$S)")"
'''
for r in range(3):
    print(subprocess.run(["/bin/sh", "-c", sh], capture_output=True,
                         text=True).stdout.strip())

print()
print("===== END =====")
