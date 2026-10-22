/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_OTP_H
#define HX_OTP_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

/* OTP uses the vendor DDReg/AHB mode (0x12/0x13), independently of the
 * controller's normal register-access mode. All callbacks run with IRQ off
 * and the hardware mutex held. This interface is also used by host fixtures.
 */
struct hx_otp_bus {
	void *context;
	int (*read)(void *context, u8 command, u8 *data, size_t length);
	int (*write)(void *context, u8 command, const u8 *data, size_t length);
	void (*sleep_ms)(void *context, unsigned int milliseconds);
};

struct hx_otp_identity {
	char project_id[11];
	u16 slot;
	u8 revision;
	u8 e2;
	u8 extra_valid;
};

int hx_otp_read_project(const struct hx_otp_bus *bus,
			struct hx_otp_identity *identity);

#endif
