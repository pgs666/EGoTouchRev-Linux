// SPDX-License-Identifier: GPL-2.0
/* Himax HX83121A baseline and frame preprocessing. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

enum hx_clean_baseline_quality {
	HX_CLEAN_BASELINE_GOOD = 0,
	HX_CLEAN_BASELINE_TEMPORAL,
	HX_CLEAN_BASELINE_SPATIAL,
	HX_CLEAN_BASELINE_NEGATIVE,
	HX_CLEAN_BASELINE_RANGE,
};

#define HX_RUNTIME_COMPARE_TRIGGER_NODES 8
#define HX_RUNTIME_COMPARE_SCORE_MARGIN 4

struct hx_baseline_compare_metrics {
	s32 common;
	s32 max_positive;
	s32 max_negative;
	u16 bad_nodes;
	u16 negative_nodes;
	u16 line_noise;
	u16 masked_bad_nodes;
	u16 out_of_range;
	u16 region_bad[HX_BASELINE_COMPARE_REGIONS];
};

static s32 hx_compare_common_shift(struct hx_algo *algo, const u16 *raw,
				   const s32 *reference_q8)
{
	s64 sum = 0;
	int common_bin;
	int common_count = 0;
	int cumulative = 0;
	int i;

	memset(algo->baseline_hist, 0, sizeof(algo->baseline_hist));
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		int bin = clamp_t(int, (sample - reference + 65536) >> 6,
				      0, HX_BASELINE_HIST_BINS - 1);

		algo->baseline_hist[bin]++;
	}
	for (common_bin = 0; common_bin < HX_BASELINE_HIST_BINS;
	     common_bin++) {
		cumulative += algo->baseline_hist[common_bin];
		if (cumulative >= HX_PIXELS / 2)
			break;
	}
	common_bin = min(common_bin, HX_BASELINE_HIST_BINS - 1);
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		s32 delta = sample - reference;
		int bin = clamp_t(int, (delta + 65536) >> 6,
				      0, HX_BASELINE_HIST_BINS - 1);

		if (bin == common_bin) {
			sum += delta;
			common_count++;
		}
	}

	return common_count ? (s32)(sum / common_count) : 0;
}

static int hx_compare_region(int r, int c)
{
	if (r < HX_ROWS / 4)
		return 1; /* top */
	if (r >= HX_ROWS * 3 / 4)
		return 2; /* bottom */
	if (c < HX_COLS / 4)
		return 3; /* left */
	if (c >= HX_COLS * 3 / 4)
		return 4; /* right */
	return 0; /* centre */
}

static u16 hx_compare_build_touch_mask(struct hx_algo *algo, const u16 *raw,
				       s32 working_common, s32 safe_common,
				       enum hx_finger_state finger_state)
{
	u16 masked = 0;
	int r, c, i;

	memset(algo->zone_map, 0, sizeof(algo->zone_map));
	if (finger_state != HX_FINGER_PRESENT)
		return 0;

	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 working = algo->baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 safe = algo->safe_baseline_q8[i] >>
			HX_BASELINE_FRACTION_BITS;
		s32 residual = max(sample - working - working_common,
				   sample - safe - safe_common);

		if (residual >= algo->baseline_peak_threshold)
			algo->zone_map[i] = 1;
	}
	for (i = 0; i < HX_PIXELS; i++) {
		if (algo->zone_map[i] != 1)
			continue;
		r = i / HX_COLS;
		c = i % HX_COLS;
		for (int dr = -2; dr <= 2; dr++) {
			int nr = r + dr;

			if (nr < 0 || nr >= HX_ROWS)
				continue;
			for (int dc = -2; dc <= 2; dc++) {
				int nc = c + dc;

				if (nc >= 0 && nc < HX_COLS &&
				    !algo->zone_map[nr * HX_COLS + nc])
					algo->zone_map[nr * HX_COLS + nc] = 2;
			}
		}
	}
	for (i = 0; i < HX_PIXELS; i++)
		if (algo->zone_map[i])
			masked++;
	return masked;
}

static void hx_compare_one_baseline(struct hx_algo *algo, const u16 *raw,
				    const s32 *reference_q8, s32 common,
				    struct hx_baseline_compare_metrics *metrics)
{
	u8 row_bad[HX_ROWS] = { 0 };
	u8 col_bad[HX_COLS] = { 0 };
	s32 threshold = max_t(s32, algo->wake_raw_jump_threshold * 2,
				  algo->baseline_peak_threshold);
	int r, c, i;

	memset(metrics, 0, sizeof(*metrics));
	metrics->common = common;
	metrics->max_positive = INT_MIN;
	metrics->max_negative = INT_MAX;
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		s32 local = sample - reference - common;

		metrics->max_positive = max(metrics->max_positive, local);
		metrics->max_negative = min(metrics->max_negative, local);
		if (sample < 0x1000 || sample > 0xf000)
			metrics->out_of_range++;
		if (abs(local) <= threshold)
			continue;
		if (algo->zone_map[i]) {
			metrics->masked_bad_nodes++;
			continue;
		}
		r = i / HX_COLS;
		c = i % HX_COLS;
		metrics->bad_nodes++;
		metrics->region_bad[hx_compare_region(r, c)]++;
		row_bad[r]++;
		col_bad[c]++;
		if (local < 0)
			metrics->negative_nodes++;
	}
	for (r = 0; r < HX_ROWS; r++)
		if (row_bad[r] >= algo->wake_max_unstable_line_nodes)
			metrics->line_noise++;
	for (c = 0; c < HX_COLS; c++)
		if (col_bad[c] >= algo->wake_max_unstable_line_nodes)
			metrics->line_noise++;
}

static bool hx_compare_metrics_valid(struct hx_algo *algo,
				     const struct hx_baseline_compare_metrics *m)
{
	return !m->out_of_range &&
	       abs(m->common) <= algo->cmf_max_correction &&
	       m->bad_nodes <= algo->wake_max_unstable_nodes &&
	       m->negative_nodes <= algo->wake_max_unstable_nodes &&
	       !m->line_noise;
}

static u32 hx_compare_metrics_score(const struct hx_baseline_compare_metrics *m)
{
	return m->bad_nodes + m->negative_nodes * 2 + m->line_noise * 8 +
	       m->out_of_range * 16;
}

static enum hx_safe_compare_result
hx_classify_baselines(struct hx_algo *algo,
		      const struct hx_baseline_compare_metrics *working,
		      const struct hx_baseline_compare_metrics *safe,
		      enum hx_finger_state finger_state)
{
	bool working_valid = hx_compare_metrics_valid(algo, working);
	bool safe_valid = hx_compare_metrics_valid(algo, safe);
	u32 working_score = hx_compare_metrics_score(working);
	u32 safe_score = hx_compare_metrics_score(safe);

	/* If firmware still sees a finger but the solver reported none, the
	 * masked region is useful evidence rather than noise: a safe baseline
	 * that restores a compact positive residual explains the raw frame better
	 * than a working baseline which has absorbed that contact.  Require the
	 * candidate baseline to remain valid outside the mask.
	 */
	if (finger_state == HX_FINGER_PRESENT) {
		bool reported = false;
		int i;

		for (i = 0; i < HIMAX_MAX_TOUCH; i++)
			if (algo->tracks[i].active && algo->tracks[i].reported) {
				reported = true;
				break;
			}
		if (reported)
			goto compare_validity;
		if (safe_valid && safe->masked_bad_nodes >=
		    working->masked_bad_nodes + 3)
			return HX_SAFE_COMPARE_SAFE_BETTER;
		if (working_valid && working->masked_bad_nodes >=
		    safe->masked_bad_nodes + 3)
			return HX_SAFE_COMPARE_WORKING_BETTER;
	}
compare_validity:
	if (working_valid && !safe_valid)
		return HX_SAFE_COMPARE_WORKING_BETTER;
	if (safe_valid && !working_valid)
		return HX_SAFE_COMPARE_SAFE_BETTER;
	if (!working_valid && !safe_valid)
		return HX_SAFE_COMPARE_BOTH_INVALID;
	if (safe_score + HX_RUNTIME_COMPARE_SCORE_MARGIN <= working_score &&
	    safe->negative_nodes <= working->negative_nodes &&
	    safe->line_noise <= working->line_noise)
		return HX_SAFE_COMPARE_SAFE_BETTER;
	if (working_score + HX_RUNTIME_COMPARE_SCORE_MARGIN <= safe_score &&
	    working->negative_nodes <= safe->negative_nodes &&
	    working->line_noise <= safe->line_noise)
		return HX_SAFE_COMPARE_WORKING_BETTER;
	if (working_score == safe_score)
		return HX_SAFE_COMPARE_BOTH_VALID;
	return HX_SAFE_COMPARE_AMBIGUOUS;
}

static bool hx_runtime_compare_triggered(struct hx_algo *algo, const u16 *raw,
					 enum hx_finger_state finger_state)
{
	u8 row_negative[HX_ROWS] = { 0 };
	u8 col_negative[HX_COLS] = { 0 };
	s64 sum = 0;
	s32 common;
	s32 threshold = max_t(s32, algo->wake_raw_jump_threshold * 2,
				  algo->baseline_peak_threshold);
	u16 positive = 0;
	u16 negative = 0;
	int r, c, i;

	for (i = 0; i < HX_PIXELS; i++)
		sum += (s32)le16_to_cpup(raw + i) -
		       (algo->baseline_q8[i] >> HX_BASELINE_FRACTION_BITS);
	common = (s32)(sum / HX_PIXELS);
	for (i = 0; i < HX_PIXELS; i++) {
		s32 local = (s32)le16_to_cpup(raw + i) -
			(algo->baseline_q8[i] >> HX_BASELINE_FRACTION_BITS) -
			common;

		if (local > threshold)
			positive++;
		else if (local < -threshold) {
			r = i / HX_COLS;
			c = i % HX_COLS;
			row_negative[r]++;
			col_negative[c]++;
			negative++;
		}
	}
	if (negative >= HX_RUNTIME_COMPARE_TRIGGER_NODES)
		return true;
	for (r = 0; r < HX_ROWS; r++)
		if (row_negative[r] >= algo->wake_max_unstable_line_nodes)
			return true;
	for (c = 0; c < HX_COLS; c++)
		if (col_negative[c] >= algo->wake_max_unstable_line_nodes)
			return true;
	if (finger_state == HX_FINGER_ABSENT &&
	    positive >= HX_RUNTIME_COMPARE_TRIGGER_NODES)
		return true;
	if (finger_state == HX_FINGER_PRESENT) {
		for (i = 0; i < HIMAX_MAX_TOUCH; i++)
			if (algo->tracks[i].active && algo->tracks[i].reported)
				return false;
		return true;
	}
	return false;
}

bool hx_safe_baseline_can_recover(const struct hx_algo *algo)
{
	u8 i;

	if (!algo->safe_baseline_count)
		return true; /* Legacy/test state with a directly installed safe BL. */
	if (algo->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS &&
	    algo->safe_baselines[algo->safe_baseline_selected].valid &&
	    !algo->safe_baselines[algo->safe_baseline_selected].reset_pending &&
	       algo->safe_baselines[algo->safe_baseline_selected].confidence >=
		HX_SAFE_BASELINE_CONFIRMED)
		return true;
	for (i = 0; i < HX_SAFE_BASELINE_SLOTS; i++)
		if (algo->safe_baselines[i].valid &&
		    !algo->safe_baselines[i].reset_pending &&
		    algo->safe_baselines[i].confidence >= HX_SAFE_BASELINE_CONFIRMED)
			return true;
	return false;
}

bool hx_algo_runtime_baseline_process(struct hx_algo *algo, const u16 *raw,
				      enum hx_finger_state finger_state,
				      bool force)
{
	struct hx_baseline_compare_metrics working;
	struct hx_baseline_compare_metrics safe;
	enum hx_safe_compare_result result;
	enum hx_safe_reset_action reset_action;
	bool recommended;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	bool was_recommended = algo->diag_blreset_recommended;

	algo->diag_blreset_recommended = false;
#endif
	if (!force && algo->runtime_blreset_cooldown_frames) {
		algo->runtime_blreset_cooldown_frames--;
		return false;
	}
	if (!algo->baseline_initialized || !algo->safe_baseline_valid ||
	    algo->wake_qualifying) {
		algo->runtime_safe_improvement_frames = 0;
		algo->runtime_safe_regression_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->diag_safe_improvement_frames = 0;
		algo->diag_safe_regression_frames = 0;
#endif
		return false;
	}
	if (!force && !hx_runtime_compare_triggered(algo, raw, finger_state)) {
		algo->runtime_safe_improvement_frames = 0;
		algo->runtime_safe_regression_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->diag_safe_improvement_frames = 0;
		algo->diag_safe_regression_frames = 0;
		algo->diag_safe_compare_result = HX_SAFE_COMPARE_NOT_RUN;
#endif
		return false;
	}
	/* Runtime faults can occur in an electrical state represented by a
	 * different safe slot than the one selected at wake.  Re-score every
	 * valid slot before comparing working data; this is the multi-baseline
	 * behaviour missing from the original two-slot implementation.
	 */
	if (algo->safe_baseline_count > 1)
		hx_safe_baseline_select_for_raw(algo, raw);
	if (!hx_safe_baseline_can_recover(algo))
		hx_safe_baseline_promote_latest_confirmed(algo);
	hx_safe_baseline_buffer_comparison(algo, raw);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_baseline_compare_triggers++;
#endif

	working.common = hx_compare_common_shift(algo, raw,
						algo->baseline_q8);
	safe.common = hx_compare_common_shift(algo, raw,
					     algo->safe_baseline_q8);
	hx_compare_build_touch_mask(algo, raw,
		working.common, safe.common, finger_state);
	hx_compare_one_baseline(algo, raw, algo->baseline_q8, working.common,
				&working);
	hx_compare_one_baseline(algo, raw, algo->safe_baseline_q8, safe.common,
				&safe);
	result = hx_classify_baselines(algo, &working, &safe, finger_state);
	hx_safe_baseline_check_with_state(algo, finger_state,
		result != HX_SAFE_COMPARE_BOTH_INVALID,
		working.line_noise || safe.line_noise ||
		working.bad_nodes > algo->wake_max_unstable_nodes * 2,
		working.negative_nodes > algo->wake_max_unstable_nodes,
		result == HX_SAFE_COMPARE_BOTH_INVALID,
		working.out_of_range || safe.out_of_range);
	reset_action = hx_safe_baseline_reset_policy(algo, result,
		false, HX_FINGER_UNKNOWN);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_touch_mask_nodes = 0;
	for (int i = 0; i < HX_PIXELS; i++)
		if (algo->zone_map[i])
			algo->diag_touch_mask_nodes++;
	algo->diag_safe_compare_result = result;
	algo->diag_baseline_compare_runs++;
	algo->diag_working_common_shift = working.common;
	algo->diag_safe_common_shift = safe.common;
	algo->diag_working_max_positive = working.max_positive;
	algo->diag_working_max_negative = working.max_negative;
	algo->diag_safe_max_positive = safe.max_positive;
	algo->diag_safe_max_negative = safe.max_negative;
	algo->diag_working_bad_nodes = working.bad_nodes;
	algo->diag_safe_bad_nodes = safe.bad_nodes;
	algo->diag_working_negative_nodes = working.negative_nodes;
	algo->diag_safe_negative_nodes = safe.negative_nodes;
	algo->diag_working_line_noise = working.line_noise;
	algo->diag_safe_line_noise = safe.line_noise;
	algo->diag_working_masked_bad_nodes = working.masked_bad_nodes;
	algo->diag_safe_masked_bad_nodes = safe.masked_bad_nodes;
	memcpy(algo->diag_working_region_bad, working.region_bad,
	       sizeof(algo->diag_working_region_bad));
	memcpy(algo->diag_safe_region_bad, safe.region_bad,
	       sizeof(algo->diag_safe_region_bad));
#endif
	if (reset_action == HX_SAFE_RESET_INVALIDATE) {
		hx_safe_baseline_reset_selected(algo);
		return false;
	}
	if (reset_action == HX_SAFE_RESET_WORKING &&
	    hx_safe_baseline_can_recover(algo)) {
		hx_restore_working_from_safe(algo, safe.common, true, false);
		algo->runtime_blreset_cooldown_frames =
			algo->runtime_blreset_cooldown;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->baseline_safe_side_reset_count++;
#endif
		return true;
	}

	if (result == HX_SAFE_COMPARE_SAFE_BETTER) {
		algo->runtime_safe_improvement_frames = min_t(u8,
			algo->runtime_safe_improvement_frames + 1, U8_MAX);
		algo->runtime_safe_regression_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->diag_safe_improvement_frames = min_t(u8,
			algo->diag_safe_improvement_frames + 1, U8_MAX);
		algo->diag_safe_regression_frames = 0;
#endif
	} else if (result == HX_SAFE_COMPARE_WORKING_BETTER) {
		algo->runtime_safe_regression_frames = min_t(u8,
			algo->runtime_safe_regression_frames + 1, U8_MAX);
		algo->runtime_safe_improvement_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->diag_safe_regression_frames = min_t(u8,
			algo->diag_safe_regression_frames + 1, U8_MAX);
		algo->diag_safe_improvement_frames = 0;
#endif
	} else {
		algo->runtime_safe_improvement_frames = 0;
		algo->runtime_safe_regression_frames = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->diag_safe_improvement_frames = 0;
		algo->diag_safe_regression_frames = 0;
#endif
	}
	recommended = algo->runtime_safe_improvement_frames >=
		max_t(u8, algo->runtime_blreset_confirm_frames, 1) &&
		!safe.bad_nodes && !safe.negative_nodes && !safe.line_noise &&
		!safe.out_of_range && hx_safe_baseline_can_recover(algo);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_blreset_recommended = recommended;
	if (algo->diag_blreset_recommended && !was_recommended)
		algo->diag_blreset_recommendation_count++;
#endif
	if (!recommended)
		return false;
	if (!algo->runtime_blreset_enabled) {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->runtime_blreset_suppressed_count++;
#endif
		return false;
	}

	/* The selected safe grid is immutable recovery authority.  Preserve the
	 * tracker across a baseline correction: clearing it creates an artificial
	 * UP/DOWN break even when the raw contact remains continuous.  Touch hold
	 * prevents the fault frame from being learned back into the restored grid.
	 */
	hx_restore_working_from_safe(algo, safe.common, true,
				      finger_state == HX_FINGER_PRESENT);
	algo->iir_initialized = false;
	algo->runtime_blreset_cooldown_frames = algo->runtime_blreset_cooldown;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->runtime_blreset_count++;
	algo->diag_safe_compare_result = result;
	algo->diag_safe_common_shift = safe.common;
	algo->diag_blreset_recommended = false;
#endif
	return true;
}

static void hx_clean_baseline_reset(struct hx_algo *algo)
{
	algo->safe_no_finger_frames = 0;
	algo->safe_candidate_confirming = false;
	algo->safe_candidate_screen_epoch = 0;
	algo->safe_confirm_common_sum = 0;
	algo->safe_confirm_common_last = 0;
}

static void hx_clean_baseline_record_reject(struct hx_algo *algo,
					    enum hx_clean_baseline_quality quality)
{
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	switch (quality) {
	case HX_CLEAN_BASELINE_TEMPORAL:
		algo->baseline_safe_temporal_reject_count++;
		break;
	case HX_CLEAN_BASELINE_SPATIAL:
		algo->baseline_safe_spatial_reject_count++;
		break;
	case HX_CLEAN_BASELINE_NEGATIVE:
		algo->baseline_safe_negative_reject_count++;
		break;
	case HX_CLEAN_BASELINE_RANGE:
		algo->baseline_safe_range_reject_count++;
		break;
	case HX_CLEAN_BASELINE_GOOD:
		break;
	}
#endif
}

/* Compare one complete raw grid with a Q8 baseline after removing a robust
 * panel-wide common shift.  Sparse positive residuals indicate a latent touch
 * or display island; persistent negative residuals are tracked separately
 * because Windows BLReset treats them as baseline-trust failures.
 */
static enum hx_clean_baseline_quality
hx_clean_raw_quality(struct hx_algo *algo, const u16 *raw,
		     const s32 *reference_q8, s32 threshold, s32 *common_out)
{
	u8 row_bad[HX_ROWS] = { 0 };
	u8 col_bad[HX_COLS] = { 0 };
	u8 row_negative[HX_ROWS] = { 0 };
	u8 col_negative[HX_COLS] = { 0 };
	s64 common_sum = 0;
	s32 common;
	u16 out_of_range = 0;
	u16 bad = 0;
	u16 negative = 0;
	int common_bin;
	int common_count = 0;
	int cumulative = 0;
	int r, c, i;

	memset(algo->baseline_hist, 0, sizeof(algo->baseline_hist));
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		int bin;

		if (sample < 0x1000 || sample > 0xf000)
			out_of_range++;
		bin = clamp_t(int, (sample - reference + 65536) >> 6,
				  0, HX_BASELINE_HIST_BINS - 1);
		algo->baseline_hist[bin]++;
	}
	/* A safe baseline is a recovery authority, not a best-effort runtime
	 * frame.  Do not persist even a small number of electrically impossible
	 * cells across the next screen cycle.
	 */
	if (out_of_range)
		return HX_CLEAN_BASELINE_RANGE;
	for (common_bin = 0; common_bin < HX_BASELINE_HIST_BINS;
	     common_bin++) {
		cumulative += algo->baseline_hist[common_bin];
		if (cumulative >= HX_PIXELS / 2)
			break;
	}
	common_bin = min(common_bin, HX_BASELINE_HIST_BINS - 1);
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		s32 delta = sample - reference;
		int bin = clamp_t(int, (delta + 65536) >> 6, 0,
				      HX_BASELINE_HIST_BINS - 1);

		if (bin == common_bin) {
			common_sum += delta;
			common_count++;
		}
	}
	if (!common_count)
		return HX_CLEAN_BASELINE_TEMPORAL;
	common = (s32)(common_sum / common_count);
	if (abs(common) > algo->cmf_max_correction)
		return HX_CLEAN_BASELINE_TEMPORAL;
	/* The vendor rejects even a small compact positive island as a ghost
	 * candidate.  Without this check 1-3 cells fall below the generic spatial
	 * count and can be absorbed into a new safe baseline.
	 */
	if (hx_safe_baseline_check_ghost(algo, raw, reference_q8, common,
					 NULL))
		return HX_CLEAN_BASELINE_SPATIAL;
	if (hx_safe_baseline_check_side_touch(algo, raw, reference_q8,
					      common))
		return HX_CLEAN_BASELINE_SPATIAL;
	if (hx_safe_baseline_check_side_very_negative(algo, raw, reference_q8,
						      common))
		return HX_CLEAN_BASELINE_NEGATIVE;

	threshold = max_t(s32, threshold, 1);
	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample = (s32)le16_to_cpup(raw + i);
		s32 reference = reference_q8[i] >> HX_BASELINE_FRACTION_BITS;
		s32 local = sample - reference - common;

		if (abs(local) <= threshold)
			continue;
		r = i / HX_COLS;
		c = i % HX_COLS;
		row_bad[r]++;
		col_bad[c]++;
		bad++;
		if (local < 0) {
			row_negative[r]++;
			col_negative[c]++;
			negative++;
		}
	}
	if (negative > algo->wake_max_unstable_nodes)
		return HX_CLEAN_BASELINE_NEGATIVE;
	for (r = 0; r < HX_ROWS; r++)
		if (row_negative[r] >= algo->wake_max_unstable_line_nodes)
			return HX_CLEAN_BASELINE_NEGATIVE;
	for (c = 0; c < HX_COLS; c++)
		if (col_negative[c] >= algo->wake_max_unstable_line_nodes)
			return HX_CLEAN_BASELINE_NEGATIVE;
	if (bad > algo->wake_max_unstable_nodes)
		return HX_CLEAN_BASELINE_SPATIAL;
	for (r = 0; r < HX_ROWS; r++)
		if (row_bad[r] >= algo->wake_max_unstable_line_nodes)
			return HX_CLEAN_BASELINE_SPATIAL;
	for (c = 0; c < HX_COLS; c++)
		if (col_bad[c] >= algo->wake_max_unstable_line_nodes)
			return HX_CLEAN_BASELINE_SPATIAL;

	*common_out = common;
	return HX_CLEAN_BASELINE_GOOD;
}

static void hx_clean_baseline_start(struct hx_algo *algo, const u16 *raw)
{
	hx_copy_raw_to_baseline(algo->wake_candidate_q8, raw);
	algo->safe_no_finger_frames = 1;
	algo->safe_candidate_confirming = false;
	algo->safe_candidate_screen_epoch = algo->screen_epoch;
	algo->safe_confirm_common_sum = 0;
	algo->safe_confirm_common_last = 0;
}

static void hx_clean_baseline_accumulate(struct hx_algo *algo,
					 const u16 *raw)
{
	u8 count = min_t(u8, algo->safe_no_finger_frames + 1, U8_MAX);
	int i;

	for (i = 0; i < HX_PIXELS; i++) {
		s32 sample_q8 = (s32)le16_to_cpup(raw + i) <<
				HX_BASELINE_FRACTION_BITS;
		s32 delta = sample_q8 - algo->wake_candidate_q8[i];

		algo->wake_candidate_q8[i] += delta / count;
	}
	algo->safe_no_finger_frames = count;
}

static void hx_clean_baseline_process(struct hx_algo *algo, const u16 *raw)
{
	enum hx_clean_baseline_quality quality;
	s32 common = 0;
	s32 threshold;
	bool reacquiring = algo->baseline_reacquire_pending;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	u8 safe_count_before;
	u32 safe_epoch_before = 0;
#endif
	int i;

	/* Do not repeatedly sample an unchanged electrical state forever.  Unlike
	 * the old implementation, a materially different same-epoch state may
	 * still fill another queue slot, matching the vendor's five-entry queue.
	 */
	if (algo->safe_baseline_valid &&
	    algo->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS &&
	    algo->safe_baselines[algo->safe_baseline_selected].valid &&
	    algo->safe_baselines[algo->safe_baseline_selected].screen_epoch ==
		algo->screen_epoch &&
	    hx_safe_baseline_raw_matches_selected(algo, raw)) {
		algo->safe_candidate_armed = false;
		hx_clean_baseline_reset(algo);
		return;
	}

	if (!algo->safe_no_finger_frames &&
	    !algo->safe_candidate_confirming) {
		if (!algo->safe_candidate_armed)
			return;
		hx_clean_baseline_start(algo, raw);
		return;
	}
	if (algo->safe_candidate_screen_epoch != algo->screen_epoch) {
		hx_clean_baseline_reset(algo);
		hx_clean_baseline_start(algo, raw);
		return;
	}

	threshold = algo->safe_candidate_confirming ?
		max_t(s32, algo->wake_raw_jump_threshold * 2,
		      algo->baseline_peak_threshold) :
		algo->wake_raw_jump_threshold;
	quality = hx_clean_raw_quality(algo, raw, algo->wake_candidate_q8,
				       threshold, &common);
	if (quality != HX_CLEAN_BASELINE_GOOD ||
	    (algo->safe_candidate_confirming &&
	     algo->safe_no_finger_frames &&
	     abs(common - algo->safe_confirm_common_last) >
		algo->wake_raw_jump_threshold)) {
		if (quality == HX_CLEAN_BASELINE_GOOD)
			quality = HX_CLEAN_BASELINE_TEMPORAL;
		hx_clean_baseline_record_reject(algo, quality);
		hx_clean_baseline_reset(algo);
		return;
	}

	if (!algo->safe_candidate_confirming) {
		hx_clean_baseline_accumulate(algo, raw);
		if (algo->safe_no_finger_frames <
		    algo->safe_commit_no_finger_frames)
			return;
		if (!hx_safe_baseline_is_ok_to_update(algo)) {
			hx_clean_baseline_record_reject(algo,
				HX_CLEAN_BASELINE_SPATIAL);
			hx_clean_baseline_reset(algo);
			return;
		}

		/* The learning grid may itself have absorbed a persistent anomaly.
		 * A valid prior safe baseline is therefore the final independent
		 * reference before starting the confirmation window.
		 */
		quality = hx_clean_raw_quality(algo, raw,
			algo->safe_baseline_valid ? algo->safe_baseline_q8 :
			algo->baseline_q8,
			max_t(s32, algo->wake_raw_jump_threshold * 2,
			      algo->baseline_peak_threshold), &common);
		if (quality != HX_CLEAN_BASELINE_GOOD) {
			hx_clean_baseline_record_reject(algo, quality);
			hx_clean_baseline_reset(algo);
			return;
		}
		algo->safe_candidate_confirming = true;
		algo->safe_no_finger_frames = 0;
		algo->safe_confirm_common_sum = 0;
		algo->safe_confirm_common_last = 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
		algo->baseline_safe_confirm_count++;
#endif
		return;
	}

	algo->safe_confirm_common_sum += common;
	algo->safe_confirm_common_last = common;
	if (algo->safe_no_finger_frames < U8_MAX)
		algo->safe_no_finger_frames++;
	if (algo->safe_no_finger_frames < algo->safe_commit_no_finger_frames)
		return;

	common = algo->safe_confirm_common_sum / algo->safe_no_finger_frames;
	for (i = 0; i < HX_PIXELS; i++)
		algo->wake_candidate_q8[i] = clamp_t(s32,
			algo->wake_candidate_q8[i] +
			common * (1 << HX_BASELINE_FRACTION_BITS),
			0, 0xffff << HX_BASELINE_FRACTION_BITS);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	safe_count_before = algo->safe_baseline_count;
	if (algo->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS &&
	    algo->safe_baselines[algo->safe_baseline_selected].valid)
		safe_epoch_before =
			algo->safe_baselines[algo->safe_baseline_selected].screen_epoch;
#endif
	hx_safe_baseline_commit(algo, algo->wake_candidate_q8);
	algo->baseline_reacquire_pending = false;
	algo->baseline_screen_on_hand_state = reacquiring ?
		HX_HAND_NONE : algo->baseline_screen_on_hand_state;
	/* A committed grid is not immediately considered a free-running BLIIR
	 * reference.  The official path keeps the newly accepted safe baseline
	 * under observation while the AFE settles; otherwise the first residual
	 * display frames are integrated back into the grid and the user sees a
	 * slow post-release degradation.
	 */
	if (reacquiring)
		algo->baseline_post_reacquire_hold = 120;
	/* A committed observation disarms the collector until a new wake or an
	 * explicit reacquire arms it.  This prevents duplicate queue entries from
	 * a stationary panel while direct commits can still represent distinct
	 * same-epoch electrical states.
	 */
	algo->safe_candidate_armed = false;
	hx_clean_baseline_reset(algo);
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	if (algo->safe_baseline_count > safe_count_before ||
	    (algo->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS &&
	     algo->safe_baselines[algo->safe_baseline_selected].screen_epoch !=
		safe_epoch_before))
		algo->baseline_safe_commit_count++;
#endif
}

static void hx_prepare_frame_baseline(struct hx_algo *algo, const u16 *raw,
				      enum hx_finger_state finger_state)
{
	const u16 *bliir_raw = algo->bliir_use_pre_cmf_raw ?
		algo->pre_cmf_raw : raw;
	s64 common_sum = 0;
	s32 common_diff;
	s32 max_delta = INT_MIN;
	int common_bin = 0;
	int common_count = 0;
	int cumulative = 0;
	bool has_signal = false;
	bool found_freeze = false;
	bool touch_hold_released = false;
	bool safe_frame_clean;
	bool operational_clean;
	bool spatial_update_allowed;
	bool stage_quality_allowed;
	bool stable_no_touch;
	bool release_candidate;
	bool blrecal_request;
	bool shb_noisy_action;
	bool no_active_tracks = true;
	bool blreset_touch_protected = false;
	u8 active_tracks = 0;
	u16 blsm_property = 0;
	u16 blreset_flags;
	u16 signal_flags;
	u16 ghost_nodes = 0;
	bool ghost_max = false;
	struct hx_baseline_frame_observation observation;
	int r, c;

	if (!algo->baseline_initialized) {
		/* Match Windows BLIIR_Reset: use a real no-finger raw snapshot.  If
		 * the first usable frame already carries a finger, retain the neutral
		 * fallback so the contact cannot disappear into the baseline.
		 */
		if (finger_state != HX_FINGER_ABSENT) {
			for (r = 0; r < HX_PIXELS; r++)
				algo->baseline_q8[r] =
					(s32)algo->baseline_initial <<
					HX_BASELINE_FRACTION_BITS;
		} else {
			hx_copy_raw_to_baseline(algo->baseline_q8, bliir_raw);
		}
		memset(algo->baseline_release_hold, 0,
		       sizeof(algo->baseline_release_hold));
		algo->baseline_initialized = true;
	}
	memset(algo->baseline_hist, 0, sizeof(algo->baseline_hist));
	/* Estimate panel-wide VCOM/temperature drift before classifying cells. */
	for (r = 1; r < HX_PIXELS; r++) {
		s32 sample = (s32)le16_to_cpup(raw + r);
		s32 baseline = algo->baseline_q8[r] >>
				HX_BASELINE_FRACTION_BITS;
		s32 delta = sample - baseline;
		int bin = (delta + 65536) >> 6;

		algo->baseline_hist[clamp_t(int, bin, 0,
			HX_BASELINE_HIST_BINS - 1)]++;
	}
	for (common_bin = 0; common_bin < HX_BASELINE_HIST_BINS;
	     common_bin++) {
		cumulative += algo->baseline_hist[common_bin];
		if (cumulative >= HX_PIXELS / 2)
			break;
	}
	common_bin = min(common_bin, HX_BASELINE_HIST_BINS - 1);
	for (r = 1; r < HX_PIXELS; r++) {
		s32 sample = (s32)le16_to_cpup(raw + r);
		s32 baseline = algo->baseline_q8[r] >>
				HX_BASELINE_FRACTION_BITS;
		s32 delta = sample - baseline;
		int bin = clamp_t(int, (delta + 65536) >> 6, 0,
				      HX_BASELINE_HIST_BINS - 1);

		max_delta = max(max_delta, delta);
		if (bin == common_bin) {
			common_sum += delta;
			common_count++;
		}
	}
	common_diff = common_count ? (s32)(common_sum / common_count) : 0;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_common_diff = common_diff;
#endif
	if (finger_state == HX_FINGER_PRESENT)
		has_signal = true;
	/* Even a validated firmware "no finger" bit may be wrong during display
	 * interference.  Never let a contradictory raw peak be learned quickly.
	 */
	if (!has_signal) {
		/* max_delta is collected while locating the common-mode bin.  This
		 * avoids a third full-grid pass on every firmware no-finger frame.
		 */
		has_signal = max_delta - common_diff >=
			algo->baseline_peak_threshold;
		for (r = 0; r < HIMAX_MAX_TOUCH; r++)
			if (algo->tracks[r].active) {
				has_signal = true;
				break;
			}
	}
	hx_baseline_observe_frame(algo, raw, common_diff, &observation);
	blrecal_request = hx_blrecal_process(algo, raw, observation.max_signal,
		observation.min_signal, false, false);
	blreset_flags = hx_blreset_classify_frame(algo, raw, common_diff,
		observation.max_signal, observation.min_signal, has_signal,
		observation.line_noise, observation.out_of_range);
	/* BLSM_GetProperty and SafeBaseline_CheckWithState must consume the same
	 * BLReset result.  Keep platform-originated flags in addition to the
	 * frame-local predicates; the latter are replaced after postprocess with
	 * the richer touch-aware view.
	 */
	algo->safe_prev_flags = algo->safe_flags;
	algo->safe_flags = blreset_flags;
	if (algo->platform.charger_noise)
		algo->safe_flags |= HX_SAFE_FLAG_VERY_NOISY;
	if (algo->platform.panel_sd)
		algo->safe_flags |= HX_SAFE_FLAG_SENSOR_BAD;
	if (algo->platform.smart_cover)
		algo->safe_flags |= HX_SAFE_FLAG_ALL_TOUCH_BAD;
	for (r = 0; r < HIMAX_MAX_TOUCH; r++)
		if (algo->tracks[r].active) {
			no_active_tracks = false;
			active_tracks++;
			/* BLReset_BaselineStateUpdate protects a young, valid touch in
			 * the panel interior.  Tracks use normalized coordinates, so a
			 * one-grid-cell-equivalent margin provides the same exclusion of
			 * the outer sensor ring.
			 */
			if (algo->tracks[r].reported && algo->tracks[r].age < 12 &&
			    algo->tracks[r].x > 4096 && algo->tracks[r].x < 61439 &&
			    algo->tracks[r].y > 4096 && algo->tracks[r].y < 61439)
				blreset_touch_protected = true;
		}
	hx_blreset_baseline_state_update(algo, false,
		blreset_touch_protected, observation.operational_bad_nodes);
	/* The vendor runs three independent abnormal-baseline timers after the
	 * baseline-state update.  Keep their classification separate from the
	 * generic reset gate; only a vote that survives the vendor-sized 300 ms
	 * window is promoted to a reset reason below.
	 */
	(void)hx_blreset_abnormal_types(algo, observation.max_signal,
		observation.min_signal, observation.operational_bad_nodes,
		observation.operational_negative_nodes, has_signal,
		blreset_touch_protected, false);
	if (algo->blreset_abnormal_type_elapsed[0] > 300)
		algo->safe_flags |= HX_SAFE_FLAG_VERY_NEGATIVE;
	if (algo->blreset_abnormal_type_elapsed[1] > 300)
		algo->safe_flags |= HX_SAFE_FLAG_VERY_NOISY;
	if (algo->blreset_abnormal_type_elapsed[2] > 300)
		algo->safe_flags |= HX_SAFE_FLAG_SENSOR_BAD;
	/* SafeBaseline_CheckWithState/CheckWithSignal are part of the normal
	 * BLSM pre-process path in TSACore, not an optional runtime-comparison
	 * pass.  Feed their signal predicates before selecting the BLIIR action so
	 * a noisy/negative/silent-ghost frame cannot be learned as baseline.
	 */
	/* The full BL2RAW buffer comparison is intentionally deferred to the
	 * postprocess path, but the signal predicates still need current-frame
	 * population counts.  Seed the counters from the observation rather than
	 * reusing the previous frame's values (which could otherwise keep a stale
	 * VERY_NOISY/VERY_NEGATIVE flag alive).
	 */
	algo->safe_current_negative_nodes = observation.safe_negative_nodes;
	algo->safe_current_positive_nodes = observation.safe_bad_nodes >
		observation.safe_negative_nodes ?
		observation.safe_bad_nodes - observation.safe_negative_nodes : 0;
	signal_flags = hx_safe_baseline_signal_flags(algo,
		observation.max_signal, observation.min_signal,
		finger_state == HX_FINGER_PRESENT || active_tracks != 0);
	if (algo->safe_baseline_valid)
		ghost_max = hx_safe_baseline_check_ghost(algo, raw,
			algo->safe_baseline_q8, common_diff, &ghost_nodes);
	algo->safe_flags |= signal_flags;
	if (ghost_max)
		algo->safe_flags |= HX_SAFE_FLAG_GHOST_MAX;
	hx_safe_baseline_check_with_state(algo, finger_state,
		/* No BL2BL/BL2RAW comparison was performed in this lightweight
		 * per-frame path.  Passing ``true`` merely because a safe slot exists
		 * would claim the vendor SafeDiff check completed and allow learning
		 * during a touch.  The dedicated runtime comparator passes its actual
		 * result separately.
		 */
		false, !!(signal_flags &
		HX_SAFE_FLAG_VERY_NOISY), !!(signal_flags &
		HX_SAFE_FLAG_VERY_NEGATIVE), ghost_max,
		observation.out_of_range);
	safe_frame_clean = !observation.out_of_range &&
		observation.safe_bad_nodes <= HX_BASELINE_CLEAN_MAX_NODES;
	operational_clean = !observation.out_of_range &&
		observation.operational_bad_nodes <=
			algo->wake_max_unstable_nodes &&
		!observation.line_noise &&
		/* BLSM_GetMinSig rejects negative excursions from BLIIR updates;
		 * otherwise a display-induced dip is slowly learned as baseline.
		 */
		!observation.operational_negative_nodes;
	/* Match BLSM's stage-0 debounce: a single apparently clean frame is
	 * not enough to authorize per-cell BLIIR tracking.  Touch/noise resets
	 * the stage; only a stable no-touch run can advance it.
	 */
	if (has_signal || !operational_clean)
		algo->baseline_no_touch_stable_frames = 0;
	else if (algo->baseline_no_touch_stable_frames < U8_MAX)
		algo->baseline_no_touch_stable_frames++;
	stable_no_touch = algo->baseline_no_touch_stable_frames >= 10;
	shb_noisy_action = hx_blsm_shb_consume_action(algo) != 0;
	/* BLSM consumes the frame classification before BLIIR is allowed to
	 * update.  Keep the stage explicit so touch, mild noise and severe line
	 * noise cannot accidentally share the same learning path.
	 */
	/* BLRecal only raises the vendor recalibration request bit here.  The
	 * hardware calibration command is owned by the SPI layer; treating the
	 * request as BLIIR_Reset would incorrectly snapshot a noisy frame.
	 */
	blsm_property = hx_baseline_get_property(algo, observation.max_signal,
		has_signal, operational_clean, blrecal_request,
		shb_noisy_action, observation.line_noise,
		observation.out_of_range);
	hx_baseline_stage_update(algo, blsm_property,
		observation.max_signal, observation.min_signal,
		active_tracks == 1);
	hx_baseline_stage_process(algo, raw, bliir_raw,
		observation.max_signal, observation.min_signal,
		has_signal, operational_clean);
	/* Firmware no-finger plus no active track is the authoritative release
	 * indication for a held-in-hand wake.  Residual spatial rebound is the
	 * reason reacquire exists; it must not keep the guard in PROTECTED forever.
	 */
	release_candidate = algo->baseline_held_in_hand &&
		algo->baseline_touch_seen &&
		finger_state == HX_FINGER_ABSENT && no_active_tracks &&
		!observation.out_of_range && !observation.line_noise &&
		observation.safe_bad_nodes <= HX_BASELINE_CLEAN_MAX_NODES;
	touch_hold_released = hx_baseline_guard_process(algo,
		release_candidate ? true : operational_clean,
		release_candidate ? false : has_signal || !operational_clean);
	/* After a held-in-hand wake, the old no-palm grid may be permanently
	 * offset even when the release window is clean.  Reacquire one complete
	 * raw grid at the release boundary instead of letting the ordinary BLIIR
	 * integrator crawl toward it and potentially absorb residual artifacts.
	 */
	if (touch_hold_released && algo->baseline_enabled &&
		(!has_signal || release_candidate) &&
		(operational_clean || release_candidate)) {
		algo->baseline_reacquire_pending = true;
		algo->baseline_screen_on_hand_state = HX_HAND_REACQUIRE;
		hx_copy_raw_to_baseline(algo->baseline_q8, raw);
		algo->baseline_prev_had_signal = false;
		algo->baseline_had_freeze = false;
		algo->baseline_recovery_frames = 0;
		algo->baseline_no_touch_stable_frames = 0;
	}
	/* Stages 7/8/9 already applied the vendor BLIIR_Update signal gate.
	 * Requiring the generic operational-clean predicate a second time would
	 * suppress the negative-recovery behavior for which those stages exist.
	 */
	stage_quality_allowed = operational_clean ||
		algo->baseline_stage == HX_BLSM_POST_TOUCH ||
		algo->baseline_stage == HX_BLSM_POST_TOUCH_ALT ||
		algo->baseline_stage == HX_BLSM_DEBOUNCE;
	spatial_update_allowed = !algo->baseline_touch_hold &&
		algo->baseline_stage_allows_update && stage_quality_allowed &&
		(algo->baseline_stage_force_update || !has_signal) &&
		(stable_no_touch ||
		algo->baseline_stage == HX_BLSM_POST_TOUCH ||
		algo->baseline_stage == HX_BLSM_POST_TOUCH_ALT ||
		algo->baseline_stage == HX_BLSM_DEBOUNCE);
	if (algo->baseline_post_reacquire_hold) {
		spatial_update_allowed = false;
		if (!has_signal && operational_clean &&
		    algo->baseline_post_reacquire_hold > 0)
			algo->baseline_post_reacquire_hold--;
	}

	algo->baseline_recovery_frames = 0;
	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 sample = (s32)le16_to_cpup(raw + idx);
			s32 baseline = algo->baseline_q8[idx] >>
					HX_BASELINE_FRACTION_BITS;
			s32 delta = sample - baseline;
			s32 local = delta - common_diff;

			if (!algo->baseline_enabled) {
				delta = sample - algo->baseline_initial;
				goto store;
			}
			if (algo->baseline_stage_reset ||
			    algo->baseline_update_suppressed_once) {
				delta = 0;
				goto store;
			}
			if (!spatial_update_allowed) {
				/* Vendor BLSM hold stages do not update BL.  CMF handles
				 * common-mode signal; letting the baseline follow common_diff
				 * during a touch slowly absorbs a held palm.
				 */
				delta = local >= algo->baseline_peak_threshold ?
					local : 0;
				if (delta) {
					algo->baseline_release_hold[idx] =
						algo->baseline_release_hold_frames;
					found_freeze = true;
				}
				goto store;
			}

			/* Freeze cells carrying a finger-sized positive signal.  A hold
			 * after release prevents the negative rebound from being absorbed
			 * into the baseline and creating a later false lift.
			 */
			if (algo->baseline_release_hold[idx]) {
				algo->baseline_release_hold[idx]--;
				if (local < -algo->baseline_negative_deadband) {
					delta = local;
					goto store;
				}
				delta = 0;
				goto store;
			}

			/* Official BLIIR selects raw or pre-CMF raw independently of
			 * the signal frame sent to detection.
			 */
			if (algo->bliir_use_pre_cmf_raw)
				delta = (s32)le16_to_cpup(bliir_raw + idx) - baseline;
			/* BLIIR_DoUpdate is deliberately independent of SafeBaseline:
			 * it moves the working grid by exactly one bounded step.  Safe
			 * history judges or resets a bad working grid afterwards; it does
			 * not clamp every BLIIR cell update.
			 */
			algo->baseline_q8[idx] = hx_bliir_update_cell_q8(
				algo->baseline_q8[idx],
				le16_to_cpup(bliir_raw + idx),
				algo->baseline_stage_update_step > 0 ?
				algo->baseline_stage_update_step :
				algo->baseline_no_finger_max_step);
			/* Match v1.1.2 ProcessNoFinger/ProcessFinger background
			 * semantics: only frozen candidate cells reach the solver.
			 * Passing every local residual fills the fixed zone arena with
			 * background islands and can evict a real finger by scan order.
			 */
			delta = 0;
store:
			algo->frame[r][c] = clamp_t(s32, delta, SHRT_MIN, SHRT_MAX);
		}
	}
	/* BLSM_SaveNormalBl keeps a separate no-touch reference.  It is updated
	 * only after the stage-0 action has accepted the frame, so a touch/noise
	 * frame can never become the normal baseline authority.
	 */
	if (algo->baseline_stage == HX_BLSM_NO_TOUCH_STABLE &&
		spatial_update_allowed && !has_signal && operational_clean &&
		!algo->baseline_hw_reset && !algo->baseline_touch_latched) {
		memcpy(algo->normal_baseline_q8, algo->baseline_q8,
		       sizeof(algo->normal_baseline_q8));
		algo->normal_baseline_valid = true;
	}
	/* The wake qualification frame is still sent through detection/tracking,
	 * but cannot become a second baseline-learning sample.
	 */
	algo->baseline_update_suppressed_once = false;
	algo->baseline_stage_reset = false;
	algo->baseline_prev_had_signal = has_signal;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	algo->diag_has_signal = has_signal;
#endif
	algo->baseline_had_freeze = found_freeze;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	if (spatial_update_allowed)
		algo->baseline_spatial_update_count++;
	else
		algo->baseline_spatial_block_count++;
#endif
	/* Safe history must never be collected while a safe-backed wake guard is
	 * active.  A stationary palm can look electrically clean and would
	 * otherwise gain false confidence.  Also skip the exact release frame;
	 * cell release-hold protects its rebound before a new candidate starts.
	 */
	if (algo->baseline_touch_hold || touch_hold_released) {
		if (has_signal || finger_state == HX_FINGER_PRESENT)
			algo->safe_candidate_armed = true;
		hx_clean_baseline_reset(algo);
	} else if (safe_frame_clean && finger_state == HX_FINGER_ABSENT &&
		   !has_signal && !found_freeze)
		hx_clean_baseline_process(algo, raw);
	else {
		if (!safe_frame_clean && (algo->safe_no_finger_frames ||
		    algo->safe_candidate_confirming))
			hx_clean_baseline_record_reject(algo,
				observation.safe_negative_nodes >
					HX_BASELINE_CLEAN_MAX_NODES ?
				HX_CLEAN_BASELINE_NEGATIVE :
				HX_CLEAN_BASELINE_SPATIAL);
		if (!safe_frame_clean || has_signal ||
		    finger_state == HX_FINGER_PRESENT)
			algo->safe_candidate_armed = true;
		hx_clean_baseline_reset(algo);
	}

	/* pixel [0][0] is always invalid on this panel layout */
	algo->frame[0][0] = 0;
}

/* ======================================================================== */
/* Phase 1A½ — edge signal boost                                            */
/*                                                                           */
/* Compensate reduced capacitive sensitivity at sensor borders by scaling   */
/* border pixels upward.  Row 0/last and col 0/last get the full boost;    */
/* row 1/last-1 and col 1/last-1 get half.  Corner pixels (on two borders) */
/* are boosted once from each axis (multiplicative).                         */
/* ======================================================================== */

static void hx_edge_boost(struct hx_algo *algo)
{
	int r, c;
	s32 pct = algo->edge_boost_pct;
	s32 half_pct = pct / 2;

	if (!algo->edge_comp_enabled || pct <= 0)
		return;

	/* Boost border rows: row 0 and row HX_ROWS-1 (full), row 1 and HX_ROWS-2 (half) */
	for (c = 0; c < HX_COLS; c++) {
		s32 v;

		/* Top edge */
		v = algo->frame[0][c];
		if (v > 0)
			algo->frame[0][c] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[1][c];
		if (v > 0)
			algo->frame[1][c] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);

		/* Bottom edge */
		v = algo->frame[HX_ROWS - 1][c];
		if (v > 0)
			algo->frame[HX_ROWS - 1][c] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[HX_ROWS - 2][c];
		if (v > 0)
			algo->frame[HX_ROWS - 2][c] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);
	}

	/* Boost border columns: col 0 and col HX_COLS-1 (full), col 1 and HX_COLS-2 (half) */
	for (r = 0; r < HX_ROWS; r++) {
		s32 v;

		/* Left edge */
		v = algo->frame[r][0];
		if (v > 0)
			algo->frame[r][0] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[r][1];
		if (v > 0)
			algo->frame[r][1] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);

		/* Right edge */
		v = algo->frame[r][HX_COLS - 1];
		if (v > 0)
			algo->frame[r][HX_COLS - 1] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[r][HX_COLS - 2];
		if (v > 0)
			algo->frame[r][HX_COLS - 2] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);
	}
}

/* ======================================================================== */
/* Phase 1B — CMF (Common Mode Filter)                                      */
/*                                                                           */
/* Removes charger-induced common-mode noise by subtracting per-row and     */
/* per-column offsets computed from "quiet" pixels (|val| < exclusion).     */
/* DualDim mode: rows first, then columns.                                   */
/* ======================================================================== */

static void hx_apply_cmf(struct hx_algo *algo)
{
	int r, c;

	/* Row pass */
	for (r = 0; r < HX_ROWS; r++) {
		s32 sum = 0, count = 0, offset;

		for (c = 0; c < HX_COLS; c++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (c = 0; c < HX_COLS; c++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}

	/* Column pass */
	for (c = 0; c < HX_COLS; c++) {
		s32 sum = 0, count = 0, offset;

		for (r = 0; r < HX_ROWS; r++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (r = 0; r < HX_ROWS; r++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}
}

/* ======================================================================== */
/* Phase 1C — GridIIR temporal filter                                       */
/*                                                                           */
/* Per-pixel exponential decay for noise suppression.  Pixels above a       */
/* dynamic threshold (proportional to the frame maximum) bypass the filter  */
/* so real touch signals are never attenuated.                               */
/* ======================================================================== */

static void hx_apply_iir(struct hx_algo *algo)
{
	int r, c;
	s32 frame_max = 0;
	s32 dyn_threshold;
	u16 decay_weight, decay_step;

	if (!algo->iir_enabled) {
		/* Do not maintain an unused 2400-cell history at frame rate.  Marking
		 * it invalid makes a later runtime enable seed from its first frame.
		 */
		algo->iir_initialized = false;
		return;
	}

	if (!algo->iir_initialized) {
		memcpy(algo->iir_history, algo->frame, sizeof(algo->frame));
		algo->iir_initialized = true;
		return;
	}

	for (r = 0; r < HX_ROWS; r++)
		for (c = 0; c < HX_COLS; c++)
			frame_max = max(frame_max, abs((int)algo->frame[r][c]));

	dyn_threshold = max((frame_max * algo->iir_gate_ratio_q8) >> 8,
			    (s32)algo->iir_gate_floor);
	decay_weight  = min_t(u16, algo->iir_decay_weight, 256);
	decay_step    = algo->iir_decay_step;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			s32 cur = algo->frame[r][c];
			s32 output;

			if (cur >= dyn_threshold) {
				output = cur;
			} else {
				s32 hist  = algo->iir_history[r][c];
				s32 mixed = decay_weight * cur +
					    (256 - decay_weight) * hist;

				output = mixed >> 8;
				output = max(0, output - (s32)decay_step);
				if (output < algo->iir_noise_floor)
					output = 0;
			}

			algo->frame[r][c]       = clamp_t(s32, output, SHRT_MIN, SHRT_MAX);
			algo->iir_history[r][c] = algo->frame[r][c];
		}
	}
}

/* ======================================================================== */
/* Phase 1 entry point                                                       */
/* ======================================================================== */

#ifdef HX_ALGO_HOST_TEST
void hx_preprocess_frame(struct hx_algo *algo, const u16 *raw)
{
	hx_preprocess_frame_state(algo, raw, HX_FINGER_UNKNOWN);
}
#endif

void hx_preprocess_frame_state(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state)
{
	memcpy(algo->pre_cmf_raw, raw, sizeof(algo->pre_cmf_raw));
	hx_prepare_frame_baseline(algo,
		algo->bliir_use_pre_cmf_raw ? algo->pre_cmf_raw : raw,
		finger_state);

	if (algo->cmf_enabled)
		hx_apply_cmf(algo);

	hx_edge_boost(algo);

	hx_apply_iir(algo);
	/* Commit raw history only after all classifiers have consumed the previous
	 * frame.  A partial/SPI-failed frame never reaches this function.
	 */
	hx_blsm_shb_process(algo, algo->pre_cmf_raw,
		algo->baseline_stage == HX_BLSM_RESET);
	memcpy(algo->prev_raw, raw, sizeof(algo->prev_raw));
	algo->prev_raw_valid = true;
}

/*
 * Clamp-to-zero accessor: returns 0 for out-of-bounds or negative values so
 * neighbour lookups near the grid edge never need special-casing.
 */
