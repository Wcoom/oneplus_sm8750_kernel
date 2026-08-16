# SPDX-License-Identifier: GPL-2.0

obj-$(CONFIG_CRYSTAL_HYBRIDSWAP) += zram.o

zram-y := zcomp.o zms.o zram_drv.o core.o param.o \
	zram_bridge.o memcg.o pressure.o stats.o

zram-$(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD) += \
	sddc/crystal_sddc_codec.o \
	sddc/lz4kd/lz4kd_encode.o \
	sddc/lz4kd/lz4kd_encode_delta.o \
	sddc/lz4kd/lz4kd_decode.o

ifeq ($(CONFIG_ARM64)$(CONFIG_KERNEL_MODE_NEON),yy)
zram-$(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD) += \
	sddc/lz4kd/lz4kd_neon.o
CFLAGS_REMOVE_sddc/lz4kd/lz4kd_neon.o += -mgeneral-regs-only
CFLAGS_sddc/lz4kd/lz4kd_neon.o += -ffreestanding \
	-isystem $(shell $(CC) -print-file-name=include)
endif

zram-$(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC) += \
	sddc/crystal_sddc.o

zram-$(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_KUNIT_TEST) += \
	sddc/crystal_sddc_codec_test.o
