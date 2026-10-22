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
	HX_RAW_INIT_FLASH_RELOAD,
	HX_RAW_INIT_INSPECTION_UNLOCK,
	HX_RAW_INIT_INSPECTION_SORTING,
	HX_RAW_INIT_NFRAME_NORMAL,
	HX_RAW_INIT_NFRAME_MODE,
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

#ifdef __cplusplus
extern "C" {
#endif

/* Windows Init's reload/inspection/N-frame preparation, BEFORE power-on.
 * Only read/write callbacks are used. Like upstream write_and_verify, all
 * four bytes are transferred but only the low two are compared.
 */
int hx_raw_prepare_scan(const struct hx_raw_init_bus *bus,
			enum hx_raw_init_step *step);
/* Active Windows master-channel startup: preparation, power-on, full reload
 * word polling, verified EGO_RAWDATA selection and producer handshake.
 * Native sense_on/GPIO timing remains the platform callback's responsibility.
 */
int hx_raw_start_scan(const struct hx_raw_init_bus *bus,
		      enum hx_raw_init_step *step);
/* step identifies the failed phase on error, COMPLETE on success. */
int hx_raw_power_on(const struct hx_raw_init_bus *bus,
		    enum hx_raw_init_step *step);
const char *hx_raw_init_step_name(enum hx_raw_init_step step);

#ifdef __cplusplus
}
#endif
#endif
