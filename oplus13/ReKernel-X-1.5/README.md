# ReKernel-X-1.5.zip + fq_guard_ko 集成说明

## 背景

上游 whitewhale v3.5 prebuilt 内核（abogki20260808-4k）不含本地 fq_guard
定制。为不重刷内核，把 fq_guard 做成可加载模块 `fq_guard_ko.ko`
（源码见 `../fq_guard_ko/`），并集成进 ReKernel-X 模块 zip 作为安装载体，
由模块的 `post-fs-data.sh` 在开机时自动加载。

## 文件布局（work/ 解包目录）

```
work/
├── META-INF/com/google/android/{update-binary, updater-script}  # 模块安装器
├── customize.sh       # 安装脚本：按 android<ver>-<core> 匹配 rkx ko，
│                      # 并把 kmod/fq_guard_ko.ko 一并拷到模块根目录
├── module.prop        # 模块元数据（id=rekernel_x, version=1.5）
├── post-fs-data.sh    # 开机脚本：先加载 fq_guard_ko，再尝试 rkx ko
└── kmod/              # 各内核版本 rkx ko + fq_guard_ko.ko
```

## 关键改动

### customize.sh（新增一行，在 rm -rf kmod 之前）
```sh
cp -fp "$MODPATH"/kmod/fq_guard_ko.ko "$MODPATH"/ 2>/dev/null || true
```
原因：原脚本 `rm -rf $MODPATH/kmod` 会清掉 kmod 目录，必须先把
fq_guard_ko.ko 拷出。

### post-fs-data.sh（rkx 加载循环之前新增）
- 卸载残留的 fq_guard_ko（重复执行保护）
- `echo 0 > /proc/sys/kernel/kptr_restrict`（kallsyms 地址可见的前提）
- 用户态解析 `fq_qdisc_ops` 地址，`insmod ... fq_ops_addr=0x$addr`
- 解析失败则退化为不带参数 insmod（内核态解析 fallback，oplus 会拒绝）

## 注意事项

1. **KASLR**：`fq_qdisc_ops` 地址每次开机变化，必须在开机脚本里现解析，
   不能写死。
2. **oplus 安全补丁**：禁止内核态读 /proc/kallsyms
   （"kernel read not supported for file /kallsyms"），地址只能用户态拿。
3. **vermagic**：fq_guard_ko.ko 针对 abogki20260808-4k 构建
   （`bash ../fq_guard_ko/build.sh`）；若上游内核版本变化需重新构建。
4. **签名**：模块未签名（MODULE_SIG_PROTECT=y 下未签名可加载）。
5. **rkx ko 兼容性**：kmod/ 里是 20260313 旧版 rkx ko，vermagic 与
   上游 v3.5 内核不匹配，insmod 会静默失败（post-fs-data 的 for 循环
   逐个尝试，失败继续）；不影响 fq_guard_ko 加载。本机内核已 built-in
   ReKernel-X，无需 rkx ko。
6. **安装**：把 zip 推回 `/storage/emulated/0/Download/ReKernel-X-1.5.zip`
   后用 KernelSU 管理器或 recovery 刷入；模块安装后下次开机生效。
7. 旧部署 `/data/adb/service.d/99-fq-guard.sh` 与
   `/data/adb/fq_guard/` 已删除（2026-08-30），加载职责移交本模块。

## 重新打包命令

```bash
cd work
# 修改脚本后：
python3 <<'EOF'
import zipfile, os, time
src = '.'
out = '../ReKernel-X-1.5-new.zip'
if os.path.exists(out): os.remove(out)
z = zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED)
for root, dirs, files in os.walk(src):
    for d in dirs:
        p = os.path.join(root, d)
        zi = zipfile.ZipInfo(os.path.relpath(p, src) + '/')
        zi.external_attr = 0o40755 << 16
        z.writestr(zi, '')
    for f in files:
        p = os.path.join(root, f)
        st = os.stat(p)
        mode = st.st_mode
        if f.endswith('.sh') or f == 'update-binary':
            mode = (mode & ~0o777) | 0o755
        zi = zipfile.ZipInfo(os.path.relpath(p, src))
        zi.external_attr = (mode & 0xFFFF) << 16
        zi.date_time = time.localtime(st.st_mtime)[:6]
        with open(p, 'rb') as fp:
            z.writestr(zi, fp.read())
z.close()
EOF
adb push ../ReKernel-X-1.5-new.zip /storage/emulated/0/Download/ReKernel-X-1.5.zip
```
