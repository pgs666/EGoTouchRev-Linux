/* SPDX-License-Identifier: GPL-2.0 */
#include "hx-raw-policy.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static void check(bool value, const char *name)
{
	if (!value) {
		fprintf(stderr, "FAIL: %s\n", name);
		exit(1);
	}
}

static void check_blocked(const struct hx_raw_power_state *state, const char *name)
{
	check(!hx_raw_can_run(state) && !hx_raw_can_start(state) &&
	      !hx_raw_can_read(state), name);
}

static void test_power_gates(void)
{
	unsigned int bits;

	/* Exercise the production predicates used by IRQ, commands, startup and
	 * charger work, including impossible/late callback combinations.
	 */
	for (bits = 0; bits < 64; bits++) {
		struct hx_raw_power_state state = {
			.panel_prepared = !!(bits & 1),
			.panel_enabled = !!(bits & 2),
			.streaming = !!(bits & 4),
			.controller_initialized = !!(bits & 8),
			.system_suspended = !!(bits & 16),
			.shutting_down = !!(bits & 32),
		};

		check(hx_raw_can_run(&state) == (bits == 7 || bits == 15),
		      "only live, enabled, prepared streams can access hardware");
		check(hx_raw_can_start(&state) == (bits == 7),
		      "AFE starts exactly when all gates open and not initialized");
		check(hx_raw_can_read(&state) == (bits == 15),
		      "IRQ/command/charger require an initialized AFE too");
	}
}

static void test_resume_order(void)
{
	struct hx_raw_power_state state = {
		.panel_prepared = true,
		.panel_enabled = true,
		.streaming = true,
		.controller_initialized = true,
	};

	check(hx_raw_can_read(&state) && !hx_raw_can_start(&state),
	      "duplicate startup work must leave the active IRQ alone");
	state.system_suspended = true;
	state.controller_initialized = false;
	check_blocked(&state, "suspend blocks an otherwise enabled panel");
	state.panel_enabled = false;
	state.panel_prepared = false;
	state.panel_prepared = true;
	state.panel_enabled = true;
	check_blocked(&state, "late panel enable cannot bypass suspend");
	state.system_suspended = false;
	check(hx_raw_can_start(&state), "panel-first resume opens the final gate");
	check(!hx_raw_can_read(&state), "resume requires fresh AFE initialization");
	state.controller_initialized = true;
	check(hx_raw_can_read(&state), "fresh AFE permits reads");

	state.system_suspended = true;
	state.controller_initialized = false;
	state.panel_enabled = false;
	state.panel_prepared = false;
	state.system_suspended = false;
	check_blocked(&state, "system-first resume waits for panel");
	state.panel_prepared = true;
	check_blocked(&state, "prepared alone is not enabled");
	state.panel_enabled = true;
	check(hx_raw_can_start(&state), "system-first resume starts after panel enable");
	state.streaming = false;
	check_blocked(&state, "STREAMOFF during suspend prevents restart");
	state.streaming = true;
	check(hx_raw_can_start(&state), "STREAMON may reopen a live panel");
	state.shutting_down = true;
	check_blocked(&state, "shutdown blocks initialization");
	state.controller_initialized = true;
	check_blocked(&state, "even stale initialized state cannot bypass shutdown");
}

static void test_charger_retry(void)
{
	struct hx_raw_charger_state state = { 0 };

	check(hx_raw_charger_needs_write(&state),
	      "unknown applied state needs a write even when unplugged");
	state.connected = true;
	hx_raw_charger_write_result(&state, true, -EIO);
	check(state.connected && !state.applied_valid &&
	      hx_raw_charger_needs_write(&state),
	      "failed startup replay preserves observation and remains retryable");
	hx_raw_charger_write_result(&state, true, 0);
	check(state.applied_valid && state.applied &&
	      !hx_raw_charger_needs_write(&state),
	      "successful retry confirms firmware state");
	state.connected = false;
	check(hx_raw_charger_needs_write(&state), "unplug requires firmware change");
	hx_raw_charger_write_result(&state, false, -EIO);
	check(!state.connected && !state.applied_valid &&
	      hx_raw_charger_needs_write(&state),
	      "failed unplug is not incorrectly cached as applied");
	state.connected = true;
	check(hx_raw_charger_needs_write(&state),
	      "replug after partial failed unplug still needs a write");
	hx_raw_charger_write_result(&state, true, 0);
	state.applied_valid = false;
	check(hx_raw_charger_needs_write(&state),
	      "hardware reset invalidates the last successful firmware state");
	hx_raw_charger_write_result(&state, true, 0);
	state.connected = false;
	hx_raw_charger_write_result(&state, false, 0);
	check(!state.applied && !hx_raw_charger_needs_write(&state),
	      "successful unplug stops retries");
}

int main(void)
{
	test_power_gates();
	test_resume_order();
	test_charger_retry();
	puts("Raw transport power gates and charger retry state tests passed");
	return 0;
}
