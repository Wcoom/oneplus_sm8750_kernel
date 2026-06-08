#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/string.h>
#include <linux/crystalfrostwork.h>

struct crystalfrostwork cf;

INPUT_STR_ATTR_RO(copyright, CRYSTALFROSTWORK_MODULES_INFO);

static struct kobj_attribute copyright_attr =
	__ATTR(copyright, 0444, show_copyright, store_copyright);

static int __init framework_init(void)
{
	int ret;

	cf_info("version %d.%d by Frostwork\n",
		MODULES_FRAMEWORK_MAJOR_VERSION,
		MODULES_FRAMEWORK_MINOR_VERSION);

	cf.sysfs = kobject_create_and_add("crystalfrostwork", NULL);
	if(!cf.sysfs) {
		cf_err("kobject create failed!\n");
		return -EINVAL;
	}

	ret = sysfs_create_file(cf.sysfs, &copyright_attr.attr);
	if (ret)
		goto sysfs_err;

	cf_info("init completed!\n");

	cf.inited = true;
	return 0;

sysfs_err:
	cf_err("sysfs create file failed! code=%d\n", ret);
	kobject_put(cf.sysfs);
	return ret;
}

MODULE_AUTHOR("Frostwork <799620521@qq.com>");
MODULE_LICENSE("Proprietary");

early_initcall(framework_init)
