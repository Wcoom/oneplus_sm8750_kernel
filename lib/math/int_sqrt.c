// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2013 Davidlohr Bueso <davidlohr.bueso@hp.com>
 *
 * Originally based on the shift-and-subtract algorithm for computing integer
 * square root from Guy L. Steele.
 *
 * Optimized in 2026 using CLZ/LUT/Newton-Raphson based exact floor(sqrt(v))
 * implementations derived from the user-provided isqrt library.
 */

#include <linux/export.h>
#include <linux/bitops.h>
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/types.h>

/* ================================================================ */
/* 8-bit: full LUT (256 bytes, 0 computation)                       */
/* ================================================================ */

static const u8 int_sqrt8_lut[256] = {
	 0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3,
	 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5,
	 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
	 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
	 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
	 8, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
	 9, 9, 9, 9,10,10,10,10,10,10,10,10,10,10,10,10,
	10,10,10,10,10,10,10,10,10,11,11,11,11,11,11,11,
	11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,
	12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,
	12,12,12,12,12,12,12,12,12,13,13,13,13,13,13,13,
	13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
	13,13,13,13,14,14,14,14,14,14,14,14,14,14,14,14,
	14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,
	14,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
	15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
};

static __always_inline u8 __int_sqrt8(u8 v)
{
	return int_sqrt8_lut[v];
}

/* ================================================================ */
/* 16-bit: zone encoding + delta LUT + fixup (0 divisions)          */
/* ================================================================ */

static const s8 int_sqrt16_delta_lut[256] = {
	  0,  0,  0, -1,  0, -1, -1, -1,  0, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -2, -2, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -3, -3,
	 -3, -3, -2, -2, -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -2,
	 -2, -2, -2, -3, -3, -3, -3, -4, -4, -4, -4, -5, -5, -5, -5, -6,
	 -6, -5, -5, -5, -4, -4, -4, -3, -3, -3, -3, -2, -2, -2, -2, -2,
	 -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	  0, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	 -1, -1, -2, -2, -2, -2, -2, -2, -2, -3, -3, -3, -3, -3, -3, -4,
	 -4, -4, -4, -4, -4, -5, -5, -5, -5, -6, -6, -6, -6, -6, -7, -7,
	 -7, -7, -8, -8, -8, -8, -9, -9, -9, -9,-10,-10,-10,-11,-11,-11,
	-11,-11,-10,-10,-10, -9, -9, -9, -8, -8, -8, -7, -7, -7, -6, -6,
	 -6, -6, -5, -5, -5, -5, -4, -4, -4, -4, -4, -3, -3, -3, -3, -3,
	 -3, -3, -2, -2, -2, -2, -2, -2, -2, -2, -1, -1, -1, -1, -1, -1,
	 -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

static __always_inline u16 __int_sqrt16(u16 v)
{
	u32 e, zone, msb, sub_bits, sub_m, h, y;

	if (v <= 1)
		return v;

	e = 31 - __builtin_clz((u32)v);
	zone = e >> 1;
	if (!zone)
		return 1;

	msb = e & 1;
	sub_bits = zone - 1;
	sub_m = (v >> (e - sub_bits)) & ((1u << sub_bits) - 1);
	h = (1u << zone) | (msb << sub_bits) | sub_m;
	y = h + int_sqrt16_delta_lut[h];
	if ((y + 1) * (y + 1) <= (u32)v)
		y++;
	if (y > 0 && y * y > (u32)v)
		y--;

	return (u16)y;
}

/* ================================================================ */
/* Shared sqrt LUT for 32/64-bit initial estimates (256 bytes)      */
/* ================================================================ */

static const u16 int_sqrt_est_lut[128] = {
	256,257,259,261,263,265,267,269,271,273,275,277,278,280,282,284,
	286,288,289,291,293,295,296,298,300,301,303,305,306,308,310,311,
	313,315,316,318,320,321,323,324,326,327,329,331,332,334,335,337,
	338,340,341,343,344,346,347,349,350,352,353,354,356,357,359,360,
	362,364,367,370,373,375,378,381,384,386,389,391,394,397,399,402,
	404,407,409,412,414,417,419,422,424,426,429,431,434,436,438,441,
	443,445,448,450,452,454,457,459,461,463,465,468,470,472,474,476,
	478,481,483,485,487,489,491,493,495,497,499,501,503,505,507,509,
};

#define __int_sqrt_clz(v, bits) ((bits) == 64 ? \
	(unsigned)__builtin_clzll((u64)(v)) : \
	(unsigned)__builtin_clz((u32)(v)))

#define __INT_SQRT_DEFINE(name, type, bits, nr) \
static __always_inline type name(type v) \
{ \
	type e, half_e, frac6, y, max_y; \
	int i; \
\
	if (v <= 1) \
		return v; \
\
	e = (bits) - 1 - __int_sqrt_clz(v, bits); \
	half_e = e >> 1; \
	frac6 = (e >= 6) ? (v >> (e - 6)) & 0x3F : (v << (6 - e)) & 0x3F; \
	y = (type)int_sqrt_est_lut[((e & 1) << 6) | frac6] << half_e >> 8; \
	y |= !y; \
	for (i = 0; i < (nr); i++) \
		y = (y + v / y) >> 1; \
	max_y = ((type)1 << ((bits) / 2)) - 1; \
	if (y > max_y) \
		y = max_y; \
	while (y > v / y) \
		y--; \
	while (y < max_y && y < v / (y + 1)) \
		y++; \
	return y; \
}

__INT_SQRT_DEFINE(__int_sqrt32, u32, 32, 1)
__INT_SQRT_DEFINE(__int_sqrt64, u64, 64, 2)

/**
 * int_sqrt - computes the integer square root
 * @x: integer of which to calculate the sqrt
 *
 * Computes: floor(sqrt(x))
 */
inline unsigned long int_sqrt(unsigned long x)
{
	if (x <= U8_MAX)
		return __int_sqrt8((u8)x);

	if (x <= U16_MAX)
		return __int_sqrt16((u16)x);

	if (BITS_PER_LONG == 64)
		return (unsigned long)__int_sqrt64((u64)x);

	return (unsigned long)__int_sqrt32((u32)x);
}
EXPORT_SYMBOL(int_sqrt);

#if BITS_PER_LONG < 64
/**
 * int_sqrt64 - strongly typed int_sqrt function when minimum 64 bit input
 * is expected.
 * @x: 64bit integer of which to calculate the sqrt
 */
u32 int_sqrt64(u64 x)
{
	if (x <= ULONG_MAX)
		return int_sqrt((unsigned long)x);

	return (u32)__int_sqrt64(x);
}
EXPORT_SYMBOL(int_sqrt64);
#endif
