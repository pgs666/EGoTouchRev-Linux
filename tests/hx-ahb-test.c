// SPDX-License-Identifier: GPL-2.0
#include "hx-ahb.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum operation { BURST, READ, WRITE, INTERFACE_ON, HW_RESET, SENSE_HW, SENSE_FLASH };

struct transaction {
	char kind;
	u8 command;
	bool address_present;
	u32 length;
	u8 bytes[516];
	int result;
};

struct fake {
	struct transaction events[64];
	unsigned int count, fail_at, sleep_count, capacity;
	unsigned int conti_reads, incr_reads, conti_mismatches, incr_mismatches;
	u8 continuous, increment;
	u32 current_address;
};

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static struct transaction *record(struct fake *fake, char kind, u8 command,
				  const u8 *address, const u8 *data, u32 length)
{
	struct transaction *event;
	size_t wire_length = (size_t)length + (address ? 4 : 0);

	check(fake->count < 64, "bounded AHB transaction history");
	event = &fake->events[fake->count++];
	*event = (struct transaction) {
		.kind = kind, .command = command,
		.address_present = address != NULL, .length = length,
		.result = fake->count == fake->fail_at ? -EREMOTEIO : 0,
	};
	if (wire_length > fake->capacity && fake->capacity)
		event->result = -EMSGSIZE;
	if (address)
		memcpy(event->bytes, address, 4);
	/* The fake rejects oversized packets before dereferencing their payload.
	 * This also permits safe maximal-length fault tests, like the native cap.
	 */
	if (!event->result || length <= 512) {
		check(wire_length <= sizeof(event->bytes), "fake packet capacity");
		if (data)
			memcpy(event->bytes + (address ? 4 : 0), data, length);
	}
	return event;
}

static int write_bus(void *context, u8 command, const u8 *address,
		      const u8 *data, u32 length)
{
	struct fake *fake = context;
	struct transaction *event = record(fake, 'w', command, address, data, length);

	if (event->result)
		return event->result;
	if (command == 0x00) {
		check(address != NULL, "AHB address accompanies command 0");
		fake->current_address = (u32)address[0] | (u32)address[1] << 8 |
			(u32)address[2] << 16 | (u32)address[3] << 24;
	} else {
		if (command == 0x31) {
			check(!address && length == 2 && !data[0] && !data[1],
			      "flash-mode sense-on releases safe-mode password, not GPIO");
			return 0;
		}
		check(!address && length == 1 && data, "single-byte bus control");
		if (command == 0x13)
			fake->continuous = data[0];
		else if (command == 0x0d)
			fake->increment = data[0];
		else
			check(command == 0x0c && data[0] == 0, "read access direction");
	}
	return 0;
}

static int read_bus(void *context, u8 command, u8 *data, u32 length)
{
	struct fake *fake = context;
	unsigned int i;

	if (command == 0x08) {
		check(length <= 256, "no oversized AHB data reads reach bus");
		for (i = 0; i < length; i++)
			data[i] = (fake->current_address >> 8) + i * 37;
	} else {
		check(length == 1, "single-byte bus control readback");
		if (command == 0x13)
			data[0] = fake->continuous ^
				(++fake->conti_reads <= fake->conti_mismatches ? 1 : 0);
		else {
			check(command == 0x0d, "only burst-control readback");
			data[0] = fake->increment ^
				(++fake->incr_reads <= fake->incr_mismatches ? 1 : 0);
		}
	}
	/* A failing transfer may still have written bytes into the destination. */
	return record(fake, 'r', command, NULL, data, length)->result;
}

static void sleep_ms(void *context, unsigned int milliseconds)
{
	struct fake *fake = context;

	check(milliseconds == 1 || milliseconds == 11,
	      "Windows interface-on/sense-on polling delay");
	fake->sleep_count++;
	record(fake, 's', 0, NULL, NULL, milliseconds);
}

static int set_reset(void *context, bool high)
{
	return record(context, 'g', high, NULL, NULL, 0)->result;
}

#ifdef HX_AHB_UPSTREAM_REFERENCE
extern int hx_ahb_reference(const struct hx_ahb_bus *bus, unsigned int operation,
			    u32 address, u8 *data, u32 length);
#endif

static int run(struct fake *fake, enum operation operation, u32 address,
		u8 data[512], u32 length)
{
	struct hx_ahb_bus bus = {
		.context = fake, .write = write_bus, .read = read_bus,
		.sleep_ms = sleep_ms,
		.set_reset = set_reset,
	};
	struct fake reference = *fake;
	u8 reference_data[512];
	int result = -EINVAL;

	memcpy(reference_data, data, sizeof(reference_data));
	switch (operation) {
	case BURST:
		result = hx_ahb_burst(&bus, length != 0);
		break;
	case READ:
		result = hx_ahb_register_read(&bus, address, data, length);
		break;
	case WRITE:
		result = hx_ahb_register_write(&bus, address, data, length);
		break;
	case INTERFACE_ON:
		result = hx_ahb_interface_on(&bus);
		break;
	case HW_RESET:
		result = hx_ahb_hw_reset(&bus);
		break;
	case SENSE_HW:
	case SENSE_FLASH:
		result = hx_ahb_sense_on(&bus, operation == SENSE_FLASH);
	}
#ifdef HX_AHB_UPSTREAM_REFERENCE
	{
		unsigned int i;
		int expected;

		bus.context = &reference;
		expected = hx_ahb_reference(&bus, operation, address, reference_data, length);
		check(result == expected, "AHB result matches exact Windows source");
		check(fake->count == reference.count &&
		      fake->sleep_count == reference.sleep_count,
		      "no extra restoration, polling or hidden retry");
		check(!memcmp(data, reference_data, sizeof(reference_data)),
		      "caller buffer and untouched suffix agree even after failed reads");
		for (i = 0; i < fake->count; i++) {
			const struct transaction *a = &fake->events[i];
			const struct transaction *b = &reference.events[i];

			if (a->kind != b->kind || a->command != b->command ||
			    a->address_present != b->address_present ||
			    a->length != b->length || a->result != b->result ||
			    memcmp(a->bytes, b->bytes, sizeof(a->bytes))) {
				fprintf(stderr, "op=%u len=%u transaction=%u\n",
					operation, length, i);
				check(false, "every command, address and payload byte matches Windows");
			}
		}
	}
#else
	(void)reference;
#endif
	return result;
}

int main(void)
{
	static const u32 addresses[] = { 0, 0x10007550, 0x900000a8, 0xffffffff };
	struct fake fake;
	u8 data[512];
	unsigned int i, j, length, operation;

	for (i = 0; i < sizeof(data); i++)
		data[i] = i * 29 + 3;
	for (operation = BURST; operation <= WRITE; operation++) {
		unsigned int count = operation == READ ? 5 : operation == WRITE ? 3 : 2;

		for (i = 0; i < 4; i++) {
			for (length = 0; length <= 512; length++) {
				bool invalid = operation == READ && length > 256;

				fake = (struct fake) { 0 };
				check(run(&fake, operation, addresses[i], data, length) ==
				      (invalid ? -EINVAL : 0), "length domain and address byte order");
				check(fake.count == (invalid ? 0 : count),
				      "read length rejected before any I/O; bounded valid sequence");
				if (invalid)
					continue;
				check(fake.events[0].command == 0x13 && fake.events[0].bytes[0] == 0x31 &&
				      fake.events[1].command == 0x0d && fake.events[1].bytes[0] ==
				      (operation == BURST ? (length ? 0x13 : 0x12) :
				       length > 4 ? 0x13 : 0x12),
				      "0x12/0x13 selection at four-byte threshold");
				if (operation != BURST)
					check(fake.events[2].address_present &&
					      fake.current_address == addresses[i],
					      "full little-endian address precedes data");
				for (j = 1; j <= count; j++) {
					fake = (struct fake) { .fail_at = j };
					check(run(&fake, operation, addresses[i], data, length) == -EREMOTEIO &&
					      fake.count == j, "stop at each failed bus step without restore");
				}
			}
		}
	}
	for (i = 0; i <= 10; i++) {
		for (j = 0; j < 2; j++) {
			fake = (struct fake) {
				.conti_mismatches = j ? i : 0,
				.incr_mismatches = j ? 0 : i,
			};
			check(run(&fake, INTERFACE_ON, 0, data, 0) == 0 &&
			      fake.sleep_count == i &&
			      fake.conti_reads == (i < 10 ? i + 1 : 10) &&
			      fake.incr_reads == (i < 10 ? i + 1 : 10),
			      "dummy read, all interface polling boundaries, last mismatch tolerated");
		}
	}
	for (i = 1; i <= 51; i++) {
		bool sleep = i >= 6 && (i - 6) % 5 == 0;

		fake = (struct fake) { .conti_mismatches = 10, .fail_at = i };
		check(run(&fake, INTERFACE_ON, 0, data, 0) == (sleep ? 0 : -EREMOTEIO),
		      "every interface read/write error aborts; Sleep has no return status");
	}
	for (operation = HW_RESET; operation <= SENSE_FLASH; operation++) {
		unsigned int count = operation == HW_RESET ? 7 :
			operation == SENSE_HW ? 11 : 10;

		fake = (struct fake) { 0 };
		check(run(&fake, operation, 0, data, 0) == 0 && fake.count == count,
		      "hardware reset and both sense-on branches match upstream");
		if (operation == HW_RESET) {
			check(fake.events[2].address_present &&
			      !memcmp(fake.events[2].bytes, "\xc0\x72\x00\x10\0\0\0\0", 8) &&
			      fake.events[3].kind == 'g' && fake.events[3].command == 0 &&
			      fake.events[4].kind == 'g' && fake.events[4].command == 1 &&
			      fake.events[6].command == 0x0d && fake.events[6].bytes[0] == 0x13,
			      "reload cleared BEFORE low/high reset pair and burst reenabled AFTER");
		} else {
			check(fake.events[0].kind == 'r' && fake.events[0].command == 0x08 &&
			      fake.events[7].address_present &&
			      !memcmp(fake.events[7].bytes, "\x5c\0\0\x90\0\0\0\0", 8) &&
			      fake.events[8].kind == 's' && fake.events[8].length == 11,
			      "interface-on precedes ISR clear, then exactly 11 ms before reset");
			if (operation == SENSE_HW)
				check(fake.events[9].kind == 'g' && fake.events[9].command == 0 &&
				      fake.events[10].kind == 'g' && fake.events[10].command == 1,
				      "sense-on false uses physical low/high reset pair");
			else
				check(fake.events[9].kind == 'w' && fake.events[9].command == 0x31,
				      "sense-on true does not toggle GPIO");
		}
		for (i = 1; i <= count; i++) {
			bool sleep = operation != HW_RESET && i == 9;

			fake = (struct fake) { .fail_at = i };
			check(run(&fake, operation, 0, data, 0) == (sleep ? 0 : -EREMOTEIO) &&
			      fake.count == (sleep ? count : i),
			      "every bus/GPIO failure stops at same phase without later reset/arming");
		}
		if (operation == HW_RESET)
			continue;
		for (i = 0; i <= 10; i++) {
			fake = (struct fake) { .conti_mismatches = i };
			check(run(&fake, operation, 0, data, 0) == 0 &&
			      fake.sleep_count == i + 1 && fake.events[fake.count - 3].length ==
			      (operation == SENSE_HW ? 11 : 4),
			      "delayed interface-on retains sense-on ordering and 11 ms delay");
		}
		for (i = 1; i <= count + 46; i++) {
			bool sleep = (i >= 6 && i <= 51 && (i - 6) % 5 == 0) || i == 55;

			fake = (struct fake) { .incr_mismatches = 10, .fail_at = i };
			check(run(&fake, operation, 0, data, 0) == (sleep ? 0 : -EREMOTEIO),
			      "every delayed sense-on transaction/GPIO fault matches upstream");
		}
	}
	fake = (struct fake) { .capacity = 256 };
	check(run(&fake, WRITE, 0x10007550, data, 252) == 0,
	      "address consumes four bytes of native transfer capacity");
	fake = (struct fake) { .capacity = 256 };
	check(run(&fake, WRITE, 0x10007550, data, 253) == -EMSGSIZE && fake.count == 3,
	      "native cap errors propagate without extra transactions");
	fake = (struct fake) { .capacity = 256 };
	check(run(&fake, WRITE, 0x10007550, data, UINT_MAX) == -EMSGSIZE &&
	      fake.count == 3, "maximal write length reaches bounded native callback safely");
	fake = (struct fake) { 0 };
	check(run(&fake, READ, 0x10007550, data, UINT_MAX) == -EINVAL && !fake.count,
	      "maximal read length rejected without I/O");
	{
		struct hx_ahb_bus bus = {
			.context = &fake, .write = write_bus,
			.read = read_bus, .sleep_ms = sleep_ms,
		};
		const struct hx_ahb_bus empty = { 0 };

		check(hx_ahb_burst(NULL, false) == -EINVAL &&
		      hx_ahb_burst(&empty, false) == -EINVAL &&
		      hx_ahb_register_read(NULL, 0, data, 4) == -EINVAL &&
		      hx_ahb_register_write(NULL, 0, data, 4) == -EINVAL &&
		      hx_ahb_interface_on(NULL) == -EINVAL &&
		      hx_ahb_interface_on(&empty) == -EINVAL,
		      "invalid native bus descriptors rejected before I/O");
		check(hx_ahb_hw_reset(NULL) == -EINVAL &&
		      hx_ahb_hw_reset(&empty) == -EINVAL &&
		      hx_ahb_sense_on(NULL, false) == -EINVAL &&
		      hx_ahb_sense_on(&empty, true) == -EINVAL &&
		      hx_ahb_sense_on(&bus, false) == -EINVAL &&
		      hx_ahb_hw_reset(&bus) == -EINVAL && !fake.count,
		      "missing reset callback rejected before I/O, not during sequence");
		check(hx_ahb_register_read(&bus, 0, NULL, 4) == -EINVAL &&
		      hx_ahb_register_write(&bus, 0, NULL, 4) == -EINVAL &&
		      !fake.count, "null payload guards do not access hardware");
		bus.read = NULL;
		check(hx_ahb_register_read(&bus, 0, data, 4) == -EINVAL &&
		      hx_ahb_interface_on(&bus) == -EINVAL, "missing read callback rejected");
		bus.read = read_bus;
		bus.sleep_ms = NULL;
		check(hx_ahb_interface_on(&bus) == -EINVAL, "missing sleep callback rejected");
	}
	puts("AHB exact bus transactions, length/address domains, all-step faults "
	     "and Windows interface-on polling passed");
	return 0;
}
