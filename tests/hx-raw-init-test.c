// SPDX-License-Identifier: GPL-2.0
#include "hx-raw-init.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct transaction {
	char kind;
	u32 address, value;
	enum hx_raw_init_step step;
};

/* Register/value order from the pinned Windows power-on and master raw
 * selection/password functions. Linux retains sorting readback and an A8
 * gate; tests below distinguish those extra checks from upstream parity.
 */
static const struct transaction expected[] = {
	{ 'w', 0x100072ec, 0, HX_RAW_INIT_RAW_CLEAR },
	{ 'w', 0x10007f04, 0, HX_RAW_INIT_SORTING_CLEAR },
	{ 'r', 0x10007f04, 0, HX_RAW_INIT_SORTING_CLEAR },
	{ 'w', 0x10007294, 1, HX_RAW_INIT_NFRAME },
	{ 'w', 0x100072c0, 0, HX_RAW_INIT_RELOAD_CLEAR },
	{ 's', 0, 0, HX_RAW_INIT_SENSE_ON },
	{ 'r', 0x900000a8, 5, HX_RAW_INIT_RUNNING },
	{ 'r', 0x100072c0, 0x72c0, HX_RAW_INIT_RELOAD_DONE },
	{ 'w', 0x100072ec, 0xf6, HX_RAW_INIT_RAW_SELECT },
	{ 'w', 0x10000000, 0x5aa5, HX_RAW_INIT_PASSWORD },
};

struct fake {
	struct transaction events[256];
	unsigned int operations, fail_at, sense_calls, slept_ms;
	unsigned int sort_reads, running_reads, reload_reads;
	unsigned int sort_mismatch, running_delay, reload_delay;
	u32 running_high, reload_high;
	enum hx_raw_init_step step;
	bool selected, armed;
};

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "FAIL: %s\n", message);
		exit(1);
	}
}

static int operation(struct fake *fake, char kind, u32 address, u32 value)
{
	struct transaction *event;

	check(fake->operations < 256, "transaction history bounded");
	event = &fake->events[fake->operations++];
	*event = (struct transaction) { kind, address, value, fake->step };
	return fake->operations == fake->fail_at ? -EREMOTEIO : 0;
}

static int write_register(void *context, u32 address, const u8 *bytes, size_t length)
{
	struct fake *fake = context;
	u32 value;
	int ret;

	check(length == 4, "all startup writes are four-byte LE words");
	value = (u32)bytes[0] | (u32)bytes[1] << 8 |
		(u32)bytes[2] << 16 | (u32)bytes[3] << 24;
	ret = operation(fake, 'w', address, value);
	if (!ret) {
		if (address == 0x100072ec)
			fake->selected = value == 0xf6;
		if (address == 0x10000000) {
			check(fake->selected && value == 0x5aa5,
			      "arm producer only after selecting raw");
			fake->armed = true;
		}
	}
	return ret;
}

static int read_register(void *context, u32 address, u8 *bytes, size_t length)
{
	struct fake *fake = context;
	u32 value = 0;
	unsigned int i;
	int ret;

	check(length == 4, "startup reads request complete words");
	switch (address) {
	case 0x10007f04:
		value = ++fake->sort_reads <= fake->sort_mismatch ? 1 : 0;
		break;
	case 0x900000a8:
		value = ++fake->running_reads <= fake->running_delay ? 0x0c : 5;
		value |= fake->running_high;
		break;
	case 0x100072c0:
		value = ++fake->reload_reads <= fake->reload_delay ? 0 : 0x72c0;
		value |= fake->reload_high;
		break;
	default:
		check(false, "only expected status registers read");
	}
	ret = operation(fake, 'r', address, value);
	if (ret)
		return ret;
	for (i = 0; i < 4; i++)
		bytes[i] = value >> (8 * i);
	return 0;
}

static int sense_on(void *context)
{
	struct fake *fake = context;

	fake->sense_calls++;
	return operation(fake, 's', 0, 0);
}

static void sleep_ms(void *context, unsigned int milliseconds)
{
	struct fake *fake = context;

	check(milliseconds == 1 || milliseconds == 10, "bounded startup poll interval");
	fake->slept_ms += milliseconds;
}

static int run(struct fake *fake)
{
	const struct hx_raw_init_bus bus = {
		.context = fake,
		.write = write_register,
		.read = read_register,
		.sense_on = sense_on,
		.sleep_ms = sleep_ms,
	};

	return hx_raw_power_on(&bus, &fake->step);
}

int main(void)
{
	struct fake fake = { 0 };
	unsigned int i;
	const unsigned int count = sizeof(expected) / sizeof(expected[0]);

	check(run(&fake) == 0 && fake.step == HX_RAW_INIT_COMPLETE && fake.armed,
	      "production initialization reaches raw producer");
	check(fake.operations == count && fake.sense_calls == 1 && fake.slept_ms == 1,
	      "no redundant raw setup or sleeps inside extracted sequence");
	for (i = 0; i < count; i++) {
		const struct transaction *got = &fake.events[i];
		const struct transaction *want = &expected[i];

		check(got->kind == want->kind && got->address == want->address &&
		      got->value == want->value && got->step == want->step,
		      "ordered startup registers and bytes match documented protocol");
	}
	for (i = 1; i <= count; i++) {
		fake = (struct fake) { .fail_at = i };
		check(run(&fake) == -EREMOTEIO && fake.operations == i && !fake.armed,
		      "every transport/sense failure stops startup without arming");
		check(fake.step == expected[i - 1].step, "error identifies exact failed phase");
		check(fake.sense_calls == (i >= 6), "preparation failure never starts sensing");
		/* The caller retries from reset. The next complete attempt must
		 * start by clearing RawOut, not continue after the failed step.
		 */
		fake = (struct fake) { 0 };
		check(run(&fake) == 0 && fake.events[0].address == 0x100072ec &&
		      fake.events[0].value == 0, "retry executes complete setup from raw clear");
	}
	fake = (struct fake) { .sort_mismatch = 2 };
	check(run(&fake) == 0 && fake.sort_reads == 3 && fake.sense_calls == 1,
	      "sorting succeeds on last bounded verification");
	fake = (struct fake) { .sort_mismatch = 3 };
	check(run(&fake) == -EINVAL && fake.step == HX_RAW_INIT_SORTING_CLEAR &&
	      fake.sort_reads == 3 && !fake.sense_calls && !fake.armed,
	      "sorting mismatch aborts before reset/sensing");

	fake = (struct fake) { .running_delay = 29, .reload_delay = 29,
			      .running_high = 0x12340000 };
	check(run(&fake) == 0 && fake.running_reads == 30 && fake.reload_reads == 30 &&
	      fake.slept_ms == 581, "ready on last poll; A8 compares low byte only");
	fake = (struct fake) { .running_delay = 30 };
	check(run(&fake) == -ETIMEDOUT && fake.step == HX_RAW_INIT_RUNNING &&
	      fake.running_reads == 30 && !fake.reload_reads && !fake.selected &&
	      fake.slept_ms == 301, "MCU timeout never reaches raw producer");
	fake = (struct fake) { .reload_delay = 30 };
	check(run(&fake) == -ETIMEDOUT && fake.step == HX_RAW_INIT_RELOAD_DONE &&
	      fake.reload_reads == 30 && !fake.selected && fake.slept_ms == 301,
	      "reload timeout never reaches raw producer");
	fake = (struct fake) { .reload_high = 0x10000000 };
	check(run(&fake) == -ETIMEDOUT && !fake.armed,
	      "reload low bytes alone cannot satisfy full-word readiness");
	fake = (struct fake) { .running_delay = 2, .fail_at = 8 };
	check(run(&fake) == -EREMOTEIO && fake.step == HX_RAW_INIT_RUNNING &&
	      fake.operations == 8 && !fake.reload_reads,
	      "MCU polling transfer error is not hidden as timeout or retried in place");
	fake = (struct fake) { .reload_delay = 2, .fail_at = 9 };
	check(run(&fake) == -EREMOTEIO && fake.step == HX_RAW_INIT_RELOAD_DONE &&
	      fake.operations == 9 && !fake.armed,
	      "reload polling transfer error cannot arm raw producer");
	check(hx_raw_power_on(NULL, &fake.step) == -EINVAL &&
	      hx_raw_power_on(&(struct hx_raw_init_bus) { 0 }, &fake.step) == -EINVAL,
	      "missing bus callbacks rejected");
	check(!strcmp(hx_raw_init_step_name(HX_RAW_INIT_RELOAD_DONE), "reload_done") &&
	      !strcmp(hx_raw_init_step_name((enum hx_raw_init_step)-1), "invalid"),
	      "stable bounded error phase names");
	puts("Raw power-on ordering, readiness, bounded retries and fault tests passed");
	return 0;
}
