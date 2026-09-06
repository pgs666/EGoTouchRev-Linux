// SPDX-License-Identifier: GPL-2.0
/* Signal-level SafeBaseline_CheckWithSignal predicates. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

/* TSACore's SafeBaseline_IsVeryNoisy uses the 0x33 abnormal-node
 * qualification.  Keep this separate from the larger runtime reset quota:
 * very-noisy is a learning guard, not yet a controller reset request. */
#define HX_SAFE_VERY_NOISY_MIN_NODES 0x33

u16 hx_safe_baseline_signal_flags(const struct hx_algo *algo,
					  s16 max_signal, s16 min_signal,
					  bool has_valid_touch)
{
	u16 flags = 0;
	s32 negative = min_signal < 0 ? -(s32)min_signal : 0;
	bool ghost_candidate;

	/* SafeBaseline_IsVeryNegative: a negative excursion must dominate the
	 * positive excursion, not merely cross the noise floor.  This prevents a
	 * normal common-mode dip from requesting a reset. */
	if (min_signal < -algo->baseline_peak_threshold &&
	    (s32)max_signal * 2 < negative)
		flags |= HX_SAFE_FLAG_VERY_NEGATIVE;
	if (algo->safe_current_negative_nodes > algo->wake_max_unstable_nodes &&
	    min_signal < -algo->baseline_noise_deadband)
		flags |= HX_SAFE_FLAG_VERY_NEGATIVE;

	/* The mutual-cap equivalent of SafeBaseline_IsVeryNoisy.  A broad
	 * abnormal-node population is noise only when it is not explained by a
	 * valid touch; valid touches are handled by the touch-protection path. */
	if (!has_valid_touch &&
	    algo->safe_current_positive_nodes +
	    algo->safe_current_negative_nodes >= HX_SAFE_VERY_NOISY_MIN_NODES &&
	    max_signal > algo->baseline_peak_threshold)
		flags |= HX_SAFE_FLAG_VERY_NOISY;
	/* SafeBaseline_CheckGhostSensorByMinSigOff first requires a weak positive
	 * maximum (below roughly one third of the signal threshold).  The old
	 * Linux approximation classified every 1--4-cell island as a ghost,
	 * including legitimate low-amplitude touches.  Keep the compact-island
	 * guard, but apply the vendor-like amplitude gate before setting flags. */
	ghost_candidate = !has_valid_touch &&
		algo->safe_current_positive_nodes > 0 &&
		algo->safe_current_positive_nodes <= HX_BASELINE_CLEAN_MAX_NODES &&
		max_signal > 0 &&
		max_signal <= max_t(s16, algo->baseline_peak_threshold / 3, 1);
	if (ghost_candidate) {
		flags |= HX_SAFE_FLAG_GHOST_MAX;
		/* The silent path additionally requires more than two silent cells. */
		if (algo->safe_current_positive_nodes > 2)
			flags |= HX_SAFE_FLAG_SILENT_GHOST;
	}
	/* Signal disparity is the vendor's final safeguard for a low signal frame
	 * whose negative and positive populations disagree.  Do not assert it for
	 * a normal small common-mode dip; require both populations and a negative
	 * excursion larger than the deadband. */
	if (!has_valid_touch && algo->safe_current_positive_nodes &&
	    algo->safe_current_negative_nodes &&
	    negative > algo->baseline_noise_deadband)
		flags |= HX_SAFE_FLAG_SIGNAL_DISPARITY;

	return flags;
}
