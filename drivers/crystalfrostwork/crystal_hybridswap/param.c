// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) "crystal_hybridswap: " fmt

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/string.h>

#include "crystal_hybridswap_internal.h"

/*
 * chs_param：hybridswap 调优参数的单一写入口。
 *
 * sysfs 的 8 个 RW 属性（zram_bridge.c）全部经此分发；解析、校验、
 * 日志与状态更新集中在同一处，替代原先散布在各 sysfs store 里的
 * 重复样板。ADR-0002 的内核参数守护（vendor 漂移改写防护）将来
 * 在此处落单一拦截点，无需再回到各属性逐一拦截。
 *
 * 行为等价红线：各参数的解析规则、错误码与日志逐字保持原样。
 */

/**
 * chs_param_store() - 调优参数写唯一入口
 * @dev:  发起写入的 zram device（部分参数按设备定位状态）
 * @id:    参数标识
 * @buf:   sysfs 写入缓冲
 * @len:   写入长度
 *
 * Return: 0 成功（调用方返回 len），负 errno 失败。
 */
int chs_param_store(struct device *dev, enum chs_param_id id,
		    const char *buf, size_t len)
{
	int ret;

	switch (id) {
	case CHS_PARAM_ENABLE: {
		unsigned long val;

		ret = kstrtoul(buf, 0, &val);
		if (ret)
			return ret;

		crystal_hybridswap_set_enabled(!!val);
		crystal_hybridswap_set_core_enabled(!!val);
		return 0;
	}
	case CHS_PARAM_CORE_ENABLE: {
		unsigned long val;

		ret = kstrtoul(buf, 0, &val);
		if (ret)
			return ret;

		crystal_hybridswap_set_core_enabled(!!val);
		return 0;
	}
	case CHS_PARAM_SWAPD_PAUSE: {
		bool val;

		ret = kstrtobool(buf, &val);
		if (ret)
			return ret;

		crystal_hybridswap_set_swapd_pause(val);
		return 0;
	}
	case CHS_PARAM_LOGLEVEL: {
		int level;

		ret = kstrtoint(buf, 0, &level);
		if (ret)
			return ret;
		if (level < 0 || level >= CHS_LOG_MAX) {
			chs_log(CHS_LOG_ERR, "val %d is not valid\n", level);
			return -EINVAL;
		}

		crystal_hybridswap_set_loglevel(level);
		return 0;
	}
	case CHS_PARAM_LOOP_DEVICE:
		ret = zram_bind_backing_dev(dev, buf, len);
		crystal_hybridswap_record_loop_device_bind(ret);
		if (ret) {
			chs_log(CHS_LOG_ERR,
				"hybridswap_loop_device backing_dev bind failed ret=%d\n",
				ret);
			return ret;
		}

		ret = crystal_hybridswap_set_loop_device(buf, len);
		if (ret) {
			chs_log(CHS_LOG_ERR,
				"hybridswap_loop_device state update failed ret=%d\n",
				ret);
			return ret;
		}

		chs_log(CHS_LOG_INFO,
			"hybridswap_loop_device backing_dev bind success\n");
		return 0;
	case CHS_PARAM_DEV_LIFE: {
		unsigned long val;

		ret = kstrtoul(buf, 0, &val);
		if (ret)
			return ret;

		crystal_hybridswap_set_dev_life(val);
		return 0;
	}
	case CHS_PARAM_QUOTA_DAY: {
		unsigned long long val;

		ret = kstrtoull(buf, 0, &val);
		if (ret)
			return ret;

		crystal_hybridswap_set_quota_day(val);
		return 0;
	}
	case CHS_PARAM_ZRAM_INCREASE: {
		struct crystal_hybridswap_zram *entry;
		unsigned long val;

		ret = kstrtoul(buf, 0, &val);
		if (ret)
			return ret;

		mutex_lock(&chs.zram_lock);
		entry = crystal_hybridswap_find_zram_locked(dev);
		if (entry)
			entry->zram_increase_pages = val << 8;
		mutex_unlock(&chs.zram_lock);

		if (!entry)
			return -ENODEV;

		atomic64_inc(&chs.stats.zram_increase_store);
		crystal_hybridswap_update_auto_policy();
		return 0;
	}
	default:
		return -EINVAL;
	}
}
