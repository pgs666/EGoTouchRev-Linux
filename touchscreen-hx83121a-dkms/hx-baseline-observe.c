// SPDX-License-Identifier: GPL-2.0
/* Raw-grid observation shared by BLSM, BLReset and SafeBaseline. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

void hx_baseline_observe_frame(struct hx_algo *algo, const u16 *raw,
			       s32 common_diff,
			       struct hx_baseline_frame_observation *obs)
{
	u8 row_bad[HX_ROWS] = { 0 };
	u8 col_bad[HX_COLS] = { 0 };
	int i, r, c;

	memset(obs, 0, sizeof(*obs));
	obs->max_signal = SHRT_MIN;
	obs->min_signal = SHRT_MAX;
	/* The raw grid is contiguous and index zero is a real sensor cell.  The
	 * vendor statistics include it; omitting it hides edge corruption from
	 * SafeBaseline and BLReset. */
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 baseline = algo->baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 local = sample - baseline - common_diff;

		obs->max_signal = max_t(s16, obs->max_signal,
			clamp_t(s32, local, SHRT_MIN, SHRT_MAX));
		obs->min_signal = min_t(s16, obs->min_signal,
			clamp_t(s32, local, SHRT_MIN, SHRT_MAX));
		if (sample < 0x1000 || sample > 0xf000) {
			obs->out_of_range++;
			continue;
		}
		if (abs(local) > HX_BASELINE_CLEAN_LOCAL_THRESHOLD) {
			obs->safe_bad_nodes++;
			if (local < 0)
				obs->safe_negative_nodes++;
		}
		if (abs(local) <= algo->wake_raw_jump_threshold)
			continue;
		obs->operational_bad_nodes++;
		if (local < 0)
			obs->operational_negative_nodes++;
		r = i / HX_COLS;
		c = i % HX_COLS;
		row_bad[r]++;
		col_bad[c]++;
	}
	for (r = 0; r < HX_ROWS; r++)
		if (row_bad[r] >= algo->wake_max_unstable_line_nodes)
			obs->line_noise++;
	for (c = 0; c < HX_COLS; c++)
		if (col_bad[c] >= algo->wake_max_unstable_line_nodes)
			obs->line_noise++;
}
