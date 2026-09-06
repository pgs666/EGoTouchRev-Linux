// SPDX-License-Identifier: GPL-2.0
/* BLIIR actions selected by the vendor baseline state machine. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

s32 hx_bliir_update_cell_q8(s32 baseline_q8, u16 raw_value, s16 step)
{
	s32 sample_q8 = (s32)raw_value << HX_BASELINE_FRACTION_BITS;
	s32 delta = sample_q8 - baseline_q8;
	s32 step_q8 = max_t(s32, step, 0) << HX_BASELINE_FRACTION_BITS;

	/* BLIIR_DoUpdate: values inside one step are intentionally held; this is
	 * a bounded tracker, not an averaging filter. */
	if (!step_q8 || abs(delta) < step_q8)
		return baseline_q8;
	if (delta > 0)
		baseline_q8 += step_q8;
	else
		baseline_q8 -= step_q8;
	return clamp_t(s32, baseline_q8, 0,
		0xffff << HX_BASELINE_FRACTION_BITS);
}

static void hx_bliir_reset(struct hx_algo *algo, const u16 *raw)
{
	hx_copy_raw_to_baseline(algo->baseline_q8, raw);
	algo->baseline_initialized = true;
	algo->baseline_recovery_frames = 0;
	algo->baseline_had_freeze = false;
	memset(algo->baseline_release_hold, 0,
	       sizeof(algo->baseline_release_hold));
}

static bool hx_bliir_limits_allow(s16 max_signal, s16 min_signal,
				  s32 limit, bool force_negative)
{
	/* BLIIR_Update(force, limit, step): force bypasses only the negative
	 * bound; the positive maximum must still remain below limit. */
	limit = max_t(s32, limit, 0);
	return (force_negative || min_signal >= -limit) &&
		max_signal <= limit;
}

void hx_baseline_stage_process(struct hx_algo *algo, const u16 *raw,
			       const u16 *pre_cmf_raw, s16 max_signal,
			       s16 min_signal, bool has_signal,
			       bool operational_clean)
{
	const u16 *reset_raw = algo->bliir_use_pre_cmf_raw ? pre_cmf_raw : raw;
	s32 no_touch_limit = max_t(s32, algo->baseline_peak_threshold, 1);
	bool limits_ok = hx_bliir_limits_allow(max_signal, min_signal,
		no_touch_limit, false);

	algo->baseline_stage_allows_update = false;
	algo->baseline_stage_force_update = false;
	algo->baseline_stage_reset = false;
	algo->baseline_stage_update_step = algo->baseline_no_finger_max_step;
	algo->baseline_stage_action = 0; /* hold/no-op */

	/* This is the action jump table in BLSM_ProcessStage.  Stage 0 uses the
	 * ordinary BLIIR_Update gate; stages 7/8 use the after-touch update; stage
	 * 9 is the explicit forced update.  Stages 1/2 reset, while touch and
	 * forced/SD stages hold the current working baseline. */
	switch (algo->baseline_stage) {
	case HX_BLSM_NO_TOUCH_STABLE:
		algo->baseline_stage_allows_update = limits_ok &&
			!has_signal && operational_clean;
		/* BLSM_GetNoTouchBlStep derives a smaller correction from the
		 * observed negative signal, capped at one quarter of the normal step.
		 * Keep the same signed-safe arithmetic in Q0 units. */
		if (min_signal < 0) {
			s16 adaptive = (s16)min_t(s32,
				abs((s32)min_signal) / 16,
				max_t(s16, algo->baseline_no_finger_max_step, 1) / 4);
			algo->baseline_stage_update_step = max_t(s16, adaptive, 1);
		}
		if (algo->baseline_stage_allows_update)
			algo->baseline_stage_action = 1;
		break;
	case HX_BLSM_RESET:
		hx_bliir_reset(algo, reset_raw);
		algo->baseline_stage_reset = true;
		algo->baseline_stage_action = 2;
		break;
	case HX_BLSM_NOISY_RESET:
		/* The recovered ProcessStage jump table sends both stage 1 and stage 2
		 * through BLSM_Reset.  Higher-level BLReset/SafeBaseline predicates own
		 * touch protection; changing stage 2 into a hold is not vendor-equivalent. */
		/* In TSACore stage 2 is reachable only through the no-touch branch of
		 * BLSM_GetProperty.  Preserve that precondition explicitly because the
		 * Linux firmware-finger/raw classifier is not the private TSA predicate. */
		if (!has_signal) {
			hx_bliir_reset(algo, reset_raw);
			algo->baseline_stage_reset = true;
			algo->baseline_stage_action = 2;
		}
		break;
	case HX_BLSM_FORCED:
		/* Jump-table case 3 copies the previous stage back.  Forced is a
		 * transient property, not a baseline action of its own. */
		algo->baseline_stage_action = 0;
		break;
	case HX_BLSM_POST_TOUCH:
	case HX_BLSM_POST_TOUCH_ALT:
		/* TSADynPrmt_UpdateStepAftTch derives +0x52 as threshold/64.
		 * Stages 7/8 use that larger after-touch step and +0x4a/+0x4c
		 * (threshold-1) as their symmetric signal limit. */
		algo->baseline_stage_update_step = clamp_t(s32,
			no_touch_limit / 64, 1, U8_MAX);
		algo->baseline_stage_allows_update = !has_signal &&
			hx_bliir_limits_allow(max_signal, min_signal,
				no_touch_limit - 1, false);
		if (algo->baseline_stage_allows_update)
			algo->baseline_stage_action = 1;
		break;
	case HX_BLSM_DEBOUNCE:
		/* Case 9 calls BLIIR_Update(1, -minSig, step).  The force bit bypasses
		 * the negative bound only; step is min(-minSig/16, threshold/4). */
		algo->baseline_stage_update_step = max_t(s16, 1,
			min_t(s32, min_signal < 0 ? -(s32)min_signal / 16 : 0,
				no_touch_limit / 4));
		algo->baseline_stage_force_update = true;
		algo->baseline_stage_allows_update = hx_bliir_limits_allow(
			max_signal, min_signal, min_signal < 0 ? -(s32)min_signal : 0,
			true);
		if (algo->baseline_stage_allows_update)
			algo->baseline_stage_action = 3;
		break;
	case HX_BLSM_PRE_TOUCH:
	case HX_BLSM_TOUCH:
	case HX_BLSM_TOUCH_RELEASE:
	default:
		break;
	}
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	switch (algo->baseline_stage_action) {
	case 1: algo->baseline_stage_update_count++; break;
	case 2: algo->baseline_stage_reset_action_count++; break;
	case 3: algo->baseline_stage_force_count++; break;
	default: algo->baseline_stage_hold_count++; break;
	}
#endif
}
