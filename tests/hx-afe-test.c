// SPDX-License-Identifier: GPL-2.0
#include "hx-afe.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HX_AFE_UPSTREAM_REFERENCE
void hx_afe_reference_packet(u8 command, u8 value, u8 *packet);
#endif

struct fake_bus {
	u8 body[16], trigger[4];
	u32 address;
	unsigned int operations, fail_at;
};

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static int write_register(void *context, u32 address, const u8 *data,
			   size_t length)
{
	struct fake_bus *fake = context;
	unsigned int step = ++fake->operations;

	if (step == fake->fail_at)
		return -EREMOTEIO;
	check(step == 1 || step == 2, "body and trigger are the only writes");
	if (step == 1) {
		check(length == 16, "whole body is written before trigger");
		check(address >= 0x10007550U && address <= 0x10007590U &&
		      (address - 0x10007550U) % 16 == 0,
		      "command stays inside the five documented slots");
		fake->address = address;
		memcpy(fake->body, data, length);
	} else {
		check(length == 4 && address == fake->address,
		      "trigger uses the same slot after complete body");
		memcpy(fake->trigger, data, length);
	}
	return 0;
}

static int read_register(void *context, u32 address, u8 *data, size_t length)
{
	struct fake_bus *fake = context;
	unsigned int step = ++fake->operations;

	if (step == fake->fail_at)
		return -EREMOTEIO;
	check(step == 3 && address == fake->address && length == 16,
	      "transport readback follows trigger at the same slot");
	/* Real firmware may consume/replace the mailbox immediately. An exact
	 * echo is not required by the upstream protocol, nor completion proof.
	 */
	memset(data, 0x5a, length);
	return 0;
}

static void check_packet(const struct fake_bus *fake, u8 command, u8 value)
{
	unsigned int sum = 0x8aa8, i;

	check(fake->body[0] == 0 && fake->body[1] == 0 &&
	      fake->body[2] == command && fake->body[4] == value,
	      "body carries command/value with cleared virtual header");
	for (i = 0; i < 16; i++) {
		if (i != 2 && i != 4 && i != 14 && i != 15)
			check(fake->body[i] == 0, "all reserved bytes remain zero");
	}
	for (i = 0; i < 16; i += 2)
		sum += fake->body[i] | ((unsigned int)fake->body[i + 1] << 8);
	check((sum & 0xffff) == 0, "virtual header plus little-endian words sums to zero");
	check(!memcmp(fake->trigger, (u8[]){ 0xa8, 0x8a, command, 0 }, 4),
	      "trigger supplies actual header after body");
#ifdef HX_AFE_UPSTREAM_REFERENCE
	{
		u8 reference[16];

		hx_afe_reference_packet(command, value, reference);
		check(!memcmp(fake->body, reference, sizeof(reference)),
		      "packet equals the compiled upstream Windows builder");
	}
#endif
}

int main(void)
{
	static const u8 vectors[][4] = {
		/* command, parameter, checksum LSB, checksum MSB */
		{ 0x01, 0, 0x57, 0x75 }, /* StartCalibration */
		{ 0x06, 4, 0x4e, 0x75 }, /* ClearStatus(freq) */
		{ 0x06, 8, 0x4a, 0x75 }, /* ClearStatus(calibration) */
		{ 0x0c, 1, 0x4b, 0x75 }, /* ForceToFreqPoint */
		{ 0x0e, 0, 0x4a, 0x75 }, /* ForceToScanRate */
		{ 0x0a, 0, 0x4e, 0x75 }, /* EnterIdle */
	};
	struct fake_bus fake = { 0 };
	struct hx_afe_bus bus = {
		.context = &fake,
		.write = write_register,
		.read = read_register,
	};
	unsigned int command, value, i, failure;
	u8 slot = 0;

	for (i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
		const u8 *vector = vectors[i];
		u8 before = slot;

		memset(&fake, 0, sizeof(fake));
		check(!hx_afe_send(&bus, &slot, vector[0], vector[1]),
		      "documented command succeeds");
		check(fake.body[14] == vector[2] && fake.body[15] == vector[3],
		      "documented command has fixed expected checksum bytes");
		check(fake.operations == 3 &&
		      fake.address == 0x10007550U + before * 16,
		      "successful command has exactly three ordered transactions");
		check(slot == (before + 1) % 5, "all five slots wrap in order");
		check_packet(&fake, vector[0], vector[1]);
	}
	/* Exhaustive byte-domain check against the actual upstream builder in
	 * CTest. No device is opened or command dispatched to real hardware.
	 */
	for (command = 0; command <= 255; command++) {
		for (value = 0; value <= 255; value++) {
			memset(&fake, 0, sizeof(fake));
			check(!hx_afe_send(&bus, &slot, command, value), "byte-domain send");
			check_packet(&fake, command, value);
		}
	}
	for (i = 0; i < 5; i++) {
		for (failure = 1; failure <= 3; failure++) {
			memset(&fake, 0, sizeof(fake));
			fake.fail_at = failure;
			slot = i;
			check(hx_afe_send(&bus, &slot, 1, 0) == -EREMOTEIO &&
			      fake.operations == failure && slot == i,
			      "each failed transaction aborts and preserves original slot");
			memset(&fake, 0, sizeof(fake));
			check(!hx_afe_send(&bus, &slot, 1, 0) &&
			      fake.address == 0x10007550U + i * 16 &&
			      slot == (i + 1) % 5,
			      "retry after failure reuses the original slot");
		}
	}
	check(hx_afe_send(NULL, &slot, 1, 0) == -EINVAL &&
	      hx_afe_send(&bus, NULL, 1, 0) == -EINVAL, "invalid pointers rejected");
	bus.read = NULL;
	check(hx_afe_send(&bus, &slot, 1, 0) == -EINVAL, "missing read callback rejected");
	bus.read = read_register;
	bus.write = NULL;
	check(hx_afe_send(&bus, &slot, 1, 0) == -EINVAL, "missing write callback rejected");
	puts("AFE packet, five-slot ordering, fault/retry and byte-domain tests passed");
	return 0;
}
