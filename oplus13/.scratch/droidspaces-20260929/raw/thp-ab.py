#!/usr/bin/env python3
"""THP A/B 实测（宿主全局开关，测完必须回滚到 never）
关键：madvise 模式下只有显式 madvise(MADV_HUGEPAGE) 的进程才拿到大页，
     故准备 plain / madv 两个变体对照，否则 madvise 档必然显示零差异。
"""
import os, statistics, subprocess, time

THP = "/sys/kernel/mm/transparent_hugepage/enabled"
WORK = "/mnt/data/.thp"
os.makedirs(WORK, exist_ok=True)

SRC = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#define N   (256UL*1024*1024)
#define IDX (N/8)
int main(int argc, char **argv){
  size_t *a = malloc(N);
  if(!a){ puts("fail"); return 1; }
  if(argc>1 && argv[1][0]=='m')
    madvise(a, N, MADV_HUGEPAGE);          /* 仅 madv 变体请求大页 */
  memset(a, 0, N);
  size_t x = 12345;
  for(size_t i=0;i<IDX;i++){ x = x*6364136223846793005ULL + 1442695040888963407ULL;
                            a[i] = (x>>33) % IDX; }
  /* 先跑一遍热身，再计时（计时由外部做） */
  size_t p = 0;
  for(long r=0;r<3000000;r++) p = a[p];
  printf("%zu\n", p);
  free(a);
  return 0;
}
"""

open(WORK + "/tlb.c", "w").write(SRC)
for v in ("plain", "madv"):
    r = subprocess.run(["gcc", "-O2", "-o", WORK + "/tlb_" + v, WORK + "/tlb.c"],
                       capture_output=True, text=True)
    if r.returncode:
        print("编译失败 %s: %s" % (v, r.stderr[:300])); raise SystemExit(1)
print("编译完成: tlb_plain / tlb_madv")


def meminfo(k):
    for ln in open("/proc/meminfo"):
        if ln.startswith(k + ":"):
            return int(ln.split()[1])
    return -1


def zoneinfo():
    """返回 free/min/low/high"""
    d, inzone = {}, False
    for ln in open("/proc/zoneinfo"):
        s = ln.split()
        if not s:
            continue
        if s[0] == "Node":
            inzone = True
        if inzone and s[0] in ("free", "min", "low", "high") and len(s) >= 2:
            d.setdefault(s[0], int(s[1]))
    return d


def snap():
    z = zoneinfo()
    return ("  PageTables=%skB AnonHugePages=%skB MemFree=%skB MemAvail=%skB "
            "free=%s high=%s PSI_mem_some10=%s"
            % (meminfo("PageTables"), meminfo("AnonHugePages"),
               meminfo("MemFree"), meminfo("MemAvailable"),
               z.get("free"), z.get("high"),
               open("/proc/pressure/memory").readline().split("avg10=")[1].split()[0]))
def timed(cmd, reps=3):
    xs = []
    for _ in range(reps):
        t0 = time.monotonic()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, check=True)
        xs.append((time.monotonic() - t0) * 1000)
    return xs


def bench(tag):
    print("  [TLB 指针追逐 256MB / 300 万次随机访问]")
    r = {}
    for v in ("plain", "madv"):
        xs = timed([WORK + "/tlb_" + v])
        r[v] = statistics.median(xs)
        print("    tlb_%-6s med=%.0f ms  raw=%s"
              % (v, r[v], ["%.0f" % x for x in xs]))
    print("  [gcc -O2 编译 x5]")
    src = "/tmp/.thp_src.c"
    with open(src, "w") as f:
        f.write("#include <math.h>\n")
        for i in range(800):
            f.write("static double f%d(double x){return sin(x*%d)+cos(x/%d.0);}\n"
                    % (i, i + 1, i + 1))
    xs = timed(["/usr/bin/gcc", "-O2", "-c", "-o", "/tmp/.thp.o", src], reps=2)
    print("    gcc5_%s med=%.0f ms  raw=%s" % (tag, statistics.median(xs),
                                               ["%.0f" % x for x in xs]))
    return r


def set_thp(v):
    try:
        with open(THP, "w") as f:
            f.write(v)
        return True
    except Exception as e:
        print("  写入 %s 失败: %s" % (THP, e))
        return False


def cur():
    return open(THP).read().strip()


print("===== THP 当前值 =====")
print("  %s" % cur())
print()

results = {}
for mode in ["never", "madvise", "never"]:
    print("########## THP = %s ##########" % mode)
    if not set_thp(mode):
        continue
    print("  实际生效: %s" % cur())
    time.sleep(2)
    snap()
    print(snap())
    results[mode] = bench(mode)
    print(snap())
    print()

print("########## 回滚确认 ##########")
print("  THP = %s" % cur())
print()
print("===== 汇总（中位数 ms）=====")
for mode in ["never", "madvise"]:
    if mode in results:
        r = results[mode]
        print("  THP=%-8s tlb_plain=%.0f  tlb_madv=%.0f"
              % (mode, r["plain"], r["madv"]))
if "never" in results and "madvise" in results:
    n, m = results["never"], results["madvise"]
    print("  === tlb_madv 在 madvise 档 vs never 档: %.2fx ==="
          % (n["madv"] / m["madv"]))
    print("  === tlb_plain 在 madvise 档 vs never 档: %.2fx（对照，应接近 1.0）==="
          % (n["plain"] / m["plain"]))
print("===== END =====")
