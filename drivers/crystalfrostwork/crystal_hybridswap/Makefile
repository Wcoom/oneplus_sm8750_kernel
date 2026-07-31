# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_CRYSTAL_HYBRIDSWAP) += zram.o

zram-y := zcomp.o zms.o zram_drv.o core.o \
	zram_bridge.o memcg.o pressure.o stats.o

zram-$(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD) += \
	sddc/crystal_sddc_codec.o \
	sddc/lz4kd/lz4kd_encode.o \
	sddc/lz4kd/lz4kd_encode_delta.o \
	sddc/lz4kd/lz4kd_decode.o \
	sddc/lz4kd/lz4kd_decode_delta.o

zram-$(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST) += \
	sddc/crystal_sddc_codec_test.o
