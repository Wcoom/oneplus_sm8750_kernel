#!/usr/bin/env python3
"""可信基准：time.monotonic() 单调时钟 + 3 次重复取中位数
修正 bench.sh 的 CLOCK_REALTIME 跳变问题（曾出现负值 -818ms）
"""
import os, statistics, subprocess, sys, time

TMP = "/tmp"
ROOT = "/"
F2FS = "/mnt/data"
WORK = "/mnt/data/.dsb2"


def med(fn, reps=3):
    xs = []
    for _ in range(reps):
        xs.append(fn())
    return min(xs), statistics.median(xs), xs


def b4(d):
    """写 256MB + fsync，返回 ms"""
    p = os.path.join(d, ".dsb_w.tmp")
    buf = b"\0" * (1 << 20)
    t0 = time.monotonic()
    with open(p, "wb") as f:
        for _ in range(256):
            f.write(buf)
        f.flush()
        os.fsync(f.fileno())
    dt = (time.monotonic() - t0) * 1000
    os.unlink(p)
    return dt


def b5(d):
    """建 2000 空文件 / 删 2000，返回 (create_ms, delete_ms)"""
    t0 = time.monotonic()
    for i in range(2000):
        open(os.path.join(d, "dsbf%04d" % i), "w").close()
    tc = (time.monotonic() - t0) * 1000
    t0 = time.monotonic()
    for i in range(2000):
        os.unlink(os.path.join(d, "dsbf%04d" % i))
    td = (time.monotonic() - t0) * 1000
    return (tc, td)


def b8():
    """分配 512MB 并每页触碰一次，返回 ms"""
    n = 512 * 1024 * 1024
    t0 = time.monotonic()
    b = bytearray(n)
    for i in range(0, n, 4096):
        b[i] = 1
    dt = (time.monotonic() - t0) * 1000
    del b
    return dt


def b3():
    """fork+exec 1000 次 /bin/true，返回 ms"""
    t0 = time.monotonic()
    for _ in range(1000):
        pid = os.fork()
        if pid == 0:
            os.execv("/usr/bin/true", ["true"])
            os._exit(1)
        os.waitpid(pid, 0)
    return (time.monotonic() - t0) * 1000


def b7():
    """python3 冷启动 100 次，返回 ms"""
    t0 = time.monotonic()
    for _ in range(100):
        subprocess.run(["/usr/bin/python3", "-c", "pass"])
    return (time.monotonic() - t0) * 1000


def b6(src):
    """gcc -O2 编译同一 TU 5 次（显式 /usr/bin/gcc，绕开 ccache），返回 ms"""
    t0 = time.monotonic()
    for _ in range(5):
        subprocess.run(["/usr/bin/gcc", "-O2", "-c", "-o", "/tmp/.dsb.o", src],
                       check=True)
    return (time.monotonic() - t0) * 1000


def main():
    os.makedirs(WORK, exist_ok=True)
    print("===== 可信基准 [monotonic, 3 reps, median] %s ====="
          % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))
    print("kernel: %s" % os.uname().release)
    print("loadavg(宿主全局): %s" % open("/proc/loadavg").read().strip())
    mi = {}
    for ln in open("/proc/meminfo"):
        k, v = ln.split(":", 1)
        mi[k] = int(v.split()[0])
    print("memfree_kb: %d  memavail_kb: %d  swaptotal-free_kb: %d"
          % (mi["MemFree"], mi["MemAvailable"], mi["SwapTotal"] - mi["SwapFree"]))
    print()

    # ---- B4 ----
    print("--- B4 顺序写 256MB + fsync ---")
    for d, lbl in [(TMP, "tmpfs"), (ROOT, "rootfs_ext4_loop"),
                   (F2FS, "f2fs_direct")]:
        try:
            lo, m, xs = med(lambda d=d: b4(d))
            print("  B4_%-18s min=%.0f med=%.0f ms  (%.0f MB/s med)  raw=%s"
                  % (lbl, lo, m, 256000.0 / m, ["%.0f" % x for x in xs]))
        except Exception as e:
            print("  B4_%-18s ERROR %s" % (lbl, e))
    print()

    # ---- B5 ----
    print("--- B5 小文件创建/删除 2000 ---")
    for d, lbl in [(TMP, "tmpfs"), (ROOT, "rootfs_ext4_loop"),
                   (F2FS, "f2fs_direct")]:
        try:
            lo, m, xs = med(lambda d=d: b5(d))
            print("  B5_%-18s create med=%.0f ms  delete med=%.0f ms  raw=%s"
                  % (lbl, m[0], m[1],
                     ["%.0f/%.0f" % (a, b) for a, b in xs]))
        except Exception as e:
            print("  B5_%-18s ERROR %s" % (lbl, e))
    print()

    # ---- B8 ----
    print("--- B8 分配 + 触碰 512MB（每页一次）---")
    lo, m, xs = med(b8)
    print("  B8_alloc_touch_512M  min=%.0f med=%.0f ms  (%.0f MB/s)  raw=%s"
          % (lo, m, 512000.0 / m, ["%.0f" % x for x in xs]))
    print()

    # ---- B3 ----
    print("--- B3 fork+exec 1000 ---")
    lo, m, xs = med(b3)
    print("  B3_fork_exec_1000    min=%.0f med=%.0f ms  (%.2f ms/进程)  raw=%s"
          % (lo, m, m / 1000.0, ["%.0f" % x for x in xs]))
    print()

    # ---- B7 ----
    print("--- B7 python3 启动 100 ---")
    lo, m, xs = med(b7)
    print("  B7_python_start_100  min=%.0f med=%.0f ms  (%.2f ms/次)  raw=%s"
          % (lo, m, m / 100.0, ["%.0f" % x for x in xs]))
    print()

    # ---- B6 ----
    print("--- B6 gcc -O2 同一 TU x5（/usr/bin/gcc 直调，不经 ccache）---")
    src = "/tmp/.dsb_src.c"
    with open(src, "w") as f:
        f.write("#include <stdio.h>\n#include <math.h>\n#include <string.h>\n")
        for i in range(800):
            f.write("static double f%d(double x){return sin(x*%d)+cos(x/(%d.0));}\n"
                    % (i, i + 1, i + 1))
        f.write("double t(double x){double s=0;\n")
        for i in range(800):
            f.write("  s+=f%d(x);\n" % i)
        f.write("  return s;}\nint main(void){printf(\"%f\\n\",t(1.5));return 0;}\n")
    lo, m, xs = med(lambda: b6(src))
    print("  B6_gcc5  min=%.0f med=%.0f ms  (%.0f ms/次)  raw=%s"
          % (lo, m, m / 5.0, ["%.0f" % x for x in xs]))
    print()

    # ---- B9 系统压力指标（累计值，需与基线比较）----
    print("--- B9 内存压力指标（累计）---")
    for f, keys in [("/proc/vmstat",
                     ["pswpin", "pswpout", "allocstall_normal",
                      "allocstall_movable", "pgmajfault", "pgsteal_direct",
                      "pgscan_direct", "compact_stall", "compact_fail"]),
                    ("/proc/pressure/cpu", None),
                    ("/proc/pressure/memory", None),
                    ("/proc/pressure/io", None)]:
        if keys:
            d = {}
            for ln in open(f):
                p = ln.split()
                if p[0] in keys:
                    d[p[0]] = p[1]
            for k in keys:
                print("  %s %s" % (k, d.get(k, "?")))
        else:
            some = full = ""
            for ln in open(f):
                if ln.startswith("some"):
                    some = ln.strip()
                elif ln.startswith("full"):
                    full = ln.strip()
            print("  %s | some %s" % (f.split("/")[-1],
                                      some.split("avg10=")[1].split()[0]))
            print("  %s | full %s" % (f.split("/")[-1],
                                      full.split("avg10=")[1].split()[0]))
    print()
    print("===== END =====")


main()
