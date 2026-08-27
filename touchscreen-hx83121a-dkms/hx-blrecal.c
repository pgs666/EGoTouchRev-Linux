// SPDX-License-Identifier: GPL-2.0
/* Gaokun BLRecal_Process frame-window logic. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

static void hx_raw_minmax(const u16 *raw, u16 *minimum, u16 *maximum)
{
	u16 lo = U16_MAX, hi = 0;
	int i;

	for (i = 0; i < HX_PIXELS; i++) {
		u16 value = le16_to_cpup(raw + i);

		lo = min(lo, value);
		hi = max(hi, value);
	}
	*minimum = lo;
	*maximum = hi;
}

bool hx_blrecal_process(struct hx_algo *algo, const u16 *raw,
			 s16 max_signal, s16 min_signal, bool reset_event,
			 bool auto_calibration)
{
	u16 raw_min, raw_max;
	bool new_request = false;
	u16 half_span = algo->blrecal_raw_span >> 1;
	u32 upper = (u32)algo->blrecal_raw_center + half_span;
	u32 lower = algo->blrecal_raw_center > half_span ?
		algo->blrecal_raw_center - half_span : 0;

	if (reset_event) {
		algo->blrecal_frame = 0;
		algo->blrecal_abnormal_count = 0;
		algo->blrecal_requested = false;
	}
	if (algo->blrecal_frame < 5) {
		algo->blrecal_frame++;
		return false;
	}
	if (algo->blrecal_frame < 15) {
		algo->blrecal_frame++;
		if (max_signal >= algo->blrecal_signal_threshold ||
		    min_signal <= -algo->blrecal_signal_threshold)
			return false;
		hx_raw_minmax(raw, &raw_min, &raw_max);
		/* Direct translation of flash+0x48/0x4a checks: excessive span,
		 * high maximum, or low minimum increments the ten-frame vote. */
		if ((u32)raw_max - raw_min > algo->blrecal_raw_span ||
		    raw_max > upper || raw_min < lower)
			algo->blrecal_abnormal_count = min_t(u8,
				algo->blrecal_abnormal_count + 1, U8_MAX);
		return false;
	}
	if (algo->blrecal_abnormal_count > 2 && !auto_calibration &&
	    !algo->blrecal_requested) {
		/* The vendor request bit is latched until the AFE consumer
		 * acknowledges it; do not make BLSM weak-signal stage flicker on and
		 * off once per frame. */
		algo->blrecal_requested = true;
		new_request = true;
	}
	algo->blrecal_abnormal_count = 0;
	return new_request;
}

void hx_blrecal_ack(struct hx_algo *algo, bool success)
{
	/* Both success and an explicit hardware failure clear the one-shot
	 * request.  The next 15-frame observation window will independently
	 * decide whether another request is warranted. */
	(void)success;
	algo->blrecal_requested = false;
	algo->blrecal_frame = 0;
	algo->blrecal_abnormal_count = 0;
}
