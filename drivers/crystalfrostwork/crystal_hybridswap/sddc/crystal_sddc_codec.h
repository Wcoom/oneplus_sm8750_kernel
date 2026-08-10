/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _CRYSTAL_SDDC_CODEC_H_
#define _CRYSTAL_SDDC_CODEC_H_

#include <linux/types.h>

struct crystal_sddc_codec;

bool crystal_sddc_codec_available(void);
struct crystal_sddc_codec *crystal_sddc_codec_create(void);
void crystal_sddc_codec_destroy(struct crystal_sddc_codec *codec);

int crystal_sddc_codec_compress(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len);
int crystal_sddc_codec_decompress(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len);

int crystal_sddc_codec_compress_delta(struct crystal_sddc_codec *codec,
		const void *ref, unsigned int ref_len, const void *src,
		unsigned int src_len, void *dst, unsigned int *dst_len,
		unsigned int out_limit);
int crystal_sddc_codec_decompress_delta(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, const void *ref,
		unsigned int ref_len, void *dst, unsigned int *dst_len);
/*
 * Restore an ordinary stream into the codec's private delta window. The
 * returned pointer remains valid while the owning zcomp stream is locked and
 * until another delta operation uses the same codec.
 */
int
crystal_sddc_codec_decompress_delta_borrowed(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, const void *ref,
		unsigned int ref_len, const void **restored,
		unsigned int *restored_len);

#endif /* _CRYSTAL_SDDC_CODEC_H_ */
