/* SPDX-License-Identifier: BSD-2-Clause */
// LZ4 compatibility wrapper for Linux kernel

#ifndef __LINUX_LZ4_H__
#define __LINUX_LZ4_H__

#include "../../lib/lz4/lz4.h"
#include "../../lib/lz4/lz4hc.h"

#define LZ4_MEM_COMPRESS	LZ4_STREAM_MINSIZE
#define LZ4HC_MEM_COMPRESS	LZ4_STREAMHC_MINSIZE

#define LZ4HC_MIN_CLEVEL	LZ4HC_CLEVEL_MIN
#define LZ4HC_DEFAULT_CLEVEL	LZ4HC_CLEVEL_DEFAULT
#define LZ4HC_MAX_CLEVEL	LZ4HC_CLEVEL_MAX

/*
 * lib/lz4/lz4.h 里这两个宏被包在 #ifdef LZ4_STATIC_LINKING_ONLY 内，
 * 内核消费者默认看不到，而 fs/erofs 等模块需要它们，故在此显式提供
 * （对齐主线 f0ef073e213a "include/linux/lz4.h: add some missing macros"）。
 */
#define LZ4_DECOMPRESS_INPLACE_MARGIN(compressedSize)          (((compressedSize) >> 8) + 32)

#ifndef LZ4_DISTANCE_MAX	/* history window size; can be user-defined at compile time */
#define LZ4_DISTANCE_MAX 65535	/* set to maximum value by default */
#endif

#endif
