// SPDX-License-Identifier: GPL-2.0
/* BLSM_GetProperty equivalent for the enabled mutual-cap path. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

u16 hx_baseline_get_property(struct hx_algo *algo, s16 max_signal,
			     bool has_signal, bool operational_clean,
			     bool blrecal_request, bool shb_noisy_action,
			     bool line_noise, bool out_of_range)
{
	/* BLSM_GetProperty priority is reset > noisy > debounce > forced.  The
	 * numeric values are the actual TSACore property bits, not stage numbers;
	 * hx_baseline_stage_update() performs the subsequent mapping. */
	if (blrecal_request)
		return HX_BLSM_PRPT_WEAK_SIGNAL;
	if (shb_noisy_action)
		return HX_BLSM_PRPT_NOISY;
	if (algo->wake_qualifying || algo->platform.panel_sd ||
	    algo->platform.proximity_active || algo->platform.idle_transition)
		return HX_BLSM_PRPT_FORCED;
	/* SafeBaseline/BLReset touch protection wraps the no-touch property
	 * branch in TSACore.  While the held-in-hand guard owns the frame, BLSM
	 * must remain in a hold/touch stage instead of resetting from its residual. */
	if (algo->baseline_touch_hold ||
	    (algo->safe_flags & HX_SAFE_FLAG_TOUCH_PROTECTED))
		return HX_BLSM_PRPT_TOUCH;
	/* TSACore reaches BLReset_Process only from the no-touch property branch.
	 * Raw-derived SENSOR_BAD/VERY_NEGATIVE evidence must therefore not turn a
	 * newly detected real contact into a baseline reset.  Explicit platform
	 * reset inputs above retain their priority. */
	if (!has_signal && (algo->safe_flags & (HX_SAFE_FLAG_SENSOR_BAD |
		HX_SAFE_FLAG_VERY_NEGATIVE)))
		return HX_BLSM_PRPT_RESET;
	if (line_noise || out_of_range)
		return HX_BLSM_PRPT_NOISY;
	if (!has_signal && (algo->safe_flags & (HX_SAFE_FLAG_GHOST_MAX |
		HX_SAFE_FLAG_SIGNAL_DISPARITY | HX_SAFE_FLAG_SILENT_GHOST)))
		return HX_BLSM_PRPT_DEBOUNCE;
	if (has_signal)
		return HX_BLSM_PRPT_TOUCH;
	if (!operational_clean)
		return HX_BLSM_PRPT_WEAK_SIGNAL;

	/* BLSM_GetProperty emits the small no-touch property only when the
	 * maximum signal is below the dynamic threshold.  The caller has already
	 * applied the panel's max/min gate; retain the argument to make that
	 * contract explicit and avoid silently classifying an out-of-range frame
	 * as a stable background frame. */
	if (max_signal < algo->baseline_peak_threshold)
		return 0;
	return HX_BLSM_PRPT_WEAK_SIGNAL;
}
