#ifndef HX_ALGO_TEST_COMMON_H
#define HX_ALGO_TEST_COMMON_H
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../touchscreen-hx83121a-dkms/hx-algo.h"
#include "../touchscreen-hx83121a-dkms/hx-frame-status.h"

static inline void init_raw(u16 raw[HX_PIXELS], u16 value)
{
	for (int i = 0; i < HX_PIXELS; i++)
		raw[i] = value;
}

static inline void add_blob(u16 raw[HX_PIXELS], int r, int c, u16 signal)
{
	static const int dr[] = {0, 0, 1, 1};
	static const int dc[] = {0, 1, 0, 1};

	for (int i = 0; i < 4; i++)
		raw[(r + dr[i]) * HX_COLS + c + dc[i]] = 0x7fee + signal - i;
}

static inline void run_detection(struct hx_algo *algo, u16 raw[HX_PIXELS])
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

static inline void init_integrated_algo(struct hx_algo *algo)
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

#endif
