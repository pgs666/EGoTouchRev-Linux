/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_ALGO_INTERNAL_H
#define HX_ALGO_INTERNAL_H

#include "hx-algo.h"

struct hx_baseline_frame_observation {
	s16 max_signal;
	s16 min_signal;
	u16 safe_bad_nodes;
	u16 safe_negative_nodes;
	u16 operational_bad_nodes;
	u16 operational_negative_nodes;
	u16 out_of_range;
	u16 line_noise;
};

void hx_baseline_observe_frame(struct hx_algo *algo, const u16 *raw,
			       s32 common_diff,
			       struct hx_baseline_frame_observation *obs);

void hx_copy_raw_to_baseline(s32 *baseline, const u16 *raw);
void hx_safe_baseline_bootstrap(struct hx_algo *algo, const s32 *baseline_q8);
void hx_safe_baseline_commit(struct hx_algo *algo, const s32 *baseline_q8);
void hx_safe_baseline_select_for_raw(struct hx_algo *algo, const u16 *raw);
bool hx_safe_baseline_promote_latest_confirmed(struct hx_algo *algo);
bool hx_safe_baseline_can_recover(const struct hx_algo *algo);
void hx_restore_working_from_safe(struct hx_algo *algo, s32 common,
				  bool touch_hold, bool touch_seen);
bool hx_baseline_guard_process(struct hx_algo *algo, bool frame_clean,
				       bool touch_evidence);
enum hx_baseline_stage hx_baseline_stage_update(struct hx_algo *algo,
						u16 property, s16 max_signal,
						s16 min_signal, bool single_touch);
void hx_baseline_stage_process(struct hx_algo *algo, const u16 *raw,
				       const u16 *pre_cmf_raw, s16 max_signal,
				       s16 min_signal, bool has_signal,
			       bool operational_clean);
s32 hx_bliir_update_cell_q8(s32 baseline_q8, u16 raw_value, s16 step);
u16 hx_blreset_classify_frame(struct hx_algo *algo, const u16 *raw,
			      s32 common_diff, s16 max_signal, s16 min_signal,
			      bool has_signal, bool line_noise, bool out_of_range);
u16 hx_blreset_abnormal_types(struct hx_algo *algo, s16 max_signal,
			      s16 min_signal, u16 abnormal_nodes,
			      u16 negative_nodes, bool has_signal,
			      bool touch_protected, bool game_scenario);
bool hx_blreset_process(struct hx_algo *algo, u16 reason_mask);
void hx_blreset_baseline_state_update(struct hx_algo *algo,
				      bool force_reset, bool touch_protected,
				      u16 activity_nodes);
bool hx_blreset_wakeup_process(struct hx_algo *algo, bool abnormal);
bool hx_blreset_dirty_process(struct hx_algo *algo, bool over_noise,
			      bool touch_protected, bool diff_dirty,
			      bool clean_matches);
bool hx_blreset_check_abnormal_touches(struct hx_algo *algo);
bool hx_blreset_is_dirty_baseline(const struct hx_algo *algo,
				  s16 max_signal, s16 min_signal,
				  s32 common_diff);
bool hx_blreset_check_clean_baseline(const struct hx_algo *algo,
				     const u16 *raw);
void hx_blsm_shb_process(struct hx_algo *algo, const u16 *pre_cmf_raw,
			 bool reset_stage);
u16 hx_blsm_shb_consume_action(struct hx_algo *algo);
bool hx_blrecal_process(struct hx_algo *algo, const u16 *raw,
			 s16 max_signal, s16 min_signal, bool reset_event,
			 bool auto_calibration);
void hx_blrecal_ack(struct hx_algo *algo, bool success);
u16 hx_baseline_get_property(struct hx_algo *algo, s16 max_signal,
			     bool has_signal, bool operational_clean,
			     bool blrecal_request, bool shb_noisy_action,
			     bool line_noise, bool out_of_range);
bool hx_safe_baseline_check_ghost(const struct hx_algo *algo,
					 const u16 *raw, const s32 *reference_q8,
					 s32 common, u16 *ghost_nodes);
bool hx_safe_baseline_check_side_touch(const struct hx_algo *algo,
					       const u16 *raw,
					       const s32 *reference_q8,
					       s32 common);
bool hx_safe_baseline_check_side_very_negative(const struct hx_algo *algo,
						       const u16 *raw,
						       const s32 *reference_q8,
						       s32 common);
u16 hx_safe_baseline_signal_flags(const struct hx_algo *algo,
					  s16 max_signal, s16 min_signal,
					  bool has_valid_touch);
void hx_safe_baseline_reset_selected(struct hx_algo *algo);
void hx_safe_baseline_queue_reset(struct hx_algo *algo);
void hx_safe_baseline_queue_record(struct hx_algo *algo, u8 slot);
void hx_safe_baseline_queue_remove(struct hx_algo *algo, u8 slot);
u8 hx_safe_baseline_queue_oldest(const struct hx_algo *algo);
u8 hx_safe_baseline_queue_latest(const struct hx_algo *algo);
bool hx_safe_baseline_queue_trustable(const struct hx_algo *algo);
bool hx_safe_baseline_queue_double_checked(const struct hx_algo *algo);
bool hx_safe_baseline_queue_all_valid(const struct hx_algo *algo);
void hx_safe_baseline_temp_reset(struct hx_algo *algo);
bool hx_safe_baseline_temp_observe(struct hx_algo *algo,
				   const s32 *baseline_q8);
bool hx_safe_baseline_is_ok_to_update(const struct hx_algo *algo);
bool hx_safe_baseline_raw_matches_selected(const struct hx_algo *algo,
						 const u16 *raw);
void hx_safe_baseline_buffer_comparison(struct hx_algo *algo,
						 const u16 *raw);
void hx_safe_baseline_collect_prpt(struct hx_safe_prpt prpt[5],
						 const s16 *dif);
enum hx_safe_compare_result
hx_safe_baseline_judge_current(const struct hx_algo *algo);
void hx_safe_baseline_replace_working_from_history(struct hx_algo *algo,
							 s32 common);
bool hx_safe_baseline_should_replace_wake(const struct hx_algo *algo);
enum hx_safe_reset_action
hx_safe_baseline_reset_policy(struct hx_algo *algo,
			      enum hx_safe_compare_result result,
			      bool side_abnormal,
			      enum hx_finger_state finger_state);
void hx_safe_baseline_check_with_state(struct hx_algo *algo,
			       enum hx_finger_state finger_state,
			       bool safe_diff_checked, bool very_noisy,
			       bool very_negative, bool ghost_max,
			       bool sensor_bad);
bool hx_safe_baseline_reset_statistics(struct hx_algo *algo,
						 u16 reason_mask, bool safe_raw_checked);
bool hx_safe_baseline_reset_statistics_variant(struct hx_algo *algo,
						 u16 reason_mask, bool safe_raw_checked,
						 bool side_variant);
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
