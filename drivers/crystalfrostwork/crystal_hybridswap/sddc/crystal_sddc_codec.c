// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/err.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include "crystal_sddc_codec.h"
#include "lz4kd/lz4kd.h"

#define CRYSTAL_SDDC_BLOCK_SIZE	SZ_4K
#define CRYSTAL_SDDC_WINDOW_SIZE	(2 * CRYSTAL_SDDC_BLOCK_SIZE)

struct crystal_sddc_codec {
	void *encode_state;
	u8 *delta_window;
};

bool crystal_sddc_codec_available(void)
{
	return PAGE_SIZE == CRYSTAL_SDDC_BLOCK_SIZE;
}

struct crystal_sddc_codec *crystal_sddc_codec_create(void)
{
	struct crystal_sddc_codec *codec;

	if (!crystal_sddc_codec_available())
		return ERR_PTR(-EOPNOTSUPP);

	codec = kzalloc(sizeof(*codec), GFP_KERNEL);
	if (!codec)
		return ERR_PTR(-ENOMEM);

	codec->encode_state = kvzalloc(
		crystal_lz4kd_encode_state_bytes_min(), GFP_KERNEL);
	codec->delta_window = kvzalloc(CRYSTAL_SDDC_WINDOW_SIZE, GFP_KERNEL);
	if (!codec->encode_state || !codec->delta_window) {
		crystal_sddc_codec_destroy(codec);
		return ERR_PTR(-ENOMEM);
	}

	return codec;
}

void crystal_sddc_codec_destroy(struct crystal_sddc_codec *codec)
{
	if (!codec)
		return;

	kvfree(codec->delta_window);
	kvfree(codec->encode_state);
	kfree(codec);
}

int crystal_sddc_codec_compress(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len)
{
	int ret;

	if (!codec || !src || !dst || !dst_len || !*dst_len ||
	    src_len != CRYSTAL_SDDC_BLOCK_SIZE)
		return -EINVAL;

	ret = crystal_lz4kd_encode(codec->encode_state, src, dst, src_len,
				    *dst_len, 0);
	if (ret < 0)
		return -EINVAL;
	if (ret > 0)
		*dst_len = ret;

	return 0;
}

int crystal_sddc_codec_decompress(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len)
{
	int ret;

	if (!codec || !src || !src_len || src_len > CRYSTAL_SDDC_BLOCK_SIZE ||
	    !dst || !dst_len ||
	    *dst_len != CRYSTAL_SDDC_BLOCK_SIZE)
		return -EINVAL;

	ret = crystal_lz4kd_decode(src, dst, src_len, *dst_len);
	if (ret != CRYSTAL_SDDC_BLOCK_SIZE)
		return -EINVAL;

	*dst_len = ret;
	return 0;
}

int crystal_sddc_codec_compress_delta(struct crystal_sddc_codec *codec,
		const void *ref, unsigned int ref_len, const void *src,
		unsigned int src_len, void *dst, unsigned int *dst_len,
		unsigned int out_limit)
{
	u8 *current_data;
	int ret;

	if (!codec || !ref || !ref_len || !src || src_len < 16 || !dst ||
	    !dst_len || !*dst_len || ref_len > CRYSTAL_SDDC_BLOCK_SIZE ||
	    src_len > CRYSTAL_SDDC_BLOCK_SIZE ||
	    ref_len + src_len > CRYSTAL_SDDC_WINDOW_SIZE)
		return -EINVAL;

	memcpy(codec->delta_window, ref, ref_len);
	current_data = codec->delta_window + ref_len;
	memcpy(current_data, src, src_len);

	ret = crystal_lz4kd_encode_delta(codec->encode_state,
			codec->delta_window, current_data, dst, src_len, *dst_len,
			out_limit);
	if (ret < 0)
		return -EINVAL;
	if (ret > 0)
		*dst_len = ret;

	return 0;
}

int
crystal_sddc_codec_decompress_delta_borrowed(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, const void *ref,
		unsigned int ref_len, const void **restored,
		unsigned int *restored_len)
{
	u8 *current_data;
	int ret;

	if (!restored || !restored_len)
		return -EINVAL;
	*restored = NULL;
	if (!codec || !src || !src_len ||
	    src_len > CRYSTAL_SDDC_BLOCK_SIZE || !ref || !ref_len ||
	    !*restored_len ||
	    ref_len > CRYSTAL_SDDC_BLOCK_SIZE ||
	    *restored_len != CRYSTAL_SDDC_BLOCK_SIZE ||
	    ref_len + *restored_len > CRYSTAL_SDDC_WINDOW_SIZE)
		return -EINVAL;

	memcpy(codec->delta_window, ref, ref_len);
	current_data = codec->delta_window + ref_len;
	ret = crystal_lz4kd_decode_delta(src, codec->delta_window, current_data,
					 src_len, *restored_len);
	if (ret <= 0 || ret > *restored_len)
		return -EINVAL;

	*restored = current_data;
	*restored_len = ret;
	return 0;
}

int crystal_sddc_codec_decompress_delta(struct crystal_sddc_codec *codec,
		const void *src, unsigned int src_len, const void *ref,
		unsigned int ref_len, void *dst, unsigned int *dst_len)
{
	const void *restored;
	int ret;

	if (!dst || !dst_len)
		return -EINVAL;

	ret = crystal_sddc_codec_decompress_delta_borrowed(codec, src, src_len,
			ref, ref_len, &restored, dst_len);
	if (ret)
		return ret;

	memcpy(dst, restored, *dst_len);
	return 0;
}
