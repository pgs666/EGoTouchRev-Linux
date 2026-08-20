#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../touchscreen-hx83121a-dkms/hx-algo.h"

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

int main(void)
{
	test_strongest_candidates_survive();
	test_compact_strong_finger_is_not_a_palm();
	test_flat_large_contact_is_a_palm();
	test_adaptive_baseline_follows_panel_drift();
	test_four_frame_gap_relinks();
	puts("hx-algo host regression tests passed");
	return 0;
}
