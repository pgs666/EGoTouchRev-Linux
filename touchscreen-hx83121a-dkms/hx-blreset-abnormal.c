// SPDX-License-Identifier: GPL-2.0
/* Mutual-cap translation of BLReset_IsBaselineAbnormalType1/2/3. */
#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif
#include "hx-algo-internal.h"

#define HX_BLRESET_TYPE1 (1U << 0)
#define HX_BLRESET_TYPE2 (1U << 1)
#define HX_BLRESET_TYPE3 (1U << 2)

u16 hx_blreset_abnormal_types(struct hx_algo *algo, s16 max_signal,
			      s16 min_signal, u16 abnormal_nodes,
			      u16 negative_nodes, bool has_signal,
			      bool touch_protected, bool game_scenario)
{
	s32 neg = min_signal < 0 ? -(s32)min_signal : 0;
	s32 pos = max_signal > 0 ? max_signal : 0;
	bool clean = algo->blreset_baseline_state == 16;
	bool over_noise = algo->blreset_over_noise_frames >= 5;
	bool type1, type2, type3;
	u16 flags = 0;
	u32 interval = max_t(u16, algo->frame_interval_ms, 1);

	/* Type 1: negative-dominant abnormal baseline.  The 3/4 and minimum
	 * node tests are the mutual-cap form of the vendor ratios. */
	type1 = over_noise && !clean && !touch_protected && !has_signal &&
		negative_nodes > 2 && neg > (s32)algo->baseline_peak_threshold * 2 &&
		pos * 3 < neg * 4 && abnormal_nodes > 2;
	/* Type 2 uses the vendor full-panel-over-noise predicate.  Gaokun SS is
	 * disabled, therefore broad mutual-cap population/line noise is the only
	 * valid evidence and no synthetic self-sense data is used. */
	type2 = !game_scenario && over_noise && !clean && !touch_protected &&
		!has_signal && abnormal_nodes > algo->wake_max_unstable_nodes * 2;
	/* Type 3 is the stricter negative excursion path, suppressed in game
	 * scenarios as in TSACore. */
	type3 = !game_scenario && over_noise && !clean && !touch_protected &&
		!has_signal && negative_nodes > 2 &&
		neg > (s32)algo->baseline_peak_threshold && pos * 2 < neg * 3 &&
		abnormal_nodes > 2;

	if (type1) {
		flags |= HX_BLRESET_TYPE1;
		algo->blreset_abnormal_type_elapsed[0] = min_t(u32,
			algo->blreset_abnormal_type_elapsed[0] + interval, ~0U);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->blreset_type1_count++;
#endif
	} else {
		algo->blreset_abnormal_type_elapsed[0] = 0;
	}
	if (type2) {
		flags |= HX_BLRESET_TYPE2;
		algo->blreset_abnormal_type_elapsed[1] = min_t(u32,
			algo->blreset_abnormal_type_elapsed[1] + interval, ~0U);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->blreset_type2_count++;
#endif
	} else {
		algo->blreset_abnormal_type_elapsed[1] = 0;
	}
	if (type3) {
		flags |= HX_BLRESET_TYPE3;
		algo->blreset_abnormal_type_elapsed[2] = min_t(u32,
			algo->blreset_abnormal_type_elapsed[2] + interval, ~0U);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->blreset_type3_count++;
#endif
	} else {
		algo->blreset_abnormal_type_elapsed[2] = 0;
	}
	algo->blreset_abnormal_type_flags = flags;
	return flags;
}
