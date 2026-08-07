// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0

#include <asm/neon-intrinsics.h>

#include "lz4kd_neon.h"

static inline u64 crystal_lz4kd_read64(const void *src)
{
	u64 value;

	__builtin_memcpy(&value, src, sizeof(value));
	return value;
}

noinline const u8 *crystal_lz4kd_repeat_end_neon(const u8 *q,
		const u8 *r, const u8 *end)
{
	while (end - r >= 64) {
		uint8x16_t d0 = veorq_u8(vld1q_u8(q), vld1q_u8(r));
		uint8x16_t d1 = veorq_u8(vld1q_u8(q + 16),
				vld1q_u8(r + 16));
		uint8x16_t d2 = veorq_u8(vld1q_u8(q + 32),
				vld1q_u8(r + 32));
		uint8x16_t d3 = veorq_u8(vld1q_u8(q + 48),
				vld1q_u8(r + 48));
		uint8x16_t diff = vorrq_u8(vorrq_u8(d0, d1),
				vorrq_u8(d2, d3));

		if (vmaxvq_u8(diff))
			break;
		q += 64;
		r += 64;
	}

	while (end - r >= 8) {
		u64 diff = crystal_lz4kd_read64(q) ^
				crystal_lz4kd_read64(r);

		if (diff)
			return r + (__builtin_ctzll(diff) >> 3);
		q += sizeof(u64);
		r += sizeof(u64);
	}
	while (r < end && *q == *r) {
		q++;
		r++;
	}
	return r;
}
