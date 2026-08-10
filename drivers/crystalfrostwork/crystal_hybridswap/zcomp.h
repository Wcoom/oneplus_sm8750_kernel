/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2014 Sergey Senozhatsky.
 */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_
#include <linux/cpuhotplug.h>
#include <linux/local_lock.h>

extern enum cpuhp_state zcomp_cpuhp_state;

struct zcomp_strm;

struct zcomp_backend_ops {
	int (*create)(struct zcomp_strm *zstrm, const char *name);
	void (*destroy)(struct zcomp_strm *zstrm);
	int (*compress)(struct zcomp_strm *zstrm, const void *src,
			unsigned int src_len, void *dst, unsigned int *dst_len);
	int (*decompress)(struct zcomp_strm *zstrm, const void *src,
			  unsigned int src_len, void *dst,
			  unsigned int *dst_len);
	int (*compress_delta)(struct zcomp_strm *zstrm, const void *ref,
			      unsigned int ref_len, const void *src,
			      unsigned int src_len, void *dst,
			      unsigned int *dst_len,
			      unsigned int out_limit);
	int (*decompress_delta)(struct zcomp_strm *zstrm, const void *src,
				unsigned int src_len, const void *ref,
				unsigned int ref_len, void *dst,
				unsigned int *dst_len);
	/*
	 * @restored borrows backend storage until this stream is unlocked or
	 * another delta operation is issued on it.
	 */
	int (*decompress_delta_borrowed)(struct zcomp_strm *zstrm,
				const void *src, unsigned int src_len,
				const void *ref, unsigned int ref_len,
				const void **restored,
				unsigned int *restored_len);
};

struct zcomp_strm {
	/* Stream members are protected by ->lock. */
	local_lock_t lock;
	/* compression/decompression buffer */
	void *buffer;
	struct crypto_comp *tfm;
	void *backend_data;
	const struct zcomp_backend_ops *ops;
};

/* dynamic per-device compression frontend */
struct zcomp {
	struct zcomp_strm __percpu *stream;
	const char *name;
	const struct zcomp_backend_ops *ops;
	struct hlist_node node;
};

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_available_show(const char *comp, char *buf);
bool zcomp_available_algorithm(const char *comp);

struct zcomp *zcomp_create(const char *alg);
void zcomp_destroy(struct zcomp *comp);

struct zcomp_strm *zcomp_stream_get(struct zcomp *comp);
void zcomp_stream_put(struct zcomp *comp);

int zcomp_compress(struct zcomp_strm *zstrm,
		const void *src, unsigned int *dst_len);

int zcomp_decompress(struct zcomp_strm *zstrm,
		const void *src, unsigned int src_len, void *dst);

bool zcomp_supports_delta(const struct zcomp *comp);
int zcomp_compress_delta(struct zcomp_strm *zstrm, const void *ref,
		unsigned int ref_len, const void *src, unsigned int src_len,
		unsigned int *dst_len, unsigned int out_limit);
int zcomp_decompress_delta(struct zcomp_strm *zstrm, const void *src,
		unsigned int src_len, const void *ref, unsigned int ref_len,
		void *dst, unsigned int *dst_len);
int zcomp_decompress_delta_borrowed(struct zcomp_strm *zstrm, const void *src,
		unsigned int src_len, const void *ref, unsigned int ref_len,
		const void **restored, unsigned int *restored_len);

bool zcomp_set_max_streams(struct zcomp *comp, int num_strm);
#endif /* _ZCOMP_H_ */
