// SPDX-License-Identifier: GPL-2.0
/*
 * Reserved active-pen translation unit.  Gaokun's mutual-cap stream does not
 * expose a verified pen packet yet, so these lifecycle hooks are no-ops.
 * Keeping the file separate prevents future pen work from entering the
 * latency-sensitive finger and baseline paths.
 */
#include "himax-spi.h"
#include "hx-stylus.h"

int hx_stylus_init(struct himax_ts_data *ts)
{
	(void)ts;
	return 0;
}

void hx_stylus_remove(struct himax_ts_data *ts)
{
	(void)ts;
}
