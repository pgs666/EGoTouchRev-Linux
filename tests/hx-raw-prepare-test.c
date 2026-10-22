// SPDX-License-Identifier: GPL-2.0
#include "hx-raw-init.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNLOCK 0x10000000U
#define SORTING 0x10007f04U
#define NFRAME 0x10007294U
#define FLASH_RELOAD 0x10007f00U

struct transaction {
	char kind;
	u32 address, value;
	int result;
};

struct fake {
	struct transaction events[128];
	unsigned int operations, fail_at, unlock_reads, sorting_reads, nframe_reads;
	unsigned int unlock_mismatches, sorting_mismatches, nframe_mismatch;
	unsigned int fault_remaining, forbidden_calls;
	char fault_kind;
	u32 fault_address, high_xor, nframe_value;
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
	int result = 0;

	check(fake->operations < 128, "bounded preparation history");
	if (fake->operations + 1 == fake->fail_at)
		result = -EREMOTEIO;
	if (kind == fake->fault_kind && address == fake->fault_address &&
	    fake->fault_remaining) {
		fake->fault_remaining--;
		result = -EREMOTEIO;
	}
	fake->events[fake->operations++] =
		(struct transaction) { kind, address, value, result };
	return result;
}

static int write_register(void *context, u32 address, const u8 *bytes, size_t length)
{
	struct fake *fake = context;
	u32 value;
	int ret;

	check(length == 4, "complete four-byte preparation write");
	value = (u32)bytes[0] | (u32)bytes[1] << 8 |
		(u32)bytes[2] << 16 | (u32)bytes[3] << 24;
	check(address == UNLOCK || address == SORTING ||
	      address == NFRAME || address == FLASH_RELOAD, "only preparation registers");
	ret = record(fake, 'w', address, value);
	if (!ret && address == NFRAME)
		fake->nframe_value = value;
	return ret;
}

static int read_register(void *context, u32 address, u8 *bytes, size_t length)
{
	struct fake *fake = context;
	u32 value = 0;
	unsigned int i;

	check(length == 4, "read complete word even when comparing low two bytes");
	switch (address) {
	case UNLOCK:
		value = ++fake->unlock_reads <= fake->unlock_mismatches ? 1 : 0;
		break;
	case SORTING:
		value = ++fake->sorting_reads <= fake->sorting_mismatches ? 1 : 0;
		break;
	case NFRAME:
		value = fake->nframe_value;
		if (++fake->nframe_reads == fake->nframe_mismatch)
			value ^= 1;
		break;
	default:
		check(false, "only verified preparation registers read");
	}
	value ^= fake->high_xor;
	/* Fill the read buffer even on failure, modelling a partial/new transfer.
	 * Neither implementation may accept its contents after a transport error.
	 */
	for (i = 0; i < 4; i++)
		bytes[i] = value >> (8 * i);
	return record(fake, 'r', address, value);
}

static int forbidden_sense(void *context)
{
	((struct fake *)context)->forbidden_calls++;
	return -EIO;
}

static void forbidden_sleep(void *context, unsigned int milliseconds)
{
	(void)milliseconds;
	((struct fake *)context)->forbidden_calls++;
}

#ifdef HX_RAW_INIT_UPSTREAM_REFERENCE
extern int hx_raw_prepare_reference(const struct hx_raw_init_bus *bus);
#endif

static int run(struct fake *fake, enum hx_raw_init_step *step)
{
	struct fake before = *fake;
	const struct hx_raw_init_bus bus = {
		.context = fake,
		.write = write_register,
		.read = read_register,
		.sense_on = forbidden_sense,
		.sleep_ms = forbidden_sleep,
	};
	int ret = hx_raw_prepare_scan(&bus, step);

	check(!fake->forbidden_calls, "prepare never sleeps or starts sensing");
#ifdef HX_RAW_INIT_UPSTREAM_REFERENCE
	{
		struct hx_raw_init_bus reference_bus = bus;
		unsigned int i;

		reference_bus.context = &before;
		check(ret == hx_raw_prepare_reference(&reference_bus),
		      "production result matches actual Windows functions");
		check(fake->operations == before.operations && !before.forbidden_calls,
		      "production and Windows execute same transaction count");
		for (i = 0; i < fake->operations; i++) {
			const struct transaction *a = &fake->events[i];
			const struct transaction *b = &before.events[i];

			check(a->kind == b->kind && a->address == b->address &&
			      a->value == b->value && a->result == b->result,
			      "every production transaction matches exact Windows reference");
		}
	}
#else
	(void)before;
#endif
	return ret;
}

int main(void)
{
	static const struct transaction expected[] = {
		{ 'w', FLASH_RELOAD, 0, 0 },
		{ 'w', UNLOCK, 0, 0 }, { 'r', UNLOCK, 0, 0 },
		{ 'w', SORTING, 0, 0 }, { 'r', SORTING, 0, 0 },
		{ 'w', NFRAME, 1, 0 }, { 'r', NFRAME, 1, 0 },
		{ 'w', NFRAME, 0x7f0c0001, 0 }, { 'r', NFRAME, 0x7f0c0001, 0 },
	};
	struct fake fake = { 0 };
	enum hx_raw_init_step step;
	unsigned int i, faults;

	check(run(&fake, &step) == 0 && step == HX_RAW_INIT_COMPLETE &&
	      fake.operations == 9, "complete active Windows preparation prefix");
	for (i = 0; i < 9; i++)
		check(fake.events[i].kind == expected[i].kind &&
		      fake.events[i].address == expected[i].address &&
		      fake.events[i].value == expected[i].value,
		      "reload, unlock, sorting and BOTH N-frame writes in order");
	for (i = 1; i <= 9; i++) {
		bool retried = i >= 2 && i <= 5;

		fake = (struct fake) { .fail_at = i };
		check(run(&fake, &step) == (retried ? 0 : -EREMOTEIO),
		      "only inspection operations retry single transport errors");
		if (!retried)
			check(fake.operations == i && step ==
			      (i == 1 ? HX_RAW_INIT_FLASH_RELOAD : i <= 7 ?
			       HX_RAW_INIT_NFRAME_NORMAL : HX_RAW_INIT_NFRAME_MODE),
			      "reload/N-frame failure stops at exact failed phase");
	}
	/* Exhaust every boundary and every type of retried transport failure. */
	for (i = 0; i < 2; i++) {
		const unsigned int attempts = i ? 10 : 20;

		for (faults = 0; faults <= attempts; faults++) {
			fake = (struct fake) { 0 };
			if (i)
				fake.sorting_mismatches = faults;
			else
				fake.unlock_mismatches = faults;
			check(run(&fake, &step) == (faults < attempts ? 0 : -EINVAL),
			      "low-word mismatch retry boundary matches Windows");
			for (unsigned int kind = 0; kind < 2; kind++) {
				fake = (struct fake) {
					.fault_address = i ? SORTING : UNLOCK,
					.fault_kind = kind ? 'r' : 'w',
					.fault_remaining = faults,
				};
				check(run(&fake, &step) ==
				      (faults < attempts ? 0 : -EINVAL),
				      "transport retry budget exhaustion becomes verification failure");
			}
		}
	}
	for (i = 1; i <= 2; i++) {
		fake = (struct fake) { .nframe_mismatch = i };
		check(run(&fake, &step) == -EINVAL && fake.operations == 5 + 2 * i &&
		      step == (i == 1 ? HX_RAW_INIT_NFRAME_NORMAL : HX_RAW_INIT_NFRAME_MODE),
		      "either N-frame verification failure stops without local retry");
	}
	fake = (struct fake) { .high_xor = 0xffff0000 };
	check(run(&fake, &step) == 0 && fake.operations == 9,
	      "upper bytes can differ; four bytes transferred, low two verified");
	fake = (struct fake) {
		.fault_address = UNLOCK, .fault_kind = 'r', .fault_remaining = UINT_MAX,
	};
	check(run(&fake, &step) == -EINVAL && fake.operations == 41 &&
	      step == HX_RAW_INIT_INSPECTION_UNLOCK,
	      "persistent unlock read error never reaches sorting or sensing");
	fake = (struct fake) {
		.fault_address = SORTING, .fault_kind = 'w', .fault_remaining = UINT_MAX,
	};
	check(run(&fake, &step) == -EINVAL && fake.operations == 13 &&
	      step == HX_RAW_INIT_INSPECTION_SORTING,
	      "persistent sorting write error never reaches N-frame");
	check(hx_raw_prepare_scan(NULL, &step) == -EINVAL &&
	      hx_raw_prepare_scan(&(struct hx_raw_init_bus) { 0 }, &step) == -EINVAL &&
	      hx_raw_prepare_scan(&(struct hx_raw_init_bus) {
		      .write = write_register, .read = read_register,
	      }, NULL) == -EINVAL, "invalid arguments rejected");
	puts("Raw preparation ordering, low-word verification and retry fault tests passed");
	return 0;
}
