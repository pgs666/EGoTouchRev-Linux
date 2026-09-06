#include "hx-algo-test-common.h"
#include "../touchscreen-hx83121a-dkms/hx-algo-internal.h"
#include "../touchscreen-hx83121a-dkms/hx-panel-profile.h"

static void test_strongest_candidates_survive(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 weakest_kept = INT_MAX;

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	/* This test exercises candidate capacity, not BLSM noise-reset policy. */
	algo.baseline_enabled = false;
	algo.baseline_initial = 0x7fee;
	algo.palm_enabled = false;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < 12; i++)
		add_blob(raw, 2 + (i / 6) * 8, 2 + (i % 6) * 8,
			 400 + i * 100);
	run_detection(&algo, raw);
	assert(algo.contact_count == HIMAX_MAX_TOUCH);
	for (int i = 0; i < algo.contact_count; i++)
		if (algo.contacts[i].signal_sum < weakest_kept)
			weakest_kept = algo.contacts[i].signal_sum;
	/* The 400/500 blobs must be the discarded pair. */
	assert(weakest_kept >= 4 * 600 - 6);
}

static void test_strong_late_macro_zone_survives_capacity_cut(void)
{
	struct hx_algo algo = {};
	bool found_late = false;

	hx_algo_init_defaults(&algo);
	memset(algo.frame, 0, sizeof(algo.frame));
	for (int i = 0; i < HX_MAX_ZONES; i++)
		algo.frame[2][i * 3] = 300 + i;
	algo.frame[35][55] = 2000;

	hx_detect_macro_zones(&algo);
	assert(algo.zone_count == HX_MAX_ZONES);
	assert(algo.zone_arena_used == HX_MAX_ZONES + 1);
	assert(algo.zones[0].signal_sum == 2000);
	for (int i = 0; i < algo.zone_count; i++)
		if (algo.zones[i].min_r == 35 && algo.zones[i].min_c == 55)
			found_late = true;
	assert(found_late);
	/* The weakest early scan-order island is the one that was displaced. */
	assert(algo.zones[algo.zone_count - 1].signal_sum == 301);
}

static void test_compact_strong_finger_is_not_a_palm(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	init_raw(raw, 0x7fee);
	/* Integrated signal exceeds the legacy 80000 one-shot reject rule. */
	for (int r = 12; r < 17; r++)
		for (int c = 20; c < 25; c++)
			raw[r * HX_COLS + c] = 0x7fee + 3500;
	raw[14 * HX_COLS + 22] = 0x7fee + 12000;
	run_detection(&algo, raw);
	assert(algo.contact_count > 0);
}

static void test_flat_large_contact_is_a_palm(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	struct input_mt_pos pos[HIMAX_MAX_TOUCH];
	int count = 0;

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	init_raw(raw, 0x7fee);
	for (int r = 8; r < 18; r++)
		for (int c = 12; c < 22; c++)
			raw[r * HX_COLS + c] = 0x7fee + 1200;
	hx_preprocess_frame(&algo, raw);
	hx_detect_macro_zones(&algo);
	hx_reject_palms(&algo);
	hx_detect_peaks(&algo);
	hx_expand_and_resolve(&algo, pos, &count);
	assert(count == 0);
}

static void test_adaptive_baseline_follows_panel_drift(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	init_raw(raw, 0x7fee + 120);
	/* The positive-drift tier intentionally uses the Windows default 1/128
	 * IIR rate, so allow roughly 2.5 time constants to settle. */
	for (int i = 0; i < 320; i++)
		hx_preprocess_frame(&algo, raw);
	assert(abs(algo.frame[10][10]) < 20);

	raw[10 * HX_COLS + 10] = 0x7fee + 1200;
	for (int i = 0; i < 20; i++)
		hx_preprocess_frame(&algo, raw);
	assert(algo.frame[10][10] > 1000);
}

static void test_disabled_iir_does_not_maintain_history(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	algo.iir_initialized = true;
	algo.iir_history[10][10] = 1234;
	init_raw(raw, 0x7fee);
	hx_preprocess_frame(&algo, raw);
	assert(!algo.iir_initialized);
	assert(algo.iir_history[10][10] == 1234);

	algo.iir_enabled = true;
	add_blob(raw, 10, 10, 1800);
	hx_preprocess_frame(&algo, raw);
	assert(algo.iir_initialized);
	assert(algo.iir_history[10][10] == algo.frame[10][10]);
}

static void test_uniform_startup_offset_is_not_a_fullscreen_palm(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	init_raw(raw, 0x7fee + 600);
	hx_preprocess_frame(&algo, raw);
	hx_detect_macro_zones(&algo);
	hx_reject_palms(&algo);
	assert(!algo.baseline_prev_had_signal);
	assert(algo.zone_count == 0);
	assert(algo.palm_box_count == 0);

	add_blob(raw, 12, 20, 1800 + 600);
	run_detection(&algo, raw);
	assert(algo.baseline_prev_had_signal);
	assert(algo.contact_count == 1);
}

static void test_firmware_finger_state_drives_baseline_dispatch(void)
{
	struct hx_algo present = {}, absent = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&present);
	hx_algo_init_defaults(&absent);
	present.cmf_enabled = false;
	present.iir_enabled = false;
	absent.cmf_enabled = false;
	absent.iir_enabled = false;
	init_raw(raw, 0x7fee);
	add_blob(raw, 12, 20, 1000);

	hx_preprocess_frame_state(&present, raw, HX_FINGER_PRESENT);
	assert(present.diag_has_signal);
	assert(present.frame[12][20] > 900);

	hx_preprocess_frame_state(&absent, raw, HX_FINGER_ABSENT);
	assert(!absent.diag_has_signal);
	assert(absent.frame[12][20] == 0);
}

static void test_native_master_frame_status_parser(void)
{
	u8 frame[HX_FRAME_HEADER_BYTES + HX_FRAME_MATRIX_BYTES +
		 HX_FRAME_SUFFIX_BYTES] = {};
	struct hx_frame_status status;
	u8 *suffix = frame + HX_FRAME_HEADER_BYTES + HX_FRAME_MATRIX_BYTES;

	frame[0] = 0x5a;
	frame[1] = 0xa5;
	suffix[6] = 0xbb;
	suffix[7] = 0x00;
	suffix[108] = 0xff;
	suffix[110] = 0xff;
	assert(hx_parse_master_frame_status(frame, sizeof(frame), &status));
	assert(!status.retry && !status.has_finger);

	suffix[108] = 0x24;
	assert(hx_parse_master_frame_status(frame, sizeof(frame), &status));
	assert(status.has_finger);

	suffix[0] = 1;
	assert(hx_parse_master_frame_status(frame, sizeof(frame), &status));
	assert(status.retry);

	/* A suffix-looking marker at the Windows seven-byte offset must not make
	 * a shifted/non-master Linux event-stack frame valid. */
	memset(frame, 0, sizeof(frame));
	frame[HX_FRAME_HEADER_BYTES + 3 + HX_FRAME_MATRIX_BYTES + 6] = 0xbb;
	assert(!hx_parse_master_frame_status(frame, sizeof(frame), &status));
}

static void test_background_residuals_do_not_exhaust_zones_after_idle(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	algo.palm_enabled = false;
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.track_start_debounce = 0;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < 600; i++)
		hx_algo_process_frame(&algo, raw);
	assert(algo.normal_baseline_valid);

	/* Residual islands below the Windows freeze threshold must not consume
	 * all 20 macro-zone slots before the real contact later in scan order. */
	for (int r = 1; r < 10; r++)
		for (int c = 1; c < 55; c += 2)
			raw[r * HX_COLS + c] = 0x7fee + 285;
	hx_preprocess_frame(&algo, raw);
	hx_detect_macro_zones(&algo);
	assert(algo.zone_count == 0);

	init_raw(raw, 0x7fee);
	add_blob(raw, 32, 52, 1200);
	assert(hx_algo_process_frame(&algo, raw) > 0);
	assert(algo.diag_frame_max >= 1100);
	assert(algo.diag_contacts_post_filter == 1);
}

static void test_four_frame_gap_relinks(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos point = {.x = 1000, .y = 700};
	int original_slot = -1;

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 0;
	algo.touch_active = true;
	hx_track_contacts(&algo, &point, 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo.tracks[i].active)
			original_slot = i;
	assert(original_slot >= 0);
	for (int i = 0; i < 4; i++)
		hx_track_contacts(&algo, NULL, 0);
	assert(algo.tracks[original_slot].active);
	point.x += 20;
	hx_track_contacts(&algo, &point, 1);
	assert(algo.tracks[original_slot].active);
	assert(algo.tracks[original_slot].missed == 0);
}

static void test_large_zone_uses_full_arena(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	init_raw(raw, 0x7fee);
	for (int r = 5; r < 25; r++)
		for (int c = 10; c < 40; c++)
			raw[r * HX_COLS + c] = 0x7fee + 800;
	hx_preprocess_frame(&algo, raw);
	hx_detect_macro_zones(&algo);
	assert(algo.zone_count == 1);
	assert(algo.zones[0].area == 600);
	assert(algo.zone_arena_used == 600);
}

static void test_palm_box_persists_while_domain_inside(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	struct input_mt_pos pos[HIMAX_MAX_TOUCH];
	int count = 0;

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	init_raw(raw, 0x7fee);
	for (int r = 10; r < 20; r++)
		for (int c = 20; c < 30; c++)
			raw[r * HX_COLS + c] = 0x7fee + 1000;
	run_detection(&algo, raw);
	assert(algo.palm_box_count == 1);
	assert(algo.contact_count == 0);

	init_raw(raw, 0x7fee);
	add_blob(raw, 14, 24, 3000);
	run_detection(&algo, raw);
	assert(algo.palm_box_count == 1);
	assert(algo.contact_count == 0);

	init_raw(raw, 0x7fee);
	run_detection(&algo, raw);
	assert(algo.palm_box_count == 0);
	(void)pos;
	(void)count;
}

static void test_saddle_splits_two_peaks(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
	algo.palm_enabled = false;
	algo.peak_saddle_radius = 8;
	init_raw(raw, 0x7fee);
	for (int c = 19; c <= 26; c++)
		raw[15 * HX_COLS + c] = 0x7fee + 400;
	raw[15 * HX_COLS + 19] = 0x7fee + 1800;
	raw[15 * HX_COLS + 26] = 0x7fee + 1700;
	/* Give each peak off-axis support so the Z8 filter keeps it without
	 * raising the saddle sampled along the line between the two maxima. */
	raw[14 * HX_COLS + 19] = 0x7fee + 400;
	raw[16 * HX_COLS + 19] = 0x7fee + 400;
	raw[14 * HX_COLS + 26] = 0x7fee + 400;
	raw[16 * HX_COLS + 26] = 0x7fee + 400;
	run_detection(&algo, raw);
	assert(algo.peak_count == 2);
	assert(algo.contact_count == 2);
}

static void test_live_clear_preserves_baseline_and_configuration(void)
{
	struct hx_algo algo = {};
	s32 baseline;

	hx_algo_init_defaults(&algo);
	algo.peak_threshold = 777;
	algo.baseline_initialized = true;
	algo.baseline_q8[123] = 0x8123 << 8;
	baseline = algo.baseline_q8[123];
	algo.iir_initialized = true;
	algo.palm_box_count = 1;
	algo.tracks[3].active = true;
	algo.tracks[3].reported = true;
	hx_algo_clear_live_state(&algo);
	assert(algo.peak_threshold == 777);
	assert(algo.baseline_initialized);
	assert(algo.baseline_q8[123] == baseline);
	assert(!algo.iir_initialized);
	assert(algo.palm_box_count == 0);
	assert(!algo.tracks[3].active);
	assert(algo.live_clear_count == 1);
}

static void test_full_reset_clears_baseline(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.baseline_q8[123] = 0x8123 << 8;
	hx_algo_full_reset(&algo);
	assert(!algo.baseline_initialized);
	assert(algo.baseline_q8[123] == 0);
	assert(algo.full_reset_count == 1);
	assert(algo.live_clear_count == 0);
}

static void test_wake_quality_commits_stable_no_finger_baseline(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	init_raw(raw, 0x8050);
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_stable_frames; frame++) {
		raw[10] = 0x8050 + frame;
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_ABSENT);
	}
	assert(result == HX_WAKE_QUALITY_READY);
	assert(algo.baseline_initialized);
	assert(algo.safe_baseline_valid);
	assert(!algo.wake_qualifying);
	assert(algo.baseline_q8[100] == (s32)0x8050 << 8);
	assert(algo.safe_baseline_q8[100] == algo.baseline_q8[100]);
	assert(algo.wake_baseline_commit_count == 1);
}

static void test_wake_quality_never_starts_unsafe_candidate(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 old_safe;

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	old_safe = algo.safe_baseline_q8[100];
	init_raw(raw, 0x7fee);
	hx_algo_begin_wake(&algo);
	assert(hx_algo_qualify_wake_frame(&algo, raw,
			HX_FINGER_ABSENT) == HX_WAKE_QUALITY_PENDING);
	for (int i = 1; i < 80; i++)
		raw[i] += 400;
	assert(hx_algo_qualify_wake_frame(&algo, raw,
			HX_FINGER_ABSENT) == HX_WAKE_QUALITY_PENDING);
	assert(algo.safe_baseline_q8[100] == old_safe);
	assert(!algo.wake_candidate_valid);
}

static void test_finger_at_wake_uses_last_safe_baseline(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	int touch = 15 * HX_COLS + 20;
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.safe_baseline_q8[i] = (s32)(0x7f00 + (i % 3)) << 8;
		raw[i] = 0x7f00 + (i % 3) + 120;
	}
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_finger_safe_frames; frame++) {
		for (int i = 0; i < HX_PIXELS; i++)
			raw[i] = 0x7f00 + (i % 3) + 120;
		touch = 15 * HX_COLS + 20 + frame;
		raw[touch] += 1800;
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_PRESENT);
	}
	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(algo.baseline_q8[101] ==
	       algo.safe_baseline_q8[101] + ((s32)120 << 8));
	assert(algo.baseline_q8[touch] != (s32)raw[touch] << 8);
	assert(algo.wake_safe_fallback_count == 1);
	assert(algo.wake_finger_mask_accept_count == 1);
}

static void test_raw_finger_at_wake_overrides_firmware_miss(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	init_raw(raw, 0x7fee);
	add_blob(raw, 15, 20, 1800);
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_finger_safe_frames; frame++)
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_ABSENT);

	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(algo.wake_raw_finger_override);
	assert(algo.wake_raw_finger_inferred_count == 1);
	assert(algo.safe_baseline_q8[15 * HX_COLS + 20] ==
	       (s32)0x7fee << 8);
	assert(hx_algo_resolve_finger_state(&algo, raw, HX_FINGER_ABSENT) ==
	       HX_FINGER_PRESENT);
}

static void test_raw_finger_override_clears_after_clean_release(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	algo.wake_raw_finger_override = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	init_raw(raw, 0x7fee);
	for (int frame = 1; frame < algo.wake_finger_safe_frames; frame++)
		assert(hx_algo_resolve_finger_state(&algo, raw,
			       HX_FINGER_ABSENT) == HX_FINGER_PRESENT);
	assert(hx_algo_resolve_finger_state(&algo, raw, HX_FINGER_ABSENT) ==
	       HX_FINGER_ABSENT);
	assert(!algo.wake_raw_finger_override);
	assert(algo.wake_raw_finger_release_count == 1);
}

static void test_clean_wake_preserves_existing_safe_baseline(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 expected_common_q8;
	s32 guarded_cell;
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7fee << 8;
	hx_safe_baseline_bootstrap(&algo, algo.baseline_q8);
	algo.safe_baselines[0].confidence = HX_SAFE_BASELINE_CONFIRMED;
	for (int i = 0; i < HX_PIXELS; i++)
		raw[i] = 0x7fee + 120;
	/* This persistent sub-threshold shape used to be copied directly into
	 * working during wake qualification.  A trusted safe grid may only carry
	 * the common shift across wake.
	 */
	raw[100] += 100;
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_stable_frames; frame++)
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_ABSENT);

	expected_common_q8 = (s32)(0x7fee + 120) << 8;
	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(algo.baseline_q8[100] == expected_common_q8);
	assert(algo.safe_baseline_q8[100] == (s32)0x7fee << 8);
	assert(algo.wake_safe_preserved_count == 1);
	assert(algo.wake_clean_safe_restore_count == 1);
	assert(algo.wake_baseline_commit_count == 0);
	assert(algo.baseline_touch_hold);
	assert(!algo.baseline_touch_seen);

	/* A stationary broad contact can remain below both the firmware finger
	 * bit and the host peak threshold.  Safe-backed wake guard must not time
	 * out merely because these frames look clean.
	 */
	guarded_cell = algo.baseline_q8[10 * HX_COLS + 10];
	init_raw(raw, 0x7fee + 120);
	for (int r = 8; r < 18; r++)
		for (int c = 8; c < 18; c++)
			raw[r * HX_COLS + c] += 200;
	for (int frame = 0; frame < 60; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_touch_hold);
	assert(algo.baseline_touch_seen);
	assert(algo.baseline_guard_state == HX_BASELINE_GUARD_PROTECTED);
	assert(algo.baseline_q8[10 * HX_COLS + 10] == guarded_cell);
	assert(algo.baseline_safe_commit_count == 0);
	assert(algo.safe_no_finger_frames == 0);

	/* Once a definite touch has been observed, a real clean release may end
	 * the guard and re-enable ordinary spatial baseline maintenance.
	 */
	init_raw(raw, 0x7fee + 120);
	add_blob(raw, 20, 20, 1800);
	hx_preprocess_frame_state(&algo, raw, HX_FINGER_PRESENT);
	assert(algo.baseline_touch_seen);
	assert(algo.baseline_release_hold[20 * HX_COLS + 20] ==
	       algo.baseline_release_hold_frames);
	init_raw(raw, 0x7fee + 120);
	for (int frame = 0; frame < 240; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(!algo.baseline_touch_hold);
	assert(algo.baseline_release_hold[20 * HX_COLS + 20] > 0);
	assert(algo.safe_no_finger_frames == 0);
}

static void test_held_in_hand_release_uses_long_guard(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	hx_restore_working_from_safe(&algo, 0, true, true);
	assert(algo.baseline_held_in_hand);
	init_raw(raw, 0x7fee);
	for (int frame = 0; frame < 2 * algo.safe_commit_no_finger_frames;
	     frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_touch_hold);
	assert(algo.baseline_held_in_hand);
	for (int frame = 0; frame < 240; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(!algo.baseline_touch_hold);
	assert(!algo.baseline_held_in_hand);
}

static void test_bootstrap_safe_protects_clean_wake(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7fee << 8;
	hx_safe_baseline_bootstrap(&algo, algo.baseline_q8);
	assert(algo.safe_baselines[0].confidence ==
	       HX_SAFE_BASELINE_BOOTSTRAP);
	init_raw(raw, 0x7fee + 120);
	raw[100] += 100;
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_stable_frames; frame++)
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_ABSENT);

	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(algo.baseline_q8[100] == (s32)(0x7fee + 120) << 8);
	assert(algo.safe_baseline_q8[100] == (s32)0x7fee << 8);
	assert(algo.baseline_touch_hold);
	assert(!algo.baseline_touch_seen);
	assert(algo.wake_clean_safe_restore_count == 1);
	assert(algo.wake_baseline_commit_count == 0);
}

static void test_ambiguous_wake_restores_safe_working(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 old_safe;
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)(0x7fee + 20) << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	old_safe = algo.safe_baseline_q8[100];
	init_raw(raw, 0x7fee);
	for (int c = 0; c < 30; c++)
		raw[12 * HX_COLS + c] += 1800;
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_finger_safe_frames; frame++)
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_ABSENT);

	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(algo.baseline_q8[100] == old_safe);
	assert(algo.safe_baseline_q8[100] == old_safe);
	assert(algo.baseline_touch_hold);
	assert(algo.wake_raw_finger_override);
	assert(algo.wake_ambiguous_safe_fallback_count == 1);
}

static void test_negative_thumb_at_wake_is_protected(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 old_safe;
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	old_safe = algo.safe_baseline_q8[20 * HX_COLS + 20];
	init_raw(raw, 0x7fee);
	for (int r = 19; r <= 21; r++)
		for (int c = 19; c <= 21; c++)
			raw[r * HX_COLS + c] -= 500;
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_finger_safe_frames; frame++)
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_ABSENT);

	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(algo.baseline_q8[20 * HX_COLS + 20] == old_safe);
	assert(algo.safe_baseline_q8[20 * HX_COLS + 20] == old_safe);
	assert(algo.baseline_touch_hold);
	assert(algo.wake_raw_finger_override);
	assert(algo.wake_ambiguous_safe_fallback_count == 1);
}

static void test_finger_wake_rejects_noisy_background(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	init_raw(raw, 0x7fee);
	for (int r = 0; r < 25; r++)
		raw[r * HX_COLS + 8] += 1900;
	add_blob(raw, 20, 20, 1800);
	hx_algo_begin_wake(&algo);
	assert(hx_algo_qualify_wake_frame(&algo, raw,
			HX_FINGER_PRESENT) == HX_WAKE_QUALITY_PENDING);
	assert(algo.wake_qualifying);
	assert(algo.wake_finger_mask_reject_count == 1);
}

static void test_finger_wake_mask_rejection_restores_safe_baseline(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)(0x7fee + 40) << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	init_raw(raw, 0x7fee);
	/* A broad, line-shaped contact is not trusted for common-mode
	 * correction, but it must not leave the controller powered down.
	 */
	for (int c = 10; c < 22; c++)
		raw[20 * HX_COLS + c] += 1800;
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_finger_safe_frames; frame++)
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_PRESENT);

	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(!algo.wake_qualifying);
	assert(algo.baseline_q8[100] == algo.safe_baseline_q8[100]);
	assert(algo.baseline_touch_hold);
	assert(algo.wake_finger_mask_reject_count ==
	       algo.wake_finger_safe_frames);
	assert(algo.wake_finger_degraded_fallback_count == 1);
}

static void test_invalid_wake_grid_is_never_committed(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS] = { 0 };

	hx_algo_init_defaults(&algo);
	hx_algo_begin_wake(&algo);
	assert(hx_algo_qualify_wake_frame(&algo, raw,
			HX_FINGER_ABSENT) == HX_WAKE_QUALITY_REJECTED);
	assert(!algo.baseline_initialized);
	assert(!algo.safe_baseline_valid);
}

static void test_finger_at_first_boot_uses_neutral_baseline(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	init_raw(raw, 0x7fee);
	raw[15 * HX_COLS + 20] += 1800;
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_finger_safe_frames; frame++)
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_PRESENT);
	assert(result == HX_WAKE_QUALITY_PROTECTED);
	assert(algo.baseline_initialized);
	assert(!algo.safe_baseline_valid);
	assert(algo.baseline_q8[15 * HX_COLS + 20] ==
	       (s32)algo.baseline_initial << 8);
}

static void test_spatially_divergent_wake_preserves_safe(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	int result = HX_WAKE_QUALITY_PENDING;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	init_raw(raw, 0x7fee);
	for (int i = 0; i < 40; i++)
		raw[(i * 47 + 13) % HX_PIXELS] += 500;
	hx_algo_begin_wake(&algo);
	for (int frame = 0; frame < algo.wake_finger_safe_frames; frame++) {
		raw[10] = 0x7fee + frame;
		result = hx_algo_qualify_wake_frame(&algo, raw,
						HX_FINGER_ABSENT);
	}
	assert(result == HX_WAKE_QUALITY_USING_SAFE);
	assert(!algo.wake_candidate_valid);
	assert(algo.baseline_q8[100] == algo.safe_baseline_q8[100]);
	assert(algo.baseline_touch_hold);
	assert(algo.safe_baseline_q8[100] == (s32)0x7fee << 8);
	assert(algo.wake_ambiguous_safe_fallback_count == 1);
}

static void test_runtime_safe_baseline_uses_average_and_confirmation(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	const u16 base = 0x7fee;

	init_integrated_algo(&algo);
	algo.safe_commit_no_finger_frames = 3;
	algo.baseline_initialized = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)base << 8;

	for (int frame = 1; frame <= 3; frame++) {
		init_raw(raw, base + frame * 10);
		assert(hx_algo_process_frame_state(&algo, raw,
					   HX_FINGER_ABSENT) == 0);
	}
	assert(!algo.safe_baseline_valid);
	assert(algo.safe_candidate_confirming);
	assert(algo.baseline_safe_confirm_count == 1);

	for (int frame = 0; frame < 3; frame++) {
		init_raw(raw, base + 24);
		assert(hx_algo_process_frame_state(&algo, raw,
					   HX_FINGER_ABSENT) == 0);
	}
	assert(algo.safe_baseline_valid);
	assert(!algo.safe_candidate_confirming);
	assert(algo.safe_baseline_q8[100] == (s32)(base + 24) << 8);
	assert(algo.baseline_safe_commit_count == 1);
	for (int frame = 0; frame < 12; frame++)
		hx_algo_process_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_safe_commit_count == 1);

	/* A same-epoch touch/release must not rewrite SafeBaseline. */
	algo.safe_candidate_armed = true;
	init_raw(raw, base + 24);
	for (int frame = 0; frame < 6; frame++)
		hx_algo_process_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_safe_commit_count == 1);

	/* A new screen epoch is an independent electrical observation and may
	 * start one new confirmation window. */
	algo.screen_epoch++;
	algo.safe_candidate_armed = true;
	for (int frame = 0; frame < 6; frame++)
		hx_algo_process_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_safe_commit_count == 2);
	assert(algo.safe_baseline_count == 1);
	assert(algo.baseline_safe_dedup_count == 1);
}

static void test_runtime_safe_baseline_rejects_negative_line(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	const u16 base = 0x7fee;
	s32 old_safe;

	init_integrated_algo(&algo);
	algo.safe_commit_no_finger_frames = 2;
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)base << 8;
		algo.safe_baseline_q8[i] = (s32)base << 8;
	}
	old_safe = algo.safe_baseline_q8[100];
	for (int frame = 0; frame < 2; frame++) {
		init_raw(raw, base + 10);
		hx_algo_process_frame_state(&algo, raw, HX_FINGER_ABSENT);
	}
	assert(algo.safe_candidate_confirming);

	init_raw(raw, base + 10);
	for (int r = 10; r < 25; r++)
		raw[r * HX_COLS + 8] -= 1000;
	hx_algo_process_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(!algo.safe_candidate_confirming);
	assert(algo.safe_no_finger_frames == 0);
	assert(algo.safe_baseline_q8[100] == old_safe);
	assert(algo.baseline_safe_negative_reject_count == 1);
}

static void test_runtime_comparator_recommends_safe_without_mutation(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 working_before;
	s32 safe_before;

	hx_algo_init_defaults(&algo);
	algo.runtime_blreset_enabled = false;
	algo.runtime_blreset_confirm_frames = 3;
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	for (int c = 0; c < 30; c++)
		algo.baseline_q8[12 * HX_COLS + c] =
			(s32)(0x7fee + 1000) << 8;
	working_before = algo.baseline_q8[12 * HX_COLS + 5];
	safe_before = algo.safe_baseline_q8[12 * HX_COLS + 5];

	for (int frame = 0; frame < 3; frame++)
		hx_algo_runtime_baseline_process(&algo, raw,
						 HX_FINGER_ABSENT, false);

	assert(algo.diag_safe_compare_result == HX_SAFE_COMPARE_SAFE_BETTER);
	assert(algo.diag_working_bad_nodes == 30);
	assert(algo.diag_safe_bad_nodes == 0);
	assert(algo.diag_working_negative_nodes == 30);
	assert(algo.diag_working_line_noise == 1);
	assert(algo.diag_safe_improvement_frames == 3);
	assert(algo.diag_blreset_recommended);
	assert(algo.diag_blreset_recommendation_count == 1);
	assert(algo.baseline_q8[12 * HX_COLS + 5] == working_before);
	assert(algo.safe_baseline_q8[12 * HX_COLS + 5] == safe_before);
}

static void test_runtime_comparator_classifies_working_and_invalid(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	for (int r = 0; r < 30; r++)
		algo.safe_baseline_q8[r * HX_COLS + 8] =
			(s32)(0x7fee + 1000) << 8;
	hx_algo_runtime_baseline_process(&algo, raw,
						 HX_FINGER_ABSENT, true);
	assert(algo.diag_safe_compare_result ==
	       HX_SAFE_COMPARE_WORKING_BETTER);

	for (int c = 0; c < 30; c++)
		algo.baseline_q8[12 * HX_COLS + c] =
			(s32)(0x7fee + 1000) << 8;
	hx_algo_runtime_baseline_process(&algo, raw,
						 HX_FINGER_ABSENT, true);
	assert(algo.diag_safe_compare_result == HX_SAFE_COMPARE_BOTH_INVALID);
	assert(!algo.diag_blreset_recommended);
}

static void test_runtime_comparator_masks_touch_region(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	add_blob(raw, 20, 20, 1800);
	hx_algo_runtime_baseline_process(&algo, raw,
						 HX_FINGER_PRESENT, true);
	assert(algo.diag_safe_compare_result == HX_SAFE_COMPARE_BOTH_VALID);
	assert(algo.diag_touch_mask_nodes > 4);
	assert(algo.diag_working_bad_nodes == 0);
	assert(algo.diag_working_masked_bad_nodes == 4);
}

static void test_runtime_comparator_detects_absorbed_touch(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	init_raw(raw, 0x7fee);
	add_blob(raw, 20, 20, 1800);
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)raw[i] << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	/* Previous reported count remains zero while firmware says finger-on. */
	hx_algo_runtime_baseline_process(&algo, raw,
						 HX_FINGER_PRESENT, false);
	assert(algo.diag_safe_compare_result == HX_SAFE_COMPARE_SAFE_BETTER);
	assert(algo.diag_working_masked_bad_nodes == 0);
	assert(algo.diag_safe_masked_bad_nodes == 4);
}

static void test_safe_baseline_store_deduplicates_across_wakes(void)
{
	struct hx_algo algo = {};
	s32 baseline[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.screen_epoch = 1;
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] = (s32)0x7fee << 8;
	hx_safe_baseline_bootstrap(&algo, baseline);
	memcpy(algo.baseline_q8, baseline, sizeof(baseline));
	assert(algo.safe_baseline_count == 1);
	assert(algo.safe_baselines[0].confidence ==
	       HX_SAFE_BASELINE_BOOTSTRAP);

	/* A panel-wide common shift is the same spatial baseline. */
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] += (s32)100 << 8;
	hx_safe_baseline_commit(&algo, baseline);
	assert(algo.safe_baseline_count == 1);
	assert(algo.safe_baselines[0].confidence ==
	       HX_SAFE_BASELINE_BOOTSTRAP);
	algo.screen_epoch = 2;
	hx_safe_baseline_commit(&algo, baseline);
	assert(algo.safe_baseline_count == 1);
	assert(algo.safe_baselines[0].confidence ==
	       HX_SAFE_BASELINE_CONFIRMED);
	algo.screen_epoch = 3;
	hx_safe_baseline_commit(&algo, baseline);
	assert(algo.safe_baseline_count == 1);
	assert(algo.safe_baselines[0].confidence ==
	       HX_SAFE_BASELINE_CROSS_WAKE);
	assert(algo.baseline_safe_dedup_count == 3);
}

static void test_safe_baseline_store_selects_best_history(void)
{
	struct hx_algo algo = {};
	s32 baseline[HX_PIXELS];
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.screen_epoch = 1;
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] = (s32)0x7fee << 8;
	hx_safe_baseline_bootstrap(&algo, baseline);
	algo.safe_baselines[0].confidence = HX_SAFE_BASELINE_CONFIRMED;
	for (int i = 100; i < 130; i++)
		baseline[i] += (s32)800 << 8;
	hx_safe_baseline_commit(&algo, baseline);
	assert(algo.safe_baseline_count == 2);
	assert(algo.safe_baselines[1].confidence ==
	       HX_SAFE_BASELINE_BOOTSTRAP);
	assert(algo.safe_baseline_selected == 0);

	init_raw(raw, 0x7fee);
	hx_safe_baseline_select_for_raw(&algo, raw);
	assert(algo.safe_baseline_selected == 0);
	assert(algo.safe_baseline_q8[110] == (s32)0x7fee << 8);
}

static void test_safe_baseline_queue_accepts_distinct_same_epoch_state(void)
{
	struct hx_algo algo = {};
	s32 baseline[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] = (s32)0x7fee << 8;
	hx_safe_baseline_bootstrap(&algo, baseline);
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] += (s32)300 << 8;
	hx_safe_baseline_commit(&algo, baseline);
	assert(algo.safe_baseline_count == 2);
}

static void test_safe_baseline_selection_prefers_spatial_fit(void)
{
	struct hx_algo algo = {};
	s32 baseline[HX_PIXELS];
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.screen_epoch = 1;
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] = (s32)0x7fee << 8;
	hx_safe_baseline_bootstrap(&algo, baseline);
	/* Slot 0 is deliberately promoted but spatially stale. */
	algo.safe_baselines[0].confidence = HX_SAFE_BASELINE_CROSS_WAKE;
	for (int i = 100; i < 130; i++)
		baseline[i] += (s32)500 << 8;
	hx_safe_baseline_commit(&algo, baseline);
	assert(algo.safe_baseline_count == 2);
	algo.safe_baselines[1].confidence = HX_SAFE_BASELINE_BOOTSTRAP;

	init_raw(raw, 0x7fee);
	hx_safe_baseline_select_for_raw(&algo, raw);
	assert(algo.safe_baseline_selected == 0);
	/* Make the high-confidence slot stale and the lower-confidence slot
	 * exact.  Selection must follow the spatial fit, not confidence. */
	for (int i = 100; i < 130; i++)
		raw[i] += 500;
	hx_safe_baseline_select_for_raw(&algo, raw);
	assert(algo.safe_baseline_selected == 1);
}

static void test_runtime_blreset_restores_only_working_baseline(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 safe_before;
	bool recovered = false;

	hx_algo_init_defaults(&algo);
	algo.runtime_blreset_confirm_frames = 3;
	algo.runtime_blreset_cooldown = 0;
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	}
	for (int c = 0; c < 30; c++)
		algo.baseline_q8[12 * HX_COLS + c] =
			(s32)(0x7fee + 1000) << 8;
	algo.tracks[0].active = true;
	algo.tracks[0].reported = true;
	algo.tracks[0].x = 1000;
	algo.tracks[0].y = 1000;
	safe_before = algo.safe_baseline_q8[12 * HX_COLS + 5];
	for (int frame = 0; frame < 3; frame++)
		recovered = hx_algo_runtime_baseline_process(&algo, raw,
			HX_FINGER_ABSENT, false);
	assert(recovered);
	assert(algo.baseline_q8[12 * HX_COLS + 5] == safe_before);
	assert(algo.safe_baseline_q8[12 * HX_COLS + 5] == safe_before);
	assert(algo.tracks[0].active);
	assert(algo.tracks[0].reported);
	assert(algo.baseline_touch_hold);
	assert(algo.runtime_blreset_count == 1);
}

static void test_touch_hold_blocks_spatial_learning_until_clean_release(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 safe_before[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	memcpy(safe_before, algo.safe_baseline_q8, sizeof(safe_before));
	hx_restore_working_from_safe(&algo, 0, true, true);
	algo.baseline_held_in_hand = false;
	init_raw(raw, 0x7fee);
	for (int r = 8; r < 18; r++)
		for (int c = 12; c < 22; c++)
			raw[r * HX_COLS + c] += 1200;
	for (int frame = 0; frame < 30; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_PRESENT);
	assert(algo.baseline_touch_hold);
	assert(!memcmp(algo.baseline_q8, safe_before, sizeof(safe_before)));
	assert(!memcmp(algo.safe_baseline_q8, safe_before, sizeof(safe_before)));

	init_raw(raw, 0x7fee);
	for (int frame = 0; frame < 59; frame++) {
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
		assert(algo.baseline_touch_hold);
	}
	hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(!algo.baseline_touch_hold);
}

static void test_unclean_no_finger_frame_cannot_change_working_shape(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 before[25];
	int cell = 0;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7fee << 8;
	for (int r = 8; r < 13; r++)
		for (int c = 8; c < 13; c++) {
			int idx = r * HX_COLS + c;

			raw[idx] += 200; /* Broad, but below touch threshold. */
			before[cell++] = algo.baseline_q8[idx];
		}
	hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	cell = 0;
	for (int r = 8; r < 13; r++)
		for (int c = 8; c < 13; c++)
			assert(algo.baseline_q8[r * HX_COLS + c] ==
			       before[cell++]);
	assert(algo.baseline_spatial_update_count == 0);
	assert(algo.baseline_spatial_block_count == 1);
	assert(algo.safe_no_finger_frames == 0);
	assert(!algo.safe_candidate_confirming);
}

static void test_small_panel_residual_updates_working_but_not_safe(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	const int cells[] = { 8 * HX_COLS + 8, 8 * HX_COLS + 9,
		9 * HX_COLS + 8, 9 * HX_COLS + 9 };
	s32 before;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7fee << 8;
	for (int i = 0; i < 4; i++)
		raw[cells[i]] += 200;
	before = algo.baseline_q8[cells[0]];
	for (int frame = 0; frame < 10; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_q8[cells[0]] > before);
	assert(algo.baseline_q8[cells[0]] <= before + (1 << 8));
	assert(algo.baseline_spatial_update_count == 1);
	assert(algo.baseline_spatial_block_count == 9);
	assert(algo.safe_no_finger_frames == 0);
	assert(!algo.safe_candidate_confirming);
}

static void test_bliir_updates_independently_of_safe_history(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	const int cell = 8 * HX_COLS + 8;
	s32 safe;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	algo.safe_baseline_count = 1;
	algo.safe_baselines[0].valid = true;
	algo.safe_baselines[0].confidence = HX_SAFE_BASELINE_CONFIRMED;
	algo.safe_baselines[0].screen_epoch = algo.screen_epoch;
	init_raw(raw, 0x7fee);
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
		algo.safe_baselines[0].baseline_q8[i] = (s32)0x7fee << 8;
	}
	/* A panel-wide common shift is a valid idle baseline movement.  A single
	 * sparse +200 cell is now correctly classified as a silent ghost by the
	 * vendor SafeBaseline path and must not be used to test ordinary BLIIR. */
	for (int i = 0; i < HX_PIXELS; i++)
		raw[i] += 200;
	safe = algo.safe_baseline_q8[cell];
	for (int frame = 0; frame < 240; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_q8[cell] == (s32)raw[cell] << 8);
	assert(algo.baseline_safe_clamp_count == 0);
	assert(algo.safe_baseline_q8[cell] == safe);
}


static void test_negative_residual_restarts_guard_qualification(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 before;

	hx_algo_init_defaults(&algo);
	algo.safe_commit_no_finger_frames = 3;
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
	hx_restore_working_from_safe(&algo, 0, true, true);
	algo.baseline_held_in_hand = false;
	init_raw(raw, 0x7fee);
	for (int frame = 0; frame < 3; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_guard_state ==
	       HX_BASELINE_GUARD_CLEAN_QUALIFY);

	for (int r = 10; r < 15; r++)
		for (int c = 10; c < 15; c++)
			raw[r * HX_COLS + c] -= 200;
	before = algo.baseline_q8[10 * HX_COLS + 10];
	hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_touch_hold);
	assert(algo.baseline_touch_seen);
	assert(algo.baseline_guard_state == HX_BASELINE_GUARD_PROTECTED);
	assert(algo.baseline_q8[10 * HX_COLS + 10] == before);
	assert(algo.baseline_guard_restart_count == 1);

	init_raw(raw, 0x7fee);
	for (int frame = 0; frame < 6; frame++)
		hx_preprocess_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(!algo.baseline_touch_hold);
}

static void test_line_noise_is_held_but_compact_touch_is_not(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	for (int i = 0; i < HX_PIXELS; i++)
		algo.baseline_q8[i] = (s32)0x7fee << 8;
	init_raw(raw, 0x7fee);
	for (int r = 14; r <= 16; r++)
		for (int c = 19; c <= 21; c++)
			raw[r * HX_COLS + c] += 2200;
	assert(!hx_algo_is_exception_frame(&algo, raw));
	init_raw(raw, 0x7fee);
	for (int c = 0; c < 30; c++)
		raw[12 * HX_COLS + c] += 2200;
	assert(hx_algo_is_exception_frame(&algo, raw));
	init_raw(raw, 0x7fee);
	for (int r = 0; r < 30; r++)
		raw[r * HX_COLS + 12] += 2200;
	assert(hx_algo_is_exception_frame(&algo, raw));
	assert(algo.noise_frame_hold_count == 2);
}

static void test_reported_track_survives_silent_gap(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos point = {.x = 1000, .y = 700};
	int slot = -1;

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	hx_track_contacts(&algo, &point, 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo.tracks[i].active)
			slot = i;
	assert(slot >= 0 && algo.tracks[slot].reported);
	for (int i = 0; i < 4; i++) {
		hx_track_contacts(&algo, NULL, 0);
		assert(algo.tracks[slot].active);
		assert(algo.tracks[slot].reported);
		assert(algo.tracks[slot].x == 1000);
	}
	hx_track_contacts(&algo, NULL, 0);
	assert(!algo.tracks[slot].active);
}

static void test_hungarian_keeps_two_ids(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos det[2] = {{.x = 100, .y = 100},
					      {.x = 500, .y = 100}};
	int left = -1, right = -1;

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	hx_track_contacts(&algo, det, 2);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (!algo.tracks[i].active)
			continue;
		if (algo.tracks[i].x == 100)
			left = i;
		else
			right = i;
	}
	assert(left >= 0 && right >= 0);
	det[0].x = 260;
	det[1].x = 340;
	hx_track_contacts(&algo, det, 2);
	det[0].x = 480;
	det[1].x = 120;
	hx_track_contacts(&algo, det, 2);
	assert(algo.tracks[left].active && algo.tracks[right].active);
	assert(algo.tracks[left].x > algo.tracks[right].x);
}
static void test_safe_slot_runtime_selection_and_reacquire_hold(void)
{
	struct hx_algo algo = { 0 };
	u16 raw[HX_PIXELS] = { 0 };
	s32 first[HX_PIXELS];
	s32 second[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	for (int i = 0; i < HX_PIXELS; i++) {
		raw[i] = 0x7fee;
		first[i] = (s32)0x7fee << HX_BASELINE_FRACTION_BITS;
		second[i] = (s32)0x7fee << HX_BASELINE_FRACTION_BITS;
	}
	/* Two electrical states with a spatially distinct panel offset must
	 * occupy separate history slots. */
	for (int i = HX_COLS * 4; i < HX_COLS * 6; i++)
		second[i] += 800 << HX_BASELINE_FRACTION_BITS;
	hx_safe_baseline_bootstrap(&algo, first);
	algo.screen_epoch = 1;
	hx_safe_baseline_commit(&algo, second);
	assert(algo.safe_baseline_count == 2);
	for (int i = HX_COLS * 4; i < HX_COLS * 6; i++)
		raw[i] = 0x7fee + 800;
	hx_safe_baseline_select_for_raw(&algo, raw);
	assert(algo.safe_baseline_selected == 1);
	algo.baseline_reacquire_pending = true;
	algo.safe_candidate_armed = true;
	/* The hold is installed by the clean-baseline commit path; this check
	 * documents that the field is available and starts clear otherwise. */
	assert(algo.baseline_post_reacquire_hold == 0);
}

static void test_baseline_stage_machine(void)
{
	struct hx_algo algo = { 0 };
	int i;

	hx_algo_init_defaults(&algo);
	assert(hx_baseline_stage_update(&algo, HX_BLSM_PRPT_TOUCH,
		1000, -1000, false) ==
	       HX_BLSM_TOUCH);
	/* Stage 5 uses the previous-frame single-touch latch.  With no latch a
	 * lift enters stage 6, then the official 408ms timer advances to 7. */
	assert(hx_baseline_stage_update(&algo, 0, 1000, -1000, false) ==
	       HX_BLSM_TOUCH_RELEASE);
	for (i = 0; i < 50; i++)
		hx_baseline_stage_update(&algo, 0, 1000, -1000, false);
	assert(algo.baseline_stage == HX_BLSM_POST_TOUCH);
	assert(hx_baseline_stage_update(&algo, 0, 0, 0, false) ==
	       HX_BLSM_NO_TOUCH_STABLE);

	/* Reset/noisy/forced/debounce properties have the same priority and
	 * stage numbering as the TSACore jump table. */
	assert(hx_baseline_stage_update(&algo, HX_BLSM_PRPT_NOISY,
		0, 0, false) == HX_BLSM_NOISY_RESET);
	assert(hx_baseline_stage_update(&algo, 0, 0, 0, false) ==
	       HX_BLSM_NO_TOUCH_STABLE);
	assert(hx_baseline_stage_update(&algo, HX_BLSM_PRPT_FORCED,
		0, 0, false) == HX_BLSM_FORCED);
	assert(hx_baseline_stage_update(&algo, 0, 0, 0, false) ==
	       HX_BLSM_NO_TOUCH_STABLE);
	assert(hx_baseline_stage_update(&algo, HX_BLSM_PRPT_DEBOUNCE,
		0, 0, false) == HX_BLSM_DEBOUNCE);
}

static void test_screen_on_hand_state_transitions(void)
{
	struct hx_algo algo = { 0 };

	hx_algo_init_defaults(&algo);
	hx_restore_working_from_safe(&algo, 0, true, true);
	assert(algo.baseline_screen_on_hand_state == HX_HAND_PROTECTED);
	algo.baseline_touch_hold = true;
	algo.baseline_touch_seen = true;
	algo.baseline_guard_state = HX_BASELINE_GUARD_CLEAN_QUALIFY;
	algo.baseline_guard_clean_frames = 255;
	algo.safe_commit_no_finger_frames = 1;
	assert(hx_baseline_guard_process(&algo, true, false));
	assert(algo.baseline_screen_on_hand_state == HX_HAND_RELEASE_PENDING);
}

static void test_safe_reset_policy_debounces_and_separates_side_reset(void)
{
	struct hx_algo algo = { 0 };
	enum hx_safe_reset_action action = HX_SAFE_RESET_NONE;

	hx_algo_init_defaults(&algo);
	algo.runtime_blreset_confirm_frames = 3;
	for (int i = 0; i < 2; i++)
		action = hx_safe_baseline_reset_policy(&algo,
			HX_SAFE_COMPARE_BOTH_INVALID, false, HX_FINGER_ABSENT);
	assert(action == HX_SAFE_RESET_NONE);
	assert(algo.safe_reset_in_debounce);
	action = hx_safe_baseline_reset_policy(&algo,
		HX_SAFE_COMPARE_BOTH_INVALID, false, HX_FINGER_ABSENT);
	assert(action == HX_SAFE_RESET_INVALIDATE);

	action = hx_safe_baseline_reset_policy(&algo,
		HX_SAFE_COMPARE_BOTH_VALID, true, HX_FINGER_PRESENT);
	assert(action == HX_SAFE_RESET_NONE);
	for (int i = 0; i < 3; i++)
		action = hx_safe_baseline_reset_policy(&algo,
			HX_SAFE_COMPARE_BOTH_VALID, false, HX_FINGER_ABSENT);
	assert(action == HX_SAFE_RESET_WORKING);
	assert(!algo.safe_sync_reset_side_area);
}

static void test_safe_side_postprocess_restores_after_release(void)
{
	struct hx_algo algo = { 0 };
	u16 raw[HX_PIXELS];
	int idx = 5;

	hx_algo_init_defaults(&algo);
	algo.baseline_initialized = true;
	algo.safe_baseline_valid = true;
	algo.runtime_blreset_confirm_frames = 3;
	for (int i = 0; i < HX_PIXELS; i++) {
		raw[i] = 0x7fee;
		algo.safe_baseline_q8[i] = (s32)0x7fee << 8;
		algo.baseline_q8[i] = (s32)0x7fee << 8;
	}
	algo.baseline_q8[idx] += 500 << 8;
	algo.peak_count = 1;
	algo.peaks[0].r = 0;
	algo.peaks[0].c = 5;
	algo.contact_count = 1;
	algo.contacts[0].peak_index = 0;
	algo.contacts[0].is_edge = true;
	for (int i = 0; i < 5; i++)
		hx_safe_baseline_postprocess(&algo, raw, HX_FINGER_PRESENT);
	assert(algo.safe_sync_reset_side_area);
	algo.peak_count = 0;
	for (int i = 0; i < 3; i++)
		hx_safe_baseline_postprocess(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_q8[idx] == algo.safe_baseline_q8[idx]);
}

static void test_safe_buffer_comparison_tracks_common_mode(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.safe_baseline_q8[i] = (s32)1000 << 8;
		algo.baseline_q8[i] = (s32)1020 << 8;
		raw[i] = 1040;
	}
	hx_safe_baseline_buffer_comparison(&algo, raw);
	assert(algo.safe_bl2bl_dif[100] == 20);
	assert(algo.safe_bl2raw_dif[100] == 40);
	assert(algo.safe_bl2bl_dif_cmf[100] == 0);
	assert(algo.safe_bl2raw_dif_cmf[100] == 0);
}

static void test_safe_reset_statistics_decay_and_trigger(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.frame_interval_ms = 1;
	algo.safe_reset_reason_trigger_frames = 3;
	assert(!hx_safe_baseline_reset_statistics(&algo, 1U << 5, false));
	assert(!hx_safe_baseline_reset_statistics(&algo, 1U << 5, false));
	assert(hx_safe_baseline_reset_statistics(&algo, 1U << 5, false));
	assert(algo.safe_reset_reason_mask & (1U << 5));
	hx_safe_baseline_reset_statistics(&algo, 0, false);
	assert(algo.safe_reset_reason_frames[5] == 2);
	/* A safe-raw check keeps reset statistics eligible, as in the vendor
	 * ResetStatisticsUnit path; it is not a blanket suppression. */
	algo.safe_reset_reason_trigger_frames = 1;
	assert(hx_safe_baseline_reset_statistics(&algo, 1U << 6, true));
}

static void test_official_safe_reset_thresholds(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	/* Values are extracted from the exported TSACore globals, not the
	 * previous one-size-fits-all Linux defaults. */
	assert(algo.safe_reset_time_threshold[0] == 500);
	assert(algo.safe_reset_time_threshold[8] == 2000);
	assert(algo.safe_reset_push_threshold[0] == 1);
	assert(algo.safe_reset_push_threshold[7] == 0);
	assert(algo.safe_reset_trigger_count[6] == 50);
	assert(algo.safe_reset_screen_on_window[7] == 600000);

	/* A reason emits one reset event when its accumulated duration crosses
	 * its own threshold; it is not triggered by a global frame threshold. */
	algo.frame_sequence = 1;
	hx_safe_baseline_reset_statistics(&algo, 1U << 0, false);
	algo.frame_sequence = 501;
	assert(hx_safe_baseline_reset_statistics(&algo, 1U << 0, false));
	assert(algo.safe_reset_push_count[0] == 1);
	/* Side statistics have independent runtime counters even though they
	 * share the same reason IDs. */
	algo.frame_sequence = 1;
	hx_safe_baseline_reset_statistics_variant(&algo, 1U << 0, false, true);
	algo.frame_sequence = 501;
	assert(hx_safe_baseline_reset_statistics_variant(&algo, 1U << 0,
		false, true));
	assert(algo.safe_reset_push_count_side[0] == 1);
	assert(algo.safe_reset_push_count[0] == 1);
	/* The global SafeBLPushed quota and the per-reason reset quota are
	 * independent IsResetAllowed gates. */
	algo.safe_reset_push_threshold[1] = 2;
	algo.safe_baseline_pushes = 2;
	algo.frame_sequence = 1;
	assert(!hx_safe_baseline_reset_statistics(&algo, 1U << 1, false));
	algo.safe_baseline_pushes = 0;
	algo.safe_reset_trigger_count[1] = 1;
	algo.safe_reset_push_count[1] = 1;
	assert(!hx_safe_baseline_reset_statistics(&algo, 1U << 1, false));
	/* SafeRawChecked short-circuits the quota checks in the vendor
	 * IsResetAllowed implementation. */
	algo.safe_baseline_pushes = 1;
	algo.safe_reset_reason_trigger_frames = 1;
	algo.frame_sequence = 1;
	assert(hx_safe_baseline_reset_statistics(&algo, 1U << 1, true));
}

static void test_safe_prpt_spatial_aggregates(void)
{
	struct hx_safe_prpt prpt[5];
	s16 dif[HX_PIXELS] = { 0 };

	for (int i = 1; i < HX_PIXELS; i++)
		dif[i] = 10;
	dif[HX_COLS + 1] = -20;
	dif[2 * HX_COLS] = 30;
	dif[2 * HX_COLS + HX_COLS - 1] = -40;
	hx_safe_baseline_collect_prpt(prpt, dif);
	assert(prpt[2].count == HX_PIXELS);
	assert(prpt[2].max == 30);
	assert(prpt[2].min == -40);
	assert(prpt[1].min == -20);
	assert(prpt[3].count == HX_ROWS - 2);
	assert(prpt[4].count == HX_ROWS - 2);
	assert(prpt[0].count > 0);
}

static void test_safe_baseline_judge_uses_prpt(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	algo.safe_baseline_count = 1;
	algo.safe_bl2bl_prpt_cmf[2].count = HX_PIXELS;
	algo.safe_bl2raw_prpt_cmf[2].count = HX_PIXELS;
	algo.safe_bl2bl_prpt_cmf[2].max = 700;
	algo.safe_bl2bl_prpt_cmf[2].min = 0;
	algo.safe_bl2raw_prpt_cmf[2].max = 20;
	algo.safe_bl2raw_prpt_cmf[2].min = -20;
	assert(hx_safe_baseline_judge_current(&algo) ==
	       HX_SAFE_COMPARE_SAFE_BETTER);
	algo.safe_bl2raw_prpt_cmf[2].max = 800;
	assert(hx_safe_baseline_judge_current(&algo) ==
	       HX_SAFE_COMPARE_WORKING_BETTER);
}

static void test_safe_baseline_judge_includes_first_cell(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	algo.safe_baseline_count = 1;
	for (int region = 0; region < 5; region++) {
		algo.safe_bl2bl_prpt_cmf[region].count = HX_PIXELS;
		algo.safe_bl2raw_prpt_cmf[region].count = HX_PIXELS;
	}
	/* Keep all aggregate extrema benign and put the only corruption in
	 * cell zero.  A complete-grid judge must still prefer the safe baseline. */
	for (int i = 0; i < 4; i++)
		algo.safe_bl2bl_dif_cmf[i] = 1000;
	assert(hx_safe_baseline_judge_current(&algo) ==
	       HX_SAFE_COMPARE_SAFE_BETTER);
}

static void test_baseline_observation_includes_first_cell(void)
{
	struct hx_algo algo = {};
	struct hx_baseline_frame_observation obs;
	u16 raw[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	for (int i = 0; i < HX_PIXELS; i++) {
		algo.baseline_q8[i] = 0x7fee << HX_BASELINE_FRACTION_BITS;
		raw[i] = 0x7fee;
	}
	raw[0] += 500;
	hx_baseline_observe_frame(&algo, raw, 0, &obs);
	assert(obs.safe_bad_nodes == 1);
	assert(obs.max_signal == 500);
}

static void test_safe_baseline_queue_is_chronological(void)
{
	struct hx_algo algo = {};
	int i;

	hx_algo_init_defaults(&algo);
	assert(hx_safe_baseline_queue_all_valid(&algo));
	for (i = 0; i < HX_SAFE_BASELINE_SLOTS; i++) {
		algo.safe_baselines[i].valid = true;
		hx_safe_baseline_queue_record(&algo, i);
	}
	assert(algo.safe_queue_count == HX_SAFE_BASELINE_SLOTS);
	assert(hx_safe_baseline_queue_oldest(&algo) < HX_SAFE_BASELINE_SLOTS);
	{
		u8 oldest = hx_safe_baseline_queue_oldest(&algo);
		hx_safe_baseline_queue_record(&algo, 0);
		assert(algo.safe_queue_count == HX_SAFE_BASELINE_SLOTS);
		assert(hx_safe_baseline_queue_oldest(&algo) != oldest);
	}
}

static void test_safe_baseline_temp_queue_requires_cross_wake_match(void)
{
	struct hx_algo algo = {};
	s32 baseline[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] = (s32)0x7fee << 8;
	algo.screen_epoch = 1;
	assert(!hx_safe_baseline_temp_observe(&algo, baseline));
	assert(!hx_safe_baseline_temp_observe(&algo, baseline));
	assert(algo.safe_temp_queue_count == 1);
	algo.screen_epoch = 2;
	assert(hx_safe_baseline_temp_observe(&algo, baseline));
	assert(algo.safe_temp_queue_count == 2);
	/* A spatially incompatible candidate clears the temporary authority. */
	for (int i = 100; i < 105; i++)
		baseline[i] += (s32)1000 << 8;
	algo.screen_epoch = 3;
	assert(!hx_safe_baseline_temp_observe(&algo, baseline));
	assert(algo.safe_temp_queue_count == 1);
}

static void test_wake_replacement_requires_double_confirmation(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.safe_baseline_valid = true;
	assert(!algo.safe_baseline_replace_enabled);
	/* The Gaokun panel parameter is zero; an explicit enable is required for
	 * the optional replacement predicate below. */
	algo.safe_baseline_replace_enabled = true;
	algo.wake_finger_frames = algo.wake_finger_safe_frames;
	for (int slot = 0; slot < 2; slot++) {
		algo.safe_baselines[slot].valid = true;
		algo.safe_baselines[slot].confidence = HX_SAFE_BASELINE_CONFIRMED;
		algo.safe_baselines[slot].screen_epoch = slot + 1;
		hx_safe_baseline_queue_record(&algo, slot);
	}
	assert(hx_safe_baseline_queue_double_checked(&algo));
	for (int i = 1; i < 8; i++) {
		algo.safe_bl2bl_dif_cmf[i] = 400;
		algo.safe_bl2raw_dif_cmf[i] = 10;
	}
	assert(hx_safe_baseline_should_replace_wake(&algo));
	algo.safe_flags = HX_SAFE_FLAG_VERY_NOISY;
	assert(!hx_safe_baseline_should_replace_wake(&algo));
}

static void test_platform_state_gates_safe_update(void)
{
	struct hx_algo algo = {};
	struct hx_baseline_platform_state state = {
		.idle_transition = true,
		.charger_connected = true,
	};

	hx_algo_init_defaults(&algo);
	hx_safe_baseline_set_platform_state(&algo, &state);
	assert(algo.platform.idle_transition);
	assert(algo.platform.charger_connected);
	state.idle_transition = false;
	state.charger_noise = true;
	hx_safe_baseline_set_platform_state(&algo, &state);
	assert(algo.safe_flags & HX_SAFE_FLAG_VERY_NOISY);
	state.charger_noise = false;
	hx_safe_baseline_set_platform_state(&algo, &state);
	assert(!(algo.safe_flags & HX_SAFE_FLAG_VERY_NOISY));
}

static void test_safe_queue_all_valid_rejects_drifted_working_grid(void)
{
	struct hx_algo algo = {};
	s32 baseline[HX_PIXELS] = { 0 };

	hx_algo_init_defaults(&algo);
	for (int i = 0; i < HX_PIXELS; i++)
		baseline[i] = (s32)0x7fee << 8;
	hx_safe_baseline_bootstrap(&algo, baseline);
	memcpy(algo.baseline_q8, baseline, sizeof(baseline));
	assert(hx_safe_baseline_queue_all_valid(&algo));
	for (int i = 100; i < 104; i++)
		algo.baseline_q8[i] += (s32)500 << 8;
	assert(!hx_safe_baseline_queue_all_valid(&algo));
}

static void test_reset_statistics_use_elapsed_frames(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.frame_interval_ms = 1;
	algo.safe_reset_reason_trigger_frames = 5;
	algo.frame_sequence = 10;
	hx_safe_baseline_reset_statistics(&algo, 1U << 4, false);
	algo.frame_sequence = 13;
	hx_safe_baseline_reset_statistics(&algo, 1U << 4, false);
	assert(algo.safe_reset_reason_frames[4] == 4);
	algo.frame_sequence = 14;
	hx_safe_baseline_reset_statistics(&algo, 1U << 4, false);
	assert(algo.safe_reset_reason_frames[4] == 5);
}

static void test_platform_state_enters_special_blsm_stage(void)
{
	struct hx_algo algo = {};
	struct hx_baseline_platform_state state = { .panel_sd = true };
	u16 raw[HX_PIXELS];

	init_integrated_algo(&algo);
	init_raw(raw, 0x7fee);
	hx_safe_baseline_set_platform_state(&algo, &state);
	hx_algo_process_frame_state(&algo, raw, HX_FINGER_ABSENT);
	assert(algo.baseline_stage == HX_BLSM_FORCED);
}

static void test_platform_state_maps_to_reset_reasons(void)
{
	struct hx_algo algo = {};
	struct hx_baseline_platform_state state = {
		.charger_noise = true,
		.panel_sd = true,
	};

	hx_algo_init_defaults(&algo);
	algo.frame_interval_ms = 1;
	algo.safe_reset_reason_trigger_frames = 1;
	hx_safe_baseline_set_platform_state(&algo, &state);
	hx_safe_baseline_reset_policy(&algo, HX_SAFE_COMPARE_BOTH_VALID,
		false, HX_FINGER_ABSENT);
	assert(algo.safe_reset_reason_mask & (1U << 4));
	assert(algo.safe_reset_reason_mask & (1U << 8));
}

static void test_reset_screen_on_window_and_safe_raw_override(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.frame_interval_ms = 1;
	algo.safe_reset_reason_trigger_frames = 1;
	algo.safe_reset_screen_on_window[4] = 2;
	algo.screen_on_frame_sequence = 10;
	algo.frame_sequence = 20;
	assert(!hx_safe_baseline_reset_statistics(&algo, 1U << 4, false));
	assert(hx_safe_baseline_reset_statistics(&algo, 1U << 4, true));
}

static void test_side_very_negative_predicate(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 reference[HX_PIXELS];

	hx_algo_init_defaults(&algo);
	for (int i = 0; i < HX_PIXELS; i++) {
		raw[i] = 0x7fff;
		reference[i] = 0x7fff << HX_BASELINE_FRACTION_BITS;
	}
	raw[1 * HX_COLS + 4] = 0x7fff + 100;
	raw[1 * HX_COLS + 5] = 0x7fff - 400;
	assert(hx_safe_baseline_check_side_very_negative(&algo, raw, reference,
		0));
	raw[1 * HX_COLS + 5] = 0x7fff - 200;
	assert(!hx_safe_baseline_check_side_very_negative(&algo, raw, reference,
		0));
}

static void test_panel_profile_registry(void)
{
	const struct hx_panel_profile *profile;

	profile = hx_panel_profile_for_id("W273AS1310");
	assert(profile->kind == HX_PANEL_PROFILE_BOE_NEW);
	assert(profile->tx == HX_COLS && profile->rx == HX_ROWS);
	assert(profile->physical_width == 0x48);
	profile = hx_panel_profile_for_id("unknown-project");
	assert(profile->kind == HX_PANEL_PROFILE_GENERIC);
	assert(!strcmp(profile->project_id, "DFLT000000"));
}


void run_detection_tests(void)
{
	test_strongest_candidates_survive();
	test_strong_late_macro_zone_survives_capacity_cut();
	test_compact_strong_finger_is_not_a_palm();
	test_flat_large_contact_is_a_palm();
	test_adaptive_baseline_follows_panel_drift();
	test_disabled_iir_does_not_maintain_history();
	test_uniform_startup_offset_is_not_a_fullscreen_palm();
	test_firmware_finger_state_drives_baseline_dispatch();
	test_native_master_frame_status_parser();
	test_background_residuals_do_not_exhaust_zones_after_idle();
	test_four_frame_gap_relinks();
	test_large_zone_uses_full_arena();
	test_palm_box_persists_while_domain_inside();
	test_saddle_splits_two_peaks();
	test_live_clear_preserves_baseline_and_configuration();
	test_full_reset_clears_baseline();
	test_wake_quality_commits_stable_no_finger_baseline();
	test_wake_quality_never_starts_unsafe_candidate();
	test_finger_at_wake_uses_last_safe_baseline();
	test_raw_finger_at_wake_overrides_firmware_miss();
	test_raw_finger_override_clears_after_clean_release();
	test_clean_wake_preserves_existing_safe_baseline();
	test_bootstrap_safe_protects_clean_wake();
	test_ambiguous_wake_restores_safe_working();
	test_negative_thumb_at_wake_is_protected();
	test_finger_wake_rejects_noisy_background();
	test_finger_wake_mask_rejection_restores_safe_baseline();
	test_invalid_wake_grid_is_never_committed();
	test_finger_at_first_boot_uses_neutral_baseline();
	test_spatially_divergent_wake_preserves_safe();
	test_runtime_safe_baseline_uses_average_and_confirmation();
	test_runtime_safe_baseline_rejects_negative_line();
	test_runtime_comparator_recommends_safe_without_mutation();
	test_runtime_comparator_classifies_working_and_invalid();
	test_runtime_comparator_masks_touch_region();
	test_runtime_comparator_detects_absorbed_touch();
	test_safe_baseline_store_deduplicates_across_wakes();
	test_safe_baseline_store_selects_best_history();
	test_safe_baseline_queue_accepts_distinct_same_epoch_state();
	test_safe_baseline_selection_prefers_spatial_fit();
	test_runtime_blreset_restores_only_working_baseline();
	test_touch_hold_blocks_spatial_learning_until_clean_release();
	test_unclean_no_finger_frame_cannot_change_working_shape();
	test_small_panel_residual_updates_working_but_not_safe();
	test_bliir_updates_independently_of_safe_history();
	test_held_in_hand_release_uses_long_guard();
	test_negative_residual_restarts_guard_qualification();
	test_line_noise_is_held_but_compact_touch_is_not();
	test_reported_track_survives_silent_gap();
	test_hungarian_keeps_two_ids();
	test_safe_slot_runtime_selection_and_reacquire_hold();
	test_baseline_stage_machine();
	test_screen_on_hand_state_transitions();
	test_safe_reset_policy_debounces_and_separates_side_reset();
	test_safe_side_postprocess_restores_after_release();
	test_safe_buffer_comparison_tracks_common_mode();
	test_safe_reset_statistics_decay_and_trigger();
	test_official_safe_reset_thresholds();
	test_safe_prpt_spatial_aggregates();
	test_safe_baseline_judge_uses_prpt();
	test_safe_baseline_judge_includes_first_cell();
	test_baseline_observation_includes_first_cell();
	test_safe_baseline_queue_is_chronological();
	test_safe_baseline_temp_queue_requires_cross_wake_match();
	test_wake_replacement_requires_double_confirmation();
	test_platform_state_gates_safe_update();
	test_safe_queue_all_valid_rejects_drifted_working_grid();
	test_reset_statistics_use_elapsed_frames();
	test_platform_state_enters_special_blsm_stage();
	test_platform_state_maps_to_reset_reasons();
	test_reset_screen_on_window_and_safe_raw_override();
	test_side_very_negative_predicate();
	test_panel_profile_registry();
}
