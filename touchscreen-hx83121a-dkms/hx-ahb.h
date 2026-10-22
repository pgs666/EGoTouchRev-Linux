/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_AHB_H
#define HX_AHB_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint32_t u32;
#endif

/* Bus commands, below register I/O and above native SPI framing/retries.
 * write length counts DATA ONLY; a non-NULL address adds four wire bytes.
 * The caller serializes all access and enforces its SPI transfer capacity.
 */
struct hx_ahb_bus {
	void *context;
	int (*write)(void *context, u8 command, const u8 *address,
		     const u8 *data, u32 length);
	int (*read)(void *context, u8 command, u8 *data, u32 length);
	void (*sleep_ms)(void *context, unsigned int milliseconds);
	/* Physical pin level, matching Windows SetReset: false=low/asserted,
	 * true=high/released. Native code owns GPIO polarity and settling time.
	 */
	int (*set_reset)(void *context, bool high);
};

#ifdef __cplusplus
extern "C" {
#endif

int hx_ahb_burst(const struct hx_ahb_bus *bus, bool increment);
int hx_ahb_register_write(const struct hx_ahb_bus *bus, u32 address,
			  const u8 *data, u32 length);
int hx_ahb_register_read(const struct hx_ahb_bus *bus, u32 address,
			 u8 *data, u32 length);
/* Like upstream, successful transfers but mismatched burst readback are
 * tolerated after ten polls. This does NOT establish firmware readiness;
 * the caller must still complete IC identity/reload/raw validation.
 */
int hx_ahb_interface_on(const struct hx_ahb_bus *bus);
/* Init caller waits 10 ms after this helper, before OTP/scan preparation. */
int hx_ahb_hw_reset(const struct hx_ahb_bus *bus);
int hx_ahb_sense_on(const struct hx_ahb_bus *bus, bool flash_mode);

#ifdef __cplusplus
}
#endif
#endif
