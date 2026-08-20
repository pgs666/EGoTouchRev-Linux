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

static void test_runtime_reset_preserves_configuration(void)
{
	struct hx_algo algo = {};

	hx_algo_init_defaults(&algo);
	algo.peak_threshold = 777;
	algo.baseline_initialized = true;
	algo.iir_initialized = true;
	algo.palm_box_count = 1;
	algo.tracks[3].active = true;
	algo.tracks[3].reported = true;
	algo.gesture_state = HX_GESTURE_DRAG;
	hx_algo_reset_runtime(&algo);
	assert(algo.peak_threshold == 777);
	assert(!algo.baseline_initialized);
	assert(!algo.iir_initialized);
	assert(algo.palm_box_count == 0);
	assert(!algo.tracks[3].active);
	assert(algo.gesture_state == HX_GESTURE_IDLE);
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
	test_runtime_reset_preserves_configuration();
	test_reported_track_survives_silent_gap();
	test_hungarian_keeps_two_ids();
	test_dynamic_debounce_uses_signal_and_edge();
	test_one_euro_filters_jitter_but_tracks_motion();
	test_integrated_ghost_and_gesture_lifecycle();
	test_edge_compensation_and_weak_rejection();
	puts("hx-algo host regression tests passed");
	return 0;
}
