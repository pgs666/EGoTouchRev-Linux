#include "hx-algo-test-common.h"

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
