// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpu.h>
#include <linux/crypto.h>
#include <linux/vmalloc.h>

#include "zcomp.h"
#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD)
#include "sddc/crystal_sddc_codec.h"
#endif

static const char * const backends[] = {
#if IS_ENABLED(CONFIG_CRYPTO_LZO)
	"lzo",
	"lzo-rle",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4)
	"lz4",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4HC)
	"lz4hc",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4K)
	"lz4k",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_LZ4KD) || \
	IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD)
	"lz4kd",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_DEFLATE)
	"deflate",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_842)
	"842",
#endif
#if IS_ENABLED(CONFIG_CRYPTO_ZSTD)
	"zstd",
#endif
};

static int crypto_backend_create(struct zcomp_strm *zstrm, const char *name)
{
	zstrm->tfm = crypto_alloc_comp(name, 0, 0);
	if (IS_ERR_OR_NULL(zstrm->tfm)) {
		zstrm->tfm = NULL;
		return -ENOMEM;
	}

	return 0;
}

static void crypto_backend_destroy(struct zcomp_strm *zstrm)
{
	if (zstrm->tfm)
		crypto_free_comp(zstrm->tfm);
	zstrm->tfm = NULL;
}

static int crypto_backend_compress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len)
{
	return crypto_comp_compress(zstrm->tfm, src, src_len, dst, dst_len);
}

static int crypto_backend_decompress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len)
{
	return crypto_comp_decompress(zstrm->tfm, src, src_len, dst, dst_len);
}

static const struct zcomp_backend_ops crypto_backend_ops = {
	.create = crypto_backend_create,
	.destroy = crypto_backend_destroy,
	.compress = crypto_backend_compress,
	.decompress = crypto_backend_decompress,
};

#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD)
static int lz4kd_backend_create(struct zcomp_strm *zstrm, const char *name)
{
	struct crystal_sddc_codec *codec;

	(void)name;
	codec = crystal_sddc_codec_create();
	if (IS_ERR(codec))
		return PTR_ERR(codec);
	zstrm->backend_data = codec;
	return 0;
}

static void lz4kd_backend_destroy(struct zcomp_strm *zstrm)
{
	crystal_sddc_codec_destroy(zstrm->backend_data);
	zstrm->backend_data = NULL;
}

static int lz4kd_backend_compress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len)
{
	return crystal_sddc_codec_compress(zstrm->backend_data, src, src_len,
					   dst, dst_len);
}

static int lz4kd_backend_decompress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst,
		unsigned int *dst_len)
{
	return crystal_sddc_codec_decompress(zstrm->backend_data, src, src_len,
					     dst, dst_len);
}

static int lz4kd_backend_compress_delta(struct zcomp_strm *zstrm,
		const void *ref, unsigned int ref_len, const void *src,
		unsigned int src_len, void *dst, unsigned int *dst_len,
		unsigned int out_limit)
{
	return crystal_sddc_codec_compress_delta(zstrm->backend_data, ref,
			ref_len, src, src_len, dst, dst_len, out_limit);
}

static int lz4kd_backend_decompress_delta(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, const void *ref,
		unsigned int ref_len, void *dst, unsigned int *dst_len)
{
	return crystal_sddc_codec_decompress_delta(zstrm->backend_data, src,
			src_len, ref, ref_len, dst, dst_len);
}

static const struct zcomp_backend_ops lz4kd_backend_ops = {
	.create = lz4kd_backend_create,
	.destroy = lz4kd_backend_destroy,
	.compress = lz4kd_backend_compress,
	.decompress = lz4kd_backend_decompress,
	.compress_delta = lz4kd_backend_compress_delta,
	.decompress_delta = lz4kd_backend_decompress_delta,
};
#endif

static const struct zcomp_backend_ops *zcomp_backend(const char *name)
{
#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD)
	if (!strcmp(name, "lz4kd"))
		return &lz4kd_backend_ops;
#endif
	return &crypto_backend_ops;
}

static void zcomp_strm_free(struct zcomp_strm *zstrm)
{
	if (zstrm->ops && zstrm->ops->destroy)
		zstrm->ops->destroy(zstrm);
	vfree(zstrm->buffer);
	zstrm->tfm = NULL;
	zstrm->backend_data = NULL;
	zstrm->buffer = NULL;
	zstrm->ops = NULL;
}

/*
 * Initialize zcomp_strm structure with ->tfm initialized by backend, and
 * ->buffer. Return a negative value on error.
 */
static int zcomp_strm_init(struct zcomp_strm *zstrm,
				       struct zcomp *comp)
{
	int ret;

	zstrm->ops = comp->ops;
	zstrm->tfm = NULL;
	zstrm->backend_data = NULL;
	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	zstrm->buffer = vzalloc(2 * PAGE_SIZE);
	if (!zstrm->buffer) {
		zcomp_strm_free(zstrm);
		return -ENOMEM;
	}

	ret = zstrm->ops->create(zstrm, comp->name);
	if (ret) {
		zcomp_strm_free(zstrm);
		return ret;
	}
	return 0;
}

bool zcomp_available_algorithm(const char *comp)
{
#if IS_ENABLED(CONFIG_CRYSTAL_HYBRIDSWAP_SDDC_LZ4KD)
	if (!strcmp(comp, "lz4kd"))
		return crystal_sddc_codec_available();
#endif
	/*
	 * Crypto does not ignore a trailing new line symbol,
	 * so make sure you don't supply a string containing
	 * one.
	 * This also means that we permit zcomp initialisation
	 * with any compressing algorithm known to crypto api.
	 */
	return crypto_has_comp(comp, 0, 0) == 1;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf)
{
	bool known_algorithm = false;
	ssize_t sz = 0;
	int i;

	for (i = 0; i < ARRAY_SIZE(backends); i++) {
		if (!strcmp(comp, backends[i])) {
			known_algorithm = true;
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"[%s] ", backends[i]);
		} else {
			sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
					"%s ", backends[i]);
		}
	}

	/*
	 * Out-of-tree module known to crypto api or a missing
	 * entry in `backends'.
	 */
	if (!known_algorithm && crypto_has_comp(comp, 0, 0) == 1)
		sz += scnprintf(buf + sz, PAGE_SIZE - sz - 2,
				"[%s] ", comp);

	sz += scnprintf(buf + sz, PAGE_SIZE - sz, "\n");
	return sz;
}

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp)
{
	local_lock(&comp->stream->lock);
	return this_cpu_ptr(comp->stream);
}

void zcomp_stream_put(struct zcomp *comp)
{
	local_unlock(&comp->stream->lock);
}

int zcomp_compress(struct zcomp_strm *zstrm,
		const void *src, unsigned int *dst_len)
{
	/*
	 * Our dst memory (zstrm->buffer) is always `2 * PAGE_SIZE' sized
	 * because sometimes we can endup having a bigger compressed data
	 * due to various reasons: for example compression algorithms tend
	 * to add some padding to the compressed buffer. Speaking of padding,
	 * comp algorithm `842' pads the compressed length to multiple of 8
	 * and returns -ENOSP when the dst memory is not big enough, which
	 * is not something that ZRAM wants to see. We can handle the
	 * `compressed_size > PAGE_SIZE' case easily in ZRAM, but when we
	 * receive -ERRNO from the compressing backend we can't help it
	 * anymore. To make `842' happy we need to tell the exact size of
	 * the dst buffer, zram_drv will take care of the fact that
	 * compressed buffer is too big.
	 */
	*dst_len = PAGE_SIZE * 2;

	return zstrm->ops->compress(zstrm, src, PAGE_SIZE, zstrm->buffer,
				    dst_len);
}

int zcomp_decompress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst)
{
	unsigned int dst_len = PAGE_SIZE;

	return zstrm->ops->decompress(zstrm, src, src_len, dst, &dst_len);
}

bool zcomp_supports_delta(const struct zcomp *comp)
{
	return comp && comp->ops && comp->ops->compress_delta &&
		comp->ops->decompress_delta;
}

int zcomp_compress_delta(struct zcomp_strm *zstrm, const void *ref,
		unsigned int ref_len, const void *src, unsigned int src_len,
		unsigned int *dst_len, unsigned int out_limit)
{
	if (!zstrm->ops->compress_delta)
		return -EOPNOTSUPP;

	*dst_len = 2 * PAGE_SIZE;
	return zstrm->ops->compress_delta(zstrm, ref, ref_len, src, src_len,
			zstrm->buffer, dst_len, out_limit);
}

int zcomp_decompress_delta(struct zcomp_strm *zstrm, const void *src,
		unsigned int src_len, const void *ref, unsigned int ref_len,
		void *dst, unsigned int *dst_len)
{
	if (!zstrm->ops->decompress_delta)
		return -EOPNOTSUPP;

	*dst_len = PAGE_SIZE;
	return zstrm->ops->decompress_delta(zstrm, src, src_len, ref,
			ref_len, dst, dst_len);
}

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp,
					       node);
	struct zcomp_strm *zstrm;
	int ret;

	zstrm = per_cpu_ptr(comp->stream, cpu);
	local_lock_init(&zstrm->lock);

	ret = zcomp_strm_init(zstrm, comp);
	if (ret)
		pr_err("Can't allocate a compression stream\n");
	return ret;
}

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp,
					       node);
	struct zcomp_strm *zstrm;

	zstrm = per_cpu_ptr(comp->stream, cpu);
	zcomp_strm_free(zstrm);
	return 0;
}

static int zcomp_init(struct zcomp *comp)
{
	int ret;

	comp->stream = alloc_percpu(struct zcomp_strm);
	if (!comp->stream)
		return -ENOMEM;

	ret = cpuhp_state_add_instance(zcomp_cpuhp_state, &comp->node);
	if (ret < 0)
		goto cleanup;
	return 0;

cleanup:
	free_percpu(comp->stream);
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	cpuhp_state_remove_instance(zcomp_cpuhp_state, &comp->node);
	free_percpu(comp->stream);
	kfree(comp);
}

/*
 * search available compressors for requested algorithm.
 * allocate new zcomp and initialize it. return compressing
 * backend pointer or ERR_PTR if things went bad. ERR_PTR(-EINVAL)
 * if requested algorithm is not supported, ERR_PTR(-ENOMEM) in
 * case of allocation error, or any other error potentially
 * returned by zcomp_init().
 */
struct zcomp *zcomp_create(const char *alg)
{
	struct zcomp *comp;
	int error;

	/*
	 * Crypto API will execute /sbin/modprobe if the compression module
	 * is not loaded yet. We must do it here, otherwise we are about to
	 * call /sbin/modprobe under CPU hot-plug lock.
	 */
	if (!zcomp_available_algorithm(alg))
		return ERR_PTR(-EINVAL);

	comp = kzalloc(sizeof(struct zcomp), GFP_KERNEL);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->name = alg;
	comp->ops = zcomp_backend(alg);
	error = zcomp_init(comp);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
