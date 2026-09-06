#include "hx-algo-test-common.h"
#include "../touchscreen-hx83121a-dkms/hx-algo-internal.h"

static void test_dynamic_debounce_uses_signal_and_edge(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos det[3] = {{.x = 100, .y = 100},
		{.x = 700, .y = 100}, {.x = 1300, .y = 100}};

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 1;
	algo.contacts[0].signal_sum = 5000;
	algo.contacts[1].signal_sum = 1000;
	algo.contacts[2].signal_sum = 5000;
	algo.contacts[2].is_edge = true;
	algo.contact_count = 3;
	hx_track_contacts(&algo, det, 3);
	assert(algo.tracks[0].debounce == 1);
	assert(algo.tracks[1].debounce == 2);
	assert(algo.tracks[2].debounce == 2);
}

static void test_blsm_property_priority(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	assert(hx_baseline_get_property(&algo, 0, false, true, false,
					 false, false, false) == 0);
	assert(hx_baseline_get_property(&algo, 0, true, true, false,
					 false, false, false) == HX_BLSM_PRPT_TOUCH);
	algo.safe_flags = HX_SAFE_FLAG_SENSOR_BAD;
	assert(hx_baseline_get_property(&algo, 0, true, true, false,
					 false, false, false) == HX_BLSM_PRPT_TOUCH);
	assert(hx_baseline_get_property(&algo, 0, false, true, false,
					 false, false, false) == HX_BLSM_PRPT_RESET);
	assert(hx_baseline_get_property(&algo, 0, false, true, false,
					 true, false, false) == HX_BLSM_PRPT_NOISY);
}

static void test_bliir_cell_update_is_bounded(void)
{
	s32 baseline = (s32)0x7000 << HX_BASELINE_FRACTION_BITS;

	assert(hx_bliir_update_cell_q8(baseline, 0x7100, 4) ==
	       baseline + ((s32)4 << HX_BASELINE_FRACTION_BITS));
	assert(hx_bliir_update_cell_q8(baseline, 0x6f00, 4) ==
	       baseline - ((s32)4 << HX_BASELINE_FRACTION_BITS));
	assert(hx_bliir_update_cell_q8(baseline, 0x7001, 4) == baseline);
}

static void test_safe_state_blocks_learning_before_safe_diff(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.tracks[0].active = true;
	algo.baseline_prev_had_signal = true;
	hx_safe_baseline_check_with_state(&algo, HX_FINGER_PRESENT, false,
					  false, false, false, false);
	assert(algo.safe_flags & HX_SAFE_FLAG_SAFE_DIFF);
}

static void test_safe_state_protects_unmatched_previous_touch(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	/* No current track: the previous valid touch is intentionally unmatched. */
	algo.safe_valid_touch_count = 1;
	algo.safe_abnormal_touch_count = 0;
	hx_safe_baseline_check_with_state(&algo, HX_FINGER_PRESENT, true,
					  false, false, false, false);
	assert(algo.safe_flags & HX_SAFE_FLAG_TOUCH_PROTECTED);
}

static void test_blreset_abnormal_touch_uses_resolved_contacts(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	/* An unfiltered peak must not be counted as a valid touch. */
	algo.peak_count = 1;
	algo.peaks[0].r = 10;
	algo.peaks[0].c = 10;
	algo.contact_count = 0;
	assert(!hx_blreset_check_abnormal_touches(&algo));
	assert(algo.safe_valid_touch_count == 0);
}

static void test_safe_diff_requires_idle_small_signal_population(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.safe_valid_touch_count = 0;
	algo.safe_signal_stable_frames = 5;
	algo.safe_current_positive_nodes = 60;
	hx_safe_baseline_check_with_state(&algo, HX_FINGER_ABSENT, false,
					  false, false, false, false);
	/* The caller's explicit false condition must remain a non-qualified diff. */
	assert(!(algo.safe_flags & HX_SAFE_FLAG_SAFE_DIFF));
}

static void test_safe_signal_predicates(void)
{
	struct hx_algo algo = {};
	u16 flags;

	hx_algo_init_defaults(&algo);
	flags = hx_safe_baseline_signal_flags(&algo, 100, -400, false);
	assert(flags & HX_SAFE_FLAG_VERY_NEGATIVE);
	algo.safe_current_positive_nodes = algo.runtime_noise_total_nodes + 1;
	flags = hx_safe_baseline_signal_flags(&algo, 400, 0, false);
	assert(flags & HX_SAFE_FLAG_VERY_NOISY);
	algo.safe_current_positive_nodes = 60;
	flags = hx_safe_baseline_signal_flags(&algo, 400, 0, false);
	assert(flags & HX_SAFE_FLAG_VERY_NOISY);
	algo.safe_current_positive_nodes = 1;
	flags = hx_safe_baseline_signal_flags(&algo, 50, 0, false);
	assert(flags & HX_SAFE_FLAG_GHOST_MAX);
	flags = hx_safe_baseline_signal_flags(&algo, 200, 0, false);
	assert(!(flags & HX_SAFE_FLAG_GHOST_MAX));
}

static void test_blreset_abnormal_type_votes(void)
{
	struct hx_algo algo = {};
	u16 flags;

	hx_algo_init_defaults(&algo);
	algo.blreset_baseline_state = 1;
	algo.blreset_over_noise_frames = 5;
	flags = hx_blreset_abnormal_types(&algo, 50, -800, 10, 8,
					  false, false, false);
	assert(flags & 1U);
	assert(algo.blreset_abnormal_type_elapsed[0] == 8);
	/* Protection suppresses all abnormal-type votes. */
	flags = hx_blreset_abnormal_types(&algo, 50, -800, 10, 8,
					  false, true, false);
	assert(flags == 0);
	assert(algo.blreset_abnormal_type_elapsed[0] == 0);
	algo.blreset_abnormal_type_elapsed[1] = 320;
	algo.blreset_abnormal_type_flags = 2;
	hx_algo_clear_live_state(&algo);
	assert(algo.blreset_abnormal_type_flags == 0);
	assert(algo.blreset_abnormal_type_elapsed[1] == 0);
}

static void test_safe_judge_uses_spatial_regions(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	algo.safe_baseline_count = 1;
	/* Make the working grid worse in three independent regions while the
	 * all-panel aggregate remains ambiguous.  The official judge considers
	 * these spatial predicates before falling back to one scalar score. */
	for (int region = 0; region < 3; region++) {
		algo.safe_bl2bl_prpt_cmf[region].max = 700;
		algo.safe_bl2bl_prpt_cmf[region].count = 1;
		algo.safe_bl2raw_prpt_cmf[region].max = 0;
		algo.safe_bl2raw_prpt_cmf[region].count = 1;
	}
	assert(hx_safe_baseline_judge_current(&algo) ==
	       HX_SAFE_COMPARE_SAFE_BETTER);
}

static void test_vendor_blreset_baseline_state_machine(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.frame_sequence = 1;
	algo.screen_on_frame_sequence = 0;
	algo.frame_interval_ms = 8;
	for (int frame = 0; frame < 7; frame++)
		hx_blreset_baseline_state_update(&algo, false, false, 0);
	assert(algo.blreset_baseline_state == 1);
	hx_blreset_baseline_state_update(&algo, false, true, 0);
	assert(algo.blreset_baseline_state == 2);
	hx_blreset_baseline_state_update(&algo, false, false, 2);
	assert(algo.blreset_baseline_state == 4);
	/* A noisy release restarts the armed state and marks normal BL pending. */
	hx_blreset_baseline_state_update(&algo, false, false, 31);
	assert(algo.blreset_baseline_state == 1);
	assert(algo.blreset_normal_baseline_ready);
	/* Repeat the protected-release path and hold a clean window for >100 ms. */
	hx_blreset_baseline_state_update(&algo, false, true, 0);
	hx_blreset_baseline_state_update(&algo, false, false, 0);
	hx_blreset_baseline_state_update(&algo, false, false, 0);
	assert(algo.blreset_baseline_state == 8);
	for (int frame = 0; frame < 14; frame++)
		hx_blreset_baseline_state_update(&algo, false, false, 0);
	assert(algo.blreset_baseline_state == 16);
	assert(algo.blreset_clean_baseline_captured);
	assert(algo.normal_baseline_valid);
}

static void test_blreset_wakeup_temporal_gate(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.frame_interval_ms = 8;
	for (int frame = 0; frame < 20; frame++)
		assert(!hx_blreset_wakeup_process(&algo, true));
	assert(hx_blreset_wakeup_process(&algo, true));
	assert(algo.blreset_wake_triggered);
	assert(!hx_blreset_wakeup_process(&algo, false));
	assert(algo.blreset_wake_abnormal_elapsed_ms == 0);
}

static void test_blreset_clean_baseline_divergence(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.normal_baseline_valid = true;
	algo.blreset_clean_baseline_captured = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.normal_baseline_q8[i] = (s32)0x7fee << 8;
		raw[i] = 0x7fee;
	}
	assert(hx_blreset_check_clean_baseline(&algo, raw));
	for (int i = 0; i < 4; i++)
		raw[i] += 101;
	assert(!hx_blreset_check_clean_baseline(&algo, raw));
	assert(hx_blreset_classify_frame(&algo, raw, 0, 0, 0, false,
					 false, false) & HX_SAFE_FLAG_SENSOR_BAD);
}

static void test_blreset_dirty_baseline_temporal_gate(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.frame_interval_ms = 8;
	/* Protection or a missing clean-baseline match clears the timer. */
	assert(!hx_blreset_dirty_process(&algo, true, true, true, true));
	assert(algo.blreset_dirty_elapsed_ms == 0);
	for (int frame = 0; frame < 38; frame++)
		assert(!hx_blreset_dirty_process(&algo, true, false, true, true));
	assert(hx_blreset_dirty_process(&algo, true, false, true, true));
	assert(algo.blreset_dirty_triggered);
	assert(algo.blreset_dirty_elapsed_ms == 0);
}

static void test_blreset_and_stage_actions(void)
{
	struct hx_algo algo = {};
	struct hx_algo recal = {};
	u16 raw[HX_PIXELS];
	u16 recal_raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7000 << HX_BASELINE_FRACTION_BITS;
	algo.baseline_stage = HX_BLSM_RESET;
	hx_baseline_stage_process(&algo, raw, raw, 0, 0, false, true);
	assert(algo.baseline_stage_reset);
	assert((algo.baseline_q8[10] >> HX_BASELINE_FRACTION_BITS) == 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7000 << HX_BASELINE_FRACTION_BITS;
	algo.baseline_stage = HX_BLSM_NOISY_RESET;
	hx_baseline_stage_process(&algo, raw, raw, 900, -900, false, false);
	assert(algo.baseline_stage_reset);
	assert(algo.baseline_stage_action == 2);
	assert((algo.baseline_q8[10] >> HX_BASELINE_FRACTION_BITS) == 0x7fee);
	algo.baseline_prev_stage = HX_BLSM_TOUCH;
	algo.baseline_stage = HX_BLSM_FORCED;
	hx_baseline_stage_process(&algo, raw, raw, 0, 0, false, true);
	assert(algo.baseline_stage_action == 0);
	algo.baseline_stage = HX_BLSM_DEBOUNCE;
	hx_baseline_stage_process(&algo, raw, raw, 0, 0, false, true);
	assert(algo.baseline_stage_force_update);
	for (int stage = HX_BLSM_PRE_TOUCH; stage <= HX_BLSM_TOUCH_RELEASE;
		stage++) {
		algo.baseline_stage = stage;
		hx_baseline_stage_process(&algo, raw, raw, 0, 0, false, true);
		assert(algo.baseline_stage_action == 0);
	}
	for (int stage = HX_BLSM_POST_TOUCH;
		stage <= HX_BLSM_POST_TOUCH_ALT; stage++) {
		algo.baseline_stage = stage;
		hx_baseline_stage_process(&algo, raw, raw, 0, 0, false, true);
		assert(algo.baseline_stage_action == 1);
		assert(algo.baseline_stage_update_step ==
		       algo.baseline_peak_threshold / 64);
	}

	/* A compact positive island with firmware no-finger is a ghost candidate,
	 * not a reason to create a touch or reset the whole panel. */
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7fee << HX_BASELINE_FRACTION_BITS;
	raw[100] += 500;
	assert(hx_blreset_classify_frame(&algo, raw, 0, 50, 0, false,
					 false, false) & HX_SAFE_FLAG_GHOST_MAX);
	algo.safe_current_negative_nodes = algo.wake_max_unstable_nodes + 1;
	assert(hx_blreset_is_dirty_baseline(&algo, 600, -400, -100));
	/* The frame classifier feeds the same flags consumed by BLSM/SafeBaseline
	 * in the live preprocess path. */
	assert(hx_blreset_classify_frame(&algo, raw, 0, 0, -500, false,
					 false, false) & HX_SAFE_FLAG_VERY_NEGATIVE);
	/* A panel-wide raw jump with no reported finger is an AFE jump, not a
	 * candidate baseline sample. */
	for (int i = 0; i < HX_PIXELS; i++)
		algo.prev_raw[i] = 0x7fee;
	algo.prev_raw_valid = true;
	for (int i = 0; i < HX_PIXELS; i++)
		raw[i] = 0x7fee + algo.wake_raw_jump_threshold + 1;
	assert(hx_blreset_classify_frame(&algo, raw, 0, 0, 0, false,
					 false, false) & HX_SAFE_FLAG_VERY_NOISY);
	for (int i = 0; i < 9; i++)
		hx_blreset_classify_frame(&algo, raw, 0, 0, 0, false,
					 false, false);
	assert(hx_blreset_classify_frame(&algo, raw, 0, 0, 0, false,
					 false, false) & HX_SAFE_FLAG_SENSOR_BAD);
	/* SHB captures a stable raw snapshot, then emits one action after each
	 * five-second hold window. */
	memset(algo.prev_raw, 0, sizeof(algo.prev_raw));
	algo.frame_sequence = 0;
	algo.baseline_stage = HX_BLSM_NO_TOUCH_STABLE;
	hx_blsm_shb_process(&algo, raw, true);
	algo.frame_sequence = 626;
	hx_blsm_shb_process(&algo, raw, false);
	assert(algo.shb_flags & 1);
	assert(algo.shb_capture_frame == 626);
	algo.frame_sequence = 627;
	hx_blsm_shb_process(&algo, raw, false);
	assert(algo.shb_flags == 0);
	algo.frame_sequence = 1252;
	hx_blsm_shb_process(&algo, raw, false);
	assert((algo.shb_flags & 3) == 3);
	assert(hx_blsm_shb_consume_action(&algo) == 2);
	assert(!(algo.shb_flags & 2));

	hx_algo_init_defaults(&recal);
	init_raw(recal_raw, 0x8000 + 5000);
	for (int i = 0; i < 15; i++)
		assert(!hx_blrecal_process(&recal, recal_raw, 0, 0,
					     false, false));
	assert(hx_blrecal_process(&recal, recal_raw, 0, 0, false, false));
	assert(recal.blrecal_requested);
	/* Requests are latched until the hardware-facing owner acknowledges
	 * them; repeated frames must not retrigger the weak-signal stage. */
	assert(!hx_blrecal_process(&recal, recal_raw, 0, 0, false, false));
	hx_blrecal_ack(&recal, true);
	assert(!recal.blrecal_requested);
}

static void test_blreset_temporal_trigger_and_rearm(void)
{
	struct hx_algo algo = {};
	struct hx_algo policy = {};
	int i;

	hx_algo_init_defaults(&algo);
	/* Eight milliseconds per frame and the vendor's 500 ms abnormal
	 * duration: transient flags must remain below the trigger boundary. */
	for (i = 0; i < 62; i++)
		assert(!hx_blreset_process(&algo, 1U << 8));
	assert(algo.blreset_state == HX_BLRESET_DEBOUNCE);
	assert(!algo.blreset_triggered);
	assert(hx_blreset_process(&algo, 1U << 8));
	assert(algo.blreset_state == HX_BLRESET_TRIGGERED);
	assert(algo.blreset_trigger_count == 1);
	/* A trigger is consumed by the reset policy in the live path.  A clean
	 * interval then clears the timer and permits one future trigger. */
	algo.blreset_triggered = false;
	for (i = 0; i < 40; i++)
		hx_blreset_process(&algo, 0);
	assert(algo.blreset_state == HX_BLRESET_IDLE);
	assert(algo.blreset_reason_elapsed_ms == 0);
	assert(!hx_blreset_process(&algo, 0));

	/* A debounced trigger is consumed by the SafeBaseline policy as a
	 * working-grid recovery, never as an immediate queue invalidation. */
	hx_algo_init_defaults(&policy);
	policy.safe_baseline_valid = true;
	policy.safe_baseline_count = 1;
	policy.safe_baselines[0].valid = true;
	policy.safe_baselines[0].confidence = HX_SAFE_BASELINE_CONFIRMED;
	policy.blreset_triggered = true;
	assert(hx_safe_baseline_reset_policy(&policy,
		HX_SAFE_COMPARE_BOTH_VALID, false, HX_FINGER_UNKNOWN) ==
		HX_SAFE_RESET_WORKING);
	assert(!policy.blreset_triggered);

	/* Per-reason thresholds override the generic fallback, matching
	 * SafeBaseline_ResetStatisticsUnit's table-driven timing. */
	policy.safe_reset_reason_trigger_frames = 3;
	policy.safe_reset_reason_frames[8] = 2;
	assert(hx_blreset_process(&policy, 1U << 8));
}

static void add_bottom_edge_blob(u16 raw[HX_PIXELS], int c, u16 signal)
{
	raw[(HX_ROWS - 1) * HX_COLS + c] = 0x7fee + signal;
	raw[(HX_ROWS - 1) * HX_COLS + c + 1] = 0x7fee + signal - 100;
	raw[(HX_ROWS - 2) * HX_COLS + c] = 0x7fee + signal - 200;
	raw[(HX_ROWS - 2) * HX_COLS + c + 1] = 0x7fee + signal - 300;
}

static void init_edge_start_algo(struct hx_algo *algo)
{
	hx_algo_init_defaults(algo);
	algo->cmf_enabled = false;
	algo->iir_enabled = false;
	algo->palm_enabled = false;
	algo->track_smoothing = false;
}

static void test_firmware_rising_edge_start_bypasses_debounce(void)
{
	struct hx_algo fast = {}, disabled = {}, sustained = {}, multiple = {};
	u16 raw[HX_PIXELS];
	int active = 0;

	init_raw(raw, 0x7fee);
	add_bottom_edge_blob(raw, 20, 4000);
	init_edge_start_algo(&fast);
	assert(hx_algo_process_frame_state(&fast, raw,
					   HX_FINGER_PRESENT) == 1);
	assert(fast.contact_count == 1 && fast.contacts[0].is_edge);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (fast.tracks[i].active) {
			active++;
			assert(fast.tracks[i].debounce == 0);
			assert(fast.tracks[i].reported);
		}
	assert(active == 1);
	assert(fast.diag_fast_edge_starts == 1);

	init_edge_start_algo(&disabled);
	disabled.firmware_edge_fast_start = false;
	assert(hx_algo_process_frame_state(&disabled, raw,
					   HX_FINGER_PRESENT) == 0);
	assert(disabled.tracks[0].active);
	assert(disabled.tracks[0].debounce == 2);
	assert(!disabled.tracks[0].reported);
	assert(disabled.diag_fast_edge_starts == 0);

	/* A sustained global firmware-present state must not validate a later
	 * edge candidate, because another finger could be keeping it asserted. */
	init_edge_start_algo(&sustained);
	sustained.firmware_finger_present = true;
	assert(hx_algo_process_frame_state(&sustained, raw,
					   HX_FINGER_PRESENT) == 0);
	assert(sustained.tracks[0].active);
	assert(sustained.tracks[0].debounce == 2);
	assert(!sustained.tracks[0].reported);
	assert(sustained.diag_fast_edge_starts == 0);

	/* Even on the firmware rising edge, ambiguous multiple candidates retain
	 * the normal per-contact debounce. */
	init_raw(raw, 0x7fee);
	add_bottom_edge_blob(raw, 8, 4000);
	add_bottom_edge_blob(raw, 42, 4000);
	init_edge_start_algo(&multiple);
	assert(hx_algo_process_frame_state(&multiple, raw,
					   HX_FINGER_PRESENT) == 0);
	assert(multiple.contact_count == 2);
	assert(multiple.diag_fast_edge_starts == 0);
}

static void test_firmware_rising_compact_near_edge_starts_immediately(void)
{
	struct hx_algo algo = {}, sustained = {}, weak = {};
	u16 raw[HX_PIXELS];
	int active = 0;

	init_edge_start_algo(&algo);
	algo.peak_z8_enabled = false;
	init_raw(raw, 0x7fee);
	/* The observed wake failure begins at r36/r37: close to the physical
	 * boundary, but not on the outermost row.  Its first footprint is only one
	 * cell and therefore used to be removed by peak_macro_min_area. */
	raw[(HX_ROWS - 4) * HX_COLS + 20] = 0x7fee + 2000;
	assert(hx_algo_process_frame_state(&algo, raw,
					   HX_FINGER_PRESENT) == 1);
	assert(algo.contact_count == 1);
	assert(algo.contacts[0].fast_start_candidate);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo.tracks[i].active) {
			active++;
			assert(algo.tracks[i].reported);
		}
	assert(active == 1);
	assert(algo.diag_fast_edge_starts == 1);

	/* A compact candidate cannot bootstrap from a sustained global finger bit
	 * or below the independent strong-signal threshold. */
	init_edge_start_algo(&sustained);
	sustained.peak_z8_enabled = false;
	sustained.firmware_finger_present = true;
	assert(hx_algo_process_frame_state(&sustained, raw,
					   HX_FINGER_PRESENT) == 0);
	assert(sustained.contact_count == 0);
	assert(sustained.diag_fast_edge_starts == 0);

	init_edge_start_algo(&weak);
	weak.peak_z8_enabled = false;
	init_raw(raw, 0x7fee);
	raw[(HX_ROWS - 4) * HX_COLS + 20] = 0x7fee + 1200;
	assert(hx_algo_process_frame_state(&weak, raw,
					   HX_FINGER_PRESENT) == 0);
	assert(weak.contact_count == 0);
	assert(weak.diag_fast_edge_starts == 0);
}

static void test_one_euro_filters_jitter_but_tracks_motion(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos det = {.x = 1000, .y = 700};
	int slot = -1;

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	hx_track_contacts(&algo, &det, 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo.tracks[i].active)
			slot = i;
	assert(slot >= 0);
	det.x = 1004;
	hx_track_contacts(&algo, &det, 1);
	assert(algo.tracks[slot].x > 1000 && algo.tracks[slot].x < 1004);
	det.x = 1200;
	hx_track_contacts(&algo, &det, 1);
	assert(algo.tracks[slot].x > 1150 && algo.tracks[slot].x < 1200);
}

static void test_integrated_ghost_filter(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	init_integrated_algo(&algo);
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 8, 2200);
	add_blob(raw, 10, 30, 500);
	assert(hx_algo_process_frame(&algo, raw) == 1);

	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 2200);
	assert(hx_algo_process_frame(&algo, raw) == 1);
}

static void test_confirmed_lift_releases_immediately(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	init_integrated_algo(&algo);
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 2200);
	assert(hx_algo_process_frame_state(&algo, raw,
					   HX_FINGER_PRESENT) == 1);
	assert(algo.tracks[0].reported);

	/* Keep the same residual matrix on purpose: an authoritative firmware
	 * lift must beat both residual signal and track_lost_frames. */
	assert(hx_algo_process_frame_state(&algo, raw,
					   HX_FINGER_ABSENT) == 0);
	assert(!algo.tracks[0].active);
}

static void test_small_peak_only_continues_reported_track(void)
{
	struct hx_algo continued = {}, fresh = {};
	u16 raw[HX_PIXELS];
	int continued_slot = -1;

	init_integrated_algo(&continued);
	continued.peak_z8_enabled = false;
	continued.peak_saddle_enabled = false;
	continued.peak_continue_min_signal = 800;
	continued.touch_active = true;
	/* Bootstrap with a normal zone, then collapse it to one strong cell. */
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 2200);
	assert(hx_algo_process_frame_state(&continued, raw,
					   HX_FINGER_PRESENT) == 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (continued.tracks[i].active)
			continued_slot = i;
	assert(continued_slot >= 0);
	init_raw(raw, 0x7fee);
	raw[10 * HX_COLS + 13] = 0x7fee + 1800;
	assert(hx_algo_process_frame_state(&continued, raw,
					   HX_FINGER_PRESENT) == 1);
	assert(continued.diag_small_peak_continued == 1);
	assert(continued.contact_count == 1);
	assert(continued.contacts[0].continuation_only);
	assert(continued.contacts[0].continuation_track_slot == continued_slot);

	/* An area-two footprint is also eligible and remains bound to the same
	 * already-reported slot. */
	init_raw(raw, 0x7fee);
	raw[10 * HX_COLS + 13] = 0x7fee + 1800;
	raw[10 * HX_COLS + 14] = 0x7fee + 1700;
	assert(hx_algo_process_frame_state(&continued, raw,
					   HX_FINGER_PRESENT) == 1);
	assert(continued.diag_small_peak_continued == 2);
	assert(continued.contact_count == 1);
	assert(continued.contacts[0].continuation_only);
	assert(continued.contacts[0].continuation_track_slot == continued_slot);

	init_integrated_algo(&fresh);
	fresh.peak_z8_enabled = false;
	fresh.peak_saddle_enabled = false;
	fresh.peak_continue_min_signal = 800;
	assert(hx_algo_process_frame_state(&fresh, raw,
					   HX_FINGER_PRESENT) == 0);
	assert(fresh.diag_small_peak_rejected == 1);
}

static void test_small_peak_rejects_ambiguous_tracks(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	const int peak_x = (13 * 256 + 128) / 6;
	const int peak_y = 5 * (10 * 256 + 128) / 32;

	init_integrated_algo(&algo);
	algo.peak_z8_enabled = false;
	algo.peak_saddle_enabled = false;
	algo.peak_continue_min_signal = 800;
	algo.touch_active = true;
	/* Both reported tracks fall within the continuation radius. */
	algo.tracks[0].active = true;
	algo.tracks[0].reported = true;
	algo.tracks[0].x = peak_x - 100;
	algo.tracks[0].y = peak_y;
	algo.tracks[1].active = true;
	algo.tracks[1].reported = true;
	algo.tracks[1].x = peak_x + 100;
	algo.tracks[1].y = peak_y;
	init_raw(raw, 0x7fee);
	raw[10 * HX_COLS + 13] = 0x7fee + 1800;
	assert(hx_algo_process_frame_state(&algo, raw,
					   HX_FINGER_PRESENT) == 2);
	assert(algo.contact_count == 0);
	assert(algo.diag_small_peak_continued == 0);
	assert(algo.diag_small_peak_rejected == 1);
}

static void test_weak_small_peak_continues_only_single_reported_track(void)
{
	struct hx_algo single = {}, multi = {};
	u16 raw[HX_PIXELS];
	int slot = -1;

	init_integrated_algo(&single);
	single.peak_z8_enabled = false;
	single.touch_active = true;
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 2200);
	assert(hx_algo_process_frame_state(&single, raw,
					   HX_FINGER_PRESENT) == 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (single.tracks[i].active)
			slot = i;
	assert(slot >= 0);

	init_raw(raw, 0x7fee);
	raw[10 * HX_COLS + 13] = 0x7fee + 720;
	assert(hx_algo_process_frame_state(&single, raw,
					   HX_FINGER_PRESENT) == 1);
	assert(single.tracks[slot].missed == 0);
	assert(single.diag_weak_peak_continued == 1);

	init_integrated_algo(&multi);
	multi.peak_z8_enabled = false;
	multi.touch_active = true;
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 2200);
	add_blob(raw, 25, 40, 2200);
	assert(hx_algo_process_frame_state(&multi, raw,
					   HX_FINGER_PRESENT) == 2);
	init_raw(raw, 0x7fee);
	raw[10 * HX_COLS + 13] = 0x7fee + 720;
	assert(hx_algo_process_frame_state(&multi, raw,
					   HX_FINGER_PRESENT) == 2);
	assert(multi.contact_count == 0);
	assert(multi.diag_weak_peak_continued == 0);
	assert(multi.diag_small_peak_rejected == 1);
}

static void test_continuation_peak_is_bound_to_eligible_track(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos initial[2] = {
		{ .x = 100, .y = 100 },
		{ .x = 350, .y = 100 },
	};
	struct input_mt_pos small = { .x = 300, .y = 100 };
	int slot_a = -1, slot_b = -1;

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 0;
	algo.track_smoothing = false;
	algo.touch_active = true;
	hx_track_contacts(&algo, initial, 2);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo.tracks[i].x == initial[0].x)
			slot_a = i;
		else if (algo.tracks[i].x == initial[1].x)
			slot_b = i;
	}
	assert(slot_a >= 0 && slot_b >= 0);
	/* An active but not-yet-reported track may be closer to the small peak.
	 * It was not eligible during continuation classification and must not
	 * steal the candidate from the reported target. */
	algo.tracks[slot_b].reported = false;
	algo.contacts[0].continuation_only = true;
	algo.contacts[0].continuation_track_slot = slot_a;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &small, 1);
	assert(algo.tracks[slot_a].x == small.x);
	assert(algo.tracks[slot_a].missed == 0);
	assert(!algo.tracks[slot_b].active);
}

static void test_regular_peak_beats_continuation_for_same_track(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos initial[2] = {
		{ .x = 100, .y = 100 },
		{ .x = 600, .y = 100 },
	};
	struct input_mt_pos detections[2] = {
		{ .x = 105, .y = 100 },
		{ .x = 300, .y = 100 },
	};
	int slot_a = -1, slot_b = -1;

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 0;
	algo.track_smoothing = false;
	algo.touch_active = true;
	hx_track_contacts(&algo, initial, 2);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo.tracks[i].x == initial[0].x)
			slot_a = i;
		else if (algo.tracks[i].x == initial[1].x)
			slot_b = i;
	}
	assert(slot_a >= 0 && slot_b >= 0);
	algo.contacts[0].continuation_only = false;
	algo.contacts[0].continuation_track_slot = -1;
	algo.contacts[1].continuation_only = true;
	algo.contacts[1].continuation_track_slot = slot_a;
	algo.contact_count = 2;
	hx_track_contacts(&algo, detections, 2);
	assert(algo.tracks[slot_a].x == detections[0].x);
	assert(algo.tracks[slot_a].missed == 0);
	assert(algo.tracks[slot_b].x == initial[1].x);
	assert(algo.tracks[slot_b].missed == 1);
	assert(hx_count_stable_tracks(&algo) == 2);
}

static void test_edge_compensation_and_weak_rejection(void)
{
	struct hx_algo comp = {}, plain = {}, reject = {};
	u16 raw[HX_PIXELS];
	struct input_mt_pos pos[HIMAX_MAX_TOUCH];
	int count;

	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 0, 1000);
	init_integrated_algo(&comp);
	init_integrated_algo(&plain);
	plain.edge_comp_enabled = false;
	run_detection(&comp, raw);
	run_detection(&plain, raw);
	assert(comp.contact_count == 1 && plain.contact_count == 1);
	assert(comp.contacts[0].x < plain.contacts[0].x);

	init_integrated_algo(&reject);
	reject.edge_reject_enabled = true;
	reject.edge_reject_margin = 256;
	reject.edge_reject_min_signal = 100000;
	hx_preprocess_frame(&reject, raw);
	hx_detect_macro_zones(&reject);
	hx_reject_palms(&reject);
	hx_detect_peaks(&reject);
	hx_expand_and_resolve(&reject, pos, &count);
	assert(count == 0);
}


void run_runtime_tests(void)
{
	test_blreset_and_stage_actions();
	test_blreset_temporal_trigger_and_rearm();
	test_blsm_property_priority();
	test_bliir_cell_update_is_bounded();
	test_safe_state_blocks_learning_before_safe_diff();
	test_safe_state_protects_unmatched_previous_touch();
	test_blreset_abnormal_touch_uses_resolved_contacts();
	test_safe_diff_requires_idle_small_signal_population();
	test_safe_signal_predicates();
	test_blreset_abnormal_type_votes();
	test_safe_judge_uses_spatial_regions();
	test_vendor_blreset_baseline_state_machine();
	test_blreset_wakeup_temporal_gate();
	test_blreset_clean_baseline_divergence();
	test_blreset_dirty_baseline_temporal_gate();
	test_dynamic_debounce_uses_signal_and_edge();
	test_firmware_rising_edge_start_bypasses_debounce();
	test_firmware_rising_compact_near_edge_starts_immediately();
	test_one_euro_filters_jitter_but_tracks_motion();
	test_integrated_ghost_filter();
	test_confirmed_lift_releases_immediately();
	test_small_peak_only_continues_reported_track();
	test_small_peak_rejects_ambiguous_tracks();
	test_weak_small_peak_continues_only_single_reported_track();
	test_continuation_peak_is_bound_to_eligible_track();
	test_regular_peak_beats_continuation_for_same_track();
	test_edge_compensation_and_weak_rejection();
}
