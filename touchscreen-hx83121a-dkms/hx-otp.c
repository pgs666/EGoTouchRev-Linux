// SPDX-License-Identifier: GPL-2.0
/*
 * Read-only panel identity sequence from upstream HimaxChipLifecycle.cpp and
 * himax_thp_drv!hx_OTP_read (0x18001bc00). No firmware/OTP programming API.
 */
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <errno.h>
#include <string.h>
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))
#endif
#include "hx-otp.h"

#define HX_OTP_RETRIES 21

static void hx_otp_pack_u32(u8 data[4], u32 value)
{
	unsigned int i;

	for (i = 0; i < 4; i++)
		data[i] = value >> (i * 8);
}

static int hx_otp_burst(const struct hx_otp_bus *bus, size_t length)
{
	u8 value = 0x31;
	int ret;

	ret = bus->write(bus->context, 0x13, &value, 1);
	if (ret)
		return ret;
	value = length > 4 ? 0x13 : 0x12;
	return bus->write(bus->context, 0x0d, &value, 1);
}

static int hx_otp_register_write(const struct hx_otp_bus *bus, u32 address,
				 const u8 *data, size_t length)
{
	u8 packet[16];
	int ret;

	if (!length || length > sizeof(packet) - 4)
		return -EINVAL;
	ret = hx_otp_burst(bus, length);
	if (ret)
		return ret;
	hx_otp_pack_u32(packet, address);
	memcpy(packet + 4, data, length);
	return bus->write(bus->context, 0x00, packet, length + 4);
}

static int hx_otp_register_read(const struct hx_otp_bus *bus, u32 address,
				u8 data[4])
{
	u8 packet[4], direction = 0;
	int ret;

	ret = hx_otp_burst(bus, 4);
	if (ret)
		return ret;
	hx_otp_pack_u32(packet, address);
	ret = bus->write(bus->context, 0x00, packet, sizeof(packet));
	if (ret)
		return ret;
	ret = bus->write(bus->context, 0x0c, &direction, 1);
	if (ret)
		return ret;
	return bus->read(bus->context, 0x08, data, 4);
}

static int hx_otp_write_u32(const struct hx_otp_bus *bus, u32 address, u32 value)
{
	u8 data[4];

	hx_otp_pack_u32(data, value);
	return hx_otp_register_write(bus, address, data, sizeof(data));
}

static int hx_otp_write_verify(const struct hx_otp_bus *bus, u32 address,
			       u32 value, u32 mask)
{
	u8 data[4];
	u32 actual;
	int ret;

	ret = hx_otp_write_u32(bus, address, value);
	if (ret)
		return ret;
	ret = hx_otp_register_read(bus, address, data);
	if (ret)
		return ret;
	actual = (u32)data[0] | (u32)data[1] << 8 |
		(u32)data[2] << 16 | (u32)data[3] << 24;
	return (actual & mask) == (value & mask) ? 0 : -EIO;
}

static int hx_otp_safe_mode(const struct hx_otp_bus *bus, bool enable)
{
	const u8 key[2] = { enable ? 0x27 : 0, enable ? 0x95 : 0 };
	u8 actual[2];
	int attempt, ret = -EIO;

	for (attempt = 0; attempt < 6; attempt++) {
		ret = bus->write(bus->context, 0x31, key, sizeof(key));
		if (!ret)
			ret = bus->read(bus->context, 0x31, actual, sizeof(actual));
		if (!ret && !memcmp(actual, key, sizeof(key)))
			return 0;
		if (!ret)
			ret = -EIO;
	}
	return ret;
}

static int hx_otp_dd_access_once(const struct hx_otp_bus *bus, bool enable)
{
	int ret, second;

	ret = hx_otp_write_verify(bus, enable ? 0x9000009c : 0x90000280,
				 enable ? 0xdd : 0, 0xff);
	bus->sleep_ms(bus->context, 10);
	/* Attempt both shutdown writes even if the first one failed. */
	second = hx_otp_write_verify(bus, enable ? 0x90000280 : 0x9000009c,
				    enable ? 0xa5 : 0, 0xff);
	bus->sleep_ms(bus->context, 10);
	return ret ? ret : second;
}

static int hx_otp_dd_access(const struct hx_otp_bus *bus, bool enable)
{
	int attempt, ret;

	for (attempt = 0; attempt < HX_OTP_RETRIES; attempt++) {
		ret = hx_otp_dd_access_once(bus, enable);
		if (!ret)
			return 0;
		if (attempt + 1 < HX_OTP_RETRIES)
			bus->sleep_ms(bus->context, 10);
	}
	return ret;
}

static int hx_otp_dd_write(const struct hx_otp_bus *bus,
			   const u8 *command, size_t length)
{
	u8 trigger = 1;
	int ret;

	ret = hx_otp_register_write(bus, ((u32)command[0] | 0x30000U) << 12,
				    command, length);
	if (!ret && length % 4)
		ret = bus->write(bus->context, 0x0c, &trigger, 1);
	return ret;
}

static int hx_otp_dd_read(const struct hx_otp_bus *bus, u8 reg, u8 bank,
			  u8 offset, u8 *value)
{
	u32 address = ((((u32)reg | 0x30000U) << 2) | (bank & 3U)) << 10;
	u8 data[4], burst = 0x13;
	int ret, restore;

	ret = hx_otp_register_read(bus, address | offset, data);
	restore = bus->write(bus->context, 0x0d, &burst, 1);
	if (!ret && !restore)
		*value = data[0];
	return ret ? ret : restore;
}

static int hx_otp_read_slot(const struct hx_otp_bus *bus, u16 slot,
			    struct hx_otp_identity *identity)
{
	static const u8 unlock1[] = { 0xeb, 0x55, 0x66, 0xcc };
	static const u8 unlock2[] = { 0xb9, 0x83, 0x12, 0x1a };
	static const u8 read_count[] = { 0xbb, 0x0a };
	static const u8 restore_bb[] = {
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x70, 0x00, 0x0b,
	};
	u8 select[] = { 0xbb, 0x02, slot >> 8, slot & 0xff };
	bool access_attempted = false, selected = false;
	int ret, cleanup, attempt;
	unsigned int i;

	memset(identity, 0, sizeof(*identity));
	identity->slot = slot;
	ret = hx_otp_safe_mode(bus, true);
	if (ret)
		goto leave_safe;
	for (attempt = 0; attempt < HX_OTP_RETRIES; attempt++) {
		ret = hx_otp_write_verify(bus, 0x9000800c, 0xac53, 0xffff);
		if (!ret)
			break;
		if (attempt + 1 < HX_OTP_RETRIES)
			bus->sleep_ms(bus->context, 10);
	}
	if (ret)
		goto leave_safe;
	for (attempt = 0; attempt < HX_OTP_RETRIES; attempt++) {
		/* Control is a trigger; the vendor never requires equal readback. */
		ret = hx_otp_write_u32(bus, 0x90008010, 0x35ca);
		if (!ret)
			break;
		if (attempt + 1 < HX_OTP_RETRIES)
			bus->sleep_ms(bus->context, 10);
	}
	if (ret)
		goto leave_safe;
	access_attempted = true;
	ret = hx_otp_dd_access(bus, true);
	if (ret)
		goto close_access;
	selected = true;
	ret = hx_otp_dd_write(bus, unlock1, sizeof(unlock1));
	if (ret)
		goto restore;
	bus->sleep_ms(bus->context, 10);
	ret = hx_otp_dd_write(bus, unlock2, sizeof(unlock2));
	if (ret)
		goto restore;
	bus->sleep_ms(bus->context, 10);
	ret = hx_otp_dd_write(bus, select, sizeof(select));
	if (ret)
		goto restore;
	bus->sleep_ms(bus->context, 10);
	ret = hx_otp_dd_write(bus, read_count, sizeof(read_count));
	if (ret)
		goto restore;
	bus->sleep_ms(bus->context, 50);
	for (i = 0; i < 10; i++) {
		ret = hx_otp_dd_read(bus, 0xb1, 3, i + 1,
				     (u8 *)&identity->project_id[i]);
		if (ret)
			goto restore;
		bus->sleep_ms(bus->context, 1);
	}
restore:
	/* Restore the vendor's exact DD state, never write the ID back as data. */
	if (selected) {
		cleanup = hx_otp_register_write(bus, 0x300bb000, restore_bb,
						sizeof(restore_bb));
		if (!ret)
			ret = cleanup;
		bus->sleep_ms(bus->context, 10);
		if (!ret) {
			if (!hx_otp_dd_read(bus, 0xb4, 0, 8, &identity->revision))
				identity->extra_valid |= 1;
			if (!hx_otp_dd_read(bus, 0xe2, 0, 1, &identity->e2))
				identity->extra_valid |= 2;
		}
	}
close_access:
	if (access_attempted) {
		cleanup = hx_otp_dd_access(bus, false);
		if (!ret)
			ret = cleanup;
	}
leave_safe:
	cleanup = hx_otp_safe_mode(bus, false);
	if (!ret)
		ret = cleanup;
	bus->sleep_ms(bus->context, 10);
	if (ret)
		memset(identity->project_id, 0, sizeof(identity->project_id));
	return ret;
}

int hx_otp_read_project(const struct hx_otp_bus *bus,
			struct hx_otp_identity *identity)
{
	static const u16 slots[] = { 0x0f18, 0x0f3a, 0x0f5c };
	static const char * const known[] = {
		"W273AS2700", "W273AS2710", "W273AS1300", "W273AS1310",
	};
	unsigned int i, j;
	int ret;

	if (!bus || !identity || !bus->read || !bus->write || !bus->sleep_ms)
		return -EINVAL;
	memset(identity, 0, sizeof(*identity));
	for (i = 0; i < ARRAY_SIZE(slots); i++) {
		ret = hx_otp_read_slot(bus, slots[i], identity);
		if (ret)
			return ret; /* Transport failure is not an unrecognized ID. */
		for (j = 0; j < ARRAY_SIZE(known); j++)
			if (!strcmp(identity->project_id, known[j]))
				return 0;
	}
	return -ENODATA;
}
