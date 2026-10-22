// SPDX-License-Identifier: GPL-2.0
#include "hx-otp.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct fake_bus {
	u32 address, lpwu, unlock1, unlock2;
	u8 safe[2], burst;
	char ids[3][11];
	unsigned int slot, stage, restores, safe_entries;
	unsigned int control_writes, control_reads, sleeps, operations;
	unsigned int fail_at, fail_ctrl, fail_offset;
	bool pending_trigger, read_started, fail_cleanup;
};

static void check(bool value, const char *name)
{
	if (!value) {
		fprintf(stderr, "FAIL: %s\n", name);
		exit(1);
	}
}

static u32 le32(const u8 *data)
{
	return (u32)data[0] | (u32)data[1] << 8 |
		(u32)data[2] << 16 | (u32)data[3] << 24;
}

static bool fault(struct fake_bus *fake)
{
	return ++fake->operations == fake->fail_at;
}

static int write_bus(void *context, u8 command, const u8 *data, size_t length)
{
	static const u8 restore[] = {
		0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x70, 0x00, 0x0b,
	};
	struct fake_bus *fake = context;
	u32 value, address;
	const u8 *payload;
	size_t bytes;

	if (fault(fake))
		return -EREMOTEIO;
	if (command == 0x13) {
		check(length == 1 && *data == 0x31, "AHB continuous mode");
		return 0;
	}
	if (command == 0x0d) {
		check(length == 1 && (*data == 0x12 || *data == 0x13),
		      "OTP uses vendor DDReg burst bits");
		fake->burst = *data;
		return 0;
	}
	if (command == 0x31) {
		check(length == 2, "safe-mode key length");
		check((data[0] == 0x27 && data[1] == 0x95) ||
		      (!data[0] && !data[1]), "safe-mode key");
		if (data[0])
			fake->safe_entries++;
		memcpy(fake->safe, data, 2);
		return 0;
	}
	if (command == 0x0c) {
		check(length == 1 && *data <= 1, "direction/short-write trigger");
		if (*data) {
			check(fake->pending_trigger, "unaligned DD write before trigger");
			fake->pending_trigger = false;
			fake->read_started = true;
		}
		return 0;
	}
	check(command == 0 && length >= 4 && length <= 16, "bounded AHB write");
	fake->address = address = le32(data);
	if (length == 4)
		return 0;
	payload = data + 4;
	bytes = length - 4;
	check(fake->safe[0] == 0x27, "register writes only in safe mode");
	check(fake->burst == (bytes > 4 ? 0x13 : 0x12), "write burst follows length");
	value = bytes >= 4 ? le32(payload) : 0;
	switch (address) {
	case 0x9000800c:
		check(bytes == 4 && value == 0xac53, "LPWU interrupt value");
		fake->lpwu = value;
		return 0;
	case 0x90008010:
		check(bytes == 4 && value == 0x35ca, "LPWU control value");
		fake->control_writes++;
		if (fake->fail_ctrl) {
			fake->fail_ctrl--;
			return -EREMOTEIO;
		}
		return 0;
	case 0x9000009c:
		check(bytes == 4 && (value == 0xdd || value == 0), "DD unlock first register");
		fake->unlock1 = value;
		return 0;
	case 0x90000280:
		check(bytes == 4 && (value == 0xa5 || value == 0), "DD unlock second register");
		if (!value && fake->fail_cleanup)
			return -EREMOTEIO;
		fake->unlock2 = value;
		return 0;
	case 0x300eb000:
		check(fake->unlock1 == 0xdd && fake->unlock2 == 0xa5, "DD access unlocked");
		check(bytes == 4 && !memcmp(payload, "\xeb\x55\x66\xcc", 4),
		      "EB includes register prefix");
		fake->stage = 1;
		return 0;
	case 0x300b9000:
		check(fake->stage == 1 && bytes == 4 &&
		      !memcmp(payload, "\xb9\x83\x12\x1a", 4), "B9 command follows EB");
		fake->stage = 2;
		return 0;
	case 0x300bb000:
		if (bytes == sizeof(restore)) {
			check(!memcmp(payload, restore, sizeof(restore)), "exact BB restore data");
			fake->restores++;
			fake->stage = 0;
			fake->read_started = false;
			fake->pending_trigger = false;
			return 0;
		}
		if (bytes == 4) {
			const u16 slot = (u16)payload[2] << 8 | payload[3];

			check(fake->stage == 2 && payload[0] == 0xbb &&
			      payload[1] == 2, "BB select includes register prefix");
			check(slot == 0xf18 || slot == 0xf3a || slot == 0xf5c,
			      "only documented read slots");
			fake->slot = slot == 0xf18 ? 0 : slot == 0xf3a ? 1 : 2;
			fake->stage = 3;
			return 0;
		}
		check(fake->stage == 3 && bytes == 2 && payload[0] == 0xbb &&
		      payload[1] == 10, "BB count requests ten bytes");
		fake->pending_trigger = true;
		return 0;
	default:
		check(false, "unexpected register write");
		return -EINVAL;
	}
}

static int read_bus(void *context, u8 command, u8 *data, size_t length)
{
	struct fake_bus *fake = context;
	u32 value = 0;
	unsigned int i;

	if (fault(fake))
		return -EREMOTEIO;
	if (command == 0x31) {
		check(length == 2, "safe-mode echo length");
		memcpy(data, fake->safe, 2);
		return 0;
	}
	check(command == 8 && length == 4 && fake->burst == 0x12, "AHB read mode");
	switch (fake->address) {
	case 0x9000800c:
		value = fake->lpwu | 0x5a5a0000; /* Only low 16 bits are specified. */
		break;
	case 0x90008010:
		fake->control_reads++;
		value = 0; /* Trigger does not echo 0x35ca. */
		break;
	case 0x9000009c:
		value = fake->unlock1 | 0x5a5a0000;
		break;
	case 0x90000280:
		value = fake->unlock2 | 0x5a5a0000;
		break;
	case 0x300b4008:
		value = 0x42;
		break;
	case 0x300e2001:
		value = 0x88;
		break;
	default: {
		const unsigned int offset = fake->address & 0x3ff;

		check((fake->address & ~0x3ffU) == 0x300b1c00 &&
		      offset >= 1 && offset <= 10, "B1 bank three address transform");
		check(fake->stage == 3 && fake->read_started &&
		      fake->unlock1 == 0xdd && fake->unlock2 == 0xa5, "OTP read prerequisites");
		if (offset == fake->fail_offset)
			return -EREMOTEIO;
		value = (u8)fake->ids[fake->slot][offset - 1];
		break;
	}
	}
	for (i = 0; i < 4; i++)
		data[i] = value >> (8 * i);
	return 0;
}

static void sleep_bus(void *context, unsigned int milliseconds)
{
	struct fake_bus *fake = context;

	check(milliseconds == 1 || milliseconds == 10 || milliseconds == 50,
	      "documented timing intervals");
	fake->sleeps += milliseconds;
}

static struct fake_bus fresh(void)
{
	struct fake_bus fake = { 0 };
	unsigned int i;

	for (i = 0; i < 3; i++)
		memcpy(fake.ids[i], "W273AS2700", 11);
	return fake;
}

static int run(struct fake_bus *fake, struct hx_otp_identity *identity)
{
	const struct hx_otp_bus bus = {
		.context = fake,
		.read = read_bus,
		.write = write_bus,
		.sleep_ms = sleep_bus,
	};

	return hx_otp_read_project(&bus, identity);
}

int main(void)
{
	struct hx_otp_identity identity;
	struct fake_bus fake = fresh();
	unsigned int operations, i;
	int ret;

	check(!run(&fake, &identity), "primary slot succeeds without control readback");
	check(!strcmp(identity.project_id, "W273AS2700") && identity.slot == 0xf18,
	      "primary identity");
	check(identity.extra_valid == 3 && identity.revision == 0x42 && identity.e2 == 0x88,
	      "optional panel bytes");
	check(fake.control_writes == 1 && !fake.control_reads && fake.restores == 1,
	      "write-only control and exact restoration");
	check(!fake.safe[0] && !fake.unlock1 && !fake.unlock2, "successful cleanup");
	operations = fake.operations;
	fake = fresh();
	memcpy(fake.ids[0], "UNKNOWN000", 11);
	memcpy(fake.ids[1], "W273AS1310", 11);
	check(!run(&fake, &identity) && identity.slot == 0xf3a &&
	      !strcmp(identity.project_id, "W273AS1310") && fake.restores == 2,
	      "unrecognized first slot advances to second");
	fake = fresh();
	for (i = 0; i < 3; i++)
		memcpy(fake.ids[i], "UNKNOWN000", 11);
	check(run(&fake, &identity) == -ENODATA && fake.safe_entries == 3 &&
	      identity.slot == 0xf5c, "three unknown slots return ENODATA");
	fake = fresh();
	fake.fail_ctrl = 1;
	check(!run(&fake, &identity) && fake.control_writes == 2, "transport retry");
	fake = fresh();
	fake.fail_ctrl = 21;
	check(run(&fake, &identity) == -EREMOTEIO && fake.control_writes == 21 &&
	      !fake.safe[0] && !identity.project_id[0], "bounded trigger failure exits safe mode");
	fake = fresh();
	fake.fail_offset = 5;
	check(run(&fake, &identity) == -EREMOTEIO && fake.restores == 1 &&
	      !fake.unlock1 && !fake.unlock2 && !fake.safe[0] && !identity.project_id[0],
	      "partial ID is rejected and DD state restored");
	fake = fresh();
	fake.fail_cleanup = true;
	check(run(&fake, &identity) == -EREMOTEIO && !identity.project_id[0] &&
	      !fake.unlock1 && !fake.safe[0], "cleanup failure cannot publish valid identity");
	/* Every individual physical transfer can fail. Cleanup must still execute,
	 * retryable steps may recover, and failed reads must never publish an ID.
	 */
	for (i = 1; i <= operations; i++) {
		fake = fresh();
		fake.fail_at = i;
		ret = run(&fake, &identity);
		check(!fake.safe[0] && !fake.unlock1 && !fake.unlock2,
		      "single-transfer fault leaves safe mode and DD access closed");
		check((!ret && !strcmp(identity.project_id, "W273AS2700")) ||
		      (ret && !identity.project_id[0]), "fault never publishes partial identity");
	}
	puts("OTP protocol, panel selection and transfer-fault cleanup tests passed");
	return 0;
}
