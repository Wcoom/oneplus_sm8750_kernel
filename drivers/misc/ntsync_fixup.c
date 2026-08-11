// SPDX-License-Identifier: GPL-2.0-only
/*
 * ntsync_fixup.c - /dev/ntsync 的 SELinux 上下文与权限修复
 *
 * 从 ntsync.c 外移的独立 fixup：misc 设备已在 ntsync_init 中注册，
 * /dev/ntsync 立即可见，此处延后 2s 执行仅作保险（Q3 语义不变）。
 * 目标内核 6.6.118，只保留 >= 6.3 的 __vfs_setxattr_noperm 签名分支。
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/mnt_idmapping.h>
#include <linux/namei.h>
#include <linux/path.h>
#include <linux/string.h>
#include <linux/workqueue.h>
#include <linux/xattr.h>

static struct delayed_work ntsync_perm_work;

static void ntsync_fix_perms_worker(struct work_struct *work)
{
	struct path path;
	char *ctx = "u:object_r:gpu_device:s0";

	if (!kern_path("/dev/ntsync", LOOKUP_FOLLOW, &path)) {
		struct inode *inode = d_backing_inode(path.dentry);

		if (inode) {
			__vfs_setxattr_noperm(&nop_mnt_idmap, path.dentry,
					      "security.selinux", ctx, strlen(ctx) + 1, 0);
			inode->i_mode = (inode->i_mode & ~S_IALLUGO) | 0666;
			pr_info("ntsync: Applied 0666 and gpu_device context\n");
		}
		path_put(&path);
	}
	/* kern_path 失败时静默返回，与原实现行为一致 */
}

static int __init ntsync_fixup_init(void)
{
	INIT_DELAYED_WORK(&ntsync_perm_work, ntsync_fix_perms_worker);
	schedule_delayed_work(&ntsync_perm_work, msecs_to_jiffies(2000));

	return 0;
}
late_initcall(ntsync_fixup_init);
