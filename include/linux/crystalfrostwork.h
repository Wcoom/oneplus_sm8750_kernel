#ifndef _LINUX_CRYSTALFROSTWORK_H
#define _LINUX_CRYSTALFROSTWORK_H

#define CRYSTALFROSTWORK_MODULES_INFO                                 \
	"CrystalFrostwork Kernel Private Modules\n"                   \
	"Copyright by 2019-2025 阿菌•未霜 (799620521@qq.com). " \
	"All rights reserved\n\0"

#define MODULES_FRAMEWORK_MAJOR_VERSION 2
#define MODULES_FRAMEWORK_MINOR_VERSION 1

#define cf_info(fmt, ...) \
	pr_info("crystalfrostwork: %s: " fmt, __func__, ##__VA_ARGS__)

#define cf_warn(fmt, ...) \
	pr_warn("crystalfrostwork: %s: " fmt, __func__, ##__VA_ARGS__)

#define cf_err(fmt, ...) \
	pr_err("crystalfrostwork: %s: " fmt, __func__, ##__VA_ARGS__)

#define INPUT_INT_ATTR(identifier, object, min, max)                          \
	static ssize_t show_##identifier(                                     \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sprintf(buf, "%d", object);                            \
	}                                                                     \
	static ssize_t store_##identifier(kobject *kobj,                      \
					  struct kobj_attribute *attr,        \
					  const char *buf, size_t count)      \
	{                                                                     \
		int tmp;                                                      \
                                                                              \
		sscanf(buf, "%d", &tmp);                                      \
                                                                              \
		if (tmp <= max && tmp >= min) {                               \
			object = tmp;                                         \
			return count;                                         \
		} else                                                        \
			return -EINVAL;                                       \
	}

#define INPUT_INT_ATTR_RO(identifier, object, min, max)                       \
	static ssize_t show_##identifier(                                     \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sprintf(buf, "%d", object);                            \
	}                                                                     \
	static ssize_t store_##identifier(struct kobject *kobj,               \
					  struct kobj_attribute *attr,        \
					  const char *buf, size_t count)      \
	{                                                                     \
		return -EPERM;                                                \
	}

#define INPUT_STR_ATTR(identifier, object)                                    \
	static ssize_t show_##identifier(                                     \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sprintf(buf, "%s", object);                            \
	}                                                                     \
	static ssize_t store_##identifier(struct device *dev,                 \
					  struct device_attribute *attr,      \
					  const char *buf, size_t count)      \
	{                                                                     \
		sscanf(buf, "%s", object);                                    \
		return count;                                                 \
	}

#define INPUT_STR_ATTR_RO(identifier, object)                                 \
	static ssize_t show_##identifier(                                     \
		struct kobject *kobj, struct kobj_attribute *attr, char *buf) \
	{                                                                     \
		return sprintf(buf, "%s", object);                            \
	}                                                                     \
	static ssize_t store_##identifier(struct kobject *kobj,               \
					  struct kobj_attribute *attr,        \
					  const char *buf, size_t count)      \
	{                                                                     \
		return -EPERM;                                                \
	}

struct crystalfrostwork {
	struct kobject *sysfs;

	bool inited;
};

extern struct crystalfrostwork cf;

#endif
