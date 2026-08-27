// SPDX-License-Identifier: GPL-2.0
/* Baseline state-machine transitions (BLSM/AFT release protection). */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"


enum hx_baseline_stage hx_baseline_stage_update(struct hx_algo *algo,
						u16 property, s16 max_signal,
						s16 min_signal, bool single_touch)
{
	enum hx_baseline_stage old_stage = algo->baseline_stage;
	enum hx_baseline_stage next = old_stage;
	u32 interval = max_t(u16, algo->frame_interval_ms, 1);
	u32 transition_ms = 408; /* TSADynPrmt_UpdateBfrTch: dynamic + 0x54 */

	algo->baseline_prev_stage = old_stage;
	/* BLSM_UpdateStage applies these flags before consulting the previous
	 * stage jump table.  This priority order is taken directly from the
	 * Gaokun TSACore binary. */
	if (property & HX_BLSM_PRPT_RESET)
		next = HX_BLSM_RESET;
	else if (property & HX_BLSM_PRPT_NOISY)
		next = HX_BLSM_NOISY_RESET;
	else if (property & HX_BLSM_PRPT_RAW_UNSTABLE)
		next = HX_BLSM_FORCED;
	else if (property & HX_BLSM_PRPT_DEBOUNCE)
		next = HX_BLSM_DEBOUNCE;
	else if (property & HX_BLSM_PRPT_FORCED)
		next = HX_BLSM_FORCED;
	else {
		algo->baseline_stage_elapsed_ms = min_t(u32,
			algo->baseline_stage_elapsed_ms + interval, UINT_MAX);
		switch (old_stage) {
		case HX_BLSM_NO_TOUCH_STABLE:
			if (property & HX_BLSM_PRPT_TOUCH)
				next = HX_BLSM_TOUCH;
			else if (property & HX_BLSM_PRPT_WEAK_SIGNAL)
				next = HX_BLSM_PRE_TOUCH;
			break;
		case HX_BLSM_RESET:
		case HX_BLSM_NOISY_RESET:
		case HX_BLSM_FORCED:
			/* These one-shot stages return to zero on the next frame when
			 * no higher-priority property remains asserted. */
			next = HX_BLSM_NO_TOUCH_STABLE;
			break;
		case HX_BLSM_PRE_TOUCH:
		case HX_BLSM_POST_TOUCH_ALT:
			if (property & HX_BLSM_PRPT_TOUCH)
				next = HX_BLSM_TOUCH;
			else if (algo->baseline_stage_elapsed_ms >= transition_ms)
				next = HX_BLSM_NO_TOUCH_STABLE;
			break;
		case HX_BLSM_TOUCH:
			if (!(property & HX_BLSM_PRPT_TOUCH))
				next = algo->baseline_touch_latched ?
					HX_BLSM_PRE_TOUCH : HX_BLSM_TOUCH_RELEASE;
			/* TSACore updates the latch after deciding this transition. */
			algo->baseline_touch_latched = single_touch;
			break;
		case HX_BLSM_TOUCH_RELEASE:
			if (property & HX_BLSM_PRPT_TOUCH)
				next = HX_BLSM_TOUCH;
			else if (algo->baseline_stage_elapsed_ms >= transition_ms)
				next = HX_BLSM_POST_TOUCH;
			break;
		case HX_BLSM_POST_TOUCH:
			if (property & HX_BLSM_PRPT_TOUCH)
				next = HX_BLSM_TOUCH;
			else if (algo->baseline_stage_elapsed_ms >= transition_ms ||
				 (max_signal < algo->baseline_peak_threshold &&
				  min_signal > -algo->baseline_peak_threshold))
				next = HX_BLSM_NO_TOUCH_STABLE;
			break;
		case HX_BLSM_DEBOUNCE:
			if (property & HX_BLSM_PRPT_TOUCH)
				next = HX_BLSM_TOUCH;
			else if (algo->baseline_stage_elapsed_ms >= transition_ms)
				next = HX_BLSM_NO_TOUCH_STABLE;
			break;
		default:
			next = HX_BLSM_NO_TOUCH_STABLE;
			break;
		}
	}

	if (next != old_stage) {
		algo->baseline_stage_frames = 1;
		algo->baseline_stage_elapsed_ms = interval;
	} else if (algo->baseline_stage_frames < U8_MAX) {
		algo->baseline_stage_frames++;
	}
	algo->baseline_stage = next;
	return next;
}

bool hx_safe_baseline_check_ghost(const struct hx_algo *algo,
					 const u16 *raw, const s32 *reference_q8,
					 s32 common, u16 *ghost_nodes)
{
	u16 nodes = 0;
	int i;

	/* SafeBaseline_CheckGhostMax: a compact positive island is treated as a
	 * latent contact even when firmware reports no finger.  It is intentionally
	 * conservative: this helper only rejects learning; it never creates a
	 * touch report. */
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		s32 local = sample - reference - common;

		if (local < algo->baseline_peak_threshold)
			continue;
		if (++nodes > HX_BASELINE_CLEAN_MAX_NODES)
			break;
	}
	if (ghost_nodes)
		*ghost_nodes = nodes;
	return nodes > 0 && nodes <= HX_BASELINE_CLEAN_MAX_NODES;
}

bool hx_safe_baseline_check_side_touch(const struct hx_algo *algo,
					       const u16 *raw,
					       const s32 *reference_q8,
					       s32 common)
{
	u16 edge_nodes = 0;
	int i;

	/* SafeBaseline_CheckSideTouch: a broad contact entering from an edge is
	 * not a valid background sample even when it is too diffuse for the
	 * compact ghost test. */
	for (i = 0; i < HX_PIXELS; i++) {
		int row = i / HX_COLS;
		int col = i % HX_COLS;
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;

		if (row > 1 && row < HX_ROWS - 2 &&
		    col > 1 && col < HX_COLS - 2)
			continue;
		if (sample - reference - common >= algo->baseline_peak_threshold)
			edge_nodes++;
	}
	return edge_nodes >= max_t(u16, 4, HX_COLS / 8);
}

bool hx_safe_baseline_check_side_very_negative(const struct hx_algo *algo,
						       const u16 *raw,
						       const s32 *reference_q8,
						       s32 common)
{
	s32 side_max = SHRT_MIN;
	s32 side_min = SHRT_MAX;
	int i;

	/* SideBaseline_IsVeryNegative compares the side baseline extrema as
	 * 4 * max < -2 * min and then requires the negative excursion to exceed
	 * the dynamic signal threshold.  Use only the observable mutual-cap edge
	 * ring; the unavailable private side buffer must not be fabricated. */
	for (i = 0; i < HX_PIXELS; i++) {
		int row = i / HX_COLS;
		int col = i % HX_COLS;
		s32 sample;
		s32 reference;
		s32 local;

		if (row > 1 && row < HX_ROWS - 2 &&
		    col > 1 && col < HX_COLS - 2)
			continue;
		sample = (s32)le16_to_cpup(raw + i);
		reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		local = sample - reference - common;
		side_max = max(side_max, local);
		side_min = min(side_min, local);
	}
	if (side_min >= 0 || side_max == SHRT_MIN)
		return false;
	return side_max * 4 < side_min * -2 &&
		algo->baseline_peak_threshold < -side_min;
}

bool hx_baseline_guard_process(struct hx_algo *algo, bool frame_clean,
				       bool touch_evidence)
{
	u8 required = max_t(u8, algo->safe_commit_no_finger_frames, 1);

	if (algo->baseline_reacquire_pending)
		required = max_t(u8, required, 90);
	if (algo->baseline_held_in_hand)
		required = max_t(u8, required, 120);
	if (!algo->baseline_touch_hold) {
		algo->baseline_guard_state = HX_BASELINE_GUARD_NORMAL;
		algo->baseline_guard_clean_frames = 0;
		return false;
	}
	if (touch_evidence || !frame_clean) {
		if (algo->baseline_guard_state != HX_BASELINE_GUARD_PROTECTED) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			algo->baseline_guard_restart_count++;
#endif
		}
		algo->baseline_touch_seen = true;
		algo->baseline_guard_state = HX_BASELINE_GUARD_PROTECTED;
		algo->baseline_guard_clean_frames = 0;
		algo->baseline_touch_release_frames = 0;
		return false;
	}
	if (!algo->baseline_touch_seen)
		return false;
	if (algo->baseline_guard_state == HX_BASELINE_GUARD_PROTECTED) {
		algo->baseline_guard_state = HX_BASELINE_GUARD_RELEASE_SETTLE;
		algo->baseline_guard_clean_frames = 1;
	} else if (algo->baseline_guard_clean_frames < U8_MAX) {
		algo->baseline_guard_clean_frames++;
	}
	algo->baseline_touch_release_frames = algo->baseline_guard_clean_frames;
	if (algo->baseline_guard_state == HX_BASELINE_GUARD_RELEASE_SETTLE &&
	    algo->baseline_guard_clean_frames >= required) {
		algo->baseline_guard_state = HX_BASELINE_GUARD_CLEAN_QUALIFY;
		algo->baseline_guard_clean_frames = 0;
		algo->baseline_touch_release_frames = 0;
		return false;
	}
	if (algo->baseline_guard_state != HX_BASELINE_GUARD_CLEAN_QUALIFY ||
	    algo->baseline_guard_clean_frames < required)
		return false;
	algo->baseline_touch_hold = false;
	algo->baseline_touch_seen = false;
	algo->baseline_held_in_hand = false;
	algo->baseline_touch_release_frames = 0;
	algo->baseline_guard_state = HX_BASELINE_GUARD_NORMAL;
	algo->baseline_guard_clean_frames = 0;
	algo->baseline_screen_on_hand_state = HX_HAND_RELEASE_PENDING;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->baseline_touch_release_count++;
#endif
	return true;
}
