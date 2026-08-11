// SPDX-License-Identifier: GPL-2.0

#include <linux/init.h>
#include <linux/string.h>
#include <linux/sysctl.h>

#include <linux/temp_offset_sysctl.h>

static int temperature_offset_celsius;

static int temperature_offset_min = -100;
static int temperature_offset_max = 100;

static struct ctl_table temperature_offset_sysctl_table[] = {
	{
		.procname	= "temperature_offset_celsius",
		.data		= &temperature_offset_celsius,
		.maxlen		= sizeof(temperature_offset_celsius),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &temperature_offset_min,
		.extra2		= &temperature_offset_max,
	},
	{ }
};

int temp_offset_get_celsius(void)
{
	return temperature_offset_celsius;
}

/*
 * 统一温度偏移校正入口。
 * battery/batt 热区由 power_supply 侧统一应用偏移（排除条件与
 * 原 thermal_helpers.c 行内判断一致，防止双倍扣减），此处不再应用。
 */
int apply_temperature_offset(const char *zone_type, int raw_temp,
			     enum temp_offset_unit unit)
{
	if (zone_type &&
	    (strncasecmp(zone_type, "battery", 7) == 0 ||
	     strncasecmp(zone_type, "batt", 4) == 0))
		return raw_temp;

	return raw_temp - temperature_offset_celsius * unit;
}

static int __init temperature_offset_sysctl_init(void)
{
	register_sysctl_init("kernel", temperature_offset_sysctl_table);
	return 0;
}
late_initcall(temperature_offset_sysctl_init);
