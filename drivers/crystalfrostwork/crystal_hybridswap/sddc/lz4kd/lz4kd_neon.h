/* SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0 */

#ifndef _LZ4KD_NEON_H
#define _LZ4KD_NEON_H

#include <linux/types.h>

const u8 *crystal_lz4kd_repeat_end_neon(const u8 *q, const u8 *r,
					const u8 *end);

#endif /* _LZ4KD_NEON_H */
