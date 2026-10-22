// SPDX-License-Identifier: GPL-2.0
#include "hx-ahb.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static void pack_address(u8 data[4], u32 address)
{
	data[0] = address;
	data[1] = address >> 8;
	data[2] = address >> 16;
	data[3] = address >> 24;
}

int hx_ahb_burst(const struct hx_ahb_bus *bus, bool increment)
{
	u8 value = 0x31;
	int ret;

	if (!bus || !bus->write)
		return -EINVAL;
	ret = bus->write(bus->context, 0x13, NULL, &value, 1);
	if (ret)
		return ret;
	value = 0x12 | increment;
	return bus->write(bus->context, 0x0d, NULL, &value, 1);
}

int hx_ahb_register_write(const struct hx_ahb_bus *bus, u32 address,
			  const u8 *data, u32 length)
{
	u8 bytes[4];
	int ret;

	if (!bus || !bus->write || (!data && length))
		return -EINVAL;
	ret = hx_ahb_burst(bus, length > 4);
	if (ret)
		return ret;
	pack_address(bytes, address);
	/* Windows leaves burst in this transaction's selected mode, on both
	 * success and error. No extra restore transaction or hidden retry.
	 */
	return bus->write(bus->context, 0x00, bytes, data, length);
}

int hx_ahb_register_read(const struct hx_ahb_bus *bus, u32 address,
			 u8 *data, u32 length)
{
	u8 bytes[4], direction = 0;
	int ret;

	if (!bus || !bus->write || !bus->read || (!data && length) ||
	    length > 256)
		return -EINVAL;
	ret = hx_ahb_burst(bus, length > 4);
	if (ret)
		return ret;
	pack_address(bytes, address);
	ret = bus->write(bus->context, 0x00, bytes, NULL, 0);
	if (ret)
		return ret;
	ret = bus->write(bus->context, 0x0c, NULL, &direction, 1);
	if (ret)
		return ret;
	return bus->read(bus->context, 0x08, data, length);
}

int hx_ahb_interface_on(const struct hx_ahb_bus *bus)
{
	u8 data[4], continuous, increment;
	unsigned int i;
	int ret;

	if (!bus || !bus->write || !bus->read || !bus->sleep_ms)
		return -EINVAL;
	ret = bus->read(bus->context, 0x08, data, sizeof(data));
	if (ret)
		return ret;
	for (i = 0; i < 10; i++) {
		ret = hx_ahb_burst(bus, false);
		if (ret)
			return ret;
		ret = bus->read(bus->context, 0x13, &continuous, 1);
		if (ret)
			return ret;
		ret = bus->read(bus->context, 0x0d, &increment, 1);
		if (ret)
			return ret;
		if (continuous == 0x31 && increment == 0x12)
			return 0;
		bus->sleep_ms(bus->context, 1);
	}
	return 0;
}
