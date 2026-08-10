// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: LZ4K compression algorithm with delta compression
 */

#ifndef _LZ4KD_PRIVATE_H
#define _LZ4KD_PRIVATE_H

#include "lz4kd.h"
#include <linux/overflow.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>

typedef uint64_t uint_fast32_t;

static inline bool crystal_lz4kd_valid_range(
	const void *in, size_t in_max, const void *out, size_t out_max,
	size_t min_in, size_t max_in, size_t max_out)
{
	uintptr_t in_end;
	uintptr_t out_end;

	if (!in || !out || in_max < min_in || in_max > max_in ||
	    !out_max || out_max > max_out)
		return false;

	if (check_add_overflow((uintptr_t)in, in_max, &in_end) ||
	    check_add_overflow((uintptr_t)out, out_max, &out_end))
		return false;

	return in_end > (uintptr_t)in && out_end > (uintptr_t)out;
}

enum {
	BYTE_BITS = 8UL,
	WORD_BITS = 32U,
	DWORD_BITS = 64UL,
	BYTE_BITS_LOG2 = 3,
	BYTE_MAX = 255U,
	REPEAT_MIN = 4,
	TAG_BYTES_MAX = 3,
	TAG_BITS_MAX  = TAG_BYTES_MAX * 8,
	BLOCK_4KB_LOG2  = 12,
	BLOCK_8KB_LOG2  = 13,
	NR_8KB_LOG2 = 5, /* for encoded_bytes_max */
	NR_4KB_LOG2 = 6,
	PATTERN_BYTES_MAX = 8 /* 1 bytes for header, 8 bytes for pattern */
};

inline static uint32_t mask(uint_fast32_t log2)
{
	return (1U << log2) - 1U;
}

inline static uint64_t max_u64(uint64_t a, uint64_t b)
{
	return a > b ? a : b;
}

inline static uint64_t min_u64(uint64_t a, uint64_t b)
{
	return a < b ? a : b;
}

inline static void m_copy(void *dst, const void *src, size_t total)
{
	(void)__builtin_memcpy(dst, src, total);
}

inline static void m_set(void *dst, uint8_t value, size_t total)
{
	(void)__builtin_memset(dst, value, total);
}

inline static uint64_t round_up_to_log2(uint64_t u, uint8_t log2)
{
	const uint64_t round_mask = (1ULL << log2) - 1;

	return (u + round_mask) & ~round_mask;
}

inline static uint32_t read4_at(const void *p)
{
	uint32_t result;
	m_copy(&result, p, sizeof(result));
	return result;
}

inline static uint64_t read8_at(const void *p)
{
	uint64_t result;
	m_copy(&result, p, sizeof(result));
	return result;
}

inline static bool equal4(const uint8_t *const q, const uint8_t *const r)
{
	return read4_at(q) == read4_at(r);
}

inline static bool equal4pv(const uint8_t *const q, const uint64_t rv)
{
	return read4_at(q) == (uint32_t)rv;
}

inline static uint_fast32_t hash32v(const uint64_t r, uint32_t shift)
{
	const uint32_t hash32_factor = 2654435761U;
	return ((uint32_t)r * hash32_factor) >> (WORD_BITS - shift);
}

inline static uint_fast32_t hash64v_5b(const uint64_t r, uint32_t shift)
{
	const uint64_t m = 889523592379ULL;
	const uint64_t up_shift = 24;
	return (uint32_t)(((r << up_shift) * m) >> (DWORD_BITS - shift));
}

inline static uint_fast32_t hash64_5b(const uint8_t *r, uint32_t shift)
{
	return hash64v_5b(read8_at(r), shift);
}

inline static void while_lt_copy_x(
	uint8_t *dst,
	const uint8_t *src,
	const uint8_t *dst_end,
	const size_t copy_min)
{
	for (; dst < dst_end; dst += copy_min, src += copy_min)
		m_copy(dst, src, copy_min);
}

inline static void copy_x_while_lt(
	uint8_t *dst,
	const uint8_t *src,
	const uint8_t *dst_end,
	const size_t copy_min)
{
	m_copy(dst, src, copy_min);
	while (dst + copy_min < dst_end)
		m_copy(dst += copy_min, src += copy_min, copy_min);
}

inline static void copy_x_while_total(
	uint8_t *dst,
	const uint8_t *src,
	size_t total,
	const size_t copy_min)
{
	m_copy(dst, src, copy_min);
	for (; total > copy_min; total -= copy_min)
		m_copy(dst += copy_min, src += copy_min, copy_min);
}

inline static void copy_2x(
	uint8_t *dst,
	const uint8_t *src,
	const size_t copy_min)
{
	m_copy(dst, src, copy_min);
	m_copy(dst + copy_min, src + copy_min, copy_min);
}

inline static void copy_2x_as_x2_while_lt(
	uint8_t *dst,
	const uint8_t *src,
	const uint8_t *dst_end,
	const size_t copy_min)
{
	copy_2x(dst, src, copy_min);
	while (dst + (copy_min << 1) < dst_end)
		copy_2x(dst += (copy_min << 1), src += (copy_min << 1), copy_min);
}

inline static void while_lt_copy_2x_as_x2(
	uint8_t *dst,
	const uint8_t *src,
	const uint8_t *dst_end,
	const size_t copy_min)
{
	for (; dst < dst_end; dst += (copy_min << 1), src += (copy_min << 1))
		copy_2x(dst, src, copy_min);
}

#endif /* _LZ4KD_PRIVATE_H */
