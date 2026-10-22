// SPDX-License-Identifier: GPL-2.0
/* HimaxProtocol::build_command_packet/send_command, master AFE mailbox. */
#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif
#include "hx-afe.h"

int hx_afe_send(const struct hx_afe_bus *bus, u8 *slot, u8 command, u8 value)
{
	u8 packet[HX_AFE_PACKET_SIZE] = { 0 };
	u8 trigger[4] = { 0xa8, 0x8a, command, 0x00 };
	u8 response[HX_AFE_PACKET_SIZE];
	u16 checksum;
	u32 address;
	int ret;

	if (!bus || !slot || !bus->write || !bus->read)
		return -EINVAL;

	/* The virtual header participates in the checksum but is supplied only
	 * by the later trigger transaction, after the complete body is written.
	 */
	checksum = (u16)(0U - (0x8aa8U + command + value));
	packet[2] = command;
	packet[4] = value;
	packet[14] = checksum & 0xff;
	packet[15] = checksum >> 8;
	address = HX_AFE_SLOT_BASE + (*slot % HX_AFE_SLOT_COUNT) *
		HX_AFE_SLOT_STRIDE;
	ret = bus->write(bus->context, address, packet, sizeof(packet));
	if (ret)
		return ret;
	ret = bus->write(bus->context, address, trigger, sizeof(trigger));
	if (ret)
		return ret;
	ret = bus->read(bus->context, address, response, sizeof(response));
	if (ret)
		return ret;

	/* Match Windows: a failed step leaves the slot unchanged for retry. */
	*slot = (*slot + 1) % HX_AFE_SLOT_COUNT;
	return 0;
}
