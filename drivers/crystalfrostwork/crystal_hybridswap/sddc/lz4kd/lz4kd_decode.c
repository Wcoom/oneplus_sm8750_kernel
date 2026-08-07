// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/*
 * Copyright (c) Huawei Technologies Co., Ltd. 2022. All rights reserved.
 * Description: LZ4K compression algorithm with delta compression
 */

#include "lz4kd.h"

#include "lz4kd_private.h" /* types, etc */

static const uint8_t *get_size(
	uint_fast32_t *size,
	const uint8_t *in_at,
	const uint8_t *const in_end)
{
	uint_fast32_t u;
	do {
		if (unlikely(in_at >= in_end))
			return NULL;
		*size += (u = *(const uint8_t*)in_at);
		++in_at;
	} while (BYTE_MAX == u);
	return in_at;
}

static int end_of_block(
	const uint_fast32_t nr_bytes_max,
	const uint_fast32_t r_bytes_max,
	const uint8_t *const in_at,
	const uint8_t *const in_end,
	const uint8_t *const out,
	const uint8_t *const out_at)
{
	if (!nr_bytes_max)
		return LZ4K_STATUS_FAILED; /* should be the last one in block */
	if (r_bytes_max != REPEAT_MIN)
		return LZ4K_STATUS_FAILED; /* should be the last one in block */
	if (in_at != in_end)
		return LZ4K_STATUS_FAILED; /* should be the last one in block */
	return (int)(out_at - out);
}

enum {
	NR_COPY_MIN = 16,
	R_COPY_MIN = 16,
	R_COPY_SAFE_2X = (R_COPY_MIN << 1) - 1
};

static bool out_non_repeat(
	const uint8_t **in_at,
	uint8_t **out_at,
	uint_fast32_t nr_bytes_max,
	const uint8_t *const in_end,
	const uint8_t *const out_end)
{
	size_t in_left;
	size_t out_left;
	size_t copy_left;
	const uint8_t *in_copy_end;
	uint8_t *out_copy_end;

	in_left = (size_t)(in_end - *in_at);
	out_left = (size_t)(out_end - *out_at);
	copy_left = in_left < out_left ? in_left : out_left;
	if (unlikely(nr_bytes_max > copy_left))
		return false;
	in_copy_end = *in_at + nr_bytes_max;
	out_copy_end = *out_at + nr_bytes_max;
	if (likely(nr_bytes_max <= NR_COPY_MIN)) {
		if (likely(copy_left >= NR_COPY_MIN))
			m_copy(*out_at, *in_at, NR_COPY_MIN);
		else
			m_copy(*out_at, *in_at, nr_bytes_max);
	} else { /* nr_bytes_max>NR_COPY_MIN */
		if (likely(copy_left - nr_bytes_max >= NR_COPY_MIN)) {
			m_copy(*out_at, *in_at, NR_COPY_MIN);
			copy_x_while_lt(*out_at + NR_COPY_MIN,
					*in_at + NR_COPY_MIN,
					out_copy_end, NR_COPY_MIN);
		} else {
			m_copy(*out_at, *in_at, nr_bytes_max);
		}
	} /* if (nr_bytes_max <= NR_COPY_MIN) */
	*in_at = in_copy_end;
	*out_at = out_copy_end;
	return true;
}

static void out_repeat_overlap(
	uint_fast32_t offset,
	uint8_t *out_at,
	const uint8_t *out_from,
	const uint8_t *const out_copy_end)
{
	enum {
		COPY_MIN = R_COPY_MIN >> 1,
		OFFSET_LIMIT = COPY_MIN >> 1
	};
	m_copy(out_at, out_from, COPY_MIN);
/* (1 < offset < R_COPY_MIN/2) && out_copy_end + R_COPY_SAFE_2X  <= out_end */
	out_at += offset;
	if (offset <= OFFSET_LIMIT)
		offset <<= 1;
	do {
		m_copy(out_at, out_from, COPY_MIN);
		out_at += offset;
		if (offset <= OFFSET_LIMIT)
			offset <<= 1;
	} while (out_at - out_from < R_COPY_MIN);
	while_lt_copy_2x_as_x2(out_at, out_from, out_copy_end, R_COPY_MIN);
}

static void out_repeat_slow(
	uint_fast32_t r_bytes_max,
	uint_fast32_t offset,
	uint8_t *out_at,
	const uint8_t *out_from,
	const uint8_t *const out_copy_end,
	const uint8_t *const out_safe_end)
{
	if (offset > 1 && out_copy_end <= out_safe_end) {
		out_repeat_overlap(offset, out_at, out_from, out_copy_end);
	} else {
		if (offset == 1) {
			m_set(out_at, *out_from, r_bytes_max);
		} else {
			do
				*out_at++ = *out_from++;
			while (out_at < out_copy_end);
		}
	}
}

static int decode(
	const uint8_t *in_at,
	const uint8_t *const out0,
	uint8_t *const out,
	const uint8_t *const in_end,
	const uint8_t *const out_end,
	const uint_fast32_t nr_log2,
	const uint_fast32_t off_log2)
{
	const uint_fast32_t r_log2 = TAG_BITS_MAX - (off_log2 + nr_log2);
	const uint8_t *const in_tag_end = in_end - TAG_BYTES_MAX;
	const uint8_t *const out_safe_end =
		(size_t)(out_end - out) >= R_COPY_SAFE_2X ?
		out_end - R_COPY_SAFE_2X : out;
	uint8_t *out_at = out;
	while (likely(in_at <= in_tag_end)) {
		const uint_fast32_t utag = read4_at(in_at - 1) >> BYTE_BITS;
		const uint_fast32_t offset = utag & mask(off_log2);
		uint_fast32_t nr_bytes_max = utag >> (off_log2 + r_log2),
			      r_bytes_max = ((utag >> off_log2) & mask(r_log2)) +
					    REPEAT_MIN;
		const uint8_t *out_from = NULL;
		uint8_t *out_copy_end = NULL;
		in_at += TAG_BYTES_MAX;
		if (unlikely(nr_bytes_max == mask(nr_log2))) {
			in_at = get_size(&nr_bytes_max, in_at, in_end);
			if (unlikely(in_at == NULL))
				return LZ4K_STATUS_READ_ERROR;
		}
		if (!out_non_repeat(&in_at, &out_at, nr_bytes_max, in_end, out_end))
			return LZ4K_STATUS_FAILED;
		if (unlikely(r_bytes_max == mask(r_log2) + REPEAT_MIN)) {
			in_at = get_size(&r_bytes_max, in_at, in_end);
			if (unlikely(in_at == NULL))
				return LZ4K_STATUS_READ_ERROR;
		}
		if (unlikely(offset == 0))
			return end_of_block(nr_bytes_max, r_bytes_max, in_at,
					    in_end, out, out_at);
		if (unlikely(offset > (uintptr_t)out_at - (uintptr_t)out0 ||
			     r_bytes_max > (size_t)(out_end - out_at)))
			return LZ4K_STATUS_FAILED;
		out_from = out_at - offset;
		out_copy_end = out_at + r_bytes_max;
		if (likely(offset >= R_COPY_MIN &&
			   out_copy_end <= out_safe_end)) {
			copy_2x_as_x2_while_lt(out_at, out_from, out_copy_end,
					       R_COPY_MIN);
		} else if (likely(offset >= (R_COPY_MIN >> 1) &&
				  out_copy_end <= out_safe_end)) {
			m_copy(out_at, out_from, R_COPY_MIN);
			out_at += offset;
			while_lt_copy_x(out_at, out_from, out_copy_end, R_COPY_MIN);
		} else {
			out_repeat_slow(r_bytes_max, offset, out_at, out_from,
				out_copy_end, out_safe_end);
		}
		out_at = out_copy_end;
	}
	return in_at == in_end ? (int)(out_at - out) : LZ4K_STATUS_FAILED;
}

static int decode_pattern_4kb(
	const uint8_t *const in,
	uint8_t *const out,
	const uint8_t *const out_end)
{
	const uint64_t pattern = read8_at(in);
	uint8_t *out_at = out;

	while ((size_t)(out_end - out_at) >= sizeof(pattern)) {
		m_copy(out_at, &pattern, sizeof(pattern));
		out_at += sizeof(pattern);
	}
	return out_at == out_end ? (int)(out_end - out) : LZ4K_STATUS_FAILED;
}

static int decode_4kb(
	const uint8_t *const in,
	uint8_t *const out,
	const uint8_t *const in_end,
	const uint8_t *const out_end)
{
	return decode(in, out, out, in_end, out_end, NR_4KB_LOG2,
			BLOCK_4KB_LOG2);
}

int crystal_lz4kd_decode(
	const void *in,
	void *const out,
	unsigned int in_max,
	unsigned int out_max)
{
	const uint8_t *in_end;
	uint8_t *out_end;

	if (unlikely(!crystal_lz4kd_valid_range(in, in_max, out, out_max,
			2 + TAG_BYTES_MAX, 2U << BLOCK_4KB_LOG2,
			1U << BLOCK_4KB_LOG2)))
		return LZ4K_STATUS_FAILED;
	in_end = (const uint8_t *)in + in_max;
	out_end = (uint8_t *)out + out_max;
	if (unlikely(in_max == PATTERN_BYTES_MAX))
		return decode_pattern_4kb((const uint8_t*)in, (uint8_t*)out,
				out_end);
	return decode_4kb((const uint8_t*)in + 1, (uint8_t*)out, in_end, out_end);
}

int crystal_lz4kd_decode_delta(
	const void *in,
	const void *const out0,
	void *const out,
	unsigned int in_max,
	unsigned int out_max)
{
	uintptr_t out0_addr = (uintptr_t)out0;
	uintptr_t out_addr = (uintptr_t)out;

	if (unlikely(!out0 || out0_addr > out_addr ||
		     out_addr - out0_addr > (1U << BLOCK_4KB_LOG2)))
		return LZ4K_STATUS_FAILED;
	if (unlikely(!crystal_lz4kd_valid_range(in, in_max, out, out_max,
			2 + TAG_BYTES_MAX, 2U << BLOCK_4KB_LOG2,
			1U << BLOCK_4KB_LOG2)))
		return LZ4K_STATUS_FAILED;

	return decode((const uint8_t *)in + 1, out0, out,
			(const uint8_t *)in + in_max,
			(uint8_t *)out + out_max, NR_8KB_LOG2,
			BLOCK_8KB_LOG2);
}
