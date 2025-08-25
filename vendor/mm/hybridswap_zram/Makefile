# SPDX-License-Identifier: GPL-2.0-only

GCOV_PROFILE := y
obj-$(CONFIG_HYBRIDSWAP_ZRAM) += oplus_bsp_hybridswap_zram.o

oplus_bsp_hybridswap_zram-y	:= buildin.o zcomp.o zram_drv.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP) += hybridswap/hybridmain.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP_SWAPD) += hybridswap/hybridswapd.o
oplus_bsp_hybridswap_zram-$(CONFIG_HYBRIDSWAP_CORE) += hybridswap/hybridswap.o
