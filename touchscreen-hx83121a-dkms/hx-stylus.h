/* SPDX-License-Identifier: GPL-2.0 */
/* Active-pen boundary; deliberately independent from the finger algorithm. */
#ifndef HX_STYLUS_H
#define HX_STYLUS_H

struct himax_ts_data;

#ifdef CONFIG_TOUCHSCREEN_HIMAX_HX83121A_STYLUS
int hx_stylus_init(struct himax_ts_data *ts);
void hx_stylus_remove(struct himax_ts_data *ts);
#else
static inline int hx_stylus_init(struct himax_ts_data *ts)
{
	(void)ts;
	return 0;
}
static inline void hx_stylus_remove(struct himax_ts_data *ts)
{
	(void)ts;
}
#endif

#endif /* HX_STYLUS_H */
