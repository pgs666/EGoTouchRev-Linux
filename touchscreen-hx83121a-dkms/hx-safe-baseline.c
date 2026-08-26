// SPDX-License-Identifier: GPL-2.0
/* SafeBaseline reset/debounce policy. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

bool hx_safe_baseline_reset_statistics(struct hx_algo *algo,
						 u16 reason_mask, bool safe_raw_checked)
{
	bool triggered = false;
	int i;

	for (i = 0; i < 10; i++) {
		u16 bit = (u16)(1U << i);
		u16 *frames = &algo->safe_reset_reason_frames[i];

		if (reason_mask & bit) {
			*frames = min_t(u16, *frames + 1,
					U16_MAX);
		} else if (*frames) {
			/* ResetStatisticsUnit decays by the frame interval when the
			 * corresponding abnormal bit is no longer asserted. */
			*frames = *frames > 1 ? *frames - 1 : 0;
		}
		if (*frames >= max_t(u16,
			algo->safe_reset_reason_trigger_frames, 1))
			triggered = true;
	}
	algo->safe_reset_reason_mask = 0;
	for (i = 0; i < 10; i++)
		if (algo->safe_reset_reason_frames[i] >= max_t(u16,
			algo->safe_reset_reason_trigger_frames, 1))
			algo->safe_reset_reason_mask |= (u16)(1U << i);
	/* A checked raw frame suppresses destructive reset until the queue has
	 * either supplied a replacement or all reason counters agree. */
	if (safe_raw_checked && algo->safe_reset_reason_mask)
		return false;
	return triggered;
}

bool hx_safe_baseline_raw_matches_selected(const struct hx_algo *algo,
						 const u16 *raw)
{
	s64 sum = 0;
	s32 common;
	u16 divergent = 0;
	int i;

	if (!algo->safe_baseline_valid ||
	    algo->safe_baseline_selected >= HX_SAFE_BASELINE_SLOTS ||
	    !algo->safe_baselines[algo->safe_baseline_selected].valid)
		return false;
	for (i = 1; i < HX_PIXELS; i++)
		sum += (s32)le16_to_cpup(raw + i) -
			(algo->safe_baseline_q8[i] >> HX_BASELINE_FRACTION_BITS);
	common = (s32)(sum / (HX_PIXELS - 1));
	if (abs(common) > algo->cmf_max_correction)
		return false;
	for (i = 1; i < HX_PIXELS; i++) {
		s32 local = (s32)le16_to_cpup(raw + i) -
			(algo->safe_baseline_q8[i] >> HX_BASELINE_FRACTION_BITS) -
			common;

		if (abs(local) > algo->wake_raw_jump_threshold &&
		    ++divergent > algo->wake_max_unstable_nodes)
			return false;
	}
	return true;
}

static s32 hx_safe_diff_common(const s16 *dif)
{
	s64 sum = 0;
	int i;

	for (i = 1; i < HX_PIXELS; i++)
		sum += dif[i];
	return (s32)(sum / (HX_PIXELS - 1));
}

void hx_safe_baseline_buffer_comparison(struct hx_algo *algo,
						 const u16 *raw)
{
	s16 bl2bl_common;
	s16 bl2raw_common;
	int i;
	u16 positive = 0;
	u16 negative = 0;

	for (i = 0; i < HX_PIXELS; i++) {
		s32 safe = algo->safe_baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 working = algo->baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 sample = (s32)le16_to_cpup(raw + i);

		algo->safe_bl2bl_dif[i] = clamp_t(s32, working - safe,
			SHRT_MIN, SHRT_MAX);
		algo->safe_bl2raw_dif[i] = clamp_t(s32, sample - safe,
			SHRT_MIN, SHRT_MAX);
		if (i && algo->safe_bl2raw_dif[i] > algo->baseline_peak_threshold)
			positive++;
		if (i && algo->safe_bl2raw_dif[i] < -algo->baseline_peak_threshold)
			negative++;
	}
	algo->safe_current_positive_nodes = positive;
	algo->safe_current_negative_nodes = negative;
	bl2bl_common = clamp_t(s32,
		hx_safe_diff_common(algo->safe_bl2bl_dif), SHRT_MIN, SHRT_MAX);
	bl2raw_common = clamp_t(s32,
		hx_safe_diff_common(algo->safe_bl2raw_dif), SHRT_MIN, SHRT_MAX);
	for (i = 0; i < HX_PIXELS; i++) {
		algo->safe_bl2bl_dif_cmf[i] =
			clamp_t(s32, algo->safe_bl2bl_dif[i] - bl2bl_common,
				SHRT_MIN, SHRT_MAX);
		algo->safe_bl2raw_dif_cmf[i] =
			clamp_t(s32, algo->safe_bl2raw_dif[i] - bl2raw_common,
				SHRT_MIN, SHRT_MAX);
	}
	/* Keep the vendor max/min abnormal statistics alongside the buffers. */
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	for (i = 1; i < HX_PIXELS; i++) {
		if (algo->safe_bl2raw_dif_cmf[i] > algo->baseline_peak_threshold)
			algo->baseline_safe_abnormal_max_count++;
		if (algo->safe_bl2raw_dif_cmf[i] < -algo->baseline_peak_threshold)
			algo->baseline_safe_abnormal_min_count++;
	}
#endif
}

void hx_safe_baseline_replace_working_from_history(struct hx_algo *algo,
							 s32 common)
{
	int i;

	if (!algo->safe_baseline_valid)
		return;
	/* Exact translation of the vendor screen-on held-in-hand path:
	 * latestSafeBL - SafeBL2BLDif + SafeBL2BLDifCMF. */
	for (i = 0; i < HX_PIXELS; i++) {
		s32 safe = algo->safe_baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 value = safe - algo->safe_bl2bl_dif[i] +
			algo->safe_bl2bl_dif_cmf[i] + common;

		algo->baseline_q8[i] = clamp_t(s32,
			value << HX_BASELINE_FRACTION_BITS, 0,
			0xffff << HX_BASELINE_FRACTION_BITS);
	}
	algo->baseline_initialized = true;
	algo->baseline_had_freeze = true;
	algo->baseline_recovery_frames = 0;
	algo->baseline_post_reacquire_hold = 120;
}

enum hx_safe_reset_action
hx_safe_baseline_reset_policy(struct hx_algo *algo,
			      enum hx_safe_compare_result result,
			      bool side_abnormal,
			      enum hx_finger_state finger_state)
{
	u8 required = max_t(u8, algo->runtime_blreset_confirm_frames, 1);
	u16 reasons = 0;

	if (result == HX_SAFE_COMPARE_BOTH_INVALID)
		reasons |= (u16)(1U << 5);
	if (side_abnormal)
		reasons |= (u16)(1U << 9);
	if (algo->safe_current_negative_nodes > algo->wake_max_unstable_nodes)
		reasons |= (u16)(1U << 7);
	if (algo->safe_current_positive_nodes > algo->wake_max_unstable_nodes)
		reasons |= (u16)(1U << 6);
	hx_safe_baseline_reset_statistics(algo, reasons,
		result != HX_SAFE_COMPARE_BOTH_INVALID);

	/* IsResetInDebounce: never invalidate recovery authority from a single
	 * bad raw frame.  A clean or merely ambiguous comparison cancels the
	 * pending reset. */
	if (result == HX_SAFE_COMPARE_BOTH_INVALID) {
		algo->safe_reset_in_debounce = true;
		algo->safe_baseline_invalid_frames = min_t(u8,
			algo->safe_baseline_invalid_frames + 1, U8_MAX);
		if (algo->safe_baseline_invalid_frames >= required) {
			algo->safe_reset_in_debounce = false;
			algo->safe_baseline_invalid_frames = 0;
			return HX_SAFE_RESET_INVALIDATE;
		}
	} else {
		algo->safe_reset_in_debounce = false;
		algo->safe_baseline_invalid_frames = 0;
	}

	/* IsToSyncResetForSideArea: side residuals are commonly contact/display
	 * coupling.  Do not destroy the safe slot; once firmware independently
	 * confirms no finger for a complete debounce window, restore only the
	 * working grid from the selected safe slot. */
	if (side_abnormal) {
		algo->safe_sync_reset_side_area = true;
		algo->safe_side_reset_frames = 0;
	} else if (algo->safe_sync_reset_side_area &&
		   finger_state == HX_FINGER_ABSENT) {
		algo->safe_side_reset_frames = min_t(u8,
			algo->safe_side_reset_frames + 1, U8_MAX);
		if (algo->safe_side_reset_frames >= required) {
			algo->safe_sync_reset_side_area = false;
			algo->safe_side_reset_frames = 0;
			return HX_SAFE_RESET_WORKING;
		}
	} else if (!algo->safe_sync_reset_side_area) {
		algo->safe_side_reset_frames = 0;
	}

	return HX_SAFE_RESET_NONE;
}

bool hx_safe_baseline_postprocess(struct hx_algo *algo, const u16 *raw,
				  enum hx_finger_state finger_state)
{
	enum hx_safe_reset_action action;
	s32 raw_common;
	bool side_abnormal = false;
	int i;

	if (!algo->safe_baseline_valid || algo->wake_qualifying) {
		algo->safe_signal_stable_frames = 0;
		return false;
	}
	hx_safe_baseline_buffer_comparison(algo, raw);
	raw_common = hx_safe_diff_common(algo->safe_bl2raw_dif);
	if (algo->baseline_stage == HX_BLSM_SEVERE_NOISE ||
	    algo->baseline_stage == HX_BLSM_SPECIAL) {
		algo->safe_signal_stable_frames = 0;
		return false;
	}
	if (algo->safe_signal_stable_frames < 5)
		algo->safe_signal_stable_frames++;
	algo->safe_valid_touch_count = 0;
	algo->safe_abnormal_touch_count = 0;
	if (algo->safe_signal_stable_frames >= 5) {
		for (i = 0; i < algo->peak_count; i++) {
			const struct hx_peak *peak = &algo->peaks[i];
			int idx;
			s32 raw_diff;
			s32 working_diff;

			if (peak->r > 1 && peak->r < HX_ROWS - 2 &&
			    peak->c > 1 && peak->c < HX_COLS - 2)
				continue;
			algo->safe_valid_touch_count++;
			idx = peak->r * HX_COLS + peak->c;
			raw_diff = abs(algo->safe_bl2raw_dif_cmf[idx]);
			working_diff = abs(algo->safe_bl2bl_dif_cmf[idx]);
			/* SafeBaseline_IsAbnormalSensor compares |safe->raw| with
			 * |safe->working|.  Raw being closer means the working grid
			 * has absorbed the edge contact. */
			if (raw_diff < working_diff) {
				algo->safe_abnormal_touch_count++;
				side_abnormal = true;
			}
		}
	}
	action = hx_safe_baseline_reset_policy(algo,
		HX_SAFE_COMPARE_BOTH_VALID, side_abnormal, finger_state);
	if (action != HX_SAFE_RESET_WORKING ||
	    !hx_safe_baseline_can_recover(algo))
		return false;
	hx_restore_working_from_safe(algo, raw_common, true, false);
	algo->runtime_blreset_cooldown_frames = algo->runtime_blreset_cooldown;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_safe_side_reset_count++;
#endif
	return true;
}
