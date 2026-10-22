// SPDX-License-Identifier: GPL-2.0
#include "hx-raw-init.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define PASSWORD 0x10000000U
#define SORTING 0x10007f04U
#define NFRAME 0x10007294U
#define FLASH_RELOAD 0x10007f00U
#define RAW_OUT 0x100072ecU
#define RELOAD_DONE 0x100072c0U
#define FW_DEBUG 0x10007f40U
#define FW_STATUS 0x900000a8U
#define DD_STATUS 0x900000e8U
#define RESET_EVENT 0x900000e4U

struct transaction {
	char kind;
	u32 address, value;
	int result;
};

struct fake {
	struct transaction events[512];
	unsigned int count, fail_at, reload_reads, raw_reads, sleeps;
	unsigned int reload_waits, raw_mismatches;
	unsigned int fault_skip, fault_remaining;
	char fault_kind;
	u32 fault_address, wait_value, high_xor, nframe;
};

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static int record(struct fake *fake, char kind, u32 address, u32 value)
{
	int ret = fake->count + 1 == fake->fail_at ? -EREMOTEIO : 0;

	check(fake->count < 512, "bounded startup transaction history");
	if (kind == fake->fault_kind && address == fake->fault_address) {
		if (fake->fault_skip)
			fake->fault_skip--;
		else if (fake->fault_remaining) {
			fake->fault_remaining--;
			ret = -EREMOTEIO;
		}
	}
	fake->events[fake->count++] =
		(struct transaction) { kind, address, value, ret };
	return ret;
}

static int write_register(void *context, u32 address, const u8 *data, size_t length)
{
	struct fake *fake = context;
	u32 value;
	int ret;

	check(length == 4, "startup writes complete words");
	value = (u32)data[0] | (u32)data[1] << 8 |
		(u32)data[2] << 16 | (u32)data[3] << 24;
	check(address == PASSWORD || address == SORTING || address == NFRAME ||
	      address == FLASH_RELOAD || address == RAW_OUT || address == RELOAD_DONE,
	      "only active Windows startup registers written");
	ret = record(fake, 'w', address, value);
	if (!ret && address == NFRAME)
		fake->nframe = value;
	return ret;
}

static int read_register(void *context, u32 address, u8 *data, size_t length)
{
	struct fake *fake = context;
	u32 value = 0;
	unsigned int i;

	check(length == 4, "startup reads complete words");
	switch (address) {
	case PASSWORD:
	case SORTING:
		value = fake->high_xor;
		break;
	case NFRAME:
		value = fake->nframe ^ fake->high_xor;
		break;
	case RAW_OUT:
		value = 0xf6 ^ fake->high_xor;
		if (++fake->raw_reads <= fake->raw_mismatches)
			value ^= 1;
		break;
	case RELOAD_DONE:
		value = ++fake->reload_reads <= fake->reload_waits ?
			fake->wait_value : 0x72c0;
		break;
	case FW_DEBUG:
	case DD_STATUS:
	case RESET_EVENT:
		value = 0x12345678;
		break;
	case FW_STATUS:
		/* Deliberately never 0x05: Windows does not use an A8 gate. */
		value = 0x0c;
		break;
	default:
		check(false, "only Windows readiness/debug/verification reads");
	}
	for (i = 0; i < 4; i++)
		data[i] = value >> (8 * i);
	return record(fake, 'r', address, value);
}

static int sense_on(void *context)
{
	return record(context, 's', 0, 0);
}

static void sleep_ms(void *context, unsigned int milliseconds)
{
	struct fake *fake = context;

	check(milliseconds == 11, "Windows reload polling waits 11 ms");
	fake->sleeps++;
	record(fake, 't', 0, milliseconds);
}

#ifdef HX_RAW_INIT_UPSTREAM_REFERENCE
extern int hx_raw_start_reference(const struct hx_raw_init_bus *bus);
#endif

static int run(struct fake *fake, enum hx_raw_init_step *step)
{
	struct fake reference = *fake;
	struct hx_raw_init_bus bus = {
		.context = fake,
		.write = write_register,
		.read = read_register,
		.sense_on = sense_on,
		.sleep_ms = sleep_ms,
	};
	int ret = hx_raw_start_scan(&bus, step);

#ifdef HX_RAW_INIT_UPSTREAM_REFERENCE
	unsigned int i;
	int expected;

	bus.context = &reference;
	expected = hx_raw_start_reference(&bus);
	if (ret != expected) {
		fprintf(stderr, "production=%d reference=%d\n", ret, expected);
		check(false, "startup result agrees with exact Windows source");
	}
	check(fake->count == reference.count, "Windows transaction count agrees");
	for (i = 0; i < fake->count; i++) {
		const struct transaction *a = &fake->events[i];
		const struct transaction *b = &reference.events[i];

		if (a->kind != b->kind || a->address != b->address ||
		    a->value != b->value || a->result != b->result) {
			fprintf(stderr, "transaction=%u production=%c/%x/%x/%d "
				"reference=%c/%x/%x/%d\n", i, a->kind, a->address,
				a->value, a->result, b->kind, b->address, b->value, b->result);
			check(false, "all startup transactions match actual Windows functions");
		}
	}
#else
	(void)reference;
#endif
	return ret;
}

int main(void)
{
	static const struct transaction expected[] = {
		{ 'w', FLASH_RELOAD, 0, 0 },
		{ 'w', PASSWORD, 0, 0 }, { 'r', PASSWORD, 0, 0 },
		{ 'w', SORTING, 0, 0 }, { 'r', SORTING, 0, 0 },
		{ 'w', NFRAME, 1, 0 }, { 'r', NFRAME, 1, 0 },
		{ 'w', NFRAME, 0x7f0c0001, 0 }, { 'r', NFRAME, 0x7f0c0001, 0 },
		{ 'w', RAW_OUT, 0, 0 }, { 'w', SORTING, 0, 0 },
		{ 'w', NFRAME, 1, 0 }, { 'w', RELOAD_DONE, 0, 0 },
		{ 's', 0, 0, 0 }, { 'r', RELOAD_DONE, 0x72c0, 0 },
		{ 'w', RAW_OUT, 0xf6, 0 }, { 'r', RAW_OUT, 0xf6, 0 },
		{ 'w', PASSWORD, 0x5aa5, 0 },
	};
	static const u32 status[] = { FW_DEBUG, FW_STATUS, DD_STATUS, RESET_EVENT };
	struct fake fake = { 0 };
	enum hx_raw_init_step step;
	unsigned int i, j;

	check(run(&fake, &step) == 0 && step == HX_RAW_INIT_COMPLETE &&
	      fake.count == 18 && !fake.sleeps, "complete active master startup");
	for (i = 0; i < 18; i++)
		check(fake.events[i].kind == expected[i].kind &&
		      fake.events[i].address == expected[i].address &&
		      fake.events[i].value == expected[i].value,
		      "preparation, power-on, raw verification and handshake order");

	for (i = 1; i <= 18; i++) {
		bool retry = (i >= 2 && i <= 5) || i == 16 || i == 17;

		fake = (struct fake) { .fail_at = i };
		check(run(&fake, &step) == (retry ? 0 : -EREMOTEIO),
		      "every normal transaction failure stops or retries as Windows does");
		if (i == 14 || i == 15 || i == 18)
			check(step == (i == 14 ? HX_RAW_INIT_SENSE_ON :
				       i == 15 ? HX_RAW_INIT_RELOAD_DONE : HX_RAW_INIT_PASSWORD),
			      "failed phase retained for lifecycle error logging");
	}
	for (i = 0; i <= 30; i++) {
		fake = (struct fake) { .reload_waits = i };
		check(run(&fake, &step) == (i < 30 ? 0 : -ETIMEDOUT) &&
		      fake.sleeps == i && fake.reload_reads == (i < 30 ? i + 1 : 30),
		      "complete readiness poll budget, including sleep after last failure");
		for (j = 0; j < i; j++) {
			unsigned int k, base = 15 + 6 * j;

			for (k = 0; k < 4; k++)
				check(fake.events[base + k].kind == 'r' &&
				      fake.events[base + k].address == status[k],
				      "four firmware status words read after each unfinished reload");
			check(fake.events[base + 4].kind == 't',
			      "status reads precede readiness sleep");
		}
	}
	fake = (struct fake) { .reload_waits = 30, .wait_value = 0x10072c0 };
	check(run(&fake, &step) == -ETIMEDOUT && step == HX_RAW_INIT_RELOAD_DONE &&
	      !fake.raw_reads, "reload low-word match with wrong high word is not ready");
	for (i = 0; i < 4; i++) {
		fake = (struct fake) {
			.reload_waits = 2, .fault_kind = 'r',
			.fault_address = status[i], .fault_remaining = 2,
		};
		check(run(&fake, &step) == 0 && fake.reload_reads == 3 &&
		      fake.sleeps == 2, "debug read errors do not abort or skip poll delay");
	}
	fake = (struct fake) { .reload_waits = 2, .fail_at = 21 };
	check(run(&fake, &step) == -EREMOTEIO && step == HX_RAW_INIT_RELOAD_DONE &&
	      fake.sleeps == 1, "reload transport failure aborts after a pending poll");

	for (i = 0; i <= 50; i++) {
		fake = (struct fake) { .raw_mismatches = i };
		check(run(&fake, &step) == (i < 50 ? 0 : -EINVAL) &&
		      fake.raw_reads == (i < 50 ? i + 1 : 50),
		      "raw mismatch retries stop at exactly 50 attempts");
		for (j = 0; j < 2; j++) {
			fake = (struct fake) {
				.fault_kind = j ? 'r' : 'w', .fault_address = RAW_OUT,
				.fault_skip = j ? 0 : 1, .fault_remaining = i,
			};
			check(run(&fake, &step) == (i < 50 ? 0 : -EREMOTEIO),
			      "raw read/write errors retry and preserve final transport error");
		}
	}
	fake = (struct fake) { .raw_mismatches = 49, .fail_at = 114 };
	check(run(&fake, &step) == -EREMOTEIO && step == HX_RAW_INIT_RAW_SELECT,
	      "last transport error supersedes earlier mismatch");
	fake = (struct fake) {
		.fault_kind = 'w', .fault_address = RAW_OUT, .fault_skip = 1,
		.fault_remaining = 49, .raw_mismatches = 1,
	};
	check(run(&fake, &step) == -EINVAL && step == HX_RAW_INIT_RAW_SELECT,
	      "last mismatch supersedes earlier transport errors");
	fake = (struct fake) { .high_xor = 0xabcd0000 };
	check(run(&fake, &step) == 0 && fake.raw_reads == 1,
	      "verification transfers four bytes but compares only low sixteen bits");

	{
		const struct hx_raw_init_bus empty = { 0 };

		check(hx_raw_start_scan(NULL, &step) == -EINVAL &&
		      hx_raw_start_scan(&empty, &step) == -EINVAL,
		      "invalid startup callbacks rejected before I/O");
	}
	puts("Windows master startup, all normal faults, reload/status boundaries "
	     "and 50-attempt raw selection passed");
	return 0;
}
