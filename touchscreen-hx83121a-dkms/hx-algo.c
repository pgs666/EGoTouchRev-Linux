// SPDX-License-Identifier: GPL-2.0
/* Himax HX83121A algorithm state and wake handling. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

#define HIMAX_TRACK_MATCH_DIST2   (420 * 420)
#define HIMAX_TRACK_LOST_FRAMES   4
#define HIMAX_NEW_TOUCH_DEBOUNCE  1
#define HIMAX_WAKE_TOUCH_MASK_RADIUS 2
#define HIMAX_WAKE_MAX_TOUCH_CELLS   (HX_PIXELS / 3)

/* Exported TSACore globals from the Gaokun Windows build.  The first seven
 * entries are the common reset reasons; the remaining entries cover the
 * less frequent side/special reasons.  Keep these as data, rather than
 * scattering magic values through the policy implementation.
 */
static const u16 hx_safe_reset_time_default[10] = {
	500, 500, 500, 500, 500, 500, 500, 500, 2000, 500,
};
static const u8 hx_safe_reset_push_default[10] = {
	1, 1, 1, 1, 1, 1, 1, 0, 0, 0,
};
static const u8 hx_safe_reset_trigger_default[10] = {
	5, 5, 5, 5, 5, 5, 50, 5, 5, 5,
};
static const u32 hx_safe_reset_screen_on_default[10] = {
	300000, 300000, 300000, 300000, 300000,
	300000, 300000, 600000, 600000, 600000,
};

void hx_algo_init_defaults(struct hx_algo *algo)
{
	/* Values track the current Windows solver defaults.  The baseline is
	 * adaptive per cell; using one immutable 0x7ffe value was a major source
	 * of weak-frame loss after temperature/VCOM/common-mode drift.
	 */
	algo->baseline_enabled              = true;
	/* The Gaokun path normally runs at 120 Hz (about 8 ms/frame). */
	algo->frame_interval_ms              = 8;
	algo->safe_baseline_replace_enabled = false;
	/* Gaokun g_tsaPrmtFlash + 0x5fc, used by BLIIR_Reset in raw-unified
	 * mode.  Ordinary reset still copies the pre-CMF raw snapshot.
	 */
	algo->baseline_initial              = 0x7fff;
	algo->blrecal_raw_center             = 0x8000;
	algo->blrecal_raw_span               = 8000;
	algo->blrecal_signal_threshold       = 600;
	algo->baseline_noise_deadband       = 90;
	algo->baseline_positive_deadband    = 14;
	algo->baseline_negative_deadband    = 13;
	algo->baseline_peak_threshold       = 305;
	algo->baseline_release_hold_frames  = 60;
	algo->baseline_positive_alpha_shift = 7;
	algo->baseline_negative_alpha_shift = 5;
	algo->baseline_noise_alpha_shift    = 6;
	algo->baseline_positive_max_step    = 20;
	algo->baseline_negative_max_step    = 20;
	/* Windows BLIIR moves by a bounded step only after the whole frame is
	 * eligible.  Keep common-mode recovery responsive, but make spatial
	 * learning deliberately slow once that global gate has passed.
	 */
	algo->baseline_background_alpha_shift = 0;
	algo->baseline_no_finger_alpha_shift = 6;
	algo->baseline_recovery_alpha_shift = 2;
	algo->baseline_background_max_step = 17;
	algo->baseline_no_finger_max_step = 1;
	algo->baseline_recovery_max_step = 256;
	algo->baseline_recovery_max_frames = 30;
	algo->baseline_noise_tracking = true;
	algo->wake_stable_frames = 6;
	algo->wake_finger_safe_frames = 3;
	algo->wake_raw_jump_threshold = 160;
	algo->wake_max_unstable_nodes = 24;
	algo->wake_max_unstable_line_nodes = 12;
	algo->safe_commit_no_finger_frames = 30;
	algo->runtime_blreset_enabled = true;
	algo->runtime_blreset_confirm_frames = 5;
	algo->runtime_blreset_cooldown = 240;
	algo->runtime_noise_threshold = 1800;
	algo->runtime_noise_line_nodes = 20;
	algo->runtime_noise_total_nodes = 180;
	for (int i = 0; i < 10; i++) {
		algo->safe_reset_push_threshold[i] =
			hx_safe_reset_push_default[i];
		algo->safe_reset_time_threshold[i] =
			hx_safe_reset_time_default[i];
		algo->safe_reset_trigger_count[i] =
			hx_safe_reset_trigger_default[i];
		algo->safe_reset_screen_on_window[i] =
			hx_safe_reset_screen_on_default[i];
		algo->safe_reset_push_threshold_side[i] =
			hx_safe_reset_push_default[i];
		algo->safe_reset_time_threshold_side[i] =
			hx_safe_reset_time_default[i];
		algo->safe_reset_trigger_count_side[i] =
			hx_safe_reset_trigger_default[i];
		algo->safe_reset_screen_on_window_side[i] =
			hx_safe_reset_screen_on_default[i];
	}
	algo->safe_candidate_armed = true;
	algo->safe_queue_full_pushes = 0;
	algo->safe_prev_flags = algo->safe_flags;
	algo->safe_flags = 0;
	algo->cmf_enabled        = true;
	/* HX83121A firmware uses flash[0x69] = 1: BLIIR consumes the
	 * pre-CMF snapshot, while detection continues to use the CMF output.
	 */
	algo->bliir_use_pre_cmf_raw = true;
	algo->cmf_exclusion      = 2000;
	algo->cmf_max_correction = 2000;
	/* v1.1.2 removed GridIIR from the active pipeline.  Keep the compatible
	 * sysfs implementation available, but do not enable it by default.
	 */
	algo->iir_enabled        = false;
	algo->iir_decay_weight   = 200;
	algo->iir_decay_step     = 80;
	algo->iir_noise_floor    = 5;
	algo->iir_gate_floor     = 200;
	algo->iir_gate_ratio_q8  = 26;
	algo->macro_threshold    = 280;
	algo->peak_threshold     = 280;
	algo->peak_local_radius = 1;
	algo->peak_z8_enabled = true;
	algo->peak_saddle_enabled = true;
	algo->peak_saddle_radius = 2;
	algo->peak_saddle_drop = 80;
	algo->peak_signal_threshold_limit = 1000;
	algo->peak_edge_threshold = 300;
	algo->peak_macro_min_area = 3;
	algo->peak_continue_min_area = 1;
	algo->peak_continue_min_signal = 900;
	algo->peak_single_track_continue_min_signal = 650;
	algo->peak_continue_dist2 = 220 * 220;
	algo->peak_fast_start_min_signal = 1500;
	algo->peak_fast_start_edge_cells = 4;
	algo->palm_enabled       = true;
	algo->palm_area_threshold    = 50;
	algo->palm_signal_threshold  = 80000;
	algo->palm_density_low       = 400;
	algo->palm_box_enabled = true;
	algo->palm_box_expand_rows = 9;
	algo->palm_box_expand_cols = 10;
	algo->palm_box_match_distance = 6;
	algo->palm_box_max_hold = 0;
	algo->zone_cleanup_enabled = true;
	algo->zone_max_radius = 3;
	algo->zone_threshold_numer = 0x40;
	algo->zone_threshold_shift = 7;
	algo->pressure_enabled   = false;
	algo->edge_comp_enabled = true;
	algo->edge_boost_pct   = 50;   /* 50% signal boost on border pixels  */
	algo->edge_push_q8     = 128;  /* push up to 0.5 grid cells outward  */
	algo->edge_blend_q8    = 512;  /* blend over 2 grid cells from edge  */
	algo->edge_reject_enabled = true;
	algo->edge_reject_margin = 24;
	algo->edge_reject_min_signal = 500;
	algo->track_dist2_max   = HIMAX_TRACK_MATCH_DIST2;
	algo->track_lost_frames = HIMAX_TRACK_LOST_FRAMES;
	algo->debounce_base     = HIMAX_NEW_TOUCH_DEBOUNCE;
	algo->track_smoothing   = true;
	algo->track_active_guard   = true;
	algo->track_start_debounce = 1;
	algo->track_jump_dist2     = 0;  /* disabled by default */
	algo->hungarian_enabled = true;
	algo->debounce_weak_extra = 1;
	algo->debounce_edge_extra = 1;
	algo->debounce_strong_signal = 3000;
	algo->firmware_edge_fast_start = true;
	algo->split_peak_confirm_frames = 8;
	algo->split_peak_dist2 = 300 * 300;
	algo->split_cross_zone_confirm_frames = 4;
	algo->split_cross_zone_dist2 = 180 * 180;
	algo->track_peak_id_penalty = 40 * 40;
	algo->ghost_enabled = true;
	algo->ghost_row_distance = 32;
	algo->ghost_weak_ratio_q8 = 96;
	algo->ghost_min_col_distance = 300;
	algo->euro_enabled = true;
	algo->euro_alpha_min_q8 = 64;
	algo->euro_alpha_max_q8 = 224;
	algo->euro_speed_threshold = 24;
}

static void hx_algo_clear_transient_state(struct hx_algo *algo)
{
	/* Scratch/result arrays are guarded by their counts or cleared by the
	 * pipeline stage that consumes them.  Reset only persistent state here;
	 * bulk-clearing every backing array added latency without changing what
	 * the next frame can observe.
	 */
	memset(algo->frame, 0, sizeof(algo->frame));
	memset(algo->peak_competition, 0, sizeof(algo->peak_competition));
	memset(algo->baseline_release_hold, 0,
	       sizeof(algo->baseline_release_hold));
	memset(algo->tracks, 0, sizeof(algo->tracks));
	/* An invalid history makes the first frame after a runtime IIR enable
	 * seed the complete buffer before it is read.
	 */
	algo->iir_initialized = false;
	algo->prev_raw_valid = false;
	memset(algo->prev_raw, 0, sizeof(algo->prev_raw));
	algo->blreset_raw_jump_frames = 0;
	algo->blreset_raw_jump_elapsed_ms = 0;
	algo->blreset_over_noise_frames = 0;
	algo->blreset_state = HX_BLRESET_IDLE;
	algo->blreset_reason_mask = 0;
	algo->blreset_reason_elapsed_ms = 0;
	algo->blreset_abnormal_type_flags = 0;
	memset(algo->blreset_abnormal_type_elapsed, 0,
	       sizeof(algo->blreset_abnormal_type_elapsed));
	algo->blreset_triggered = false;
	algo->blreset_all_touch_abnormal = false;
	algo->blreset_concurrent_touch = false;
	algo->blreset_baseline_state = 0;
	algo->blreset_baseline_stable_frames = 0;
	algo->blreset_baseline_elapsed_ms = 0;
	algo->blreset_normal_baseline_ready = false;
	algo->blreset_clean_baseline_captured = false;
	algo->blreset_wake_abnormal_frames = 0;
	algo->blreset_wake_abnormal_elapsed_ms = 0;
	algo->blreset_wake_triggered = false;
	algo->blreset_dirty_elapsed_ms = 0;
	algo->blreset_dirty_triggered = false;
	algo->shb_state = 0;
	algo->shb_flags = 0;
	algo->shb_capture_frame = 0;
	memset(algo->shb_raw_capture, 0, sizeof(algo->shb_raw_capture));
	algo->blrecal_frame = 0;
	algo->blrecal_abnormal_count = 0;
	algo->blrecal_requested = false;
	/* Preserve the converged per-cell baseline across display/lid/idle and
	 * hardware reinitialisation.  Force the next valid frame to re-evaluate
	 * recovery instead of inheriting a stale touch/freeze transition.
	 */
	algo->baseline_prev_had_signal = false;
	algo->baseline_had_freeze = algo->baseline_initialized;
	algo->baseline_recovery_frames = 0;
	algo->baseline_no_touch_stable_frames = 0;
	algo->baseline_stage = HX_BLSM_NO_TOUCH_STABLE;
	algo->baseline_prev_stage = HX_BLSM_NO_TOUCH_STABLE;
	algo->baseline_stage_frames = 0;
	algo->baseline_stage_elapsed_ms = 0;
	algo->baseline_touch_latched = false;
	algo->baseline_stage_allows_update = false;
	algo->baseline_stage_force_update = false;
	algo->baseline_stage_reset = false;
	algo->baseline_stage_update_step = 0;
	algo->baseline_stage_action = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_stage_update_count = 0;
	algo->baseline_stage_reset_action_count = 0;
	algo->baseline_stage_hold_count = 0;
	algo->baseline_stage_force_count = 0;
#endif
	algo->baseline_touch_hold = false;
	algo->baseline_touch_seen = false;
	algo->baseline_held_in_hand = false;
	algo->baseline_touch_release_frames = 0;
	algo->baseline_post_reacquire_hold = 0;
	algo->baseline_reacquire_pending = false;
	algo->baseline_screen_on_hand_state = HX_HAND_NONE;
	algo->baseline_guard_state = HX_BASELINE_GUARD_NORMAL;
	algo->baseline_guard_clean_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_safe_compare_result = HX_SAFE_COMPARE_NOT_RUN;
	algo->diag_safe_improvement_frames = 0;
	algo->diag_safe_regression_frames = 0;
	algo->diag_blreset_recommended = false;
	algo->diag_frame_seq = 0;
	algo->diag_common_diff = 0;
	algo->diag_frame_max = 0;
	algo->diag_has_signal = 0;
	algo->diag_zones = 0;
	algo->diag_peaks = 0;
	algo->diag_contacts_pre_filter = 0;
	algo->diag_contacts_post_filter = 0;
	algo->diag_active_tracks = 0;
	algo->diag_reported_tracks = 0;
#endif
	algo->zone_arena_used = 0;
	algo->zone_count = 0;
	algo->peak_count = 0;
	algo->prev_peak_count = 0;
	algo->next_peak_id = 1;
	algo->contact_count = 0;
	algo->palm_box_count = 0;
	algo->touch_active = false;
	algo->touch_start_frames = 0;
	algo->firmware_finger_present = false;
	algo->fast_edge_start_pending = false;
	algo->wake_raw_finger_override = false;
	algo->wake_raw_finger_release_frames = 0;
	algo->safe_no_finger_frames = 0;
	algo->safe_candidate_armed = false;
	algo->safe_candidate_confirming = false;
	algo->safe_candidate_screen_epoch = 0;
	algo->safe_confirm_common_sum = 0;
	algo->safe_confirm_common_last = 0;
	algo->runtime_safe_improvement_frames = 0;
	algo->runtime_safe_regression_frames = 0;
	algo->safe_baseline_invalid_frames = 0;
	algo->safe_side_reset_frames = 0;
	algo->safe_reset_in_debounce = false;
	algo->safe_sync_reset_side_area = false;
	algo->safe_signal_stable_frames = 0;
	algo->safe_valid_touch_count = 0;
	algo->safe_abnormal_touch_count = 0;
	algo->safe_current_positive_nodes = 0;
	algo->safe_current_negative_nodes = 0;
	algo->runtime_blreset_cooldown_frames = 0;
}

void hx_algo_clear_live_state(struct hx_algo *algo)
{
	hx_algo_clear_transient_state(algo);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->live_clear_count++;
#endif
}

void hx_algo_full_reset(struct hx_algo *algo)
{
	hx_algo_clear_transient_state(algo);
	memset(algo->baseline_q8, 0, sizeof(algo->baseline_q8));
	memset(algo->safe_baseline_q8, 0, sizeof(algo->safe_baseline_q8));
	memset(algo->wake_candidate_q8, 0,
	       sizeof(algo->wake_candidate_q8));
	memset(algo->safe_baselines, 0, sizeof(algo->safe_baselines));
	memset(&algo->platform, 0, sizeof(algo->platform));
	hx_safe_baseline_queue_reset(algo);
	hx_safe_baseline_temp_reset(algo);
	/* TSACore_ResetInit owns these statistics.  A display/idle live-clear
	 * must not erase accumulated abnormal-duration evidence.
	 */
	memset(algo->safe_reset_reason_frames, 0,
	       sizeof(algo->safe_reset_reason_frames));
	memset(algo->safe_reset_reason_last_frame, 0,
	       sizeof(algo->safe_reset_reason_last_frame));
	memset(algo->safe_reset_reason_frames_side, 0,
	       sizeof(algo->safe_reset_reason_frames_side));
	memset(algo->safe_reset_reason_last_frame_side, 0,
	       sizeof(algo->safe_reset_reason_last_frame_side));
	for (int i = 0; i < 10; i++)
		algo->safe_reset_push_count[i] = 0;
	for (int i = 0; i < 10; i++)
		algo->safe_reset_push_count_side[i] = 0;
	algo->safe_reset_reason_mask = 0;
	algo->safe_reset_reason_mask_side = 0;
	/* Zero means use the per-reason official time threshold.  Host tests may
	 * set this explicitly as a deterministic threshold override.
	 */
	algo->safe_reset_reason_trigger_frames = 0;
	algo->baseline_initialized = false;
	algo->blreset_state = HX_BLRESET_IDLE;
	algo->blreset_reason_mask = 0;
	algo->blreset_reason_elapsed_ms = 0;
	algo->blreset_trigger_count = 0;
	algo->blreset_clear_count = 0;
	algo->blreset_triggered = false;
	algo->blreset_baseline_state = 0;
	algo->blreset_baseline_stable_frames = 0;
	algo->blreset_baseline_elapsed_ms = 0;
	algo->blreset_normal_baseline_ready = false;
	algo->blreset_clean_baseline_captured = false;
	algo->blreset_wake_abnormal_frames = 0;
	algo->blreset_wake_abnormal_elapsed_ms = 0;
	algo->blreset_wake_triggered = false;
	algo->blreset_dirty_elapsed_ms = 0;
	algo->blreset_dirty_triggered = false;
	algo->normal_baseline_valid = false;
	memset(algo->normal_baseline_q8, 0,
	       sizeof(algo->normal_baseline_q8));
	algo->baseline_hw_reset = false;
	algo->safe_baseline_valid = false;
	algo->safe_baseline_count = 0;
	algo->safe_baseline_selected = 0;
	algo->safe_flags = 0;
	algo->safe_prev_flags = 0;
	algo->safe_baseline_selected_score = 0;
	algo->safe_baseline_next = 0;
	algo->safe_baseline_generation = 0;
	algo->screen_epoch = 0;
	algo->frame_sequence = 0;
	algo->wake_qualifying = false;
	algo->wake_candidate_valid = false;
	algo->wake_needs_double_confirm = false;
	algo->wake_candidate_frames = 0;
	algo->wake_finger_frames = 0;
	algo->wake_finger_reject_frames = 0;
	algo->wake_finger_common_sum = 0;
	algo->wake_finger_common_last = 0;
	algo->safe_no_finger_frames = 0;
	algo->safe_candidate_armed = true;
	algo->safe_candidate_confirming = false;
	algo->safe_candidate_screen_epoch = 0;
	algo->safe_confirm_common_sum = 0;
	algo->safe_confirm_common_last = 0;
	algo->runtime_safe_improvement_frames = 0;
	algo->runtime_safe_regression_frames = 0;
	algo->safe_baseline_invalid_frames = 0;
	algo->safe_side_reset_frames = 0;
	algo->safe_reset_in_debounce = false;
	algo->safe_sync_reset_side_area = false;
	algo->safe_signal_stable_frames = 0;
	algo->safe_valid_touch_count = 0;
	algo->safe_abnormal_touch_count = 0;
	algo->safe_current_positive_nodes = 0;
	algo->safe_current_negative_nodes = 0;
	algo->safe_reset_reason_trigger_frames = 0;
	algo->runtime_blreset_cooldown_frames = 0;
	algo->baseline_prev_had_signal = false;
	algo->baseline_had_freeze = false;
	algo->baseline_recovery_frames = 0;
	algo->baseline_no_touch_stable_frames = 0;
	algo->baseline_stage = HX_BLSM_NO_TOUCH_STABLE;
	algo->baseline_prev_stage = HX_BLSM_NO_TOUCH_STABLE;
	algo->baseline_stage_frames = 0;
	algo->baseline_stage_elapsed_ms = 0;
	algo->baseline_touch_latched = false;
	algo->baseline_stage_allows_update = false;
	algo->baseline_stage_force_update = false;
	algo->baseline_stage_reset = false;
	algo->baseline_stage_update_step = 0;
	algo->baseline_stage_action = 0;
	algo->baseline_post_reacquire_hold = 0;
	algo->baseline_screen_on_hand_state = HX_HAND_NONE;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_generation++;
	algo->full_reset_count++;
#endif
}

void hx_copy_raw_to_baseline(s32 *dst, const u16 *raw)
{
	int i;

	for (i = 0; i < HX_PIXELS; i++)
		dst[i] = (s32)le16_to_cpup(raw + i) <<
			 HX_BASELINE_FRACTION_BITS;
}

void hx_restore_working_from_safe(struct hx_algo *algo, s32 common,
				  bool touch_hold, bool touch_seen)
{
	int i;

	/* Diagnostics can freeze the working grid.  Wake fallback and runtime
	 * BLReset must obey the same switch as ordinary per-frame learning.
	 */
	if (!algo->baseline_enabled)
		return;
	for (i = 0; i < HX_PIXELS; i++)
		algo->baseline_q8[i] = clamp_t(s32,
			algo->safe_baseline_q8[i] +
			common * (1 << HX_BASELINE_FRACTION_BITS),
			0, 0xffff << HX_BASELINE_FRACTION_BITS);
	memset(algo->baseline_release_hold, 0,
	       sizeof(algo->baseline_release_hold));
	algo->baseline_initialized = true;
	algo->baseline_prev_had_signal = true;
	algo->baseline_had_freeze = true;
	algo->baseline_recovery_frames = 0;
	algo->baseline_touch_hold = touch_hold;
	algo->baseline_touch_seen = touch_hold && touch_seen;
	algo->baseline_held_in_hand = touch_hold;
	algo->baseline_touch_release_frames = 0;
	algo->baseline_guard_state = touch_hold ?
		HX_BASELINE_GUARD_PROTECTED : HX_BASELINE_GUARD_NORMAL;
	algo->baseline_screen_on_hand_state = touch_hold ?
		HX_HAND_PROTECTED : HX_HAND_NONE;
	algo->baseline_guard_clean_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	if (touch_hold)
		algo->baseline_touch_hold_count++;
#endif
}

static bool hx_safe_baselines_compatible(const s32 *first_q8,
					 const s32 *second_q8)
{
	s64 sum = 0;
	s32 common;
	s32 threshold = HX_BASELINE_CLEAN_LOCAL_THRESHOLD;
	u16 divergent = 0;
	int i;

	for (i = 1; i < HX_PIXELS; i++)
		sum += (first_q8[i] - second_q8[i]) >>
			HX_BASELINE_FRACTION_BITS;
	common = (s32)(sum / (HX_PIXELS - 1));
	if (abs(common) > HX_BASELINE_CLEAN_LOCAL_THRESHOLD)
		return false;
	for (i = 1; i < HX_PIXELS; i++) {
		s32 local = ((first_q8[i] - second_q8[i]) >>
			HX_BASELINE_FRACTION_BITS) - common;

		if (abs(local) > threshold &&
		    ++divergent > HX_BASELINE_CLEAN_MAX_NODES)
			return false;
	}
	return true;
}

static void hx_safe_baseline_select_slot(struct hx_algo *algo, u8 slot)
{
	if (slot >= HX_SAFE_BASELINE_SLOTS ||
	    !algo->safe_baselines[slot].valid ||
	    algo->safe_baselines[slot].reset_pending)
		return;
	memcpy(algo->safe_baseline_q8,
	       algo->safe_baselines[slot].baseline_q8,
	       sizeof(algo->safe_baseline_q8));
	algo->safe_baseline_selected = slot;
	algo->safe_baseline_valid = true;
	algo->safe_baselines[slot].pushed = true;
	algo->safe_baselines[slot].use_count++;
}

void hx_safe_baseline_reset_selected(struct hx_algo *algo)
{
	struct hx_safe_baseline_entry *entry;

	if (algo->safe_baseline_selected >= HX_SAFE_BASELINE_SLOTS)
		return;
	entry = &algo->safe_baselines[algo->safe_baseline_selected];
	if (!entry->valid)
		return;
	entry->reset_pending = true;
	entry->state = HX_SAFE_SLOT_RESET;
	entry->pushed = false;
	hx_safe_baseline_queue_remove(algo, algo->safe_baseline_selected);
	algo->safe_baseline_valid = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_safe_reset_count++;
#endif
}

bool hx_safe_baseline_promote_latest_confirmed(struct hx_algo *algo)
{
	u8 best = HX_SAFE_BASELINE_SLOTS;
	u8 i;

	/* BaselineQueue_GetLatestBuf walks the queue in reverse chronological
	 * order.  Do not select by confidence/generation: those fields describe
	 * trust, not recency.
	 */
	for (i = 0; i < algo->safe_queue_count; i++) {
		u8 offset = algo->safe_queue_count - 1 - i;
		u8 index = (algo->safe_queue_tail + offset) %
			HX_SAFE_BASELINE_SLOTS;
		u8 slot = algo->safe_queue_order[index];
		struct hx_safe_baseline_entry *entry = &algo->safe_baselines[slot];

		if (!entry->valid || entry->reset_pending ||
		    entry->confidence < HX_SAFE_BASELINE_CONFIRMED)
			continue;
		best = slot;
		break;
	}
	if (best == HX_SAFE_BASELINE_SLOTS)
		return false;
	hx_safe_baseline_select_slot(algo, best);
	return true;
}

void hx_safe_baseline_bootstrap(struct hx_algo *algo, const s32 *baseline_q8)
{
	struct hx_safe_baseline_entry *entry;

	if (!algo->baseline_enabled)
		return;
	if (algo->safe_baseline_count) {
		hx_safe_baseline_select_slot(algo, algo->safe_baseline_selected);
		return;
	}
	entry = &algo->safe_baselines[0];
	memcpy(entry->baseline_q8, baseline_q8, sizeof(entry->baseline_q8));
	entry->valid = true;
	entry->confidence = HX_SAFE_BASELINE_BOOTSTRAP;
	entry->state = HX_SAFE_SLOT_PROVISIONAL;
	entry->stable_frames = 1;
	entry->reset_pending = false;
	entry->pushed = false;
	entry->generation = ++algo->safe_baseline_generation;
	entry->screen_epoch = algo->screen_epoch;
	entry->captured_frame = algo->frame_sequence;
	hx_safe_baseline_queue_record(algo, 0);
	algo->safe_baseline_pushes = min_t(u8,
		algo->safe_baseline_pushes + 1, U8_MAX);
	hx_safe_baseline_select_slot(algo, 0);
}

void hx_safe_baseline_commit(struct hx_algo *algo, const s32 *baseline_q8)
{
	struct hx_safe_baseline_entry *entry;
	u8 slot = HX_SAFE_BASELINE_SLOTS;
	u8 i;

	if (!algo->baseline_enabled)
		return;
	for (i = 0; i < HX_SAFE_BASELINE_SLOTS; i++) {
		if (!algo->safe_baselines[i].valid ||
		    algo->safe_baselines[i].reset_pending)
			continue;
		if (hx_safe_baselines_compatible(
			algo->safe_baselines[i].baseline_q8, baseline_q8)) {
			slot = i;
			break;
		}
	}
	if (slot < HX_SAFE_BASELINE_SLOTS) {
		entry = &algo->safe_baselines[slot];
		/* Re-observing a grid in the same electrical session is not an
		 * independent confirmation.  Only a later screen epoch may promote
		 * recovery authority.
		 */
		if (entry->screen_epoch != algo->screen_epoch) {
			if (entry->confidence < HX_SAFE_BASELINE_CONFIRMED)
				entry->confidence = HX_SAFE_BASELINE_CONFIRMED;
			else
				entry->confidence = HX_SAFE_BASELINE_CROSS_WAKE;
			entry->screen_epoch = algo->screen_epoch;
			entry->state = entry->confidence >= HX_SAFE_BASELINE_CROSS_WAKE ?
				HX_SAFE_SLOT_CROSS_WAKE : HX_SAFE_SLOT_CONFIRMED;
			entry->stable_frames = 0;
		}
		entry->stable_frames = min_t(u16, entry->stable_frames + 1,
						U16_MAX);
		entry->reset_pending = false;
		entry->generation = ++algo->safe_baseline_generation;
		entry->captured_frame = algo->frame_sequence;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->baseline_safe_dedup_count++;
#endif
		hx_safe_baseline_select_slot(algo, slot);
		return;
	}
	/* SafeBaseline_IsOKToPush limits replacements once the normal queue is
	 * full; this prevents a noisy screen-on epoch from cycling all history.
	 */
	if (algo->safe_queue_count == HX_SAFE_BASELINE_SLOTS &&
	    algo->safe_queue_full_pushes >= 3) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->baseline_safe_temporal_reject_count++;
#endif
		return;
	}

	if (algo->safe_baseline_count < HX_SAFE_BASELINE_SLOTS) {
		for (i = 0; i < HX_SAFE_BASELINE_SLOTS; i++)
			if (!algo->safe_baselines[i].valid) {
				slot = i;
				break;
			}
		algo->safe_baseline_count++;
	} else {
		/* BaselineQueue_Push is chronological: a full queue replaces the
		 * oldest unit, irrespective of confidence.
		 */
		slot = hx_safe_baseline_queue_oldest(algo);
		if (slot >= HX_SAFE_BASELINE_SLOTS)
			slot = algo->safe_baseline_selected;
		algo->safe_queue_full_pushes = min_t(u8,
			algo->safe_queue_full_pushes + 1, U8_MAX);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->baseline_safe_eviction_count++;
#endif
	}
	entry = &algo->safe_baselines[slot];
	memcpy(entry->baseline_q8, baseline_q8, sizeof(entry->baseline_q8));
	entry->valid = true;
	/* A spatially new grid is provisional until another screen epoch sees
	 * the same shape.  Selecting it immediately would turn one contaminated
	 * runtime window into the next wake's recovery authority.
	 */
	entry->confidence = HX_SAFE_BASELINE_BOOTSTRAP;
	entry->state = HX_SAFE_SLOT_PROVISIONAL;
	entry->stable_frames = 1;
	entry->reset_pending = false;
	entry->pushed = false;
	entry->generation = ++algo->safe_baseline_generation;
	entry->screen_epoch = algo->screen_epoch;
	entry->captured_frame = algo->frame_sequence;
	hx_safe_baseline_queue_record(algo, slot);
	algo->safe_baseline_pushes = min_t(u8,
		algo->safe_baseline_pushes + 1, U8_MAX);
	if (algo->safe_baseline_valid &&
	    algo->safe_baseline_selected == slot &&
	    algo->safe_baseline_count > 1) {
		u8 best = HX_SAFE_BASELINE_SLOTS;

		for (i = 0; i < HX_SAFE_BASELINE_SLOTS; i++) {
			if (i == slot || !algo->safe_baselines[i].valid)
				continue;
			if (best == HX_SAFE_BASELINE_SLOTS ||
			    algo->safe_baselines[i].confidence >
				algo->safe_baselines[best].confidence ||
			    (algo->safe_baselines[i].confidence ==
				algo->safe_baselines[best].confidence &&
			     algo->safe_baselines[i].generation >
				algo->safe_baselines[best].generation))
				best = i;
		}
		if (best < HX_SAFE_BASELINE_SLOTS)
			hx_safe_baseline_select_slot(algo, best);
	} else if (!algo->safe_baseline_valid ||
		   algo->safe_baselines[algo->safe_baseline_selected].confidence <=
			entry->confidence) {
		hx_safe_baseline_select_slot(algo, slot);
	}
}

void hx_algo_begin_wake(struct hx_algo *algo)
{
	hx_algo_clear_transient_state(algo);
	algo->screen_epoch++;
	algo->screen_on_frame_sequence = algo->frame_sequence;
	algo->safe_candidate_armed = true;
	algo->safe_queue_full_pushes = 0;
	algo->safe_prev_flags = algo->safe_flags;
	algo->safe_flags = 0;
	algo->wake_qualifying = true;
	algo->wake_candidate_valid = false;
	algo->wake_needs_double_confirm = false;
	algo->wake_candidate_frames = 0;
	algo->wake_finger_frames = 0;
	algo->wake_finger_reject_frames = 0;
	algo->wake_finger_common_sum = 0;
	algo->wake_finger_common_last = 0;
	algo->safe_no_finger_frames = 0;
	algo->safe_candidate_confirming = false;
	algo->safe_confirm_common_sum = 0;
	algo->safe_confirm_common_last = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->live_clear_count++;
#endif
}

static bool hx_wake_reference_common_shift(struct hx_algo *algo,
					   const u16 *raw,
					   const s32 *reference_q8,
					   s32 *common_out)
{
	s64 common_sum = 0;
	int common_bin;
	int common_count = 0;
	int cumulative = 0;
	int i;

	memset(algo->baseline_hist, 0, sizeof(algo->baseline_hist));
	for (i = 1; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 baseline = reference_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		int bin = clamp_t(int, (sample - baseline + 65536) >> 6,
				      0, HX_BASELINE_HIST_BINS - 1);

		algo->baseline_hist[bin]++;
	}
	for (common_bin = 0; common_bin < HX_BASELINE_HIST_BINS;
	     common_bin++) {
		cumulative += algo->baseline_hist[common_bin];
		if (cumulative >= (HX_PIXELS - 1) / 2)
			break;
	}
	common_bin = min(common_bin, HX_BASELINE_HIST_BINS - 1);
	for (i = 1; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 baseline = reference_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 delta = sample - baseline;
		int bin = clamp_t(int, (delta + 65536) >> 6, 0,
				      HX_BASELINE_HIST_BINS - 1);

		if (bin == common_bin) {
			common_sum += delta;
			common_count++;
		}
	}
	if (!common_count)
		return false;
	*common_out = (s32)(common_sum / common_count);
	return abs(*common_out) <= algo->cmf_max_correction;
}

static bool hx_wake_safe_common_shift(struct hx_algo *algo, const u16 *raw,
				      s32 *common_out)
{
	return hx_wake_reference_common_shift(algo, raw,
		algo->safe_baseline_q8, common_out);
}

void hx_safe_baseline_select_for_raw(struct hx_algo *algo, const u16 *raw)
{
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	u8 old_selected = algo->safe_baseline_selected;
#endif
	u32 best_score = ~0U;
	u8 best_confidence = HX_SAFE_BASELINE_EMPTY;
	u8 best = HX_SAFE_BASELINE_SLOTS;
	s32 threshold = max_t(s32, algo->wake_raw_jump_threshold * 2,
				  algo->baseline_peak_threshold);
	u8 slot;

	for (slot = 0; slot < HX_SAFE_BASELINE_SLOTS; slot++) {
		struct hx_safe_baseline_entry *entry =
			&algo->safe_baselines[slot];
		s32 common;
		u32 score = 0;
		int i;

		if (!entry->valid || entry->reset_pending)
			continue;
		if (!hx_wake_reference_common_shift(algo, raw,
			entry->baseline_q8, &common))
			continue;
		for (i = 1; i < HX_PIXELS; i++) {
			s32 sample = (s32)le16_to_cpup(raw + i);
			s32 baseline = entry->baseline_q8[i] >>
				HX_BASELINE_FRACTION_BITS;
			s32 local = sample - baseline - common;

			if (abs(local) > threshold)
				score++;
		}
		/* A candidate that needs a large spatial explanation is not a
		 * usable wake reference.  In particular, do not let a stale
		 * confirmed slot win merely because its confidence is higher than a
		 * newer slot that actually matches this raw frame.  The Windows
		 * SafeBaseline judge compares the current raw first; confidence is
		 * only a tie breaker after the spatial fit has passed.
		 */
		if (score > algo->wake_max_unstable_nodes)
			continue;
		if (score < best_score ||
		    (score == best_score &&
		     (entry->confidence > best_confidence ||
		      (entry->confidence == best_confidence &&
		       (best == HX_SAFE_BASELINE_SLOTS ||
			entry->generation > algo->safe_baselines[best].generation))))) {
			best_confidence = entry->confidence;
			best_score = score;
			best = slot;
		}
	}
	if (best < HX_SAFE_BASELINE_SLOTS) {
		hx_safe_baseline_select_slot(algo, best);
		algo->safe_baseline_selected_score = min_t(u32, best_score,
							  U16_MAX);
	}
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	else
		algo->baseline_safe_slot_reject_count++;
	if (best < HX_SAFE_BASELINE_SLOTS && old_selected != best)
		algo->baseline_safe_slot_switch_count++;
#endif
}

/*
 * Validate a last-safe baseline while one or more fingers are already on the
 * panel.  A compact positive residual is masked (including its low-amplitude
 * fringe), then only the remaining background participates in line-noise and
 * common-shift checks.  This is the minimum useful subset of the Windows
 * BLIIR/BLSM behaviour: touch cells never become baseline input, while a
 * panel-wide display/VCOM shift can still be applied to the working copy.
 */
static bool hx_wake_finger_background_quality(struct hx_algo *algo,
					      const u16 *raw,
					      s32 *common_out)
{
	u8 row_unstable[HX_ROWS] = { 0 };
	u8 col_unstable[HX_COLS] = { 0 };
	s32 common;
	s32 background_threshold;
	u16 touch_cells = 0;
	u16 unstable = 0;
	u8 components = 0;
	int r, c, i;

	memset(algo->zone_map, 0, sizeof(algo->zone_map));
	if (!hx_wake_safe_common_shift(algo, raw, &common))
		return false;

	/* First mark strong touch cells, then dilate without recursively growing
	 * the mask.  Value 1 is a seed and value 2 is its protected fringe.
	 */
	for (i = 1; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 baseline = algo->safe_baseline_q8[i] >>
				HX_BASELINE_FRACTION_BITS;

		if (sample - baseline - common >=
		    algo->baseline_peak_threshold) {
			algo->zone_map[i] = 1;
			row_unstable[i / HX_COLS]++;
			col_unstable[i % HX_COLS]++;
			touch_cells++;
		}
	}
	if (!touch_cells || touch_cells > HIMAX_WAKE_MAX_TOUCH_CELLS)
		return false;
	for (r = 0; r < HX_ROWS; r++)
		if (row_unstable[r] >= algo->wake_max_unstable_line_nodes)
			return false;
	for (c = 0; c < HX_COLS; c++)
		if (col_unstable[c] >= algo->wake_max_unstable_line_nodes)
			return false;
	/* The firmware finger bit is useful evidence, but it must not turn
	 * scattered or diagonal display noise into an unlimited exclusion mask.
	 * Require at most one compact connected seed region per reportable slot.
	 */
	for (i = 1; i < HX_PIXELS; i++) {
		u16 head = 0;
		u16 tail = 0;
		int min_r, max_r, min_c, max_c;

		if (algo->zone_map[i] != 1)
			continue;
		if (++components > HIMAX_MAX_TOUCH)
			return false;
		min_r = max_r = i / HX_COLS;
		min_c = max_c = i % HX_COLS;
		algo->zone_map[i] = 3;
		algo->bfs_queue[tail++] = i;
		while (head < tail) {
			int idx = algo->bfs_queue[head++];
			int cr = idx / HX_COLS;
			int cc = idx % HX_COLS;

			min_r = min(min_r, cr);
			max_r = max(max_r, cr);
			min_c = min(min_c, cc);
			max_c = max(max_c, cc);
			for (int dr = -1; dr <= 1; dr++) {
				int nr = cr + dr;

				if (nr < 0 || nr >= HX_ROWS)
					continue;
				for (int dc = -1; dc <= 1; dc++) {
					int nc = cc + dc;
					int next;

					if ((!dr && !dc) || nc < 0 ||
					    nc >= HX_COLS)
						continue;
					next = nr * HX_COLS + nc;
					if (algo->zone_map[next] != 1)
						continue;
					algo->zone_map[next] = 3;
					algo->bfs_queue[tail++] = next;
				}
			}
		}
		if (max_r - min_r + 1 > algo->wake_max_unstable_line_nodes ||
		    max_c - min_c + 1 > algo->wake_max_unstable_line_nodes)
			return false;
	}
	memset(row_unstable, 0, sizeof(row_unstable));
	memset(col_unstable, 0, sizeof(col_unstable));
	for (i = 1; i < HX_PIXELS; i++) {
		if (algo->zone_map[i] != 3)
			continue;
		r = i / HX_COLS;
		c = i % HX_COLS;
		for (int dr = -HIMAX_WAKE_TOUCH_MASK_RADIUS;
		     dr <= HIMAX_WAKE_TOUCH_MASK_RADIUS; dr++) {
			int nr = r + dr;

			if (nr < 0 || nr >= HX_ROWS)
				continue;
			for (int dc = -HIMAX_WAKE_TOUCH_MASK_RADIUS;
			     dc <= HIMAX_WAKE_TOUCH_MASK_RADIUS; dc++) {
				int nc = c + dc;
				int idx;

				if (nc < 0 || nc >= HX_COLS)
					continue;
				idx = nr * HX_COLS + nc;
				if (!algo->zone_map[idx])
					algo->zone_map[idx] = 2;
			}
		}
	}

	background_threshold = max_t(s32, algo->wake_raw_jump_threshold * 2,
					 algo->baseline_peak_threshold);
	for (i = 1; i < HX_PIXELS; i++) {
		s32 sample;
		s32 baseline;
		s32 local;

		if (algo->zone_map[i])
			continue;
		sample = (s32)le16_to_cpup(raw + i);
		baseline = algo->safe_baseline_q8[i] >>
			   HX_BASELINE_FRACTION_BITS;
		local = sample - baseline - common;
		if (abs(local) <= background_threshold)
			continue;
		row_unstable[i / HX_COLS]++;
		col_unstable[i % HX_COLS]++;
		unstable++;
	}
	if (unstable > algo->wake_max_unstable_nodes)
		return false;
	for (r = 0; r < HX_ROWS; r++)
		if (row_unstable[r] >= algo->wake_max_unstable_line_nodes)
			return false;
	for (c = 0; c < HX_COLS; c++)
		if (col_unstable[c] >= algo->wake_max_unstable_line_nodes)
			return false;

	*common_out = common;
	return true;
}

enum hx_wake_safe_observation {
	HX_WAKE_SAFE_CLEAN,
	HX_WAKE_SAFE_FINGER,
	HX_WAKE_SAFE_AMBIGUOUS,
};

/* Windows does not decide held-in-hand recovery from one firmware flag.  It
 * compares raw, working BL and the last independently safe BL, protects
 * touch-shaped residuals and rejects an untrustworthy replacement.  Keep the
 * same ordering here: first recognize a compact touch, then accept a clean
 * common-shift-only frame; everything else is ambiguous and may not become a
 * wake baseline.
 */
static enum hx_wake_safe_observation
hx_wake_observe_against_safe(struct hx_algo *algo, const u16 *raw,
			     s32 *common_out, bool *common_valid)
{
	u8 row_bad[HX_ROWS] = { 0 };
	u8 col_bad[HX_COLS] = { 0 };
	s32 common;
	s32 threshold = algo->baseline_peak_threshold;
	u16 bad = 0;
	int r, c, i;

	*common_valid = false;
	if (hx_wake_finger_background_quality(algo, raw, &common)) {
		*common_out = common;
		*common_valid = true;
		return HX_WAKE_SAFE_FINGER;
	}
	if (!hx_wake_safe_common_shift(algo, raw, &common))
		return HX_WAKE_SAFE_AMBIGUOUS;
	*common_out = common;
	*common_valid = true;
	for (i = 1; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 baseline = algo->safe_baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 local = sample - baseline - common;

		if (abs(local) <= threshold)
			continue;
		r = i / HX_COLS;
		c = i % HX_COLS;
		row_bad[r]++;
		col_bad[c]++;
		bad++;
	}
	for (r = 0; r < HX_ROWS; r++)
		if (row_bad[r] >= algo->wake_max_unstable_line_nodes)
			return HX_WAKE_SAFE_AMBIGUOUS;
	for (c = 0; c < HX_COLS; c++)
		if (col_bad[c] >= algo->wake_max_unstable_line_nodes)
			return HX_WAKE_SAFE_AMBIGUOUS;
	/* Official SafeBaseline checks both abnormal maxima and minima before
	 * accepting a screen-on baseline.  A large thumb can depress a compact
	 * group of cells instead of producing the usual positive peak; allowing a
	 * small number of such cells as "clean" learns the held contact into the
	 * working baseline.  Temporal confirmation already filters one-frame
	 * noise, so any persistent spatial residual is protected here.
	 */
	if (bad)
		return HX_WAKE_SAFE_AMBIGUOUS;
	*common_out = common;
	return HX_WAKE_SAFE_CLEAN;
}

int hx_algo_qualify_wake_frame(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state)
{
	enum hx_wake_safe_observation safe_observation = HX_WAKE_SAFE_CLEAN;
	u8 row_unstable[HX_ROWS] = { 0 };
	u8 col_unstable[HX_COLS] = { 0 };
	s32 observed_common = 0;
	bool observed_common_valid = false;
	u16 out_of_range = 0;
	u16 unstable = 0;
	u8 required_frames;
	bool had_safe_baseline = algo->safe_baseline_valid;
	int i;

	if (!algo->wake_qualifying)
		return HX_WAKE_QUALITY_READY;
	for (i = 1; i < HX_PIXELS; i++) {
		u16 sample = le16_to_cpup(raw + i);

		if (sample < 0x1000 || sample > 0xf000)
			out_of_range++;
	}
	if (out_of_range > algo->wake_max_unstable_nodes) {
		algo->wake_candidate_valid = false;
		algo->wake_candidate_frames = 0;
		algo->wake_needs_double_confirm = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->wake_candidate_reject_count++;
#endif
		return HX_WAKE_QUALITY_REJECTED;
	}
	if (algo->safe_baseline_count)
		hx_safe_baseline_select_for_raw(algo, raw);
	if (algo->safe_baseline_valid)
		safe_observation = hx_wake_observe_against_safe(algo, raw,
							       &observed_common,
							       &observed_common_valid);

	/* A finger already present at screen-on must never be learned into a new
	 * baseline.  Firmware may miss a stationary finger during reset/reload,
	 * so a compact raw residual against last-safe is equally authoritative.
	 * Validate the background outside a dilated touch mask and apply only its
	 * stable common shift to the working last-safe snapshot.
	 */
	if (finger_state == HX_FINGER_PRESENT ||
	    safe_observation == HX_WAKE_SAFE_FINGER) {
		s32 common = 0;
		bool inferred = finger_state != HX_FINGER_PRESENT;

		algo->wake_candidate_valid = false;
		algo->wake_candidate_frames = 0;
		algo->wake_needs_double_confirm = false;
		if (algo->safe_baseline_valid) {
			s32 average;

			common = observed_common;
			if (safe_observation != HX_WAKE_SAFE_FINGER ||
			    (algo->wake_finger_frames &&
			     abs(common - algo->wake_finger_common_last) >
				algo->wake_raw_jump_threshold)) {
				algo->wake_finger_frames = 0;
				algo->wake_finger_common_sum = 0;
				algo->wake_finger_common_last = 0;
				if (algo->wake_finger_reject_frames < U8_MAX)
					algo->wake_finger_reject_frames++;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
				algo->wake_finger_mask_reject_count++;
#endif
				/*
				 * The strict mask decides whether a common-mode
				 * correction is trustworthy; it must not decide whether
				 * touch IRQs are enabled.  Real fingers, palms and edge
				 * contacts need not match the compact synthetic shape.
				 * After several electrically valid finger frames, restore
				 * the independently trusted safe grid without learning the
				 * contact or applying an untrusted correction.
				 */
				if (algo->wake_finger_reject_frames <
				    algo->wake_finger_safe_frames)
					return HX_WAKE_QUALITY_PENDING;
				hx_restore_working_from_safe(algo, 0, true, true);
				algo->wake_qualifying = false;
				algo->wake_raw_finger_override = inferred;
				algo->wake_raw_finger_release_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
				algo->wake_safe_fallback_count++;
				algo->wake_finger_degraded_fallback_count++;
				algo->wake_qualification_count++;
#endif
				return HX_WAKE_QUALITY_USING_SAFE;
			}
			algo->wake_finger_reject_frames = 0;
			algo->wake_finger_common_sum += common;
			algo->wake_finger_common_last = common;
			if (algo->wake_finger_frames < U8_MAX)
				algo->wake_finger_frames++;
			if (algo->wake_finger_frames <
			    algo->wake_finger_safe_frames)
				return HX_WAKE_QUALITY_PENDING;

			average = algo->wake_finger_common_sum /
				  algo->wake_finger_frames;
		/* The vendor held-in-hand path reconstructs BL from the latest
		 * safe snapshot and BL2BL difference buffers.  On first boot there
		 * is no working BL history yet, so use the safe grid plus CM shift.
		 */
		if (algo->baseline_initialized) {
			hx_safe_baseline_buffer_comparison(algo, raw);
			if (hx_safe_baseline_should_replace_wake(algo))
				hx_safe_baseline_replace_working_from_history(algo, average);
			else
				hx_restore_working_from_safe(algo, average, true, true);
		} else {
			hx_restore_working_from_safe(algo, average, true, true);
		}
			algo->baseline_touch_hold = true;
			algo->baseline_touch_seen = true;
			algo->baseline_held_in_hand = true;
			algo->baseline_guard_state = HX_BASELINE_GUARD_PROTECTED;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->wake_finger_mask_accept_count++;
#endif
		} else {
			algo->wake_finger_reject_frames = 0;
			if (algo->wake_finger_frames < U8_MAX)
				algo->wake_finger_frames++;
			if (algo->wake_finger_frames <
			    algo->wake_finger_safe_frames)
				return HX_WAKE_QUALITY_PENDING;
			if (!algo->baseline_initialized) {
				for (i = 0; i < HX_PIXELS; i++)
					algo->baseline_q8[i] =
						(s32)algo->baseline_initial <<
						HX_BASELINE_FRACTION_BITS;
			}
			algo->baseline_touch_hold = true;
			algo->baseline_touch_seen = true;
			algo->baseline_touch_release_frames = 0;
			algo->baseline_guard_state = HX_BASELINE_GUARD_PROTECTED;
			algo->baseline_guard_clean_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->baseline_touch_hold_count++;
#endif
		}
		algo->baseline_initialized = true;
		algo->wake_qualifying = false;
		algo->wake_raw_finger_override = inferred;
		algo->wake_raw_finger_release_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->wake_safe_fallback_count++;
		algo->wake_qualification_count++;
		if (inferred)
			algo->wake_raw_finger_inferred_count++;
#endif
		return algo->safe_baseline_valid ?
			HX_WAKE_QUALITY_USING_SAFE :
			HX_WAKE_QUALITY_PROTECTED;
	}
	/* A stable but non-clean shape against last-safe is neither permission to
	 * overwrite safe BL nor a reason to power the input device down.  Match
	 * the official protection bias: rebuild working from trusted history,
	 * treat the raw state as held-in-hand until a clean release is confirmed,
	 * and let runtime diagnostics decide whether BLReset would help.
	 */
	if (algo->safe_baseline_valid &&
	    safe_observation == HX_WAKE_SAFE_AMBIGUOUS) {
		algo->wake_candidate_valid = false;
		algo->wake_candidate_frames = 0;
		algo->wake_needs_double_confirm = false;
		algo->wake_finger_frames = 0;
		algo->wake_finger_common_sum = 0;
		algo->wake_finger_common_last = 0;
		if (algo->wake_finger_reject_frames < U8_MAX)
			algo->wake_finger_reject_frames++;
		if (algo->wake_finger_reject_frames <
		    algo->wake_finger_safe_frames)
			return HX_WAKE_QUALITY_PENDING;
		hx_restore_working_from_safe(algo,
			observed_common_valid ? observed_common : 0, true, true);
		algo->wake_qualifying = false;
		algo->wake_raw_finger_override = true;
		algo->wake_raw_finger_release_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->wake_safe_fallback_count++;
		algo->wake_ambiguous_safe_fallback_count++;
		algo->wake_qualification_count++;
#endif
		return HX_WAKE_QUALITY_USING_SAFE;
	}
	algo->wake_finger_frames = 0;
	algo->wake_finger_reject_frames = 0;
	algo->wake_finger_common_sum = 0;
	algo->wake_finger_common_last = 0;

	if (!algo->wake_candidate_valid) {
		hx_copy_raw_to_baseline(algo->wake_candidate_q8, raw);
		algo->wake_candidate_valid = true;
		algo->wake_candidate_frames = 1;
		return HX_WAKE_QUALITY_PENDING;
	}

	/* A candidate is only committed after several complete raw grids agree.
	 * Count spatially-local changes; a panel-wide DC shift is naturally
	 * represented by the first candidate and is not confused with activity.
	 */
	for (i = 1; i < HX_PIXELS; i++) {
		s32 sample_q8 = (s32)le16_to_cpup(raw + i) <<
				HX_BASELINE_FRACTION_BITS;
		s32 delta = (sample_q8 - algo->wake_candidate_q8[i]) >>
			    HX_BASELINE_FRACTION_BITS;

		if (abs(delta) > algo->wake_raw_jump_threshold) {
			row_unstable[i / HX_COLS]++;
			col_unstable[i % HX_COLS]++;
			unstable++;
		}
	}
	for (i = 0; i < HX_ROWS; i++)
		if (row_unstable[i] >= algo->wake_max_unstable_line_nodes)
			unstable = algo->wake_max_unstable_nodes + 1;
	for (i = 0; i < HX_COLS; i++)
		if (col_unstable[i] >= algo->wake_max_unstable_line_nodes)
			unstable = algo->wake_max_unstable_nodes + 1;
	if (unstable > algo->wake_max_unstable_nodes) {
		/* Start the next qualification window from the newest complete frame.
		 * Do not poison either the working or last-safe baseline.
		 */
		hx_copy_raw_to_baseline(algo->wake_candidate_q8, raw);
		algo->wake_candidate_frames = 1;
		algo->wake_needs_double_confirm = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->wake_candidate_reject_count++;
#endif
		return HX_WAKE_QUALITY_REJECTED;
	}

	if (algo->wake_candidate_frames < U8_MAX)
		algo->wake_candidate_frames++;
	if (algo->wake_candidate_frames < algo->wake_stable_frames)
		return HX_WAKE_QUALITY_PENDING;

	/* A candidate that differs spatially from last-safe is not rejected just
	 * because the display environment genuinely changed.  It must, however,
	 * survive a second complete stability window before becoming the working
	 * baseline.  Existing safe data remains a separate recovery authority.
	 */
	if (algo->safe_baseline_valid && !algo->wake_needs_double_confirm) {
		s64 sum = 0;
		s32 common;
		u16 divergent = 0;

		for (i = 1; i < HX_PIXELS; i++)
			sum += (algo->wake_candidate_q8[i] -
				algo->safe_baseline_q8[i]) >>
				HX_BASELINE_FRACTION_BITS;
		common = (s32)(sum / (HX_PIXELS - 1));
		for (i = 1; i < HX_PIXELS; i++) {
			s32 delta = ((algo->wake_candidate_q8[i] -
				algo->safe_baseline_q8[i]) >>
				HX_BASELINE_FRACTION_BITS) - common;

			if (abs(delta) > algo->wake_raw_jump_threshold * 2)
				divergent++;
		}
		if (divergent > algo->wake_max_unstable_nodes) {
			algo->wake_needs_double_confirm = true;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->wake_safe_divergence_count++;
#endif
		}
	}
	required_frames = algo->wake_stable_frames;
	if (algo->wake_needs_double_confirm)
		required_frames = min_t(u8, algo->wake_stable_frames * 2,
					 U8_MAX);
	if (algo->wake_candidate_frames < required_frames)
		return HX_WAKE_QUALITY_PENDING;

	/* A confirmed history has already survived an independent no-touch
	 * window.  A wake frame only proves that scanning resumed; it must not
	 * replace the spatial grid with display-startup residue.  Apply the
	 * verified panel-wide shift now and leave promotion of the new raw grid to
	 * the normal post-wake double-window SafeBaseline collector.
	 */
	/* BOOTSTRAP is not trusted for an autonomous runtime BLReset, but it is
	 * still strictly safer than copying an unclassified screen-on raw grid.
	 * Wake protection keeps it read-only, applies only a robust common shift,
	 * and requires the post-wake guard before spatial learning resumes.
	 */
	if (had_safe_baseline) {
		hx_restore_working_from_safe(algo,
			observed_common_valid ? observed_common : 0, true, false);
		algo->wake_qualifying = false;
		algo->wake_candidate_valid = false;
		algo->wake_needs_double_confirm = false;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->baseline_generation++;
		algo->wake_qualification_count++;
		algo->wake_safe_preserved_count++;
		algo->wake_clean_safe_restore_count++;
#endif
		return HX_WAKE_QUALITY_USING_SAFE;
	}

	if (algo->baseline_enabled)
		memcpy(algo->baseline_q8, algo->wake_candidate_q8,
		       sizeof(algo->baseline_q8));
	if (!had_safe_baseline)
		hx_safe_baseline_bootstrap(algo, algo->wake_candidate_q8);
	algo->baseline_initialized = true;
	algo->wake_qualifying = false;
	algo->wake_candidate_valid = false;
	algo->wake_needs_double_confirm = false;
	algo->baseline_prev_had_signal = false;
	algo->baseline_had_freeze = false;
	algo->baseline_recovery_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_generation++;
	algo->wake_baseline_commit_count++;
	algo->wake_qualification_count++;
	if (had_safe_baseline)
		algo->wake_safe_preserved_count++;
#endif
	return HX_WAKE_QUALITY_READY;
}

enum hx_finger_state
hx_algo_resolve_finger_state(struct hx_algo *algo, const u16 *raw,
			     enum hx_finger_state firmware_state)
{
	enum hx_wake_safe_observation observation;
	s32 common;
	bool common_valid;

	if (firmware_state == HX_FINGER_PRESENT) {
		algo->wake_raw_finger_release_frames = 0;
		return HX_FINGER_PRESENT;
	}
	if (!algo->wake_raw_finger_override)
		return firmware_state;
	if (!algo->safe_baseline_valid) {
		algo->wake_raw_finger_override = false;
		algo->wake_raw_finger_release_frames = 0;
		return firmware_state;
	}

	observation = hx_wake_observe_against_safe(algo, raw, &common,
						     &common_valid);
	if (observation != HX_WAKE_SAFE_CLEAN) {
		algo->wake_raw_finger_release_frames = 0;
		return HX_FINGER_PRESENT;
	}
	if (algo->wake_raw_finger_release_frames < U8_MAX)
		algo->wake_raw_finger_release_frames++;
	if (algo->wake_raw_finger_release_frames <
	    algo->wake_finger_safe_frames)
		return HX_FINGER_PRESENT;

	algo->wake_raw_finger_override = false;
	algo->wake_raw_finger_release_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->wake_raw_finger_release_count++;
#endif
	return HX_FINGER_ABSENT;
}

bool hx_algo_is_exception_frame(struct hx_algo *algo, const u16 *raw)
{
	u8 col_bad[HX_COLS] = { 0 };
	s64 sum = 0;
	s32 common;
	u16 total = 0;
	int r, c;

	if (!algo->baseline_initialized || algo->wake_qualifying)
		return false;
	for (r = 1; r < HX_PIXELS; r++)
		sum += (s32)le16_to_cpup(raw + r) -
		       (algo->baseline_q8[r] >> HX_BASELINE_FRACTION_BITS);
	common = (s32)(sum / (HX_PIXELS - 1));

	for (r = 0; r < HX_ROWS; r++) {
		u8 row_bad = 0;

		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 local;

			if (!idx)
				continue;
			local = (s32)le16_to_cpup(raw + idx) -
				(algo->baseline_q8[idx] >>
				 HX_BASELINE_FRACTION_BITS) - common;
			if (abs(local) >= algo->runtime_noise_threshold) {
				row_bad++;
				col_bad[c]++;
				total++;
			}
		}
		if (row_bad >= algo->runtime_noise_line_nodes)
			goto exception;
	}
	for (c = 0; c < HX_COLS; c++)
		if (col_bad[c] >= algo->runtime_noise_line_nodes)
			goto exception;
	if (total < algo->runtime_noise_total_nodes)
		return false;

exception:
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->noise_frame_hold_count++;
#endif
	return true;
}

#ifdef HX_ALGO_HOST_TEST
void hx_algo_reset_runtime(struct hx_algo *algo)
{
	hx_algo_clear_live_state(algo);
}
#endif

/* ======================================================================== */
/* Phase 1A — baseline subtraction                                          */
/* ======================================================================== */
