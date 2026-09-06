// SPDX-License-Identifier: GPL-2.0
/* SafeBaseline reset/debounce policy. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

void hx_safe_baseline_set_platform_state(struct hx_algo *algo,
					 const struct hx_baseline_platform_state *state)
{
	if (!state)
		return;
	algo->platform = *state;
	if (state->charger_noise)
		algo->safe_flags |= HX_SAFE_FLAG_VERY_NOISY;
	else
		algo->safe_flags &= ~HX_SAFE_FLAG_VERY_NOISY;
	if (state->panel_sd)
		algo->safe_flags |= HX_SAFE_FLAG_SENSOR_BAD;
	else
		algo->safe_flags &= ~HX_SAFE_FLAG_SENSOR_BAD;
}

void hx_safe_baseline_check_with_state(struct hx_algo *algo,
			       enum hx_finger_state finger_state,
			       bool safe_diff_checked, bool very_noisy,
			       bool very_negative, bool ghost_max,
			       bool sensor_bad)
{
	u16 flags = 0;
	u16 frame_flags = algo->safe_flags & (HX_SAFE_FLAG_GHOST_MAX |
		HX_SAFE_FLAG_VERY_NOISY | HX_SAFE_FLAG_VERY_NEGATIVE |
		HX_SAFE_FLAG_SENSOR_BAD | HX_SAFE_FLAG_SILENT_GHOST);
	u8 active = 0;
	int i;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo->tracks[i].active)
			active++;
	/* SafeBaseline_IsTouchToBeProtected is based on the previous-touch
	 * accounting, not on the current track array.  A touch which vanished from
	 * the solver is precisely the case that must be protected; requiring an
	 * active current track would therefore defeat the vendor guard. */
	if (finger_state != HX_FINGER_ABSENT &&
	    algo->safe_valid_touch_count > algo->safe_abnormal_touch_count)
		flags |= HX_SAFE_FLAG_TOUCH_PROTECTED;
	if (ghost_max)
		flags |= HX_SAFE_FLAG_GHOST_MAX;
	if (algo->safe_valid_touch_count &&
	    algo->safe_valid_touch_count == algo->safe_abnormal_touch_count)
		flags |= HX_SAFE_FLAG_ALL_TOUCH_BAD;
	if (very_noisy)
		flags |= HX_SAFE_FLAG_VERY_NOISY;
	if (very_negative)
		flags |= HX_SAFE_FLAG_VERY_NEGATIVE;
	if (safe_diff_checked)
		flags |= HX_SAFE_FLAG_SAFE_DIFF;
	if (finger_state == HX_FINGER_PRESENT && !active)
		flags |= HX_SAFE_FLAG_SIGNAL_DISPARITY;
	if (sensor_bad)
		flags |= HX_SAFE_FLAG_SENSOR_BAD;
	/* SafeBaseline_CheckWithState marks a touch-protection condition when the
	 * safe-diff pass has not completed but a previous/current touch exists.
	 * Keep this separate from TOUCH_PROTECTED: the former blocks baseline
	 * learning, while the latter describes an unmatched touch. */
	if (!safe_diff_checked && (active || algo->baseline_prev_had_signal))
		flags |= HX_SAFE_FLAG_SAFE_DIFF;
	if (ghost_max && finger_state == HX_FINGER_ABSENT)
		flags |= HX_SAFE_FLAG_SILENT_GHOST;
	if (algo->platform.charger_noise)
		flags |= HX_SAFE_FLAG_VERY_NOISY;
	if (algo->platform.panel_sd)
		flags |= HX_SAFE_FLAG_SENSOR_BAD;
	if (algo->platform.smart_cover)
		flags |= HX_SAFE_FLAG_ALL_TOUCH_BAD;
	/* Preserve the BLReset predicates produced before peak solving.  The
	 * touch-aware additions below must not erase frame-local raw/jump evidence. */
	flags |= frame_flags;
	algo->safe_prev_flags = algo->safe_flags;
	algo->safe_flags = flags;
}

bool hx_safe_baseline_is_ok_to_update(const struct hx_algo *algo)
{
	/* SafeBaseline_IsOKToUpdate: only an idle/no-touch window with enough
	 * valid touches excluded and no outstanding safety flags may update the
	 * queue.  The platform-specific grip/idle predicates collapse to the
	 * explicit BLSM stable stage on this single-panel target. */
	if ((algo->baseline_stage != HX_BLSM_NO_TOUCH_STABLE &&
	     algo->safe_commit_no_finger_frames > 3) ||
	    algo->safe_valid_touch_count || algo->safe_abnormal_touch_count)
		return false;
	if (algo->platform.idle_transition || algo->platform.charger_noise ||
	    algo->platform.proximity_active || algo->platform.panel_sd ||
	    algo->platform.smart_cover)
		return false;
	if (algo->safe_flags & (HX_SAFE_FLAG_TOUCH_PROTECTED |
		HX_SAFE_FLAG_GHOST_MAX | HX_SAFE_FLAG_ALL_TOUCH_BAD |
		HX_SAFE_FLAG_VERY_NOISY | HX_SAFE_FLAG_VERY_NEGATIVE |
		HX_SAFE_FLAG_SIGNAL_DISPARITY | HX_SAFE_FLAG_SENSOR_BAD |
		HX_SAFE_FLAG_SILENT_GHOST))
		return false;
	return true;
}

bool hx_safe_baseline_reset_statistics_variant(struct hx_algo *algo,
						 u16 reason_mask, bool safe_raw_checked,
						 bool side_variant)
{
	bool triggered = false;
	const u8 *push_thresholds = side_variant ?
		algo->safe_reset_push_threshold_side :
		algo->safe_reset_push_threshold;
	const u16 *time_thresholds = side_variant ?
		algo->safe_reset_time_threshold_side :
		algo->safe_reset_time_threshold;
	const u8 *trigger_thresholds = side_variant ?
		algo->safe_reset_trigger_count_side :
		algo->safe_reset_trigger_count;
	const u32 *screen_windows = side_variant ?
		algo->safe_reset_screen_on_window_side :
		algo->safe_reset_screen_on_window;
	u16 *reason_frames = side_variant ? algo->safe_reset_reason_frames_side :
		algo->safe_reset_reason_frames;
	u32 *last_frames = side_variant ? algo->safe_reset_reason_last_frame_side :
		algo->safe_reset_reason_last_frame;
	u8 *push_counts = side_variant ? algo->safe_reset_push_count_side :
		algo->safe_reset_push_count;
	int i;

	for (i = 0; i < 10; i++) {
		u16 bit = (u16)(1U << i);
		u16 *frames = &reason_frames[i];
		u16 threshold = algo->safe_reset_reason_trigger_frames ?
			algo->safe_reset_reason_trigger_frames :
			time_thresholds[i];
		u16 old_frames = *frames;
		u32 interval = max_t(u16, algo->frame_interval_ms, 1);
		u32 now = algo->frame_sequence * interval;
		u32 elapsed = now - last_frames[i];
		u32 since_screen_on = (algo->frame_sequence -
			algo->screen_on_frame_sequence) * interval;
		bool allowed = safe_raw_checked ||
			since_screen_on <= screen_windows[i];
		bool legacy_override = algo->safe_reset_reason_trigger_frames != 0;

		/* frame_sequence may be zero during the first preprocessing pass;
		 * use one unit there, and cap large gaps so a scheduler stall cannot
		 * immediately force a destructive reset. */
		if (!elapsed || !last_frames[i])
			elapsed = 1;
		elapsed = min_t(u32, elapsed, U16_MAX);
		last_frames[i] = now;

		/* IsResetAllowed gates the statistics unit before it is updated.  A
		 * reason whose trigger quota is exhausted must decay, not repeatedly
		 * retrigger on every subsequent frame.  The push threshold is the
		 * vendor's per-reason allowance; zero means unlimited. */
		if (!safe_raw_checked && !legacy_override &&
		    trigger_thresholds[i] &&
		    push_counts[i] >=
		    trigger_thresholds[i])
			allowed = false;
		if (!safe_raw_checked && !legacy_override && push_thresholds[i] &&
		    algo->safe_baseline_pushes >=
		    push_thresholds[i])
			allowed = false;

		if ((reason_mask & bit) && allowed) {
			*frames = min_t(u16, *frames + elapsed,
					U16_MAX);
			/* Official ResetStatisticsUnit emits one reset event when the
			 * accumulated duration crosses this reason's time threshold. */
			if (old_frames < threshold && *frames >= threshold) {
				push_counts[i] = min_t(u8,
					push_counts[i] + 1, U8_MAX);
				triggered = true;
			}
		} else if (*frames) {
			/* ResetStatisticsUnit decays by the frame interval when the
			 * corresponding abnormal bit is no longer asserted. */
			*frames = *frames > elapsed ? *frames - elapsed : 0;
			if (!*frames && push_counts[i])
				push_counts[i]--;
		}
		/* A legacy override retains the old deterministic test contract. */
		if (legacy_override && old_frames < threshold &&
		    *frames >= threshold)
			triggered = true;
	}
	if (side_variant)
		algo->safe_reset_reason_mask_side = 0;
	else
		algo->safe_reset_reason_mask = 0;
	for (i = 0; i < 10; i++)
		if (reason_frames[i] >=
			(algo->safe_reset_reason_trigger_frames ?
				 algo->safe_reset_reason_trigger_frames :
				 time_thresholds[i])) {
			if (side_variant)
				algo->safe_reset_reason_mask_side |= (u16)(1U << i);
			else
				algo->safe_reset_reason_mask |= (u16)(1U << i);
		}
	return triggered;
}

bool hx_safe_baseline_reset_statistics(struct hx_algo *algo,
						 u16 reason_mask, bool safe_raw_checked)
{
	return hx_safe_baseline_reset_statistics_variant(algo, reason_mask,
		safe_raw_checked, false);
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
	for (i = 0; i < HX_PIXELS; i++)
		sum += (s32)le16_to_cpup(raw + i) -
			(algo->safe_baseline_q8[i] >> HX_BASELINE_FRACTION_BITS);
	common = (s32)(sum / HX_PIXELS);
	if (abs(common) > algo->cmf_max_correction)
		return false;
	for (i = 0; i < HX_PIXELS; i++) {
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

	for (i = 0; i < HX_PIXELS; i++)
		sum += dif[i];
	return (s32)(sum / HX_PIXELS);
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
		if (algo->safe_bl2raw_dif[i] > algo->baseline_peak_threshold)
			positive++;
		if (algo->safe_bl2raw_dif[i] < -algo->baseline_peak_threshold)
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
	hx_safe_baseline_collect_prpt(algo->safe_bl2bl_prpt,
		algo->safe_bl2bl_dif);
	hx_safe_baseline_collect_prpt(algo->safe_bl2raw_prpt,
		algo->safe_bl2raw_dif);
	hx_safe_baseline_collect_prpt(algo->safe_bl2bl_prpt_cmf,
		algo->safe_bl2bl_dif_cmf);
	hx_safe_baseline_collect_prpt(algo->safe_bl2raw_prpt_cmf,
		algo->safe_bl2raw_dif_cmf);
	/* Keep the vendor max/min abnormal statistics alongside the buffers. */
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	for (i = 0; i < HX_PIXELS; i++) {
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

bool hx_safe_baseline_should_replace_wake(const struct hx_algo *algo)
{
	u16 raw_better = 0;
	u16 working_shifted = 0;
	int i;

	/* IsToReplaceCurrentBaseline requires two independent observations of
	 * the same wake state.  The Linux wake path has already accumulated the
	 * configured finger-safe frames; the remaining predicate mirrors the
	 * vendor BL2BL/BL2RAW comparison: enough cells must show that the raw
	 * frame explains the working-baseline error better than the saved safe
	 * grid, while the global residual remains bounded. */
	if (!algo->safe_baseline_replace_enabled ||
	    algo->wake_finger_frames < algo->wake_finger_safe_frames ||
	    !algo->safe_baseline_valid || algo->safe_flags &
		(HX_SAFE_FLAG_VERY_NOISY | HX_SAFE_FLAG_VERY_NEGATIVE |
		 HX_SAFE_FLAG_SENSOR_BAD))
		return false;
	/* The vendor requires SafeBaseline_IsDoubleChecked before replacing the
	 * current grid.  A populated Linux queue must contain observations from
	 * at least two screen epochs; a directly installed single baseline (used
	 * by legacy panels and host tests) remains eligible for the explicit
	 * opt-in path. */
	if (!hx_safe_baseline_queue_double_checked(algo))
		return false;
	for (i = 0; i < HX_PIXELS; i++) {
		s32 bl2bl = algo->safe_bl2bl_dif_cmf[i];
		s32 bl2raw = algo->safe_bl2raw_dif_cmf[i];

		if (abs(bl2bl) > algo->baseline_peak_threshold)
			working_shifted++;
		if (abs(bl2raw) < abs(bl2bl) &&
		    abs(bl2bl) > algo->baseline_peak_threshold)
			raw_better++;
	}
	return working_shifted > 4 && raw_better > 4 &&
		algo->wake_finger_frames <= algo->wake_finger_safe_frames + 5;
}

enum hx_safe_reset_action
hx_safe_baseline_reset_policy(struct hx_algo *algo,
			      enum hx_safe_compare_result result,
			      bool side_abnormal,
			      enum hx_finger_state finger_state)
{
	u8 required = max_t(u8, algo->runtime_blreset_confirm_frames, 1);
	u16 reasons = 0;

	/* BLReset_Process emits a debounced trigger independently of the
	 * SafeBaseline comparator.  Consume it exactly once: prefer restoring the
	 * immutable safe grid, and invalidate only when no recovery authority is
	 * available. */
	if (algo->blreset_triggered) {
		algo->blreset_triggered = false;
		if (hx_safe_baseline_can_recover(algo))
			return HX_SAFE_RESET_WORKING;
		return HX_SAFE_RESET_INVALIDATE;
	}
	if (algo->blreset_wake_triggered) {
		/* BLReset_IsTriggered2AfeWakeUp emits a single reset request after
		 * its independent ~160 ms vote.  Consume it exactly once, just like
		 * the normal temporal gate. */
		algo->blreset_wake_triggered = false;
		if (hx_safe_baseline_can_recover(algo))
			return HX_SAFE_RESET_WORKING;
		return HX_SAFE_RESET_INVALIDATE;
	}
	if (algo->blreset_dirty_triggered) {
		algo->blreset_dirty_triggered = false;
		if (hx_safe_baseline_can_recover(algo))
			return HX_SAFE_RESET_WORKING;
		return HX_SAFE_RESET_INVALIDATE;
	}

	if (result == HX_SAFE_COMPARE_BOTH_INVALID)
		reasons |= (u16)(1U << 5);
	if (side_abnormal)
		reasons |= (u16)(1U << 9);
	if (algo->safe_current_negative_nodes > algo->wake_max_unstable_nodes)
		reasons |= (u16)(1U << 7);
	if (algo->safe_current_positive_nodes > algo->wake_max_unstable_nodes)
		reasons |= (u16)(1U << 6);
	/* Preserve the vendor ResetCheck bit taxonomy in the statistics input,
	 * rather than collapsing every failure into BOTH_INVALID. */
	if (algo->safe_flags & HX_SAFE_FLAG_GHOST_MAX)
		reasons |= (u16)(1U << 1);
	if (algo->safe_flags & HX_SAFE_FLAG_ALL_TOUCH_BAD)
		reasons |= (u16)(1U << 3);
	if (algo->safe_flags & HX_SAFE_FLAG_VERY_NOISY)
		reasons |= (u16)(1U << 4);
	if (algo->safe_flags & HX_SAFE_FLAG_VERY_NEGATIVE)
		reasons |= (u16)(1U << 5);
	if (algo->safe_flags & HX_SAFE_FLAG_SIGNAL_DISPARITY)
		reasons |= (u16)(1U << 7);
	if (algo->safe_flags & HX_SAFE_FLAG_SENSOR_BAD)
		reasons |= (u16)(1U << 8);
	if (algo->platform.charger_noise)
		reasons |= (u16)(1U << 4);
	if (algo->platform.panel_sd)
		reasons |= (u16)(1U << 8);
	if (algo->platform.smart_cover)
		reasons |= (u16)(1U << 2);
	hx_safe_baseline_reset_statistics_variant(algo, reasons,
		result != HX_SAFE_COMPARE_BOTH_INVALID, side_abnormal);

	/* IsResetInDebounce: never invalidate recovery authority from a single
	 * bad raw frame.  A clean or merely ambiguous comparison cancels the
	 * pending reset. */
	if (result == HX_SAFE_COMPARE_BOTH_INVALID) {
		algo->safe_reset_in_debounce = true;
		algo->safe_baseline_invalid_frames = min_t(u8,
			algo->safe_baseline_invalid_frames + 1, U8_MAX);
		if (algo->safe_baseline_invalid_frames >= required) {
			bool new_trustable;

			algo->safe_reset_in_debounce = false;
			algo->safe_baseline_invalid_frames = 0;
			new_trustable = hx_safe_baseline_temp_observe(algo,
				algo->baseline_q8);
			if (new_trustable)
				return HX_SAFE_RESET_INVALIDATE;
			if (!hx_safe_baseline_queue_all_valid(algo))
				return HX_SAFE_RESET_INVALIDATE;
			/* Official IsOKToReset keeps the old queue when it is
			 * independently trustable; an invalid current comparison alone
			 * must not destroy that recovery authority. */
			if (hx_safe_baseline_queue_trustable(algo))
				return HX_SAFE_RESET_NONE;
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
	enum hx_safe_compare_result judge;
	s32 raw_common;
	bool side_abnormal = false;
	bool side_very_negative = false;

	if (!algo->safe_baseline_valid || algo->wake_qualifying) {
		/* BLReset is a controller-health state machine, not a SafeBaseline
		 * comparator.  Keep its debounce/clear lifecycle running even while no
		 * safe slot is available (or while wake qualification temporarily
		 * suppresses baseline comparisons). */
		if (algo->wake_qualifying)
			hx_blreset_process(algo, 0);
		else
			hx_blreset_process(algo, algo->safe_flags &
				(HX_SAFE_FLAG_VERY_NOISY |
				 HX_SAFE_FLAG_VERY_NEGATIVE |
				 HX_SAFE_FLAG_SENSOR_BAD));
		algo->safe_signal_stable_frames = 0;
		return false;
	}
	hx_safe_baseline_buffer_comparison(algo, raw);
	judge = hx_safe_baseline_judge_current(algo);
	raw_common = hx_safe_diff_common(algo->safe_bl2raw_dif);
	if (algo->baseline_stage == HX_BLSM_RESET ||
	    algo->baseline_stage == HX_BLSM_NOISY_RESET ||
	    algo->baseline_stage == HX_BLSM_FORCED ||
	    algo->baseline_stage == HX_BLSM_DEBOUNCE) {
		algo->safe_signal_stable_frames = 0;
		return false;
	}
	if (algo->safe_signal_stable_frames < 5)
		algo->safe_signal_stable_frames++;
	algo->safe_valid_touch_count = 0;
	algo->safe_abnormal_touch_count = 0;
	if (algo->safe_signal_stable_frames >= 5)
		side_abnormal = hx_blreset_check_abnormal_touches(algo);
	side_very_negative = hx_safe_baseline_check_side_very_negative(algo,
		raw, algo->safe_baseline_q8, raw_common);
	if (side_very_negative)
		algo->safe_flags |= HX_SAFE_FLAG_VERY_NEGATIVE;
	if (algo->safe_valid_touch_count &&
		algo->safe_valid_touch_count == algo->safe_abnormal_touch_count)
		algo->safe_flags |= HX_SAFE_FLAG_ALL_TOUCH_BAD;
	hx_blreset_wakeup_process(algo,
		algo->blreset_all_touch_abnormal ||
		(algo->safe_flags & (HX_SAFE_FLAG_ALL_TOUCH_BAD |
			HX_SAFE_FLAG_SENSOR_BAD | HX_SAFE_FLAG_VERY_NEGATIVE)));
	hx_blreset_dirty_process(algo,
		algo->blreset_over_noise_frames >= 5,
		!!(algo->safe_flags & HX_SAFE_FLAG_TOUCH_PROTECTED),
		judge == HX_SAFE_COMPARE_SAFE_BETTER ||
		!!(algo->safe_flags & (HX_SAFE_FLAG_SENSOR_BAD |
			HX_SAFE_FLAG_VERY_NEGATIVE)),
		hx_blreset_check_clean_baseline(algo, raw));
	/* SafeBaseline_CheckSafeDiff is only set after a completed safe-raw
	 * comparison, with no current touches and a small signal population.
	 * Passing this condition unconditionally while a finger is down made the
	 * Linux flag mean "comparison ran" rather than the vendor's safe-diff
	 * qualification. */
	{
		bool safe_diff_checked = algo->safe_signal_stable_frames >= 5 &&
			finger_state == HX_FINGER_ABSENT &&
			algo->safe_current_positive_nodes +
			algo->safe_current_negative_nodes < 50;
		hx_safe_baseline_check_with_state(algo, finger_state,
			safe_diff_checked,
		algo->baseline_stage == HX_BLSM_NOISY_RESET ||
			algo->baseline_stage == HX_BLSM_DEBOUNCE,
		algo->safe_current_negative_nodes > algo->wake_max_unstable_nodes,
		side_abnormal,
		algo->safe_current_positive_nodes > algo->wake_max_unstable_nodes ||
		algo->safe_current_negative_nodes > algo->wake_max_unstable_nodes);
	}
	algo->safe_flags |= hx_safe_baseline_signal_flags(algo,
		algo->safe_bl2raw_prpt_cmf[2].max,
		algo->safe_bl2raw_prpt_cmf[2].min,
		algo->safe_valid_touch_count > algo->safe_abnormal_touch_count);
	/* Feed the official-style temporal BLReset gate only after the touch
	 * geometry predicates have been evaluated.  This ordering is important:
	 * a protected/abnormal touch must contribute to the same frame's reason
	 * vote instead of being noticed one frame later. */
	{
		u16 blreset_reasons = 0;

		if (algo->safe_flags & HX_SAFE_FLAG_GHOST_MAX)
			blreset_reasons |= 1U << 1;
		if (algo->safe_flags & HX_SAFE_FLAG_ALL_TOUCH_BAD)
			blreset_reasons |= 1U << 3;
		if (algo->blreset_all_touch_abnormal)
			blreset_reasons |= 1U << 3;
		if (algo->safe_flags & HX_SAFE_FLAG_VERY_NOISY)
			blreset_reasons |= 1U << 4;
		if (algo->safe_flags & HX_SAFE_FLAG_VERY_NEGATIVE)
			blreset_reasons |= 1U << 5;
		if (algo->safe_flags & HX_SAFE_FLAG_SENSOR_BAD)
			blreset_reasons |= 1U << 8;
		if (side_abnormal)
			blreset_reasons |= 1U << 9;
		if (algo->blreset_raw_jump_elapsed_ms >= 84)
			blreset_reasons |= 1U << 7;
		hx_blreset_process(algo, blreset_reasons);
	}
	action = hx_safe_baseline_reset_policy(algo, judge, side_abnormal,
		finger_state);
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
