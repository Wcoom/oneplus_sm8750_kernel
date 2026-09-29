#!/usr/bin/env python3
"""与 bench.sh 逐字同源的可比复测（monotonic 单调时钟，3 reps 取中位数）
B6 源 = bench.sh 的 400 函数 x 40 次内循环（804 行）
"""
import statistics, subprocess, time

B = "/mnt/data/.b3"
import os
os.makedirs(B, exist_ok=True)

# ---- B6 源：与 bench.sh 完全一致 ----
with open(B + "/big.c", "w") as f:
    f.write("#include <stdio.h>\n#include <math.h>\n")
    for i in range(400):
        f.write("static double f%d(double x){ double r=0; "
                "for(int j=0;j<40;j++) r+=sin(x*%d+j)*cos(x*j+%d); return r; }\n"
                % (i, i + 1, i))
    f.write("int main(void){ double s=0;\n")
    for i in range(400):
        f.write("  s+=f%d((double)%d);\n" % (i, i))
    f.write('  printf("%f\\n", s); return 0; }\n')
print("B6 源行数 = %d" % len(open(B + "/big.c").read().splitlines()))


def timed(cmd, reps=3):
    xs = []
    for _ in range(reps):
        t0 = time.monotonic()
        subprocess.run(cmd, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)
        xs.append((time.monotonic() - t0) * 1000)
    return xs


print()
print("===== 可比复测 [monotonic, 3 reps] %s ====="
      % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
print("loadavg: %s" % open("/proc/loadavg").read().strip())
mi = {}
for ln in open("/proc/meminfo"):
    k, v = ln.split(":", 1)
    mi[k] = int(v.split()[0])
print("memfree=%dkB memavail=%dkB swap_used=%dkB"
      % (mi["MemFree"], mi["MemAvailable"], mi["SwapTotal"] - mi["SwapFree"]))
print()

# B6: 同一 TU 编译 5 次（显式 /usr/bin/gcc，绕开 ccache，与 bench.sh 语义一致）
print("--- B6 gcc -O2 同一 TU x5（/usr/bin/gcc 直调）---")
xs = timed(["/usr/bin/gcc", "-O2", "-c", "-o", B + "/big.o", B + "/big.c"], reps=5)
for i, x in enumerate(xs):
    print("  run%d = %.0f ms" % (i + 1, x))
print("  B6_gcc5_single_med=%.0f ms  5x_total=%.0f ms  (%.0f ms/次)"
      % (statistics.median(xs), statistics.median(xs) * 5,
         statistics.median(xs)))

# B7: python3 启动 100 次
print()
print("--- B7 python3 冷启动 x100 ---")


def b7():
    t0 = time.monotonic()
    for _ in range(100):
        subprocess.run(["/usr/bin/python3", "-c", "pass"])
    return (time.monotonic() - t0) * 1000


xs = [b7() for _ in range(3)]
print("  raw=%s" % ["%.0f" % x for x in xs])
print("  B7_python_start_100_med=%.0f ms  (%.2f ms/次)"
      % (statistics.median(xs), statistics.median(xs) / 100))

# B3: fork+exec 1000 次（用 /usr/bin/true，shell 层，/proc/uptime）
print()
print("--- B3 fork+exec x1000（shell 层）---")


def b3():
    t0 = time.monotonic()
    for _ in range(1000):
        pid = os.fork()
        if pid == 0:
            os.execv("/usr/bin/true", ["true"])
            os._exit(1)
        os.waitpid(pid, 0)
    return (time.monotonic() - t0) * 1000


xs = [b3() for _ in range(3)]
print("  raw=%s  (python fork 较 shell 重，仅供相对比较)"
      % ["%.0f" % x for x in xs])
print("  B3_med=%.0f ms  (%.2f ms/进程)" % (statistics.median(xs),
                                            statistics.median(xs) / 1000))

# B1/B2: openssl 单核 / 8 核
print()
print("--- B1 单核 / B2 8 核（openssl sha256 16K, 3s）---")
for n in (1, 8):
    procs = [subprocess.Popen(
        ["/usr/bin/openssl", "speed", "-seconds", "3", "sha256"],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        for _ in range(n)]
    tot = 0
    for p in procs:
        out = p.communicate()[0]
        for ln in out.splitlines():
            if ln.startswith("sha256"):
                tot += float(ln.split()[5])
    print("  B%d_%dcore_sum_16k_kps=%.0f  (%.2f GB/s)"
          % (1 if n == 1 else 2, n, tot, tot / 1e6 * 16))

subprocess.run(["rm", "-rf", B], check=False)
print()
print("===== END =====")
