// SPDX-License-Identifier: GPL-2.0
/* Spatial SafeBaseline_GetPrpt aggregates. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#endif

#include "hx-algo-internal.h"

static void hx_safe_prpt_init(struct hx_safe_prpt *prpt)
{
	prpt->max = SHRT_MIN;
	prpt->min = SHRT_MAX;
	prpt->count = 0;
}

static void hx_safe_prpt_update(struct hx_safe_prpt *prpt, s16 value)
{
	prpt->max = max_t(s16, prpt->max, value);
	prpt->min = min_t(s16, prpt->min, value);
	if (prpt->count < U16_MAX)
		prpt->count++;
}

void hx_safe_baseline_collect_prpt(struct hx_safe_prpt prpt[5],
						 const s16 *dif)
{
	int i;

	for (i = 0; i < 5; i++)
		hx_safe_prpt_init(&prpt[i]);
	/* The reference loops over the complete grid.  Pixel zero is marked
	 * invalid by the touch solver later, but remains part of SafeBaseline's
	 * spatial statistics. */
	for (i = 0; i < HX_PIXELS; i++) {
		int row = i / HX_COLS;
		int col = i % HX_COLS;
		s16 value = dif[i];

		hx_safe_prpt_update(&prpt[2], value);
		if (row > 0 && row < HX_ROWS - 1 && col > 0 &&
		    col < HX_COLS - 1)
			hx_safe_prpt_update(&prpt[1], value);
		if (row >= 4 && row < HX_ROWS - 4 && col >= 4 &&
		    col < HX_COLS - 4)
			hx_safe_prpt_update(&prpt[0], value);
		if (col == 0 && row > 0 && row < HX_ROWS - 1)
			hx_safe_prpt_update(&prpt[3], value);
		if (col == HX_COLS - 1 && row > 0 && row < HX_ROWS - 1)
			hx_safe_prpt_update(&prpt[4], value);
	}
}
