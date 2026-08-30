# fopbatt（opbatt 电池工具包 8.2.14-beta）BBG 刷写兼容改造

## 背景（2026-08-30 真机取证）

- 设备：OnePlus 13（PJZ110，SM8750），自研内核分支 `6.6.118-13T`，BBG（Baseband-guard）启用
- `vendor_dlkm` 是 super 动态分区内的 logical partition（`/dev/block/mapper/vendor_dlkm_b` → dm-18，无分区元信息）
- 模块原刷写方式：`lpadd_auto --readonly --replace /dev/block/by-name/super vendor_dlkm_b ...`
  - BBG deny 日志（实测）：`deny write to protected partition dev=8:14 path=/dev/block/sda14 comm=lpadd_auto`
  - 根因：**lpadd_auto 写 super 分区（sda14）**，super 不在 BBG allowlist（承载 system/vendor 等全部动态分区，刻意保护）
- 结论：**不能放行 super 分区**（安全权衡），改为改造模块刷写方式

## 内核侧修改（已提交，子仓库 c17ecbc / 主仓库 77ded64）

BBG 放行 vendor_dlkm 动态分区直写，**不放行 super**：

1. `baseband_guard.h`：allowlist 追加 `"vendor_dlkm"`（slot 后缀 `_a/_b` 由既有匹配逻辑处理）
2. `blkdev_helper.c`：新增 `is_allowed_dm_partition_dev()`——经 `dm_get_md`/`dm_copy_name_and_uuid`
   解析 dm 设备名称后走同一 allowlist 判定（dm-ioctl 同款惯用法；非 dm 设备零开销；
   仅 `CONFIG_BLK_DEV_DM` built-in 时启用）
3. `baseband_guard.c`：判定接入 `reverse_allow_match_and_cache` 单一入口（新旧内核路径共用）

## 模块侧改造（kotools.sh 的 kot_run_lpadd_once）

原 lpadd_auto 写 super 被 BBG 拦截；改造为 **dm 镜像直写**（不写 super 分区表）：

1. `dmctl table vendor_dlkm<slot>` 读取当前 slot 的 dm-linear 映射（只读，BBG 不拦）
2. 解析表行 `start-end: linear, dev offset`，构造镜像设备参数
3. `dmctl create vendor_dlkm<a|b>`（**非当前 slot 名字**，复用 BBG 的 vendor_dlkm allowlist，
   实测 BBG 放行且 ro=0 可写）
4. `dd if=vendor_dlkm_new_erofs.img of=/dev/block/mapper/<镜像> bs=4M conv=fsync`
5. `dmctl delete` 清理；重启后 init 按 LP 元数据重建分区，新内容生效

> 注：直接 dd 写 dm-18 不可行——动态分区 ro 为 dm 表属性，`blockdev --setrw`
> 的 BLKROSET 会被 dm-linear ioctl 转发到底层设备（sda14），清不掉 dm 设备自身
> 的 bd_read_only，写仍被拒（EPERM）。镜像设备方案绕开此限制。

## 使用

- 重装模块：`ksud module install fopbatt-8.2.14-beta-bbg-fix.zip`（或手机 KernelSU 管理器）
- 或仅替换已装模块的 `kotools.sh` 后重启（开机不自动刷写，下次安装/更新时生效）
- 已装模块手动同步：`cp kotools.sh /data/adb/modules/fopbatt/kotools.sh`

## 真机验证记录（2026-08-30）

- 镜像设备创建/写/恢复往返：`vendor_dlkm_a` → dm-19，RO=0，写成功，BBG 无 deny，
  头部与 dm-18 逐字节一致（erofs superblock `e2e1f5e0` 恢复确认）
- 完整模块安装：卸载旧模块 → `ksud module install` 新 zip → customize.sh → kotools_main
  → vendor_boot 补丁（allowlist 放行）→ **"vendor_dlkm 写入成功"** → BBG deny 计数 0
- 重启后：boot_completed=1、vendor_dlkm 挂载正常、`oplus_chg_v2.ko` 从新分区加载运行、
  /data/opbatt 数据完整

## 文件

- `kotools.sh`：改造后的模块主脚本（zip 内同名文件已替换）
- `opbatt-8.2.14-beta-bbg-fix.zip`：完整可刷模块包（16M，设备端
  `/storage/emulated/0/Download/QQ/` 亦有一份）
