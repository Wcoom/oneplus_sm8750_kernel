#!/usr/bin/env python3
"""B1/B2 CPU 吞吐复测（修正 k 后缀解析）
注意：bench.sh 取的 awk $6 实为 256 字节块列，非 16K（标签有误，两侧同样取法故仍可比）
"""
import subprocess, time

def run(n, secs=3):
    ps = [subprocess.Popen(["/usr/bin/openssl","speed","-seconds",str(secs),"sha256"],
          stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True) for _ in range(n)]
    cols = None
    vals = []
    for p in ps:
        out = p.communicate()[0]
        for ln in out.splitlines():
            if ln.startswith("sha256") and "bytes" in ln:
                f = ln.split()
                # f[3:] 是各块大小的吞吐，带 k 后缀
                nums = [float(x.rstrip("k")) * (1000 if x.endswith("k") else 1)
                        for x in f[4:] if x[0].isdigit()]
                vals.append(nums)
    # 逐列求和
    ncol = min(len(v) for v in vals)
    return [sum(v[i] for v in vals) for i in range(ncol)]

print("===== B1/B2 CPU 吞吐复测 %s =====" % time.strftime("%H:%M:%SZ", time.gmtime()))
print("loadavg: %s" % open("/proc/loadavg").read().strip())
for n in (1, 8):
    sums = run(n)
    print("  %d 核并行: 各块大小总吞吐(k/s) = %s"
          % (n, ["%.0f" % s for s in sums]))
    print("     256B列=%.0f k/s  16K列=%.0f k/s" % (sums[2], sums[5]))
