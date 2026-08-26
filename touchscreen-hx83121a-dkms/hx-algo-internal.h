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
bool hx_baseline_guard_process(struct hx_algo *algo, bool frame_clean,
				       bool touch_evidence);
enum hx_baseline_stage hx_baseline_stage_update(struct hx_algo *algo,
						bool touch, bool mild_noise,
						bool severe_noise, bool special);
bool hx_safe_baseline_check_ghost(const struct hx_algo *algo,
					 const u16 *raw, const s32 *reference_q8,
					 s32 common, u16 *ghost_nodes);
bool hx_safe_baseline_check_side_touch(const struct hx_algo *algo,
					       const u16 *raw,
					       const s32 *reference_q8,
					       s32 common);
void hx_safe_baseline_reset_selected(struct hx_algo *algo);
bool hx_safe_baseline_raw_matches_selected(const struct hx_algo *algo,
						 const u16 *raw);
void hx_safe_baseline_buffer_comparison(struct hx_algo *algo,
						 const u16 *raw);
void hx_safe_baseline_replace_working_from_history(struct hx_algo *algo,
							 s32 common);
enum hx_safe_reset_action
hx_safe_baseline_reset_policy(struct hx_algo *algo,
			      enum hx_safe_compare_result result,
			      bool side_abnormal,
			      enum hx_finger_state finger_state);
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
