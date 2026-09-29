#!/usr/bin/env python3
"""nodelalloc vs delalloc A/B（运行时 remount，非持久，容器重启即自动恢复）
另附 fork+exec 5000 次精确测量
"""
import os, statistics, subprocess, time

def mounts():
    for ln in open("/proc/mounts"):
        p = ln.split()
        if p[1] == "/":
            return p[3]
    return "?"

def w_large(d, mb=256):
    p = os.path.join(d, ".dl.tmp")
    buf = b"\0" * (1 << 20)
    t0 = time.monotonic()
    with open(p, "wb") as f:
        for _ in range(mb):
            f.write(buf)
        f.flush(); os.fsync(f.fileno())
    dt = (time.monotonic() - t0) * 1000
    os.unlink(p)
    return dt

def w_many(d, n=500):
    t0 = time.monotonic()
    payload = b"x" * 8192
    for i in range(n):
        with open(os.path.join(d, ".dl%03d" % i), "wb") as f:
            f.write(payload)
    for i in range(n):
        os.unlink(os.path.join(d, ".dl%03d" % i))
    return (time.monotonic() - t0) * 1000

print("===== remount 现状 =====")
print("  / 挂载选项: %s" % mounts())

print()
print("===== A: 现状（nodelalloc）=====")
a1 = [w_large("/") for _ in range(3)]
a2 = [w_many("/") for _ in range(3)]
print("  写256MB  raw=%s  med=%.0f ms (%.0f MB/s)"
      % (["%.0f" % x for x in a1], statistics.median(a1),
         256000.0 / statistics.median(a1)))
print("  500x8KB  raw=%s  med=%.0f ms"
      % (["%.0f" % x for x in a2], statistics.median(a2)))

print()
print("===== 执行 remount,delalloc =====")
r = subprocess.run(["mount", "-o", "remount,delalloc", "/"],
                   capture_output=True, text=True)
print("  rc=%d  %s%s" % (r.returncode, r.stdout.strip(), r.stderr.strip()))
print("  / 挂载选项: %s" % mounts())

if r.returncode == 0:
    print()
    print("===== B: delalloc =====")
    b1 = [w_large("/") for _ in range(3)]
    b2 = [w_many("/") for _ in range(3)]
    print("  写256MB  raw=%s  med=%.0f ms (%.0f MB/s)"
          % (["%.0f" % x for x in b1], statistics.median(b1),
             256000.0 / statistics.median(b1)))
    print("  500x8KB  raw=%s  med=%.0f ms"
          % (["%.0f" % x for x in b2], statistics.median(b2)))
    print()
    ma, mb = statistics.median(a1), statistics.median(b1)
    print("  === 提升: 大文件写 %.0f -> %.0f ms  (%.2fx) ===" % (ma, mb, ma / mb))
    na, nb = statistics.median(a2), statistics.median(b2)
    print("  === 提升: 小文件   %.0f -> %.0f ms  (%.2fx) ===" % (na, nb, na / nb))

    print()
    print("===== 回滚 remount,nodelalloc =====")
    r2 = subprocess.run(["mount", "-o", "remount,nodelalloc", "/"],
                        capture_output=True, text=True)
    print("  rc=%d  %s%s" % (r2.returncode, r2.stdout.strip(), r2.stderr.strip()))
    print("  / 挂载选项: %s" % mounts())

print()
print("===== fork+exec 5000 次（/proc/uptime 单调）=====")
sh = r'''
u() { cut -d' ' -f1 /proc/uptime; }
S=$(u); i=0; while [ $i -lt 5000 ]; do /usr/bin/true; i=$((i+1)); done; E=$(u)
python3 -c "print('  shell_fork_exec_5000_s=%.3f  (%.3f ms/proc)' % ($E-$S, ($E-$S)*1000/5000))"
'''
for _ in range(2):
    print(subprocess.run(["/bin/sh", "-c", sh], capture_output=True,
                         text=True).stdout.strip())
print("===== END =====")
