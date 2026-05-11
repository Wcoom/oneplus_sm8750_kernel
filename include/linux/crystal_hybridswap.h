/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CRYSTAL_HYBRIDSWAP_H
#define _LINUX_CRYSTAL_HYBRIDSWAP_H

#include <linux/errno.h>
#include <linux/kconfig.h>

#if IS_REACHABLE(CONFIG_CRYSTAL_HYBRIDSWAP)
int crystal_hybridswap_signal_pressure(unsigned int level);
#else
static inline int crystal_hybridswap_signal_pressure(unsigned int level)
{
	return -ENODEV;
}
#endif

#endif /* _LINUX_CRYSTAL_HYBRIDSWAP_H */
