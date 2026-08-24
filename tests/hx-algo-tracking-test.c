#include "hx-algo-test-common.h"

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

static void test_peak_ids_follow_fast_predicted_motion(void)
{
	struct hx_algo algo = {};
	u16 raw[HX_PIXELS];
	u8 id;

	init_integrated_algo(&algo);
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 8, 1800);
	run_detection(&algo, raw);
	id = algo.peaks[0].id;

	/* Both steps exceed the legacy adjacent-cell (<4 Manhattan) gate. */
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 13, 1800);
	run_detection(&algo, raw);
	assert(algo.peaks[0].id == id);
	init_raw(raw, 0x7fee);
	add_blob(raw, 10, 19, 1800);
	run_detection(&algo, raw);
	assert(algo.peaks[0].id == id);
	assert(algo.peaks[0].age == 2);
}

static void test_fast_motion_handoff_defers_mature_residual(void)
{
	struct hx_algo algo = {};
	struct input_mt_pos p0 = { .x = 1291, .y = 2212 };
	struct input_mt_pos p1 = { .x = 1221, .y = 2107 };
	struct input_mt_pos p2 = { .x = 1128, .y = 1990 };
	struct input_mt_pos split[2] = {
		{ .x = 1124, .y = 1988 }, /* stalled mature residual */
		{ .x = 760, .y = 1717 },  /* actual fast-moving lobe */
	};
	int slot = -1;

	hx_algo_init_defaults(&algo);
	algo.track_smoothing = false;
	algo.debounce_base = 0;
	algo.debounce_weak_extra = 0;
	algo.debounce_edge_extra = 0;
	algo.touch_active = true;
	algo.contacts[0].source_peak_id = 186;
	algo.contacts[0].source_peak_age = 20;
	algo.contacts[0].source_zone_index = 1;
	algo.contact_count = 1;
	hx_track_contacts(&algo, &p0, 1);
	hx_track_contacts(&algo, &p1, 1);
	hx_track_contacts(&algo, &p2, 1);
	for (int i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo.tracks[i].active)
			slot = i;
	assert(slot >= 0);

	algo.contacts[0].source_peak_id = 186;
	algo.contacts[0].source_peak_age = 23;
	algo.contacts[0].source_zone_index = 1;
	algo.contacts[1].source_peak_id = 187;
	algo.contacts[1].source_peak_age = 0;
	algo.contacts[1].source_zone_index = 2;
	algo.contact_count = 2;
	hx_track_contacts(&algo, split, 2);
	assert(hx_count_stable_tracks(&algo) == 1);
	assert(algo.tracks[slot].source_peak_id == 187);
	assert(algo.tracks[slot].x == split[1].x);
	assert(algo.diag_peak_id_handoffs == 1);
	assert(algo.diag_handoff_residual_deferred == 1);

	/* Once the track owns the moving ID, the old residual is no longer the
	 * immediate handoff ID.  Its competition history must still keep it from
	 * becoming a second slot on the following frames. */
	for (int age = 1; age < 4; age++) {
		split[1].x -= 90;
		split[1].y -= 70;
		algo.contacts[0].source_peak_id = 186;
		algo.contacts[0].source_peak_age = 23 + age;
		algo.contacts[0].source_zone_index = 1;
		algo.contacts[1].source_peak_id = 187;
		algo.contacts[1].source_peak_age = age;
		algo.contacts[1].source_zone_index = 2;
		algo.contact_count = 2;
		hx_track_contacts(&algo, split, 2);
		assert(hx_count_stable_tracks(&algo) == 1);
		assert(algo.tracks[slot].source_peak_id == 187);
	}
	assert(algo.diag_handoff_residual_deferred == 4);
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


void run_tracking_tests(void)
{
	test_peak_ids_persist_across_motion();
	test_peak_ids_follow_fast_predicted_motion();
	test_fast_motion_handoff_defers_mature_residual();
	test_close_same_zone_split_requires_confirmation();
	test_cross_zone_split_hands_off_existing_track();
	test_persistent_cross_zone_second_touch_is_eventually_reported();
	test_transient_split_cannot_steal_parent_peak();
	test_independent_second_touch_has_no_split_delay();
	test_unreported_track_does_not_enter_silent_gap();
}
