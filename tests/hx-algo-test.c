#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../touchscreen-hx83121a-dkms/hx-algo.h"
#include "../touchscreen-hx83121a-dkms/hx-frame-status.h"

static void init_raw(u16 raw[HX_PIXELS], u16 value)
{
	for (int i = 0; i < HX_PIXELS; i++)
		raw[i] = value;
}

static void add_blob(u16 raw[HX_PIXELS], int r, int c, u16 signal)
{
	static const int dr[] = {0, 0, 1, 1};
	static const int dc[] = {0, 1, 0, 1};

	for (int i = 0; i < 4; i++)
		raw[(r + dr[i]) * HX_COLS + c + dc[i]] = 0x7fee + signal - i;
}

static void run_detection(struct hx_algo *algo, u16 raw[HX_PIXELS])
{
	struct input_mt_pos pos[HIMAX_MAX_TOUCH];
	int count = 0;

	hx_preprocess_frame(algo, raw);
	hx_detect_macro_zones(algo);
	hx_reject_palms(algo);
	hx_detect_peaks(algo);
	hx_expand_and_resolve(algo, pos, &count);
	assert(count == algo->contact_count);
}

static void test_strongest_candidates_survive(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	s32 weakest_kept = INT_MAX;

	hx_algo_init_defaults(&algo);
	algo.cmf_enabled = false;
	algo.iir_enabled = false;
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
	algo.gesture_state = HX_GESTURE_DRAG;
	hx_algo_clear_live_state(&algo);
	assert(algo.peak_threshold == 777);
	assert(algo.baseline_initialized);
	assert(algo.baseline_q8[123] == baseline);
	assert(!algo.iir_initialized);
	assert(algo.palm_box_count == 0);
	assert(!algo.tracks[3].active);
	assert(algo.gesture_state == HX_GESTURE_IDLE);
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

static void init_integrated_algo(struct hx_algo *algo)
{
	hx_algo_init_defaults(algo);
	algo->cmf_enabled = false;
	algo->iir_enabled = false;
	algo->palm_enabled = false;
	algo->edge_reject_enabled = false;
	algo->debounce_base = 0;
	algo->debounce_weak_extra = 0;
	algo->debounce_edge_extra = 0;
	algo->track_start_debounce = 0;
	algo->track_smoothing = false;
}

static void test_integrated_ghost_and_gesture_lifecycle(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];

	init_integrated_algo(&algo);
	algo.gesture_long_press_frames = 3;
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 8, 2200);
	add_blob(raw, 10, 30, 500);
	assert(hx_algo_process_frame(&algo, raw) == 1);
	assert(algo.gesture_state == HX_GESTURE_PRESS);

	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 2200);
	assert(hx_algo_process_frame(&algo, raw) == 1);
	assert(algo.gesture_state == HX_GESTURE_DRAG);

	/* Start a stationary lifetime and reach long press. */
	hx_algo_reset_runtime(&algo);
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 2200);
	assert(hx_algo_process_frame(&algo, raw) == 1);
	assert(hx_algo_process_frame(&algo, raw) == 1);
	assert(hx_algo_process_frame(&algo, raw) == 1);
	assert(algo.gesture_state == HX_GESTURE_LONG_PRESS);

	init_raw(raw, 0x7fee);
	for (int i = 0; i <= algo.track_lost_frames; i++)
		hx_algo_process_frame(&algo, raw);
	assert(algo.gesture_state == HX_GESTURE_RELEASE);
	hx_algo_process_frame(&algo, raw);
	assert(algo.gesture_state == HX_GESTURE_IDLE);
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
	assert(algo.gesture_state == HX_GESTURE_RELEASE);
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

static void test_peak_ids_persist_across_motion(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	u8 first_id;

	init_integrated_algo(&algo);
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 10, 1800);
	run_detection(&algo, raw);
	assert(algo.peak_count == 1);
	first_id = algo.peaks[0].id;
	assert(first_id != 0 && algo.peaks[0].age == 0);

	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 11, 1800);
	run_detection(&algo, raw);
	assert(algo.peak_count == 1);
	assert(algo.peaks[0].id == first_id);
	assert(algo.peaks[0].age == 1);
	assert(algo.contacts[0].source_peak_id == first_id);
	assert(algo.contacts[0].source_peak_age == 1);
}

static void test_close_same_zone_split_requires_confirmation(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos first = { .x = 1000, .y = 700 };
	struct input_mt_pos det[2] = {
		{ .x = 1010, .y = 700 },
		{ .x = 1160, .y = 760 },
	};

	hx_algo_init_defaults(&algo);
	algo.track_smoothing = false;
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	algo.split_peak_confirm_frames = 4;
	algo.contacts[0].source_peak_id = 10;
	algo.contacts[0].source_zone_index = 2;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &first, 1);
	assert(hx_count_stable_tracks(&algo) == 1);

	for (int age = 0; age < 3; age++) {
		algo.contacts[0].source_peak_id = 10;
		algo.contacts[0].source_peak_age = age + 1;
		algo.contacts[0].source_zone_index = 2;
		algo.contacts[1].source_peak_id = 11;
		algo.contacts[1].source_peak_age = age;
		algo.contacts[1].source_zone_index = 2;
		algo.contact_count = 2;
		hx_track_contacts(&algo, det, 2);
		assert(hx_count_stable_tracks(&algo) == 1);
	}

	algo.contacts[0].source_peak_id = 10;
	algo.contacts[0].source_peak_age = 4;
	algo.contacts[0].source_zone_index = 2;
	algo.contacts[1].source_peak_id = 11;
	algo.contacts[1].source_peak_age = 3;
	algo.contacts[1].source_zone_index = 2;
	algo.contact_count = 2;
	hx_track_contacts(&algo, det, 2);
	assert(hx_count_stable_tracks(&algo) == 2);
	assert(algo.diag_split_peak_deferred == 3);
}

static void test_cross_zone_split_hands_off_existing_track(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos first = { .x = 1000, .y = 700 };
	struct input_mt_pos split[2] = {
		{ .x = 1010, .y = 700 },
		{ .x = 1140, .y = 760 },
	};
	int slot = -1;

	hx_algo_init_defaults(&algo);
	algo.track_smoothing = false;
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	algo.contacts[0].source_peak_id = 10;
	algo.contacts[0].source_peak_age = 8;
	algo.contacts[0].source_zone_index = 1;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &first, 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo.tracks[i].active)
			slot = i;
	assert(slot >= 0);

	/* For three frames the footprint is disconnected into nearby zones.  The
	 * young lobe must not create a second slot. */
	for (int age = 0; age < 3; age++) {
		algo.contacts[0].source_peak_id = 10;
		algo.contacts[0].source_peak_age = 9 + age;
		algo.contacts[0].source_zone_index = 1;
		algo.contacts[1].source_peak_id = 11;
		algo.contacts[1].source_peak_age = age;
		algo.contacts[1].source_zone_index = 2;
		algo.contact_count = 2;
		hx_track_contacts(&algo, split, 2);
		assert(hx_count_stable_tracks(&algo) == 1);
		assert(algo.tracks[slot].source_peak_id == 10);
	}
	assert(algo.diag_cross_zone_split_deferred == 3);

	/* Once the old lobe vanishes, the survivor maps onto the original slot and
	 * preserves the system tracking ID. */
	algo.contacts[0].source_peak_id = 11;
	algo.contacts[0].source_peak_age = 3;
	algo.contacts[0].source_zone_index = 2;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &split[1], 1);
	assert(hx_count_stable_tracks(&algo) == 1);
	assert(algo.tracks[slot].active);
	assert(algo.tracks[slot].source_peak_id == 11);
}

static void test_persistent_cross_zone_second_touch_is_eventually_reported(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos first = { .x = 1000, .y = 700 };
	struct input_mt_pos det[2] = {
		{ .x = 1010, .y = 700 },
		{ .x = 1140, .y = 760 },
	};

	hx_algo_init_defaults(&algo);
	algo.track_smoothing = false;
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	algo.contacts[0].source_peak_id = 20;
	algo.contacts[0].source_peak_age = 8;
	algo.contacts[0].source_zone_index = 1;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &first, 1);

	for (int age = 0; age < 4; age++) {
		algo.contacts[0].source_peak_id = 20;
		algo.contacts[0].source_peak_age = 9 + age;
		algo.contacts[0].source_zone_index = 1;
		algo.contacts[1].source_peak_id = 21;
		algo.contacts[1].source_peak_age = age;
		algo.contacts[1].source_zone_index = 2;
		algo.contact_count = 2;
		hx_track_contacts(&algo, det, 2);
		assert(hx_count_stable_tracks(&algo) == (age < 3 ? 1 : 2));
	}
}

static void test_transient_split_cannot_steal_parent_peak(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos first = { .x = 1000, .y = 700 };
	struct input_mt_pos split[2] = {
		/* New child is closer to the old coordinate than the parent. */
		{ .x = 1004, .y = 700 },
		{ .x = 1055, .y = 700 },
	};
	int slot = -1;

	hx_algo_init_defaults(&algo);
	algo.track_smoothing = false;
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	algo.contacts[0].source_peak_id = 10;
	algo.contacts[0].source_peak_age = 10;
	algo.contacts[0].source_zone_index = 2;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &first, 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo.tracks[i].active)
			slot = i;
	assert(slot >= 0);

	/* Reproduce the captured corner failure: for four frames a younger
	 * split child is geometrically closer, while the mature parent remains
	 * present in the same zone.  The parent must retain the existing slot and
	 * the child must not become a second reported contact. */
	for (int age = 0; age < 4; age++) {
		algo.contacts[0].source_peak_id = 11;
		algo.contacts[0].source_peak_age = age;
		algo.contacts[0].source_zone_index = 2;
		algo.contacts[1].source_peak_id = 10;
		algo.contacts[1].source_peak_age = 11 + age;
		algo.contacts[1].source_zone_index = 2;
		algo.contact_count = 2;
		hx_track_contacts(&algo, split, 2);
		assert(hx_count_stable_tracks(&algo) == 1);
		assert(algo.tracks[slot].active);
		assert(algo.tracks[slot].source_peak_id == 10);
	}

	algo.contacts[0].source_peak_id = 10;
	algo.contacts[0].source_peak_age = 15;
	algo.contacts[0].source_zone_index = 2;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &split[1], 1);
	assert(hx_count_stable_tracks(&algo) == 1);
	assert(algo.tracks[slot].source_peak_id == 10);
}

static void test_independent_second_touch_has_no_split_delay(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos first = { .x = 1000, .y = 700 };
	struct input_mt_pos det[2] = {
		{ .x = 1010, .y = 700 },
		{ .x = 1700, .y = 760 },
	};

	hx_algo_init_defaults(&algo);
	algo.track_smoothing = false;
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	algo.contacts[0].source_peak_id = 20;
	algo.contacts[0].source_zone_index = 3;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &first, 1);

	algo.contacts[0].source_peak_id = 20;
	algo.contacts[0].source_peak_age = 1;
	algo.contacts[0].source_zone_index = 3;
	algo.contacts[1].source_peak_id = 21;
	algo.contacts[1].source_peak_age = 0;
	algo.contacts[1].source_zone_index = 3;
	algo.contact_count = 2;
	hx_track_contacts(&algo, det, 2);
	assert(hx_count_stable_tracks(&algo) == 2);
}

static void test_unreported_track_does_not_enter_silent_gap(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos point = { .x = 1000, .y = 700 };

	hx_algo_init_defaults(&algo);
	algo.debounce_base = 3;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	hx_track_contacts(&algo, &point, 1);
	assert(algo.tracks[0].active && !algo.tracks[0].reported);
	hx_track_contacts(&algo, NULL, 0);
	assert(!algo.tracks[0].active);
}

int main(void)
{
	test_strongest_candidates_survive();
	test_strong_late_macro_zone_survives_capacity_cut();
	test_compact_strong_finger_is_not_a_palm();
	test_flat_large_contact_is_a_palm();
	test_adaptive_baseline_follows_panel_drift();
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
	test_reported_track_survives_silent_gap();
	test_hungarian_keeps_two_ids();
	test_dynamic_debounce_uses_signal_and_edge();
	test_firmware_rising_edge_start_bypasses_debounce();
	test_firmware_rising_compact_near_edge_starts_immediately();
	test_one_euro_filters_jitter_but_tracks_motion();
	test_integrated_ghost_and_gesture_lifecycle();
	test_confirmed_lift_releases_immediately();
	test_small_peak_only_continues_reported_track();
	test_small_peak_rejects_ambiguous_tracks();
	test_weak_small_peak_continues_only_single_reported_track();
	test_continuation_peak_is_bound_to_eligible_track();
	test_regular_peak_beats_continuation_for_same_track();
	test_edge_compensation_and_weak_rejection();
	test_peak_ids_persist_across_motion();
	test_close_same_zone_split_requires_confirmation();
	test_cross_zone_split_hands_off_existing_track();
	test_persistent_cross_zone_second_touch_is_eventually_reported();
	test_transient_split_cannot_steal_parent_peak();
	test_independent_second_touch_has_no_split_delay();
	test_unreported_track_does_not_enter_silent_gap();
	puts("hx-algo host regression tests passed");
	return 0;
}
