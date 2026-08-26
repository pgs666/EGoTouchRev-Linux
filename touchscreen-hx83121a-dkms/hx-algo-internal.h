/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_ALGO_INTERNAL_H
#define HX_ALGO_INTERNAL_H

#include "hx-algo.h"

void hx_copy_raw_to_baseline(s32 *baseline, const u16 *raw);
void hx_safe_baseline_bootstrap(struct hx_algo *algo, const s32 *baseline_q8);
void hx_safe_baseline_commit(struct hx_algo *algo, const s32 *baseline_q8);
void hx_safe_baseline_select_for_raw(struct hx_algo *algo, const u16 *raw);
bool hx_safe_baseline_can_recover(const struct hx_algo *algo);
void hx_restore_working_from_safe(struct hx_algo *algo, s32 common,
				  bool touch_hold, bool touch_seen);
s64 hx_dist2_predicted(const struct input_mt_pos *position,
			const struct hx_track *track);

static inline s16 hx_frame_at(const struct hx_algo *algo, int row, int col)
{
	s16 value;

	if (row < 0 || row >= HX_ROWS || col < 0 || col >= HX_COLS)
		return 0;
	value = algo->frame[row][col];
	return value > 0 ? value : 0;
}

#endif /* HX_ALGO_INTERNAL_H */
