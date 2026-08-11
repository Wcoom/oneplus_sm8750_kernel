/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TEMP_OFFSET_SYSCTL_H
#define _LINUX_TEMP_OFFSET_SYSCTL_H

/* 温度偏移单位：调用方报告原始温度所用的单位 */
enum temp_offset_unit {
	TEMP_OFFSET_MILLI_C = 1000,   /* 毫摄氏度（thermal 子系统） */
	TEMP_OFFSET_DECI_C  = 10,     /* 0.1 摄氏度（power_supply 子系统） */
};

int temp_offset_get_celsius(void);
int apply_temperature_offset(const char *zone_type, int raw_temp,
			     enum temp_offset_unit unit);

#endif /* _LINUX_TEMP_OFFSET_SYSCTL_H */
