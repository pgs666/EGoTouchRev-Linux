/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_RAW_INIT_H
#define HX_RAW_INIT_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

enum hx_raw_init_step {
	HX_RAW_INIT_RAW_CLEAR,
	HX_RAW_INIT_SORTING_CLEAR,
	HX_RAW_INIT_NFRAME,
	HX_RAW_INIT_RELOAD_CLEAR,
	HX_RAW_INIT_SENSE_ON,
	HX_RAW_INIT_RUNNING,
	HX_RAW_INIT_RELOAD_DONE,
	HX_RAW_INIT_RAW_SELECT,
	HX_RAW_INIT_PASSWORD,
	HX_RAW_INIT_COMPLETE,
};

/* Register I/O with IRQ quiesced and the caller's hardware mutex held.
 * sense_on includes the host's AHB wake, ISR clear and physical pin reset;
 * those bus/GPIO primitives remain platform-owned, not simulated here.
 */
struct hx_raw_init_bus {
	void *context;
	int (*write)(void *context, u32 address, const u8 *data, size_t length);
	int (*read)(void *context, u32 address, u8 *data, size_t length);
	int (*sense_on)(void *context);
	void (*sleep_ms)(void *context, unsigned int milliseconds);
};

/* step identifies the failed phase on error, COMPLETE on success. */
int hx_raw_power_on(const struct hx_raw_init_bus *bus,
		    enum hx_raw_init_step *step);
const char *hx_raw_init_step_name(enum hx_raw_init_step step);

#endif
