// SPDX-License-Identifier: GPL-2.0
/* Sysfs controls and optional diagnostic tracing for HX83121A. */

#include <linux/slab.h>
#include <linux/sysfs.h>
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
#include <linux/timekeeping.h>
#endif

#include "himax-spi.h"

static ssize_t inplace_reset_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	bool do_reset;
	int ret;

	ret = kstrtobool(buf, &do_reset);
	if (ret)
		return ret;

	if (!do_reset)
		return count;

	ret = himax_manual_reset(ts);
	if (ret)
		return ret;

	return count;
}

static DEVICE_ATTR_WO(inplace_reset);

static ssize_t baseline_full_reset_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	bool do_reset;
	int ret;

	ret = kstrtobool(buf, &do_reset);
	if (ret)
		return ret;
	if (!do_reset)
		return count;

	ret = himax_manual_full_baseline_reset(ts);
	if (ret)
		return ret;
	dev_info(ts->dev, "manual full baseline reset completed\n");
	return count;
}

static DEVICE_ATTR_WO(baseline_full_reset);

/* ---- sysfs: algo parameter group ---- */

#define HX_ALGO_ATTR_BOOL_RW(_name)					\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%d\n", ts->algo->_name ? 1 : 0);	\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	bool val;							\
	int ret = kstrtobool(buf, &val);				\
	if (ret)							\
		return ret;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_S16_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%d\n", (int)ts->algo->_name);		\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	s16 val;							\
	int ret = kstrtos16(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_U16_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%u\n", (unsigned int)ts->algo->_name);	\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	u16 val;							\
	int ret = kstrtou16(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_U8_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%u\n", (unsigned int)ts->algo->_name);	\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	u8 val;							\
	int ret = kstrtou8(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

#define HX_ALGO_ATTR_S32_RW(_name, _min, _max)				\
static ssize_t _name##_show(struct device *dev,				\
			    struct device_attribute *attr, char *buf)	\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	return sysfs_emit(buf, "%d\n", ts->algo->_name);		\
}									\
static ssize_t _name##_store(struct device *dev,			\
			     struct device_attribute *attr,		\
			     const char *buf, size_t count)		\
{									\
	struct himax_ts_data *ts = dev_get_drvdata(dev);		\
	s32 val;							\
	int ret = kstrtos32(buf, 0, &val);				\
	if (ret)							\
		return ret;						\
	if (val < (_min) || val > (_max))				\
		return -ERANGE;						\
	mutex_lock(&ts->op_lock);				\
	ts->algo->_name = val;						\
	mutex_unlock(&ts->op_lock);				\
	return count;							\
}									\
static DEVICE_ATTR_RW(_name)

/* Preprocessing */
HX_ALGO_ATTR_BOOL_RW(baseline_enabled);
HX_ALGO_ATTR_BOOL_RW(safe_baseline_replace_enabled);
HX_ALGO_ATTR_U16_RW(frame_interval_ms, 1, 1000);
HX_ALGO_ATTR_S16_RW(baseline_noise_deadband, 0, 200);
HX_ALGO_ATTR_S16_RW(baseline_peak_threshold, 1, 2000);
HX_ALGO_ATTR_U8_RW(baseline_release_hold_frames, 0, 255);
HX_ALGO_ATTR_U8_RW(baseline_background_alpha_shift, 0, 15);
HX_ALGO_ATTR_U8_RW(baseline_no_finger_alpha_shift, 0, 15);
HX_ALGO_ATTR_U8_RW(baseline_recovery_alpha_shift, 0, 15);
HX_ALGO_ATTR_S16_RW(baseline_background_max_step, 1, 2048);
HX_ALGO_ATTR_S16_RW(baseline_no_finger_max_step, 1, 2048);
HX_ALGO_ATTR_S16_RW(baseline_recovery_max_step, 1, 2048);
HX_ALGO_ATTR_U8_RW(baseline_recovery_max_frames, 1, 120);
HX_ALGO_ATTR_BOOL_RW(baseline_noise_tracking);
HX_ALGO_ATTR_U8_RW(wake_stable_frames, 2, 30);
HX_ALGO_ATTR_U8_RW(wake_finger_safe_frames, 1, 15);
HX_ALGO_ATTR_S16_RW(wake_raw_jump_threshold, 1, 4095);
HX_ALGO_ATTR_U16_RW(wake_max_unstable_nodes, 0, HX_PIXELS);
HX_ALGO_ATTR_U8_RW(wake_max_unstable_line_nodes, 1, HX_ROWS);
HX_ALGO_ATTR_U8_RW(safe_commit_no_finger_frames, 1, 240);
HX_ALGO_ATTR_BOOL_RW(runtime_blreset_enabled);
HX_ALGO_ATTR_U8_RW(runtime_blreset_confirm_frames, 1, 30);
HX_ALGO_ATTR_U16_RW(runtime_blreset_cooldown, 0, 3600);
HX_ALGO_ATTR_S16_RW(runtime_noise_threshold, 1, 8192);
HX_ALGO_ATTR_U8_RW(runtime_noise_line_nodes, 1, HX_COLS);
HX_ALGO_ATTR_U16_RW(runtime_noise_total_nodes, 1, HX_PIXELS);
HX_ALGO_ATTR_BOOL_RW(cmf_enabled);
HX_ALGO_ATTR_S16_RW(cmf_exclusion, 0, 32767);
HX_ALGO_ATTR_S16_RW(cmf_max_correction, 0, 32767);
HX_ALGO_ATTR_BOOL_RW(iir_enabled);
HX_ALGO_ATTR_U16_RW(iir_decay_weight, 0, 256);
HX_ALGO_ATTR_U16_RW(iir_decay_step, 0, 4095);
HX_ALGO_ATTR_S16_RW(iir_noise_floor, 0, 4095);
HX_ALGO_ATTR_S16_RW(iir_gate_floor, 0, 4095);
HX_ALGO_ATTR_U8_RW(iir_gate_ratio_q8, 0, 255);
/* Detection */
HX_ALGO_ATTR_S16_RW(macro_threshold, 1, 4095);
HX_ALGO_ATTR_S16_RW(peak_threshold, 1, 4095);
HX_ALGO_ATTR_U8_RW(peak_local_radius, 1, 5);
HX_ALGO_ATTR_BOOL_RW(peak_z8_enabled);
HX_ALGO_ATTR_BOOL_RW(peak_saddle_enabled);
HX_ALGO_ATTR_U8_RW(peak_saddle_radius, 1, 8);
HX_ALGO_ATTR_S16_RW(peak_saddle_drop, 0, 4095);
HX_ALGO_ATTR_S16_RW(peak_signal_threshold_limit, 1, 4095);
HX_ALGO_ATTR_S16_RW(peak_edge_threshold, 0, 4095);
HX_ALGO_ATTR_U8_RW(peak_macro_min_area, 1, 64);
HX_ALGO_ATTR_U8_RW(peak_continue_min_area, 1, 2);
HX_ALGO_ATTR_S16_RW(peak_continue_min_signal, 1, 4095);
HX_ALGO_ATTR_S16_RW(peak_single_track_continue_min_signal, 1, 4095);
HX_ALGO_ATTR_S32_RW(peak_continue_dist2, 1, 1000000);
HX_ALGO_ATTR_S16_RW(peak_fast_start_min_signal, 1, 4095);
HX_ALGO_ATTR_U8_RW(peak_fast_start_edge_cells, 1, 16);
HX_ALGO_ATTR_BOOL_RW(palm_enabled);
HX_ALGO_ATTR_U8_RW(palm_area_threshold, 0, 250);
HX_ALGO_ATTR_S32_RW(palm_signal_threshold, 0, 1000000);
HX_ALGO_ATTR_S16_RW(palm_density_low, 0, 4095);
HX_ALGO_ATTR_BOOL_RW(palm_box_enabled);
HX_ALGO_ATTR_U8_RW(palm_box_expand_rows, 0, 10);
HX_ALGO_ATTR_U8_RW(palm_box_expand_cols, 0, 10);
HX_ALGO_ATTR_U8_RW(palm_box_match_distance, 0, 30);
HX_ALGO_ATTR_U16_RW(palm_box_max_hold, 0, 300);
HX_ALGO_ATTR_BOOL_RW(zone_cleanup_enabled);
HX_ALGO_ATTR_U8_RW(zone_max_radius, 0, 16);
HX_ALGO_ATTR_U8_RW(zone_threshold_numer, 0, 255);
HX_ALGO_ATTR_U8_RW(zone_threshold_shift, 0, 15);
/* Pressure / touch-major reporting */
HX_ALGO_ATTR_BOOL_RW(pressure_enabled);
/* Edge compensation */
HX_ALGO_ATTR_BOOL_RW(edge_comp_enabled);
HX_ALGO_ATTR_S16_RW(edge_boost_pct, 0, 200);
HX_ALGO_ATTR_S16_RW(edge_push_q8, 0, 1280);
HX_ALGO_ATTR_S16_RW(edge_blend_q8, 1, 1280);
HX_ALGO_ATTR_BOOL_RW(edge_reject_enabled);
HX_ALGO_ATTR_U16_RW(edge_reject_margin, 0, 256);
HX_ALGO_ATTR_S32_RW(edge_reject_min_signal, 0, 1000000);
/* Tracking */
HX_ALGO_ATTR_S32_RW(track_dist2_max, 1, 16777216);
HX_ALGO_ATTR_U8_RW(track_lost_frames, 1, 16);
HX_ALGO_ATTR_U8_RW(debounce_base, 0, 16);
HX_ALGO_ATTR_BOOL_RW(track_smoothing);
HX_ALGO_ATTR_BOOL_RW(track_active_guard);
HX_ALGO_ATTR_U8_RW(track_start_debounce, 0, 16);
HX_ALGO_ATTR_S32_RW(track_jump_dist2, 0, 16777216);
HX_ALGO_ATTR_BOOL_RW(hungarian_enabled);
HX_ALGO_ATTR_U8_RW(debounce_weak_extra, 0, 16);
HX_ALGO_ATTR_U8_RW(debounce_edge_extra, 0, 16);
HX_ALGO_ATTR_S32_RW(debounce_strong_signal, 0, 1000000);
HX_ALGO_ATTR_BOOL_RW(firmware_edge_fast_start);
HX_ALGO_ATTR_U8_RW(split_peak_confirm_frames, 1, 16);
HX_ALGO_ATTR_S32_RW(split_peak_dist2, 1, 1000000);
HX_ALGO_ATTR_U8_RW(split_cross_zone_confirm_frames, 1, 16);
HX_ALGO_ATTR_S32_RW(split_cross_zone_dist2, 1, 1000000);
HX_ALGO_ATTR_S32_RW(track_peak_id_penalty, 0, 16777216);
HX_ALGO_ATTR_BOOL_RW(ghost_enabled);
HX_ALGO_ATTR_U16_RW(ghost_row_distance, 0, 512);
HX_ALGO_ATTR_U8_RW(ghost_weak_ratio_q8, 0, 255);
HX_ALGO_ATTR_U16_RW(ghost_min_col_distance, 0, 4096);
HX_ALGO_ATTR_BOOL_RW(euro_enabled);
HX_ALGO_ATTR_U8_RW(euro_alpha_min_q8, 1, 255);
HX_ALGO_ATTR_U8_RW(euro_alpha_max_q8, 1, 255);
HX_ALGO_ATTR_U16_RW(euro_speed_threshold, 1, 4096);

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
static const char *hx_safe_compare_result_name(enum hx_safe_compare_result result)
{
	switch (result) {
	case HX_SAFE_COMPARE_WORKING_BETTER:
		return "WORKING_BETTER";
	case HX_SAFE_COMPARE_SAFE_BETTER:
		return "SAFE_BETTER";
	case HX_SAFE_COMPARE_BOTH_VALID:
		return "BOTH_VALID";
	case HX_SAFE_COMPARE_BOTH_INVALID:
		return "BOTH_INVALID";
	case HX_SAFE_COMPARE_AMBIGUOUS:
		return "AMBIGUOUS";
	case HX_SAFE_COMPARE_NOT_RUN:
	default:
		return "NOT_RUN";
	}
}

static ssize_t diagnostics_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	struct hx_algo *a = ts->algo;
	u32 working_hash = 2166136261U;
	u32 safe_hash = 2166136261U;
	s64 baseline_common_sum = 0;
	s32 baseline_common;
	u16 baseline_divergent = 0;
	ssize_t len = 0;
	int i;

	mutex_lock(&ts->op_lock);
	for (i = 0; i < HX_PIXELS; i++) {
		working_hash = (working_hash ^ (u32)a->baseline_q8[i]) *
			16777619U;
		safe_hash = (safe_hash ^ (u32)a->safe_baseline_q8[i]) *
			16777619U;
		baseline_common_sum += (a->baseline_q8[i] -
			a->safe_baseline_q8[i]) >> HX_BASELINE_FRACTION_BITS;
	}
	baseline_common = (s32)(baseline_common_sum / HX_PIXELS);
	for (i = 0; i < HX_PIXELS; i++) {
		s32 local = ((a->baseline_q8[i] -
			a->safe_baseline_q8[i]) >> HX_BASELINE_FRACTION_BITS) -
			baseline_common;

		if (abs(local) > HX_BASELINE_CLEAN_LOCAL_THRESHOLD)
			baseline_divergent++;
	}
	len += sysfs_emit_at(buf, len, "frame=%u common=%d max=%d signal=%u ",
			     a->diag_frame_seq, a->diag_common_diff,
			     a->diag_frame_max, a->diag_has_signal);
	len += sysfs_emit_at(buf, len, "zones=%u peaks=%u contacts_pre=%u ",
			     a->diag_zones, a->diag_peaks,
			     a->diag_contacts_pre_filter);
	len += sysfs_emit_at(buf, len, "contacts_post=%u active=%u reported=%u ",
			     a->diag_contacts_post_filter,
			     a->diag_active_tracks, a->diag_reported_tracks);
	len += sysfs_emit_at(buf, len,
			     "baseline_generation=%u full_resets=%u live_clears=%u ",
			     a->baseline_generation, a->full_reset_count,
			     a->live_clear_count);
	len += sysfs_emit_at(buf, len,
			     "cold_inits=%u warm_resumes=%u warm_fallbacks=%u ",
			     ts->cold_init_count, ts->warm_resume_count,
			     ts->warm_resume_fallback_count);
	len += sysfs_emit_at(buf, len,
			     "resume_stability_failures=%u afe_calibrations=%u ",
			     ts->resume_stability_failures,
			     ts->afe_calibration_count);
	len += sysfs_emit_at(buf, len,
			     "afe_calibration_failures=%u full_resume=%u ",
			     ts->afe_calibration_failures,
			     himax_full_resume_enabled());
	len += sysfs_emit_at(buf, len,
			     "wake_qualifications=%u wake_candidate_rejects=%u ",
			     a->wake_qualification_count,
			     a->wake_candidate_reject_count);
	len += sysfs_emit_at(buf, len,
			     "wake_safe_fallbacks=%u wake_baseline_commits=%u ",
			     a->wake_safe_fallback_count,
			     a->wake_baseline_commit_count);
	len += sysfs_emit_at(buf, len,
			     "wake_safe_divergences=%u safe_baseline_commits=%u ",
			     a->wake_safe_divergence_count,
			     a->baseline_safe_commit_count);
	len += sysfs_emit_at(buf, len,
			     "wake_finger_mask_accepts=%u wake_finger_mask_rejects=%u ",
			     a->wake_finger_mask_accept_count,
			     a->wake_finger_mask_reject_count);
	len += sysfs_emit_at(buf, len,
			     "wake_finger_degraded_fallbacks=%u ",
			     a->wake_finger_degraded_fallback_count);
	len += sysfs_emit_at(buf, len,
			     "wake_raw_finger_inferred=%u wake_raw_finger_releases=%u ",
			     a->wake_raw_finger_inferred_count,
			     a->wake_raw_finger_release_count);
	len += sysfs_emit_at(buf, len,
			     "wake_ambiguous_fallbacks=%u wake_safe_preserved=%u ",
			     a->wake_ambiguous_safe_fallback_count,
			     a->wake_safe_preserved_count);
	len += sysfs_emit_at(buf, len, "wake_clean_safe_restores=%u ",
			     a->wake_clean_safe_restore_count);
	len += sysfs_emit_at(buf, len,
			     "safe_slot=%u safe_slot_score=%u safe_slot_switches=%u safe_slot_rejects=%u safe_replace_wake=%u post_reacquire_hold=%u ",
			     a->safe_baseline_selected,
			     a->safe_baseline_selected_score,
			     a->baseline_safe_slot_switch_count,
			     a->baseline_safe_slot_reject_count,
			     a->safe_baseline_replace_enabled,
			     a->baseline_post_reacquire_hold);
	len += sysfs_emit_at(buf, len,
			     "baseline_stage=%u stage_frames=%u frame_interval_ms=%u screen_on_hand_state=%u hw_reset=%u ",
			     a->baseline_stage, a->baseline_stage_frames,
			     a->frame_interval_ms,
			     a->baseline_screen_on_hand_state, a->baseline_hw_reset);
	len += sysfs_emit_at(buf, len, "normal_baseline_valid=%u ",
			     a->normal_baseline_valid);
	len += sysfs_emit_at(buf, len,
			     "blreset_raw_jump_frames=%u blreset_raw_jump_elapsed_ms=%u blreset_over_noise_frames=%u blrecal_frame=%u blrecal_abnormal=%u blrecal_requested=%u shb_state=%u shb_flags=0x%04x shb_capture_frame=%u ",
			     a->blreset_raw_jump_frames,
			     a->blreset_raw_jump_elapsed_ms,
			     a->blreset_over_noise_frames,
			     a->blrecal_frame,
			     a->blrecal_abnormal_count, a->blrecal_requested,
			     a->shb_state, a->shb_flags, a->shb_capture_frame);
	len += sysfs_emit_at(buf, len,
			     "blreset_state=%u blreset_reason=0x%04x blreset_elapsed_ms=%u blreset_triggers=%u blreset_clears=%u ",
			     a->blreset_state, a->blreset_reason_mask,
			     a->blreset_reason_elapsed_ms,
			     a->blreset_trigger_count, a->blreset_clear_count);
	len += sysfs_emit_at(buf, len,
			     "blreset_all_touch_abnormal=%u blreset_concurrent_touch=%u ",
			     a->blreset_all_touch_abnormal,
			     a->blreset_concurrent_touch);
	len += sysfs_emit_at(buf, len,
			     "blreset_baseline_state=%u blreset_baseline_stable=%u blreset_baseline_elapsed_ms=%u blreset_normal_ready=%u blreset_clean_captured=%u ",
			     a->blreset_baseline_state,
			     a->blreset_baseline_stable_frames,
			     a->blreset_baseline_elapsed_ms,
			     a->blreset_normal_baseline_ready,
			     a->blreset_clean_baseline_captured);
	len += sysfs_emit_at(buf, len,
			     "blreset_wake_abnormal_frames=%u blreset_wake_abnormal_elapsed_ms=%u blreset_wake_triggered=%u blreset_dirty_elapsed_ms=%u blreset_dirty_triggered=%u ",
			     a->blreset_wake_abnormal_frames,
			     a->blreset_wake_abnormal_elapsed_ms,
			     a->blreset_wake_triggered,
			     a->blreset_dirty_elapsed_ms,
			     a->blreset_dirty_triggered);
	len += sysfs_emit_at(buf, len,
			     "baseline_stage_action=%u stage_updates=%u stage_resets=%u stage_holds=%u stage_forced=%u ",
			     a->baseline_stage_action,
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
			     a->baseline_stage_update_count,
			     a->baseline_stage_reset_action_count,
			     a->baseline_stage_hold_count,
			     a->baseline_stage_force_count
#else
			     0U, 0U, 0U, 0U
#endif
			     );
	len += sysfs_emit_at(buf, len,
			     "wake_raw_override=%u wake_raw_release_frames=%u ",
			     a->wake_raw_finger_override,
			     a->wake_raw_finger_release_frames);
	len += sysfs_emit_at(buf, len,
			     "safe_confirms=%u safe_temporal_rejects=%u ",
			     a->baseline_safe_confirm_count,
			     a->baseline_safe_temporal_reject_count);
	len += sysfs_emit_at(buf, len,
			     "safe_spatial_rejects=%u safe_negative_rejects=%u ",
			     a->baseline_safe_spatial_reject_count,
			     a->baseline_safe_negative_reject_count);
	len += sysfs_emit_at(buf, len, "safe_range_rejects=%u ",
			     a->baseline_safe_range_reject_count);
	len += sysfs_emit_at(buf, len,
			     "safe_slots=%u safe_selected=%u safe_confidence=%u screen_epoch=%u ",
			     a->safe_baseline_count, a->safe_baseline_selected,
			     a->safe_baseline_count &&
			     a->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS ?
				a->safe_baselines[a->safe_baseline_selected].confidence : 0,
			     a->screen_epoch);
	len += sysfs_emit_at(buf, len,
			     "safe_flags=0x%04x safe_prev_flags=0x%04x queue_head=%u queue_tail=%u queue_count=%u queue_trustable=%u queue_all_valid=%u temp_queue_count=%u ",
			     a->safe_flags, a->safe_prev_flags, a->safe_queue_head,
			     a->safe_queue_tail, a->safe_queue_count,
			     hx_safe_baseline_queue_trustable(a),
			     hx_safe_baseline_queue_all_valid(a),
			     a->safe_temp_queue_count);
	len += sysfs_emit_at(buf, len,
			     "platform_idle=%u platform_charger_noise=%u platform_charger_connected=%u platform_proximity=%u platform_panel_sd=%u platform_raw_unified=%u platform_cover=%u ",
			     a->platform.idle_transition, a->platform.charger_noise,
			     a->platform.charger_connected,
			     a->platform.proximity_active, a->platform.panel_sd,
			     a->platform.raw_unified, a->platform.smart_cover);
	len += sysfs_emit_at(buf, len,
			     "safe_state=%u safe_reset=%u safe_stable_frames=%u safe_uses=%u ",
			     a->safe_baseline_count &&
			     a->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS ?
			     a->safe_baselines[a->safe_baseline_selected].state :
			     HX_SAFE_SLOT_EMPTY,
			     a->safe_baseline_count &&
			     a->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS ?
			     a->safe_baselines[a->safe_baseline_selected].reset_pending : 0,
			     a->safe_baseline_count &&
			     a->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS ?
			     a->safe_baselines[a->safe_baseline_selected].stable_frames : 0,
			     a->safe_baseline_count &&
			     a->safe_baseline_selected < HX_SAFE_BASELINE_SLOTS ?
			     a->safe_baselines[a->safe_baseline_selected].use_count : 0);
	len += sysfs_emit_at(buf, len,
			     "safe_deduplications=%u safe_evictions=%u safe_resets=%u safe_side_resets=%u runtime_blresets=%u runtime_blreset_suppressed=%u ",
			     a->baseline_safe_dedup_count,
			     a->baseline_safe_eviction_count,
			     a->baseline_safe_reset_count,
			     a->baseline_safe_side_reset_count,
			     a->runtime_blreset_count,
			     a->runtime_blreset_suppressed_count);
	len += sysfs_emit_at(buf, len,
			     "safe_abnormal_max=%u safe_abnormal_min=%u safe_current_positive=%u safe_current_negative=%u ",
			     a->baseline_safe_abnormal_max_count,
			     a->baseline_safe_abnormal_min_count,
			     a->safe_current_positive_nodes,
			     a->safe_current_negative_nodes);
	len += sysfs_emit_at(buf, len,
			     "safe_reset_debounce=%u safe_invalid_frames=%u safe_side_sync=%u safe_side_frames=%u safe_signal_stable=%u safe_valid_touches=%u safe_abnormal_touches=%u ",
			     a->safe_reset_in_debounce,
			     a->safe_baseline_invalid_frames,
			     a->safe_sync_reset_side_area,
			     a->safe_side_reset_frames,
			     a->safe_signal_stable_frames,
			     a->safe_valid_touch_count,
			     a->safe_abnormal_touch_count);
	len += sysfs_emit_at(buf, len,
			     "safe_reset_reason_mask=0x%04x safe_reset_reason_frames=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u ",
			     a->safe_reset_reason_mask,
			     a->safe_reset_reason_frames[0],
			     a->safe_reset_reason_frames[1],
			     a->safe_reset_reason_frames[2],
			     a->safe_reset_reason_frames[3],
			     a->safe_reset_reason_frames[4],
			     a->safe_reset_reason_frames[5],
			     a->safe_reset_reason_frames[6],
			     a->safe_reset_reason_frames[7],
			     a->safe_reset_reason_frames[8],
			     a->safe_reset_reason_frames[9]);
	len += sysfs_emit_at(buf, len,
			     "screen_on_frame=%u safe_reset_last_frames=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u ",
			     a->screen_on_frame_sequence,
			     a->safe_reset_reason_last_frame[0],
			     a->safe_reset_reason_last_frame[1],
			     a->safe_reset_reason_last_frame[2],
			     a->safe_reset_reason_last_frame[3],
			     a->safe_reset_reason_last_frame[4],
			     a->safe_reset_reason_last_frame[5],
			     a->safe_reset_reason_last_frame[6],
			     a->safe_reset_reason_last_frame[7],
			     a->safe_reset_reason_last_frame[8],
			     a->safe_reset_reason_last_frame[9]);
	len += sysfs_emit_at(buf, len,
			     "safe_reset_pushes=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u safe_reset_triggers=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u ",
			     a->safe_reset_push_count[0], a->safe_reset_push_count[1],
			     a->safe_reset_push_count[2], a->safe_reset_push_count[3],
			     a->safe_reset_push_count[4], a->safe_reset_push_count[5],
			     a->safe_reset_push_count[6], a->safe_reset_push_count[7],
			     a->safe_reset_push_count[8], a->safe_reset_push_count[9],
			     a->safe_reset_trigger_count[0], a->safe_reset_trigger_count[1],
			     a->safe_reset_trigger_count[2], a->safe_reset_trigger_count[3],
			     a->safe_reset_trigger_count[4], a->safe_reset_trigger_count[5],
			     a->safe_reset_trigger_count[6], a->safe_reset_trigger_count[7],
			     a->safe_reset_trigger_count[8], a->safe_reset_trigger_count[9]);
	len += sysfs_emit_at(buf, len,
			     "safe_reset_push_thresholds=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u safe_reset_time_thresholds=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u ",
			     a->safe_reset_push_threshold[0], a->safe_reset_push_threshold[1],
			     a->safe_reset_push_threshold[2], a->safe_reset_push_threshold[3],
			     a->safe_reset_push_threshold[4], a->safe_reset_push_threshold[5],
			     a->safe_reset_push_threshold[6], a->safe_reset_push_threshold[7],
			     a->safe_reset_push_threshold[8], a->safe_reset_push_threshold[9],
			     a->safe_reset_time_threshold[0], a->safe_reset_time_threshold[1],
			     a->safe_reset_time_threshold[2], a->safe_reset_time_threshold[3],
			     a->safe_reset_time_threshold[4], a->safe_reset_time_threshold[5],
			     a->safe_reset_time_threshold[6], a->safe_reset_time_threshold[7],
			     a->safe_reset_time_threshold[8], a->safe_reset_time_threshold[9]);
	len += sysfs_emit_at(buf, len,
			     "safe_reset_windows=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u ",
			     a->safe_reset_screen_on_window[0],
			     a->safe_reset_screen_on_window[1],
			     a->safe_reset_screen_on_window[2],
			     a->safe_reset_screen_on_window[3],
			     a->safe_reset_screen_on_window[4],
			     a->safe_reset_screen_on_window[5],
			     a->safe_reset_screen_on_window[6],
			     a->safe_reset_screen_on_window[7],
			     a->safe_reset_screen_on_window[8],
			     a->safe_reset_screen_on_window[9]);
	len += sysfs_emit_at(buf, len,
			     "safe_baseline_pushes=%u safe_reset_side_mask=0x%04x safe_reset_side_pushes=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u ",
			     a->safe_baseline_pushes,
			     a->safe_reset_reason_mask_side,
			     a->safe_reset_push_count_side[0], a->safe_reset_push_count_side[1],
			     a->safe_reset_push_count_side[2], a->safe_reset_push_count_side[3],
			     a->safe_reset_push_count_side[4], a->safe_reset_push_count_side[5],
			     a->safe_reset_push_count_side[6], a->safe_reset_push_count_side[7],
			     a->safe_reset_push_count_side[8], a->safe_reset_push_count_side[9]);
	len += sysfs_emit_at(buf, len,
			     "blreset_abnormal_types=0x%04x blreset_type_elapsed=%u,%u,%u blreset_type_counts=%u,%u,%u ",
			     a->blreset_abnormal_type_flags,
			     a->blreset_abnormal_type_elapsed[0],
			     a->blreset_abnormal_type_elapsed[1],
			     a->blreset_abnormal_type_elapsed[2],
			     a->blreset_type1_count, a->blreset_type2_count,
			     a->blreset_type3_count);
	len += sysfs_emit_at(buf, len,
			     "baseline_touch_hold=%u baseline_touch_seen=%u baseline_touch_release_frames=%u baseline_guard_state=%u baseline_guard_clean_frames=%u baseline_touch_holds=%u baseline_touch_releases=%u baseline_guard_restarts=%u baseline_spatial_updates=%u baseline_spatial_blocks=%u baseline_safe_clamps=%u ",
			     a->baseline_touch_hold,
			     a->baseline_touch_seen,
			     a->baseline_touch_release_frames,
			     a->baseline_guard_state,
			     a->baseline_guard_clean_frames,
			     a->baseline_touch_hold_count,
			     a->baseline_touch_release_count,
			     a->baseline_guard_restart_count,
			     a->baseline_spatial_update_count,
			     a->baseline_spatial_block_count,
			     a->baseline_safe_clamp_count);
	len += sysfs_emit_at(buf, len,
			     "working_hash=%08x safe_hash=%08x working_safe_common=%d working_safe_divergent=%u ",
			     working_hash, safe_hash, baseline_common,
			     baseline_divergent);
	len += sysfs_emit_at(buf, len,
			     "safe_compare_result=%s compare_triggers=%u compare_runs=%u ",
			     hx_safe_compare_result_name(a->diag_safe_compare_result),
			     a->diag_baseline_compare_triggers,
			     a->diag_baseline_compare_runs);
	len += sysfs_emit_at(buf, len,
			     "working_bad_nodes=%u safe_bad_nodes=%u ",
			     a->diag_working_bad_nodes,
			     a->diag_safe_bad_nodes);
	len += sysfs_emit_at(buf, len,
			     "working_negative_nodes=%u safe_negative_nodes=%u ",
			     a->diag_working_negative_nodes,
			     a->diag_safe_negative_nodes);
	len += sysfs_emit_at(buf, len,
			     "working_line_noise=%u safe_line_noise=%u ",
			     a->diag_working_line_noise,
			     a->diag_safe_line_noise);
	len += sysfs_emit_at(buf, len,
			     "working_common_shift=%d safe_common_shift=%d ",
			     a->diag_working_common_shift,
			     a->diag_safe_common_shift);
	len += sysfs_emit_at(buf, len,
			     "working_max_positive=%d working_max_negative=%d ",
			     a->diag_working_max_positive,
			     a->diag_working_max_negative);
	len += sysfs_emit_at(buf, len,
			     "safe_max_positive=%d safe_max_negative=%d ",
			     a->diag_safe_max_positive,
			     a->diag_safe_max_negative);
	len += sysfs_emit_at(buf, len,
			     "touch_mask_nodes=%u working_masked_bad=%u safe_masked_bad=%u ",
			     a->diag_touch_mask_nodes,
			     a->diag_working_masked_bad_nodes,
			     a->diag_safe_masked_bad_nodes);
	len += sysfs_emit_at(buf, len,
			     "working_regions=%u,%u,%u,%u,%u safe_regions=%u,%u,%u,%u,%u ",
			     a->diag_working_region_bad[0],
			     a->diag_working_region_bad[1],
			     a->diag_working_region_bad[2],
			     a->diag_working_region_bad[3],
			     a->diag_working_region_bad[4],
			     a->diag_safe_region_bad[0],
			     a->diag_safe_region_bad[1],
			     a->diag_safe_region_bad[2],
			     a->diag_safe_region_bad[3],
			     a->diag_safe_region_bad[4]);
	len += sysfs_emit_at(buf, len,
			     "safe_improvement_frames=%u safe_regression_frames=%u ",
			     a->diag_safe_improvement_frames,
			     a->diag_safe_regression_frames);
	len += sysfs_emit_at(buf, len,
			     "runtime_blreset_recommended=%u blreset_recommendations=%u ",
			     a->diag_blreset_recommended,
			     a->diag_blreset_recommendation_count);
	len += sysfs_emit_at(buf, len,
			     "noise_frame_holds=%u small_peak_continued=%u ",
			     a->noise_frame_hold_count,
			     a->diag_small_peak_continued);
	len += sysfs_emit_at(buf, len,
			     "weak_peak_continued=%u small_peak_rejected=%u ",
			     a->diag_weak_peak_continued,
			     a->diag_small_peak_rejected);
	len += sysfs_emit_at(buf, len,
			     "split_peak_deferred=%u cross_zone_split_deferred=%u ",
			     a->diag_split_peak_deferred,
			     a->diag_cross_zone_split_deferred);
	len += sysfs_emit_at(buf, len,
			     "peak_id_handoffs=%u handoff_residual_deferred=%u ",
			     a->diag_peak_id_handoffs,
			     a->diag_handoff_residual_deferred);
	len += sysfs_emit_at(buf, len, "fast_edge_starts=%u\n",
			     a->diag_fast_edge_starts);
	mutex_unlock(&ts->op_lock);
	return len;
}
static DEVICE_ATTR_RO(diagnostics);

/*
 * A bounded, read-only snapshot of the most recently received event stack.
 * This is intentionally exposed as hex rather than as a binary sysfs file so
 * an unprivileged diagnostic helper can preserve it without a custom ioctl.
 * The buffer is protected by op_lock, just like diagnostics_show().
 */
static ssize_t event_stack_hex_show(struct device *dev,
					struct device_attribute *attr, char *buf)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	ssize_t len = 0;
	u32 i;

	mutex_lock(&ts->op_lock);
	for (i = 0; i < ts->event_buf_sz && len < PAGE_SIZE - 3; i++)
		len += sysfs_emit_at(buf, len, "%02x%s", ts->event_buf[i],
					(i + 1) % 32 ? "" : "\n");
	if (len && buf[len - 1] != '\n' && len < PAGE_SIZE - 1)
		buf[len++] = '\n';
	mutex_unlock(&ts->op_lock);
	return len;
}
static DEVICE_ATTR_RO(event_stack_hex);

/* Full last-good event stack.  Reads are snapshots only; no SPI I/O occurs. */
static ssize_t event_stack_read(struct file *file, struct kobject *kobj,
				const struct bin_attribute *attr, char *buf,
				loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	size_t available;

	if (offset < 0 || offset >= ts->event_buf_sz)
		return 0;

	available = ts->event_buf_sz - offset;
	count = min(count, available);

	mutex_lock(&ts->op_lock);
	memcpy(buf, ts->event_buf + offset, count);
	mutex_unlock(&ts->op_lock);

	return count;
}

static const struct bin_attribute event_stack_attr = {
	.attr = {
		.name = "event_stack",
		.mode = 0444,
	},
	.size = HIMAX_FULL_EVENT_STACK_SIZE,
	.read = event_stack_read,
};

void himax_trace_record_irq(struct himax_ts_data *ts, int read_error,
			    bool master_valid,
			    const struct hx_frame_status *frame_status,
			    bool noise_hold)
{
	struct himax_trace_record *record;
	struct hx_algo *algo = ts->algo;
	u32 flags = 0;
	int i;

	if (!ts->trace_ring || !algo)
		return;

	record = &ts->trace_ring[ts->trace_write_index];
	memset(record, 0, sizeof(*record));
	record->boottime_ns = cpu_to_le64(ktime_get_boottime_ns());
	record->irq_sequence = cpu_to_le32(++ts->trace_irq_sequence);
	record->reset_generation = cpu_to_le32(ts->reset_generation);
	record->read_error = cpu_to_le32((u32)read_error);

	if (!read_error)
		flags |= HIMAX_TRACE_F_READ_OK;
	if (master_valid)
		flags |= HIMAX_TRACE_F_MASTER_VALID;
	if (master_valid && frame_status->retry)
		flags |= HIMAX_TRACE_F_RETRY;
	if (master_valid && frame_status->has_finger)
		flags |= HIMAX_TRACE_F_HAS_FINGER;
	if (noise_hold)
		flags |= HIMAX_TRACE_F_NOISE_HOLD;
	record->flags = cpu_to_le32(flags);

	record->algo_frame_sequence = cpu_to_le32(algo->diag_frame_seq);
	record->common_diff = cpu_to_le32((u32)algo->diag_common_diff);
	record->frame_max = cpu_to_le16((u16)algo->diag_frame_max);
	record->has_signal = algo->diag_has_signal;
	record->zones = algo->diag_zones;
	record->peaks = algo->diag_peaks;
	record->contacts_pre = algo->diag_contacts_pre_filter;
	record->contacts_post = algo->diag_contacts_post_filter;
	record->active_tracks = algo->diag_active_tracks;
	record->reported_tracks = algo->diag_reported_tracks;

	for (i = 0; i < min_t(int, algo->peak_count, HX_MAX_PEAKS); i++) {
		record->peak[i].row = algo->peaks[i].r;
		record->peak[i].col = algo->peaks[i].c;
		record->peak[i].signal = cpu_to_le16((u16)algo->peaks[i].z);
		record->peak[i].zone_area = cpu_to_le16(algo->peaks[i].zone_area);
		record->peak[i].zone_index = algo->peaks[i].zone_index;
		record->peak[i].flags = algo->peaks[i].on_edge ? BIT(0) : 0;
		record->peak[i].id = algo->peaks[i].id;
		record->peak[i].age = algo->peaks[i].age;
	}

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		const struct hx_track *track = &algo->tracks[i];

		record->track[i].flags = (track->active ? BIT(0) : 0) |
					 (track->reported ? BIT(1) : 0);
		record->track[i].age = track->age;
		record->track[i].missed = track->missed;
		record->track[i].debounce = track->debounce;
		record->track[i].source_peak_id = track->source_peak_id;
		record->track[i].source_peak_age = track->source_peak_age;
		record->track[i].x = cpu_to_le32((u32)track->x);
		record->track[i].y = cpu_to_le32((u32)track->y);
		record->track[i].vx = cpu_to_le32((u32)track->vx);
		record->track[i].vy = cpu_to_le32((u32)track->vy);
		record->track[i].signal_sum = cpu_to_le32((u32)track->signal_sum);
		record->track[i].filtered_x_q8 =
			cpu_to_le32((u32)track->filtered_x_q8);
		record->track[i].filtered_y_q8 =
			cpu_to_le32((u32)track->filtered_y_q8);
		record->track[i].deriv_x_q8 = cpu_to_le32((u32)track->deriv_x_q8);
		record->track[i].deriv_y_q8 = cpu_to_le32((u32)track->deriv_y_q8);
	}

	if (master_valid && !frame_status->retry) {
		for (i = 0; i < HX_PIXELS; i++)
			record->processed_frame[i] =
				cpu_to_le16((u16)((s16 *)algo->frame)[i]);
	}
	if (!read_error)
		memcpy(record->event_stack, ts->event_buf, ts->event_buf_sz);

	ts->trace_write_index =
		(ts->trace_write_index + 1) % HIMAX_TRACE_CAPACITY;
	if (ts->trace_count < HIMAX_TRACE_CAPACITY)
		ts->trace_count++;
}

static void himax_trace_build_snapshot(struct himax_ts_data *ts)
{
	struct himax_trace_header *header;
	u32 count = ts->trace_count;
	u32 start;
	u32 i;

	header = (struct himax_trace_header *)ts->trace_snapshot;
	memset(header, 0, sizeof(*header));
	header->magic = cpu_to_le32(HIMAX_TRACE_MAGIC);
	header->version = cpu_to_le16(HIMAX_TRACE_VERSION);
	header->header_size = cpu_to_le16(sizeof(*header));
	header->record_size = cpu_to_le32(sizeof(struct himax_trace_record));
	header->capacity = cpu_to_le32(HIMAX_TRACE_CAPACITY);
	header->count = cpu_to_le32(count);
	header->snapshot_boottime_ns = cpu_to_le64(ktime_get_boottime_ns());
	header->reset_generation = cpu_to_le64(ts->reset_generation);

	start = (ts->trace_write_index + HIMAX_TRACE_CAPACITY - count) %
		HIMAX_TRACE_CAPACITY;
	for (i = 0; i < count; i++) {
		void *dst = ts->trace_snapshot + sizeof(*header) +
			i * sizeof(struct himax_trace_record);
		u32 src = (start + i) % HIMAX_TRACE_CAPACITY;

		memcpy(dst, &ts->trace_ring[src], sizeof(struct himax_trace_record));
	}
	ts->trace_snapshot_len = sizeof(*header) +
		count * sizeof(struct himax_trace_record);
}

static ssize_t event_trace_read(struct file *file, struct kobject *kobj,
				const struct bin_attribute *attr, char *buf,
				loff_t offset, size_t count)
{
	struct device *dev = kobj_to_dev(kobj);
	struct himax_ts_data *ts = dev_get_drvdata(dev);
	size_t available;

	mutex_lock(&ts->op_lock);
	if (offset == 0)
		himax_trace_build_snapshot(ts);
	if (offset < 0 || offset >= ts->trace_snapshot_len) {
		count = 0;
		goto out_unlock;
	}

	available = ts->trace_snapshot_len - offset;
	count = min(count, available);
	memcpy(buf, ts->trace_snapshot + offset, count);

out_unlock:
	mutex_unlock(&ts->op_lock);
	return count;
}

static const struct bin_attribute event_trace_attr = {
	.attr = {
		.name = "event_trace",
		.mode = 0444,
	},
	.size = sizeof(struct himax_trace_header) +
		HIMAX_TRACE_CAPACITY * sizeof(struct himax_trace_record),
	.read = event_trace_read,
};
#endif

static struct attribute *hx_algo_attrs[] = {
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	&dev_attr_diagnostics.attr,
	&dev_attr_event_stack_hex.attr,
#endif
	&dev_attr_baseline_enabled.attr,
	&dev_attr_safe_baseline_replace_enabled.attr,
	&dev_attr_frame_interval_ms.attr,
	&dev_attr_baseline_noise_deadband.attr,
	&dev_attr_baseline_peak_threshold.attr,
	&dev_attr_baseline_release_hold_frames.attr,
	&dev_attr_baseline_background_alpha_shift.attr,
	&dev_attr_baseline_no_finger_alpha_shift.attr,
	&dev_attr_baseline_recovery_alpha_shift.attr,
	&dev_attr_baseline_background_max_step.attr,
	&dev_attr_baseline_no_finger_max_step.attr,
	&dev_attr_baseline_recovery_max_step.attr,
	&dev_attr_baseline_recovery_max_frames.attr,
	&dev_attr_baseline_noise_tracking.attr,
	&dev_attr_wake_stable_frames.attr,
	&dev_attr_wake_finger_safe_frames.attr,
	&dev_attr_wake_raw_jump_threshold.attr,
	&dev_attr_wake_max_unstable_nodes.attr,
	&dev_attr_wake_max_unstable_line_nodes.attr,
	&dev_attr_safe_commit_no_finger_frames.attr,
	&dev_attr_runtime_blreset_enabled.attr,
	&dev_attr_runtime_blreset_confirm_frames.attr,
	&dev_attr_runtime_blreset_cooldown.attr,
	&dev_attr_runtime_noise_threshold.attr,
	&dev_attr_runtime_noise_line_nodes.attr,
	&dev_attr_runtime_noise_total_nodes.attr,
	&dev_attr_cmf_enabled.attr,
	&dev_attr_cmf_exclusion.attr,
	&dev_attr_cmf_max_correction.attr,
	&dev_attr_iir_enabled.attr,
	&dev_attr_iir_decay_weight.attr,
	&dev_attr_iir_decay_step.attr,
	&dev_attr_iir_noise_floor.attr,
	&dev_attr_iir_gate_floor.attr,
	&dev_attr_iir_gate_ratio_q8.attr,
	&dev_attr_macro_threshold.attr,
	&dev_attr_peak_threshold.attr,
	&dev_attr_peak_local_radius.attr,
	&dev_attr_peak_z8_enabled.attr,
	&dev_attr_peak_saddle_enabled.attr,
	&dev_attr_peak_saddle_radius.attr,
	&dev_attr_peak_saddle_drop.attr,
	&dev_attr_peak_signal_threshold_limit.attr,
	&dev_attr_peak_edge_threshold.attr,
	&dev_attr_peak_macro_min_area.attr,
	&dev_attr_peak_continue_min_area.attr,
	&dev_attr_peak_continue_min_signal.attr,
	&dev_attr_peak_single_track_continue_min_signal.attr,
	&dev_attr_peak_continue_dist2.attr,
	&dev_attr_peak_fast_start_min_signal.attr,
	&dev_attr_peak_fast_start_edge_cells.attr,
	&dev_attr_palm_enabled.attr,
	&dev_attr_palm_area_threshold.attr,
	&dev_attr_palm_signal_threshold.attr,
	&dev_attr_palm_density_low.attr,
	&dev_attr_palm_box_enabled.attr,
	&dev_attr_palm_box_expand_rows.attr,
	&dev_attr_palm_box_expand_cols.attr,
	&dev_attr_palm_box_match_distance.attr,
	&dev_attr_palm_box_max_hold.attr,
	&dev_attr_zone_cleanup_enabled.attr,
	&dev_attr_zone_max_radius.attr,
	&dev_attr_zone_threshold_numer.attr,
	&dev_attr_zone_threshold_shift.attr,
	&dev_attr_pressure_enabled.attr,
	&dev_attr_edge_comp_enabled.attr,
	&dev_attr_edge_boost_pct.attr,
	&dev_attr_edge_push_q8.attr,
	&dev_attr_edge_blend_q8.attr,
	&dev_attr_edge_reject_enabled.attr,
	&dev_attr_edge_reject_margin.attr,
	&dev_attr_edge_reject_min_signal.attr,
	&dev_attr_track_dist2_max.attr,
	&dev_attr_track_lost_frames.attr,
	&dev_attr_debounce_base.attr,
	&dev_attr_track_smoothing.attr,
	&dev_attr_track_active_guard.attr,
	&dev_attr_track_start_debounce.attr,
	&dev_attr_track_jump_dist2.attr,
	&dev_attr_hungarian_enabled.attr,
	&dev_attr_debounce_weak_extra.attr,
	&dev_attr_debounce_edge_extra.attr,
	&dev_attr_debounce_strong_signal.attr,
	&dev_attr_firmware_edge_fast_start.attr,
	&dev_attr_split_peak_confirm_frames.attr,
	&dev_attr_split_peak_dist2.attr,
	&dev_attr_split_cross_zone_confirm_frames.attr,
	&dev_attr_split_cross_zone_dist2.attr,
	&dev_attr_track_peak_id_penalty.attr,
	&dev_attr_ghost_enabled.attr,
	&dev_attr_ghost_row_distance.attr,
	&dev_attr_ghost_weak_ratio_q8.attr,
	&dev_attr_ghost_min_col_distance.attr,
	&dev_attr_euro_enabled.attr,
	&dev_attr_euro_alpha_min_q8.attr,
	&dev_attr_euro_alpha_max_q8.attr,
	&dev_attr_euro_speed_threshold.attr,
	NULL,
};

static const struct attribute_group hx_algo_attr_group = {
	.name = "algo",
	.attrs = hx_algo_attrs,
};

int himax_sysfs_init(struct himax_ts_data *ts)
{
	int ret;

	ret = device_create_file(ts->dev, &dev_attr_inplace_reset);
	if (ret)
		return ret;
	ret = device_create_file(ts->dev, &dev_attr_baseline_full_reset);
	if (ret)
		goto remove_inplace_reset;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	ret = sysfs_create_bin_file(&ts->dev->kobj, &event_stack_attr);
	if (ret)
		goto remove_full_reset;
	ret = sysfs_create_bin_file(&ts->dev->kobj, &event_trace_attr);
	if (ret)
		goto remove_event_stack;
#endif

	ret = devm_device_add_group(ts->dev, &hx_algo_attr_group);
	if (!ret)
		return 0;
	goto remove_full_reset;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	sysfs_remove_bin_file(&ts->dev->kobj, &event_trace_attr);
remove_event_stack:
	sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
#endif
remove_full_reset:
	device_remove_file(ts->dev, &dev_attr_baseline_full_reset);
remove_inplace_reset:
	device_remove_file(ts->dev, &dev_attr_inplace_reset);
	return ret;
}

void himax_sysfs_remove(struct himax_ts_data *ts)
{
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	sysfs_remove_bin_file(&ts->dev->kobj, &event_trace_attr);
	sysfs_remove_bin_file(&ts->dev->kobj, &event_stack_attr);
#endif
	device_remove_file(ts->dev, &dev_attr_inplace_reset);
	device_remove_file(ts->dev, &dev_attr_baseline_full_reset);
}
