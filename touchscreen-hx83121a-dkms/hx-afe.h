/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_AFE_H
#define HX_AFE_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HX_AFE_PACKET_SIZE 16
#define HX_AFE_SLOT_COUNT 5
#define HX_AFE_SLOT_BASE 0x10007550U
#define HX_AFE_SLOT_STRIDE 0x10U

/* Register transactions, not raw SPI. The caller holds the hardware mutex.
 * A successful read acknowledges transport only, not firmware completion.
 */
struct hx_afe_bus {
	void *context;
	int (*write)(void *context, u32 address, const u8 *data, size_t length);
	int (*read)(void *context, u32 address, u8 *data, size_t length);
};

int hx_afe_send(const struct hx_afe_bus *bus, u8 *slot, u8 command, u8 value);

#ifdef __cplusplus
}
#endif
#endif
