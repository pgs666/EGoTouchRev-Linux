// SPDX-License-Identifier: GPL-2.0
/* BLReset predicates.  This module deliberately contains classification only;
 * the caller owns debounce, queue selection, and reset side effects. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

static bool hx_blreset_check_abnormal(s32 signal, s32 threshold)
{
	/* BLReset_CheckAbnormal compares distance from the positive/negative
	 * limits.  The symmetric form is equivalent for the mutual-cap panel. */
	return signal > threshold || signal < -threshold;
}

static bool hx_blreset_check_sensor(const struct hx_algo *algo,
				    const u16 *raw, s32 common, u16 *bad,
				    u16 *negative)
{
	u16 bad_count = 0, negative_count = 0;
	int i;

	for (i = 0; i < HX_PIXELS; i++) {
		s32 value = (s32)le16_to_cpup(raw + i);
		s32 baseline = algo->baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 signal = value - baseline - common;

		if (hx_blreset_check_abnormal(signal,
					      algo->baseline_peak_threshold)) {
			bad_count++;
			if (signal < 0)
				negative_count++;
		}
	}
	if (bad)
		*bad = bad_count;
	if (negative)
		*negative = negative_count;
	return bad_count != 0;
}

bool hx_blreset_check_clean_baseline(const struct hx_algo *algo,
				     const u16 *raw)
{
	u16 divergent = 0;
	int i;

	/* BLReset_CheckCleanBaseline compares pre-CMF raw with the clean
	 * baseline and accepts it only when fewer than four cells differ by more
	 * than 100 counts.  The normal-baseline snapshot is the Linux equivalent
	 * of TSAStatic's clean-baseline buffer. */
	if (!algo->normal_baseline_valid ||
	    !algo->blreset_clean_baseline_captured)
		return false;
	for (i = 0; i < HX_PIXELS; i++) {
		s32 baseline = algo->normal_baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 delta = (s32)le16_to_cpup(raw + i) - baseline;

		if (abs(delta) > 100 && ++divergent >= 4)
			return false;
	}
	return true;
}

u16 hx_blreset_classify_frame(struct hx_algo *algo, const u16 *raw,
			      s32 common_diff, s16 max_signal, s16 min_signal,
			      bool has_signal, bool line_noise, bool out_of_range)
{
	u16 bad = 0, negative = 0, flags = 0;
	u16 jump_nodes = 0;
	bool abnormal;
	int i;

	abnormal = hx_blreset_check_sensor(algo, raw, common_diff,
					   &bad, &negative);
	/* A large positive maximum is normally a real finger; BLReset's peak
	 * predicates inspect its geometry separately, so it is not by itself a
	 * sensor failure. */
	if (line_noise || out_of_range ||
		bad > algo->runtime_noise_total_nodes)
		algo->blreset_over_noise_frames = min_t(u16,
			algo->blreset_over_noise_frames + 1, U16_MAX);
	else
		algo->blreset_over_noise_frames = 0;
	/* CheckNumOverNoise requires a short consecutive vote before declaring
	 * the panel continuously noisy.  Five frames matches the default
	 * SafeBaseline reset window used by Gaokun. */
	if (algo->blreset_over_noise_frames >= 5)
		flags |= HX_SAFE_FLAG_VERY_NOISY;
	if (algo->prev_raw_valid) {
		u32 interval = max_t(u16, algo->frame_interval_ms, 1);
		for (i = 0; i < HX_PIXELS; i++) {
			s32 jump = (s32)le16_to_cpup(raw + i) -
				(s32)le16_to_cpup(algo->prev_raw + i);
			if (abs(jump) > algo->wake_raw_jump_threshold)
				jump_nodes++;
		}
		if (jump_nodes > algo->wake_max_unstable_nodes) {
			algo->blreset_raw_jump_frames = min_t(u16,
				algo->blreset_raw_jump_frames + 1, U16_MAX);
			algo->blreset_raw_jump_elapsed_ms = min_t(u32,
				algo->blreset_raw_jump_elapsed_ms + interval, ~0U);
		} else {
			algo->blreset_raw_jump_frames = 0;
			algo->blreset_raw_jump_elapsed_ms = 0;
		}
		if (jump_nodes > algo->wake_max_unstable_nodes && !has_signal)
			flags |= HX_SAFE_FLAG_VERY_NOISY;
		/* IsLongTimeRawJumpDetected: a persistent jump is promoted to a
		 * sensor-baseline reset only after the normal debounce window. */
		/* The vendor's raw-jump accumulator trips after roughly 0x54 ms,
		 * rather than after a fixed frame count; this matters when scan rate
		 * changes during screen-on recovery. */
		if (algo->blreset_raw_jump_elapsed_ms >= 84)
			flags |= HX_SAFE_FLAG_SENSOR_BAD;
	} else {
		algo->blreset_raw_jump_frames = 0;
		algo->blreset_raw_jump_elapsed_ms = 0;
	}
	if (line_noise || out_of_range)
		flags |= HX_SAFE_FLAG_VERY_NOISY;
	if ((!has_signal && negative > algo->wake_max_unstable_nodes) ||
		(!has_signal && min_signal < -algo->baseline_peak_threshold))
		flags |= HX_SAFE_FLAG_VERY_NEGATIVE;
	if (!has_signal && bad > algo->wake_max_unstable_nodes)
		flags |= HX_SAFE_FLAG_SENSOR_BAD;
	if (hx_blreset_is_dirty_baseline(algo, max_signal, min_signal,
					 common_diff))
		flags |= HX_SAFE_FLAG_SENSOR_BAD | HX_SAFE_FLAG_VERY_NEGATIVE;
	if (!has_signal && algo->blreset_clean_baseline_captured &&
		!hx_blreset_check_clean_baseline(algo, raw))
		flags |= HX_SAFE_FLAG_SENSOR_BAD;
	/* A compact positive island with no firmware finger is the vendor's
	 * CheckGhostMax case: it blocks baseline learning but is not a touch. */
	if (!has_signal && abnormal && bad <= HX_BASELINE_CLEAN_MAX_NODES &&
	    max_signal > 0 &&
	    max_signal <= max_t(s16, algo->baseline_peak_threshold / 3, 1))
		flags |= HX_SAFE_FLAG_GHOST_MAX | HX_SAFE_FLAG_SILENT_GHOST;
	if (has_signal && bad && negative == bad)
		flags |= HX_SAFE_FLAG_ALL_TOUCH_BAD;
	return flags;
}

bool hx_blreset_check_abnormal_touches(struct hx_algo *algo)
{
	bool side_abnormal = false;
	u16 abnormal = 0;
	s32 min_size = INT_MAX;
	u8 active_tracks = 0, clustered_tracks = 0;
	int i;

	algo->safe_valid_touch_count = 0;
	algo->safe_abnormal_touch_count = 0;
	algo->blreset_all_touch_abnormal = false;
	algo->blreset_concurrent_touch = false;
	/* SafeBaseline_CheckTouch walks Peak_IsTouchValid results, not every raw
	 * local maximum.  `contacts` is the Linux equivalent after the resolver's
	 * validity/area filters; using the unfiltered peak list counted noise
	 * lobes as real touches and could incorrectly assert all-touch-abnormal. */
	for (i = 0; i < algo->contact_count; i++) {
		const struct hx_contact *contact = &algo->contacts[i];
		const struct hx_peak *peak;
		bool edge;
		int idx;

		if (contact->peak_index >= algo->peak_count)
			continue;
		peak = &algo->peaks[contact->peak_index];
		edge = contact->is_edge || peak->r <= 1 ||
			peak->r >= HX_ROWS - 2 || peak->c <= 1 ||
			peak->c >= HX_COLS - 2;
		idx = peak->r * HX_COLS + peak->c;
		s32 raw_diff = abs(algo->safe_bl2raw_dif_cmf[idx]);
		s32 working_diff = abs(algo->safe_bl2bl_dif_cmf[idx]);

		algo->safe_valid_touch_count++;
		/* BLReset_CheckAbnormalPeak -> CheckAbnormalSensor.  If raw is
		 * closer to the safe grid than working BL at the peak sensor, the
		 * working grid has absorbed that contact. */
		if (raw_diff >= working_diff)
			continue;
		algo->safe_abnormal_touch_count++;
		abnormal++;
		if (edge)
			side_abnormal = true;
	}
	/* This is the mutual-cap equivalent of
	 * BLReset_IsAllTouchByAbnormalBaseline: every current touch is explained
	 * better by the saved safe grid than by the working grid. */
	algo->blreset_all_touch_abnormal = algo->safe_valid_touch_count != 0 &&
		abnormal == algo->safe_valid_touch_count;
	/* BLReset_CocurrentTouchDetect uses the minimum touch-size value and
	 * accepts a group only when at least three touches are large and within a
	 * 50-unit size band.  signal_sum is the closest stable mutual-cap proxy
	 * available in the Linux touch record. */
	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo->tracks[i].active) {
			active_tracks++;
			min_size = min(min_size, algo->tracks[i].signal_sum);
		}
	if (active_tracks >= 3) {
		for (i = 0; i < HIMAX_MAX_TOUCH; i++)
			if (algo->tracks[i].active && algo->tracks[i].signal_sum > 200 &&
			    algo->tracks[i].signal_sum - min_size < 50)
				clustered_tracks++;
		algo->blreset_concurrent_touch = clustered_tracks >= 3 &&
			clustered_tracks == active_tracks;
	}
	return side_abnormal;
}

bool hx_blreset_is_dirty_baseline(const struct hx_algo *algo,
				  s16 max_signal, s16 min_signal,
				  s32 common_diff)
{
	/* BLReset_IsDirtyBaseline: a strong negative excursion together with a
	 * negative panel common term indicates that BL has absorbed a contact. */
	return min_signal < -(max_signal / 2) &&
		common_diff < -algo->baseline_noise_deadband &&
		algo->safe_current_negative_nodes > algo->wake_max_unstable_nodes;
}

bool hx_blreset_process(struct hx_algo *algo, u16 reason_mask)
{
	u32 interval = max_t(u16, algo->frame_interval_ms, 1);
	u32 threshold = algo->safe_reset_reason_trigger_frames ?
		(u32)algo->safe_reset_reason_trigger_frames * interval : 500;
	bool reason_crossed = false;
	int i;

	/* The Windows implementation keeps independent timers per reason.  The
	 * Linux port already records those detailed counters in
	 * hx_safe_baseline_reset_statistics(); this aggregate timer is the
	 * equivalent of BLReset_IsTriggered2's debounce gate and prevents a
	 * one-frame flag from causing a recovery action. */
	algo->blreset_reason_mask = reason_mask;
	if (reason_mask) {
		if (algo->blreset_state == HX_BLRESET_TRIGGERED)
			return false; /* wait for a clean frame before re-arming */
		algo->blreset_state = HX_BLRESET_DEBOUNCE;
		algo->blreset_reason_elapsed_ms = min_t(u32,
			algo->blreset_reason_elapsed_ms + interval, ~0U);
		/* ResetStatisticsUnit updates these counters in the SafeBaseline
		 * policy.  Look ahead by one frame here so BLReset_Process can emit its
		 * trigger using the same per-reason thresholds without maintaining a
		 * second, subtly different timer table. */
		for (i = 0; i < 10; i++) {
			u16 bit = (u16)(1U << i);
			u32 reason_threshold = algo->safe_reset_reason_trigger_frames ?
				(u32)algo->safe_reset_reason_trigger_frames :
				algo->safe_reset_time_threshold[i];

			if ((reason_mask & bit) &&
			    (u32)algo->safe_reset_reason_frames[i] + interval >=
				reason_threshold) {
				reason_crossed = true;
				break;
			}
		}
		if (reason_crossed || algo->blreset_reason_elapsed_ms >= threshold) {
			algo->blreset_state = HX_BLRESET_TRIGGERED;
			algo->blreset_triggered = true;
			algo->blreset_trigger_count++;
			return true;
		}
		return false;
	}

	/* Clear faster than accumulation, matching the vendor timer update: a
	 * transient clean frame cancels a pending debounce but does not instantly
	 * erase a long-lived abnormal state. */
	if (algo->blreset_reason_elapsed_ms > interval * 2)
		algo->blreset_reason_elapsed_ms -= interval * 2;
	else
		algo->blreset_reason_elapsed_ms = 0;
	if (!algo->blreset_reason_elapsed_ms) {
		if (algo->blreset_state != HX_BLRESET_IDLE)
			algo->blreset_clear_count++;
		algo->blreset_state = HX_BLRESET_IDLE;
		algo->blreset_triggered = false;
		algo->blreset_reason_mask = 0;
	}
	return false;
}
