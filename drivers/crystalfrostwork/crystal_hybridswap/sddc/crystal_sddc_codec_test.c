// SPDX-License-Identifier: GPL-2.0-or-later

#include <kunit/test.h>
#include <linux/err.h>
#include <linux/mm.h>
#include <linux/string.h>

#include "crystal_sddc_codec.h"

struct crystal_sddc_codec_test_ctx {
	struct crystal_sddc_codec *codec;
};

static int crystal_sddc_codec_test_init(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx;

	ctx = kunit_kzalloc(test, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->codec = crystal_sddc_codec_create();
	if (IS_ERR(ctx->codec))
		return PTR_ERR(ctx->codec);

	test->priv = ctx;
	return 0;
}

static void crystal_sddc_codec_test_exit(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;

	if (!ctx)
		return;

	crystal_sddc_codec_destroy(ctx->codec);
}

static void crystal_sddc_fill_prandom(u8 *buf, size_t len, u32 seed)
{
	u32 state = seed;
	size_t i;

	for (i = 0; i < len; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		buf[i] = state;
	}
}

static void crystal_sddc_codec_roundtrip_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int encoded_len = 2 * PAGE_SIZE;
	unsigned int decoded_len = PAGE_SIZE;
	u8 *encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, encoded);
	KUNIT_ASSERT_NOT_NULL(test, decoded);
	KUNIT_ASSERT_NOT_NULL(test, page);
	memset(page, 0x5a, PAGE_SIZE);

	ret = crystal_sddc_codec_compress(ctx->codec, page, PAGE_SIZE,
					  encoded, &encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, encoded_len, (unsigned int)PAGE_SIZE);

	ret = crystal_sddc_codec_decompress(ctx->codec, encoded, encoded_len,
					    decoded, &decoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_EQ(test, decoded_len, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_MEMEQ(test, decoded, page, PAGE_SIZE);
}

static void crystal_sddc_codec_raw_contract_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int encoded_len = 2 * PAGE_SIZE;
	unsigned int decoded_len = PAGE_SIZE;
	u8 *encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, encoded);
	KUNIT_ASSERT_NOT_NULL(test, decoded);
	KUNIT_ASSERT_NOT_NULL(test, page);
	crystal_sddc_fill_prandom(page, PAGE_SIZE, 0x13579bdf);

	ret = crystal_sddc_codec_compress(ctx->codec, page, PAGE_SIZE,
					  encoded, &encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	if (encoded_len >= PAGE_SIZE) {
		KUNIT_EXPECT_EQ(test, encoded_len, 2U * PAGE_SIZE);
		return;
	}

	ret = crystal_sddc_codec_decompress(ctx->codec, encoded, encoded_len,
					    decoded, &decoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, decoded, page, PAGE_SIZE);
}

static void crystal_sddc_codec_delta_chain_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int ref_encoded_len = 2 * PAGE_SIZE;
	unsigned int cur_encoded_len = 2 * PAGE_SIZE;
	unsigned int delta_len = 2 * PAGE_SIZE;
	unsigned int restored_encoded_len = PAGE_SIZE;
	unsigned int restored_page_len = PAGE_SIZE;
	u8 *ref_encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *cur_encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *delta = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *restored_encoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *restored_page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *ref_page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *cur_page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ref_encoded);
	KUNIT_ASSERT_NOT_NULL(test, cur_encoded);
	KUNIT_ASSERT_NOT_NULL(test, delta);
	KUNIT_ASSERT_NOT_NULL(test, restored_encoded);
	KUNIT_ASSERT_NOT_NULL(test, restored_page);
	KUNIT_ASSERT_NOT_NULL(test, ref_page);
	KUNIT_ASSERT_NOT_NULL(test, cur_page);

	crystal_sddc_fill_prandom(ref_page, PAGE_SIZE / 2, 0x2468ace1);
	memcpy(ref_page + PAGE_SIZE / 2, ref_page, PAGE_SIZE / 2);
	memcpy(cur_page, ref_page, PAGE_SIZE);
	cur_page[127] ^= 0x55;
	cur_page[PAGE_SIZE / 2 + 127] ^= 0x55;

	ret = crystal_sddc_codec_compress(ctx->codec, ref_page, PAGE_SIZE,
					  ref_encoded, &ref_encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, ref_encoded_len, (unsigned int)PAGE_SIZE);
	ret = crystal_sddc_codec_compress(ctx->codec, cur_page, PAGE_SIZE,
					  cur_encoded, &cur_encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, cur_encoded_len, (unsigned int)PAGE_SIZE);

	ret = crystal_sddc_codec_compress_delta(ctx->codec, ref_encoded,
			ref_encoded_len, cur_encoded, cur_encoded_len, delta,
			&delta_len, cur_encoded_len - 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, delta_len, cur_encoded_len);

	ret = crystal_sddc_codec_decompress_delta(ctx->codec, delta, delta_len,
			ref_encoded, ref_encoded_len, restored_encoded,
			&restored_encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, restored_encoded_len, cur_encoded_len);
	KUNIT_EXPECT_MEMEQ(test, restored_encoded, cur_encoded, cur_encoded_len);

	ret = crystal_sddc_codec_decompress(ctx->codec, restored_encoded,
			restored_encoded_len, restored_page, &restored_page_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_EXPECT_MEMEQ(test, restored_page, cur_page, PAGE_SIZE);
}

static void crystal_sddc_codec_full_window_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int delta_len = 2 * PAGE_SIZE;
	unsigned int restored_len = PAGE_SIZE;
	u8 *ref = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *current_data = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *delta = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *restored = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, ref);
	KUNIT_ASSERT_NOT_NULL(test, current_data);
	KUNIT_ASSERT_NOT_NULL(test, delta);
	KUNIT_ASSERT_NOT_NULL(test, restored);
	crystal_sddc_fill_prandom(ref, PAGE_SIZE, 0x10203040);
	memcpy(current_data, ref, PAGE_SIZE);
	current_data[0] ^= 0xa5;
	current_data[PAGE_SIZE - 1] ^= 0x5a;

	ret = crystal_sddc_codec_compress_delta(ctx->codec, ref, PAGE_SIZE,
			current_data, PAGE_SIZE, delta, &delta_len,
			PAGE_SIZE - 1);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, delta_len, (unsigned int)PAGE_SIZE);

	ret = crystal_sddc_codec_decompress_delta(ctx->codec, delta, delta_len,
			ref, PAGE_SIZE, restored, &restored_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, restored_len, (unsigned int)PAGE_SIZE);
	KUNIT_EXPECT_MEMEQ(test, restored, current_data, PAGE_SIZE);
}

static void crystal_sddc_codec_malformed_test(struct kunit *test)
{
	struct crystal_sddc_codec_test_ctx *ctx = test->priv;
	unsigned int encoded_len = 2 * PAGE_SIZE;
	unsigned int decoded_len = PAGE_SIZE;
	u8 malformed[4] = { 0 };
	u8 *encoded = kunit_kmalloc(test, 2 * PAGE_SIZE, GFP_KERNEL);
	u8 *decoded = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	u8 *page = kunit_kmalloc(test, PAGE_SIZE, GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, encoded);
	KUNIT_ASSERT_NOT_NULL(test, decoded);
	KUNIT_ASSERT_NOT_NULL(test, page);
	memset(page, 0x6b, PAGE_SIZE);
	ret = crystal_sddc_codec_compress(ctx->codec, page, PAGE_SIZE,
					  encoded, &encoded_len);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_LT(test, 1U, encoded_len);
	ret = crystal_sddc_codec_decompress(ctx->codec, encoded,
			encoded_len - 1, decoded, &decoded_len);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);

	decoded_len = PAGE_SIZE;
	ret = crystal_sddc_codec_decompress(ctx->codec, malformed,
			sizeof(malformed), decoded, &decoded_len);
	KUNIT_EXPECT_EQ(test, ret, -EINVAL);
}

static struct kunit_case crystal_sddc_codec_test_cases[] = {
	KUNIT_CASE(crystal_sddc_codec_roundtrip_test),
	KUNIT_CASE(crystal_sddc_codec_raw_contract_test),
	KUNIT_CASE(crystal_sddc_codec_delta_chain_test),
	KUNIT_CASE(crystal_sddc_codec_full_window_test),
	KUNIT_CASE(crystal_sddc_codec_malformed_test),
	{}
};

static struct kunit_suite crystal_sddc_codec_test_suite = {
	.name = "crystal-sddc-codec",
	.init = crystal_sddc_codec_test_init,
	.exit = crystal_sddc_codec_test_exit,
	.test_cases = crystal_sddc_codec_test_cases,
};

kunit_test_suite(crystal_sddc_codec_test_suite);
