// SPDX-License-Identifier: GPL-2.0
/* Current-baseline judge built from SafeBaseline_GetPrpt aggregates. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

static bool hx_safe_prpt_abnormal(const struct hx_safe_prpt *prpt,
						 s32 threshold)
{
	if (!prpt->count)
		return true;
	return prpt->max > threshold || prpt->min < -threshold;
}

static u32 hx_safe_prpt_score(const struct hx_safe_prpt *prpt,
						 s32 threshold)
{
	u32 score = 0;

	if (prpt->max > threshold)
		score += (u32)(prpt->max - threshold);
	if (prpt->min < -threshold)
		score += (u32)(-threshold - prpt->min);
	return score;
}

enum hx_safe_compare_result
hx_safe_baseline_judge_current(const struct hx_algo *algo)
{
	const struct hx_safe_prpt *working = &algo->safe_bl2bl_prpt_cmf[2];
	const struct hx_safe_prpt *raw = &algo->safe_bl2raw_prpt_cmf[2];
	s32 threshold = algo->baseline_peak_threshold;
	bool working_bad;
	bool raw_bad;
	u32 working_score;
	u32 raw_score;
	u16 working_bad_nodes = 0, raw_bad_nodes = 0;
	u16 safe_explained_nodes = 0;
	u8 safe_better_regions = 0, working_better_regions = 0;
	int i;

	if (!algo->safe_baseline_valid || !algo->safe_baseline_count)
		return HX_SAFE_COMPARE_BOTH_VALID;
	/* SafeBaseline_IsAbnormalSensor compares the same sensor in BL2BL and
	 * BL2RAW buffers.  Count that per-cell evidence in addition to the PRPT
	 * extrema; this catches a distributed failure whose max/min remains within
	 * the extrema margin. */
	/* Include the first cell as well.  The vendor compares the complete
	 * baseline grid; skipping index zero can hide a single-cell corruption
	 * (and is especially damaging when that cell is an edge node). */
	for (i = 0; i < HX_PIXELS; i++) {
		s32 working_abs = abs((s32)algo->safe_bl2bl_dif_cmf[i]);
		s32 raw_abs = abs((s32)algo->safe_bl2raw_dif_cmf[i]);

		if (working_abs > threshold)
			working_bad_nodes++;
		if (raw_abs > threshold)
			raw_bad_nodes++;
		/* SafeBaseline_IsAbnormalSensor itself compares the absolute
		 * BL2RAW and BL2BL residuals; the caller has already selected a
		 * meaningful peak/sensor.  Use a half-threshold floor here only to
		 * keep insignificant quantisation noise from dominating the Linux
		 * full-grid approximation. */
		if (raw_abs < working_abs && working_abs > threshold / 2)
			safe_explained_nodes++;
	}
	working_bad = hx_safe_prpt_abnormal(working, threshold);
	raw_bad = hx_safe_prpt_abnormal(raw, threshold);
	if (!working_bad && raw_bad)
		return HX_SAFE_COMPARE_WORKING_BETTER;
	if (working_bad && !raw_bad)
		return HX_SAFE_COMPARE_SAFE_BETTER;
	working_score = hx_safe_prpt_score(working, threshold);
	raw_score = hx_safe_prpt_score(raw, threshold);
	if (working_bad_nodes || raw_bad_nodes) {
		working_score += working_bad_nodes;
		raw_score += raw_bad_nodes;
		if (safe_explained_nodes >= 4 &&
		    raw_bad_nodes + 4 <= working_bad_nodes)
			return HX_SAFE_COMPARE_SAFE_BETTER;
	}
	/* A distributed cell-level failure may leave both aggregate extrema below
	 * the scalar threshold.  Do not let that early-return as BOTH_VALID: the
	 * complete-grid evidence above must still select the safer baseline. */
	if (!working_bad && !raw_bad) {
		if (working_bad_nodes > raw_bad_nodes + 3)
			return HX_SAFE_COMPARE_SAFE_BETTER;
		if (raw_bad_nodes > working_bad_nodes + 3)
			return HX_SAFE_COMPARE_WORKING_BETTER;
		return HX_SAFE_COMPARE_BOTH_VALID;
	}
	/* The vendor does not judge only the all-panel maximum.  It also checks
	 * center/inner/edge aggregates and rejects a replacement when one region
	 * is catastrophically worse.  Preserve that spatial distinction when the
	 * scalar score is inconclusive. */
	for (i = 0; i < 5; i++) {
		const struct hx_safe_prpt *w = &algo->safe_bl2bl_prpt_cmf[i];
		const struct hx_safe_prpt *r = &algo->safe_bl2raw_prpt_cmf[i];
		u32 ws = hx_safe_prpt_score(w, threshold);
		u32 rs = hx_safe_prpt_score(r, threshold);

		if (ws > rs + threshold)
			safe_better_regions++;
		else if (rs > ws + threshold)
			working_better_regions++;
	}
	if (safe_better_regions >= 2 &&
	    safe_better_regions > working_better_regions)
		return HX_SAFE_COMPARE_SAFE_BETTER;
	if (working_better_regions >= 2 &&
	    working_better_regions > safe_better_regions)
		return HX_SAFE_COMPARE_WORKING_BETTER;
	if (raw_score + 1 < working_score)
		return HX_SAFE_COMPARE_SAFE_BETTER;
	if (working_score + 1 < raw_score)
		return HX_SAFE_COMPARE_WORKING_BETTER;
	return HX_SAFE_COMPARE_BOTH_INVALID;
}
