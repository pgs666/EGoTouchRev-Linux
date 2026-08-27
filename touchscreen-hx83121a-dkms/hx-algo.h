/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Himax HX83121A touch algorithm — data structures and interface.
 *
 * All algorithm state lives in struct hx_algo, which is allocated once in
 * probe via devm_kzalloc and referenced from struct himax_ts_data.
 */
#ifndef HX_ALGO_H
#define HX_ALGO_H

#ifdef HX_ALGO_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
struct input_mt_pos { int x; int y; };
#else
#include <linux/input/mt.h>
#include <linux/types.h>
#endif

/* Grid dimensions — must match the firmware raw frame layout. */
#define HX_ROWS        40
#define HX_COLS        60
#define HX_PIXELS      (HX_ROWS * HX_COLS)  /* 2400 */

/* Detection limits */
#define HX_MAX_ZONES   20
#define HX_MAX_PEAKS   20
#define HX_MAX_PALM_BOXES 8
#define HX_ASSIGN_COLS (HIMAX_MAX_TOUCH * 2)
#define HX_BASELINE_HIST_BINS 2048
#define HX_BASELINE_COMPARE_REGIONS 5
#define HX_SAFE_BASELINE_SLOTS 5
#define HX_BASELINE_FRACTION_BITS 8
#define HX_BASELINE_CLEAN_LOCAL_THRESHOLD 100
#define HX_BASELINE_CLEAN_MAX_NODES 3

enum hx_safe_baseline_confidence {
	HX_SAFE_BASELINE_EMPTY,
	HX_SAFE_BASELINE_BOOTSTRAP,
	HX_SAFE_BASELINE_CONFIRMED,
	HX_SAFE_BASELINE_CROSS_WAKE,
};

enum hx_safe_baseline_state {
	HX_SAFE_SLOT_EMPTY,
	HX_SAFE_SLOT_PROVISIONAL,
	HX_SAFE_SLOT_CONFIRMED,
	HX_SAFE_SLOT_CROSS_WAKE,
	HX_SAFE_SLOT_RESET,
};

struct hx_safe_baseline_entry {
	s32 baseline_q8[HX_PIXELS];
	u32 generation;
	u32 screen_epoch;
	u32 captured_frame;
	u8 confidence;
	u8 state;
	u16 stable_frames;
	u32 use_count;
	bool reset_pending;
	bool pushed;
	bool valid;
};

/* SafeBaseline_GetPrpt's five spatial aggregates. */
struct hx_safe_prpt {
	s16 max;
	s16 min;
	u16 count;
};

enum hx_safe_compare_result {
	HX_SAFE_COMPARE_NOT_RUN,
	HX_SAFE_COMPARE_WORKING_BETTER,
	HX_SAFE_COMPARE_SAFE_BETTER,
	HX_SAFE_COMPARE_BOTH_VALID,
	HX_SAFE_COMPARE_BOTH_INVALID,
	HX_SAFE_COMPARE_AMBIGUOUS,
};

enum hx_baseline_guard_state {
	HX_BASELINE_GUARD_NORMAL,
	HX_BASELINE_GUARD_PROTECTED,
	HX_BASELINE_GUARD_RELEASE_SETTLE,
	HX_BASELINE_GUARD_CLEAN_QUALIFY,
};

/* Exact TSACore BLSM stage numbers.  Keep the numeric values stable: the
 * vendor transition and process-stage jump tables index these values.
 */
enum hx_baseline_stage {
	HX_BLSM_NO_TOUCH_STABLE = 0,
	HX_BLSM_RESET = 1,
	HX_BLSM_NOISY_RESET = 2,
	HX_BLSM_FORCED = 3,
	HX_BLSM_PRE_TOUCH = 4,
	HX_BLSM_TOUCH = 5,
	HX_BLSM_TOUCH_RELEASE = 6,
	HX_BLSM_POST_TOUCH = 7,
	HX_BLSM_POST_TOUCH_ALT = 8,
	HX_BLSM_DEBOUNCE = 9,
};

/* g_tsaPrpt + 0x280, as produced by BLSM_GetProperty(). */
#define HX_BLSM_PRPT_WEAK_SIGNAL  0x001
#define HX_BLSM_PRPT_TOUCH        0x002
#define HX_BLSM_PRPT_RESET        0x010
#define HX_BLSM_PRPT_FORCED       0x040
#define HX_BLSM_PRPT_DEBOUNCE     0x080
#define HX_BLSM_PRPT_RAW_UNSTABLE 0x100
#define HX_BLSM_PRPT_NOISY        0x200

enum hx_screen_on_hand_state {
	HX_HAND_NONE,
	HX_HAND_SUSPECTED,
	HX_HAND_PROTECTED,
	HX_HAND_RELEASE_PENDING,
	HX_HAND_REACQUIRE,
};

enum hx_safe_reset_action {
	HX_SAFE_RESET_NONE,
	HX_SAFE_RESET_WORKING,
	HX_SAFE_RESET_INVALIDATE,
};

/* A predicate is not a reset.  TSACore accumulates abnormal reasons for a
 * bounded interval, emits one trigger, then rearms only after recovery.
 */
enum hx_blreset_state {
	HX_BLRESET_IDLE,
	HX_BLRESET_DEBOUNCE,
	HX_BLRESET_TRIGGERED,
};

/* SafeBaseline_CheckWithState/CheckWithSignal flags from TSACore. */
enum hx_safe_baseline_flag {
	HX_SAFE_FLAG_TOUCH_PROTECTED = 1U << 0,
	HX_SAFE_FLAG_GHOST_MAX        = 1U << 1,
	HX_SAFE_FLAG_ALL_TOUCH_BAD    = 1U << 3,
	HX_SAFE_FLAG_VERY_NOISY       = 1U << 4,
	HX_SAFE_FLAG_VERY_NEGATIVE    = 1U << 5,
	HX_SAFE_FLAG_SAFE_DIFF        = 1U << 6,
	HX_SAFE_FLAG_SIGNAL_DISPARITY = 1U << 7,
	HX_SAFE_FLAG_SENSOR_BAD       = 1U << 8,
	HX_SAFE_FLAG_SILENT_GHOST     = 1U << 9,
};

struct hx_baseline_platform_state {
	bool idle_transition;
	bool charger_noise;
	bool charger_connected;
	bool proximity_active;
	bool panel_sd;
	bool raw_unified;
	bool smart_cover;
};

/* Maximum simultaneous reported contacts. */
#define HIMAX_MAX_TOUCH 10

/**
 * struct hx_peak - single detected local-maximum candidate.
 * @r:          row in the grid (0-based)
 * @c:          column in the grid
 * @z:          signal value at the peak
 * @nbr_sum:    sum of all 8-neighbour signals (used for Z8 filter)
 * @zone_area:  area of the macro-zone this peak belongs to
 * @zone_index: index of the owning macro-zone
 * @id:         persistent peak identity across adjacent frames
 * @age:        consecutive matched-frame age (first frame is zero)
 * @vr, @vc:    last matched grid velocity, used for peak-ID prediction
 * @on_edge:    true when the peak lies on the outermost grid row or column
 * @fast_start_candidate: firmware-confirmed strong, compact edge onset
 * @continuation_only: candidate may only update its bound existing slot
 * @continuation_track_slot: sole existing slot eligible for this candidate
 */
struct hx_peak {
	u8  r;
	u8  c;
	s16 z;
	s32 nbr_sum;
	u16 zone_area;
	u8  zone_index;
	u8  id;
	u8  age;
	s8  vr;
	s8  vc;
	bool on_edge;
	bool fast_start_candidate;
	bool continuation_only;
	s8   continuation_track_slot;
};

/**
 * struct hx_contact - sub-pixel coordinate after centroid expansion.
 * @x, @y:      Q8.8 fixed-point grid coordinates
 * @area:       number of pixels contributing to this contact
 * @signal_sum: integrated signal over the contact area
 * @is_edge:    true when the originating peak lies on the grid boundary
 * @fast_start_candidate: contact may bypass new-touch debounce on FW rising
 * @peak_index: index of the peak that produced this contact
 * @source_peak_id/source_peak_age: persistent source-peak identity
 * @source_zone_index: owning macro-zone in the current frame
 * @continuation_only: contact may not bootstrap a new tracking slot
 * @continuation_track_slot: sole existing slot eligible for this contact
 */
struct hx_contact {
	s32  x;
	s32  y;
	u16  area;
	s32  signal_sum;
	bool is_edge;
	bool fast_start_candidate;
	u8   peak_index;
	u8   source_peak_id;
	u8   source_peak_age;
	u8   source_zone_index;
	bool continuation_only;
	s8   continuation_track_slot;
};

/**
 * struct hx_track - persistent touch slot state.
 * @active:     slot is in use
 * @x, @y:      current output coordinates [0, 65535]
 * @vx, @vy:    velocity in output units per frame (for prediction)
 * @signal_sum: integrated signal (forwarded to pressure reporting)
 * @age:        frames the slot has been active
 * @missed:     consecutive frames the slot had no matching detection
 * @debounce:   remaining debounce frames before the slot is reported
 * @source_peak_id/source_peak_age: most recently matched source peak
 */
struct hx_track {
	bool active;
	bool reported;
	s32  x;
	s32  y;
	s32  vx;
	s32  vy;
	s32  signal_sum;
	u8   age;
	u8   missed;
	u8   debounce;
	u8   source_peak_id;
	u8   source_peak_age;
	s32  filtered_x_q8;
	s32  filtered_y_q8;
	s32  deriv_x_q8;
	s32  deriv_y_q8;
};

/* Age a possible second touch from the moment it starts competing with an
 * existing reported track.  Peak lifetime is not suitable for this: a mature
 * residual lobe can become unmatched for the first time after a split.
 */
struct hx_peak_competition {
	u8 peak_id;
	u8 age;
	bool seen;
	bool handoff_residual;
};

/**
 * struct hx_macro_zone - contiguous above-threshold region.
 * @arena_start: first pixel in hx_algo.zone_arena
 * @area:       total pixel count
 * @signal_sum: sum of positive pixel values within the zone
 * @min_r … max_c: bounding box
 */
struct hx_macro_zone {
	u16 arena_start;
	u16 area;
	s32 signal_sum;
	u8  min_r;
	u8  max_r;
	u8  min_c;
	u8  max_c;
};

struct hx_palm_box {
	u8 min_r, max_r, min_c, max_c;
	u16 missed;
};

/**
 * struct hx_algo - all algorithm state, allocated once in probe.
 *
 * Memory budget: ~112 KB with five historical grids plus the live, selected
 * safe and candidate baselines.  Allocate with a vmalloc fallback and never
 * place this structure on the kernel stack.
 */
struct hx_algo {
	/* ---- Frame buffers ---- */
	s16 frame[HX_ROWS][HX_COLS];        /* baseline-subtracted signal  */
	s16 iir_history[HX_ROWS][HX_COLS];  /* IIR temporal-filter history */
	bool iir_initialized;

	/* ---- Scratch buffers (shared between pipeline stages) ---- */
	u8  visited[HX_PIXELS];              /* BFS visited flags           */
	u8  zone_map[HX_PIXELS];             /* per-pixel zone-ID map       */
	u16 bfs_queue[HX_PIXELS];            /* ring-buffer BFS queue       */
	u16 zone_arena[HX_PIXELS];           /* all zone pixels, no truncation */
	u16 zone_arena_used;

	/* ---- Detection results ---- */
	struct hx_macro_zone zones[HX_MAX_ZONES];
	u8   zone_count;

	struct hx_peak peaks[HX_MAX_PEAKS];
	u8   peak_count;
	struct hx_peak prev_peaks[HX_MAX_PEAKS];
	u8   prev_peak_count;
	u8   next_peak_id;
	struct hx_peak_competition peak_competition[HX_MAX_PEAKS];

	/* Keep every peak candidate until the final strength-based capacity cut.
	 * The old implementation truncated the ascending peak list to ten first,
	 * which selected the ten weakest candidates on noisy frames.
	 */
	struct hx_contact contacts[HX_MAX_PEAKS];
	u8   contact_count;

	/* ---- Per-cell adaptive baseline (Q8 fixed point) ---- */
	s32 baseline_q8[HX_PIXELS];
	s32 normal_baseline_q8[HX_PIXELS];
	bool normal_baseline_valid;
	/* Raw frame preserved before CMF. The vendor selects raw or pre-CMF
	 * data for BLIIR from a flash capability bit; retaining both makes that
	 * choice explicit instead of silently feeding filtered data.
	 */
	u16 pre_cmf_raw[HX_PIXELS];
	/* Previous raw frame for BLReset AFE-jump predicates.  It is only used by
	 * the classifier and is invalid until one complete frame has finished.
	 */
	u16 prev_raw[HX_PIXELS];
	bool prev_raw_valid;
	u16 blreset_raw_jump_frames;
	u32 blreset_raw_jump_elapsed_ms;
	u16 blreset_over_noise_frames;
	u8 blreset_state;
	u16 blreset_reason_mask;
	u16 blreset_abnormal_type_flags;
	u32 blreset_abnormal_type_elapsed[3];
	u32 blreset_reason_elapsed_ms;
	u32 blreset_trigger_count;
	u32 blreset_clear_count;
	bool blreset_triggered;
	bool blreset_all_touch_abnormal;
	bool blreset_concurrent_touch;
	/* BLReset_BaselineStateMachine is separate from the temporal reset gate
	 * above.  These fields mirror TSAStatic +0x3d8/+0x3e0/+0x3e8 and the
	 * clean-baseline-ready flags recovered from the vendor jump table.
	 */
	u8 blreset_baseline_state;
	u8 blreset_baseline_stable_frames;
	u32 blreset_baseline_elapsed_ms;
	bool blreset_normal_baseline_ready;
	bool blreset_clean_baseline_captured;
	u8 blreset_wake_abnormal_frames;
	u32 blreset_wake_abnormal_elapsed_ms;
	bool blreset_wake_triggered;
	u32 blreset_dirty_elapsed_ms;
	bool blreset_dirty_triggered;
	u16 shb_raw_capture[HX_PIXELS];
	u8 shb_state;
	u16 shb_flags;
	u32 shb_capture_frame;
	/* BLRecal_Process state.  Gaokun flash values are center=0x8000,
	 * allowed span=8000, and signal threshold=600.
	 */
	u8 blrecal_frame;
	u8 blrecal_abnormal_count;
	bool blrecal_requested;
	u16 blrecal_raw_center;
	u16 blrecal_raw_span;
	s16 blrecal_signal_threshold;
	/* Keep the selected safe baseline as a hot working copy.  The five-entry
	 * store preserves independently qualified history without making the
	 * per-frame path compare every saved grid.
	 */
	s32 safe_baseline_q8[HX_PIXELS];
	s32 wake_candidate_q8[HX_PIXELS];
	/* SafeBaseline_BufferComparison outputs, retained for the post-tracker
	 * touch/side-area checks and diagnostics.
	 */
	s16 safe_bl2bl_dif[HX_PIXELS];
	s16 safe_bl2raw_dif[HX_PIXELS];
	s16 safe_bl2bl_dif_cmf[HX_PIXELS];
	s16 safe_bl2raw_dif_cmf[HX_PIXELS];
	struct hx_safe_baseline_entry safe_baselines[HX_SAFE_BASELINE_SLOTS];
	struct hx_safe_baseline_entry safe_temp_baselines[HX_SAFE_BASELINE_SLOTS];
	u8  baseline_release_hold[HX_PIXELS];
	u16 baseline_hist[HX_BASELINE_HIST_BINS];
	bool baseline_initialized;
	bool safe_baseline_valid;
	u8 safe_baseline_count;
	u8 safe_baseline_selected;
	u16 safe_flags;
	u16 safe_prev_flags;
	struct hx_baseline_platform_state platform;
	u16 safe_baseline_selected_score;
	u8 safe_baseline_next;
	/* Vendor BaselineQueue ring metadata.  Slot indices are deliberately
	 * separate from the physical array so reset/selection can invalidate a
	 * unit without changing chronological order.
	 */
	u8 safe_queue_order[HX_SAFE_BASELINE_SLOTS];
	u8 safe_queue_head;
	u8 safe_queue_tail;
	u8 safe_queue_count;
	u8 safe_queue_full_pushes;
	/* TSACore SafeBLPushed (g_safeBaseline + 0x21), reset when the
	 * corresponding baseline queue is reset.
	 */
	u8 safe_baseline_pushes;
	u8 safe_temp_queue_order[HX_SAFE_BASELINE_SLOTS];
	u8 safe_temp_queue_head;
	u8 safe_temp_queue_tail;
	u8 safe_temp_queue_count;
	u32 safe_baseline_generation;
	u32 screen_epoch;
	u32 frame_sequence;
	/* Formatted Touch_GetTimeInterval equivalent, in milliseconds. */
	u16 frame_interval_ms;
	bool wake_qualifying;
	bool wake_candidate_valid;
	bool wake_needs_double_confirm;
	u8 wake_candidate_frames;
	u8 wake_finger_frames;
	u8 wake_finger_reject_frames;
	s32 wake_finger_common_sum;
	s32 wake_finger_common_last;
	bool wake_raw_finger_override;
	u8 wake_raw_finger_release_frames;
	u8 safe_no_finger_frames;
	bool safe_candidate_armed;
	bool safe_candidate_confirming;
	u32 safe_candidate_screen_epoch;
	s32 safe_confirm_common_sum;
	s32 safe_confirm_common_last;
	u8 runtime_safe_improvement_frames;
	u8 runtime_safe_regression_frames;
	u8 safe_baseline_invalid_frames;
	u8 safe_side_reset_frames;
	bool safe_reset_in_debounce;
	bool safe_sync_reset_side_area;
	u8 safe_signal_stable_frames;
	u8 safe_valid_touch_count;
	u8 safe_abnormal_touch_count;
	u16 safe_current_positive_nodes;
	u16 safe_current_negative_nodes;
	struct hx_safe_prpt safe_bl2bl_prpt[5];
	struct hx_safe_prpt safe_bl2bl_prpt_cmf[5];
	struct hx_safe_prpt safe_bl2raw_prpt[5];
	struct hx_safe_prpt safe_bl2raw_prpt_cmf[5];
	u16 safe_reset_reason_frames[10];
	u32 safe_reset_reason_last_frame[10];
	/* Official TSACore g_pushAbnormalThold: maximum number of reset
	 * pushes permitted for each abnormal-reason unit.
	 */
	u8 safe_reset_push_threshold[10];
	u16 safe_reset_time_threshold[10];
	/* Runtime g_safeBaselineReset counters, one per reason. */
	u8 safe_reset_push_count[10];
	/* Official g_triggerAbnormalThold: per-reason reset trigger limit. */
	u8 safe_reset_trigger_count[10];
	u32 safe_reset_screen_on_window[10];
	/* SideBaseline_ResetStatistics uses a separate state block and the
	 * exported *Side threshold tables.
	 */
	u8 safe_reset_push_threshold_side[10];
	u16 safe_reset_time_threshold_side[10];
	u8 safe_reset_trigger_count_side[10];
	u32 safe_reset_screen_on_window_side[10];
	u16 safe_reset_reason_frames_side[10];
	u32 safe_reset_reason_last_frame_side[10];
	u8 safe_reset_push_count_side[10];
	u16 safe_reset_reason_mask_side;
	u32 screen_on_frame_sequence;
	u16 safe_reset_reason_mask;
	u16 safe_reset_reason_trigger_frames;
	u16 runtime_blreset_cooldown_frames;
	bool baseline_prev_had_signal;
	bool baseline_had_freeze;
	u8 baseline_recovery_frames;
	/* BLSM stage-0 equivalent: require a stable no-touch interval before
	 * allowing BLIIR-style per-cell baseline tracking.
	 */
	u8 baseline_no_touch_stable_frames;
	u8 baseline_stage;
	u8 baseline_prev_stage;
	u8 baseline_stage_frames;
	bool baseline_touch_latched;
	u32 baseline_stage_elapsed_ms;
	/* Actions selected by the official BLSM_ProcessStage equivalent.  Keeping
	 * these separate from the stage number prevents the frame preprocessor
	 * from silently inventing its own stage semantics.
	 */
	bool baseline_stage_allows_update;
	bool baseline_stage_force_update;
	bool baseline_stage_reset;
	s16 baseline_stage_update_step;
	u8 baseline_stage_action;
#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	u32 baseline_stage_update_count;
	u32 baseline_stage_reset_action_count;
	u32 baseline_stage_hold_count;
	u32 baseline_stage_force_count;
#endif
	bool baseline_touch_hold;
	bool baseline_touch_seen;
	bool baseline_held_in_hand;
	bool baseline_reacquire_pending;
	/* Hardware reset provenance used by the vendor held-in-hand wake gate. */
	bool baseline_hw_reset;
	u8 baseline_screen_on_hand_state;
	/* Number of clean frames for which a newly reacquired baseline remains
	 * protected from ordinary BLIIR spatial learning.
	 */
	u16 baseline_post_reacquire_hold;
	u8 baseline_touch_release_frames;
	u8 baseline_guard_state;
	u8 baseline_guard_clean_frames;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_DIAGNOSTICS
	enum hx_safe_compare_result diag_safe_compare_result;
	s32 diag_working_common_shift;
	s32 diag_safe_common_shift;
	s32 diag_working_max_positive;
	s32 diag_working_max_negative;
	s32 diag_safe_max_positive;
	s32 diag_safe_max_negative;
	u16 diag_working_bad_nodes;
	u16 diag_safe_bad_nodes;
	u16 diag_working_negative_nodes;
	u16 diag_safe_negative_nodes;
	u16 diag_working_line_noise;
	u16 diag_safe_line_noise;
	u16 diag_touch_mask_nodes;
	u16 diag_working_masked_bad_nodes;
	u16 diag_safe_masked_bad_nodes;
	u16 diag_working_region_bad[HX_BASELINE_COMPARE_REGIONS];
	u16 diag_safe_region_bad[HX_BASELINE_COMPARE_REGIONS];
	u8 diag_safe_improvement_frames;
	u8 diag_safe_regression_frames;
	bool diag_blreset_recommended;
	u32 diag_baseline_compare_triggers;
	u32 diag_baseline_compare_runs;
	u32 diag_blreset_recommendation_count;
	/* Read-only pipeline snapshot for diagnosing hardware-only dropouts. */
	u32 diag_frame_seq;
	s32 diag_common_diff;
	s16 diag_frame_max;
	u8 diag_has_signal;
	u8 diag_zones;
	u8 diag_peaks;
	u8 diag_contacts_pre_filter;
	u8 diag_contacts_post_filter;
	u8 diag_active_tracks;
	u8 diag_reported_tracks;
	u32 diag_small_peak_continued;
	u32 diag_weak_peak_continued;
	u32 diag_small_peak_rejected;
	u32 diag_split_peak_deferred;
	u32 diag_cross_zone_split_deferred;
	u32 diag_peak_id_handoffs;
	u32 diag_handoff_residual_deferred;
	u32 diag_fast_edge_starts;
	u32 baseline_generation;
	u32 full_reset_count;
	u32 live_clear_count;
	u32 wake_qualification_count;
	u32 wake_candidate_reject_count;
	u32 wake_safe_fallback_count;
	u32 wake_baseline_commit_count;
	u32 wake_safe_divergence_count;
	u32 wake_finger_mask_accept_count;
	u32 wake_finger_mask_reject_count;
	u32 wake_finger_degraded_fallback_count;
	u32 wake_raw_finger_inferred_count;
	u32 wake_raw_finger_release_count;
	u32 wake_ambiguous_safe_fallback_count;
	u32 wake_safe_preserved_count;
	u32 wake_clean_safe_restore_count;
	u32 baseline_safe_commit_count;
	u32 baseline_safe_confirm_count;
	u32 baseline_safe_temporal_reject_count;
	u32 baseline_safe_spatial_reject_count;
	u32 baseline_safe_negative_reject_count;
	u32 baseline_safe_range_reject_count;
	u32 baseline_safe_dedup_count;
	u32 baseline_safe_eviction_count;
	u32 runtime_blreset_count;
	u32 runtime_blreset_suppressed_count;
	u32 baseline_touch_hold_count;
	u32 baseline_touch_release_count;
	u32 baseline_guard_restart_count;
	u32 baseline_spatial_update_count;
	u32 baseline_spatial_block_count;
	u32 baseline_safe_clamp_count;
	u32 baseline_safe_slot_switch_count;
	u32 baseline_safe_slot_reject_count;
	u32 baseline_safe_reset_count;
	u32 baseline_safe_side_reset_count;
	u32 baseline_safe_abnormal_max_count;
	u32 baseline_safe_abnormal_min_count;
	u32 noise_frame_hold_count;
	u32 blreset_type1_count;
	u32 blreset_type2_count;
	u32 blreset_type3_count;
#endif

	struct hx_palm_box palm_boxes[HX_MAX_PALM_BOXES];
	u8 palm_box_count;

	/* ---- Tracking state ---- */
	struct hx_track tracks[HIMAX_MAX_TOUCH];
	bool touch_active;
	u8   touch_start_frames;
	bool firmware_finger_present;
	bool fast_edge_start_pending;

	/* Hungarian scratch is kept off the kernel stack. */
	s64 assign_cost[HIMAX_MAX_TOUCH][HX_ASSIGN_COLS];
	s64 assign_u[HIMAX_MAX_TOUCH + 1];
	s64 assign_v[HX_ASSIGN_COLS + 1];
	u8 assign_p[HX_ASSIGN_COLS + 1];
	u8 assign_way[HX_ASSIGN_COLS + 1];

	/* ---- Tunable parameters (sysfs-writable, atomically updated) ---- */
	/* Preprocessing */
	bool baseline_enabled;
	/* TSACore g_tsaPrmtFlash[0x1743].  The shipped Gaokun panel blobs
	 * contain zero, so held-in-hand wake must not replace the working grid
	 * unless a panel-specific parameter explicitly enables it.
	 */
	bool safe_baseline_replace_enabled;
	/* Set for the wake qualification frame: it is consumed by the tracker
	 * exactly once, but must not also advance baseline learning.
	 */
	bool baseline_update_suppressed_once;
	u16  baseline_initial;
	s16  baseline_noise_deadband;
	s16  baseline_positive_deadband;
	s16  baseline_negative_deadband;
	s16  baseline_peak_threshold;
	u8   baseline_release_hold_frames;
	u8   baseline_positive_alpha_shift;
	u8   baseline_negative_alpha_shift;
	u8   baseline_noise_alpha_shift;
	s16  baseline_positive_max_step;
	s16  baseline_negative_max_step;
	u8   baseline_background_alpha_shift;
	u8   baseline_no_finger_alpha_shift;
	u8   baseline_recovery_alpha_shift;
	s16  baseline_background_max_step;
	s16  baseline_no_finger_max_step;
	s16  baseline_recovery_max_step;
	u8   baseline_recovery_max_frames;
	bool baseline_noise_tracking;
	u8   wake_stable_frames;
	u8   wake_finger_safe_frames;
	s16  wake_raw_jump_threshold;
	u16  wake_max_unstable_nodes;
	u8   wake_max_unstable_line_nodes;
	u8   safe_commit_no_finger_frames;
	bool runtime_blreset_enabled;
	u8   runtime_blreset_confirm_frames;
	u16  runtime_blreset_cooldown;
	s16  runtime_noise_threshold;
	u8   runtime_noise_line_nodes;
	u16  runtime_noise_total_nodes;
	bool cmf_enabled;          /* CMF on/off (default: true)           */
	bool bliir_use_pre_cmf_raw;
	s16  cmf_exclusion;        /* exclude pixels > this from CMF mean  */
	s16  cmf_max_correction;   /* clamp per-row/col offset             */
	bool iir_enabled;          /* IIR temporal filter on/off           */
	u16  iir_decay_weight;     /* blend weight 0-256 (256 = no blend)  */
	u16  iir_decay_step;       /* per-frame decay in signal units      */
	s16  iir_noise_floor;      /* clamp-to-zero below this             */
	s16  iir_gate_floor;       /* min dynamic threshold                */
	u8   iir_gate_ratio_q8;    /* dyn threshold = max * ratio/256      */
	/* Detection */
	s16  macro_threshold;      /* minimum pixel value to seed BFS      */
	s16  peak_threshold;       /* minimum peak signal                  */
	u8   peak_local_radius;
	bool peak_z8_enabled;
	bool peak_saddle_enabled;
	u8   peak_saddle_radius;
	s16  peak_saddle_drop;
	s16  peak_signal_threshold_limit;
	s16  peak_edge_threshold;
	u8   peak_macro_min_area;
	u8   peak_continue_min_area;
	s16  peak_continue_min_signal;
	s16  peak_single_track_continue_min_signal;
	s32  peak_continue_dist2;
	s16  peak_fast_start_min_signal;
	u8   peak_fast_start_edge_cells;
	bool palm_enabled;         /* palm-rejection on/off                */
	u8   palm_area_threshold;  /* area >= this → palm                  */
	s32  palm_signal_threshold;/* signal_sum >= this → palm            */
	s16  palm_density_low;     /* signal/area < this → palm            */
	bool palm_box_enabled;
	u8 palm_box_expand_rows;
	u8 palm_box_expand_cols;
	u8 palm_box_match_distance;
	u16 palm_box_max_hold;
	bool zone_cleanup_enabled;
	u8 zone_max_radius;
	u8 zone_threshold_numer;
	u8 zone_threshold_shift;
	/* Pressure / touch-major reporting */
	bool pressure_enabled;     /* report PRESSURE + TOUCH_MAJOR        */
	/* Edge compensation */
	bool edge_comp_enabled;    /* edge compensation on/off             */
	s16  edge_boost_pct;       /* signal boost for border pixels (%)   */
	s16  edge_push_q8;         /* max outward push in Q8.8 (128=0.5)  */
	s16  edge_blend_q8;        /* blend range in Q8.8 (512=2 cells)   */
	bool edge_reject_enabled;
	u16 edge_reject_margin;
	s32 edge_reject_min_signal;
	/* Tracking */
	s32  track_dist2_max;      /* max squared distance for match       */
	u8   track_lost_frames;    /* missed frames before slot release    */
	u8   debounce_base;        /* new-slot debounce count              */
	bool track_smoothing;      /* position smoothing on/off            */
	bool track_active_guard;   /* kill stray tracks before 1st stable  */
	u8   track_start_debounce; /* frames to confirm touch_active       */
	s32  track_jump_dist2;     /* position jump → force lift+repress   */
	bool hungarian_enabled;
	u8 debounce_weak_extra;
	u8 debounce_edge_extra;
	s32 debounce_strong_signal;
	bool firmware_edge_fast_start;
	u8 split_peak_confirm_frames;
	s32 split_peak_dist2;
	u8 split_cross_zone_confirm_frames;
	s32 split_cross_zone_dist2;
	s32 track_peak_id_penalty;
	bool ghost_enabled;
	u16 ghost_row_distance;
	u8 ghost_weak_ratio_q8;
	u16 ghost_min_col_distance;
	bool euro_enabled;
	u8 euro_alpha_min_q8;
	u8 euro_alpha_max_q8;
	u16 euro_speed_threshold;
};

void hx_safe_baseline_set_platform_state(struct hx_algo *algo,
					 const struct hx_baseline_platform_state *state);

enum hx_finger_state {
	HX_FINGER_UNKNOWN,
	HX_FINGER_ABSENT,
	HX_FINGER_PRESENT,
};

enum hx_wake_quality_result {
	HX_WAKE_QUALITY_REJECTED = -1,
	HX_WAKE_QUALITY_PENDING = 0,
	HX_WAKE_QUALITY_READY = 1,
	HX_WAKE_QUALITY_USING_SAFE = 2,
	HX_WAKE_QUALITY_PROTECTED = 3,
	HX_WAKE_QUALITY_USING_WORKING = 4,
};

/* ---- Public API ---- */

void hx_algo_init_defaults(struct hx_algo *algo);
bool hx_safe_baseline_queue_trustable(const struct hx_algo *algo);
bool hx_safe_baseline_queue_all_valid(const struct hx_algo *algo);
void hx_algo_clear_live_state(struct hx_algo *algo);
void hx_algo_full_reset(struct hx_algo *algo);
void hx_blrecal_ack(struct hx_algo *algo, bool success);
void hx_algo_begin_wake(struct hx_algo *algo);
int hx_algo_qualify_wake_frame(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state);
enum hx_finger_state
hx_algo_resolve_finger_state(struct hx_algo *algo, const u16 *raw,
			     enum hx_finger_state firmware_state);
bool hx_algo_is_exception_frame(struct hx_algo *algo, const u16 *raw);
bool hx_algo_runtime_baseline_process(struct hx_algo *algo, const u16 *raw,
				      enum hx_finger_state finger_state,
				      bool force);
bool hx_safe_baseline_postprocess(struct hx_algo *algo, const u16 *raw,
				  enum hx_finger_state finger_state);
int hx_algo_process_frame_state(struct hx_algo *algo, const u16 *raw,
				enum hx_finger_state finger_state);

/* Phase 1: preprocessing (baseline subtraction, CMF, IIR) */
void hx_preprocess_frame_state(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state);

#ifdef HX_ALGO_HOST_TEST
/* Convenience wrappers retained only for hardware-independent fixtures. */
void hx_algo_reset_runtime(struct hx_algo *algo);
int hx_algo_process_frame(struct hx_algo *algo, const u16 *raw);
void hx_preprocess_frame(struct hx_algo *algo, const u16 *raw);
#endif

/* Phase 2A: macro-zone detection */
void hx_detect_macro_zones(struct hx_algo *algo);

/* Phase 2B: palm rejection */
void hx_reject_palms(struct hx_algo *algo);

/* Phase 2C: peak detection within surviving zones */
void hx_detect_peaks(struct hx_algo *algo);

/* Phase 2D: zone expansion + weighted centroid → contacts → output positions */
void hx_expand_and_resolve(struct hx_algo *algo,
			    struct input_mt_pos *pos, int *cnt);

/* Phase 3A: Hungarian or greedy tracker update with velocity prediction */
void hx_track_contacts(struct hx_algo *algo,
		       struct input_mt_pos *det, int det_cnt);

/* Count slots that have passed debounce */
int hx_count_stable_tracks(struct hx_algo *algo);

#endif /* HX_ALGO_H */
