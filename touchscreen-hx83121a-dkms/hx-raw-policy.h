/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HX_RAW_POLICY_H
#define HX_RAW_POLICY_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#endif

/* Protected by op_lock in the transport. Panel and system power are separate
 * inputs: an enabled-panel notification must never undo system suspend.
 */
struct hx_raw_power_state {
	bool panel_prepared;
	bool panel_enabled;
	bool system_suspended;
	bool shutting_down;
	bool streaming;
	bool controller_initialized;
};

static inline bool hx_raw_can_run(const struct hx_raw_power_state *state)
{
	return state->streaming && state->panel_prepared &&
		state->panel_enabled && !state->system_suspended &&
		!state->shutting_down;
}

static inline bool hx_raw_can_start(const struct hx_raw_power_state *state)
{
	return hx_raw_can_run(state) && !state->controller_initialized;
}

static inline bool hx_raw_can_read(const struct hx_raw_power_state *state)
{
	return hx_raw_can_run(state) && state->controller_initialized;
}

/* DeviceRuntime::BringUpBackoff, without sleeping under the hardware mutex. */
struct hx_raw_retry_state {
	unsigned int failures;
};

/* Hardware is initialized while the producer consumes startup queue data.
 * Consumer-facing running is published only after three consume attempts.
 */
struct hx_raw_startup_state {
	unsigned int remaining;
	unsigned int discarded;
	unsigned int timeouts;
	bool active;
};

static inline void hx_raw_startup_cancel(struct hx_raw_startup_state *state)
{
	state->active = false;
	state->remaining = 0;
}

static inline void hx_raw_retry_reset(struct hx_raw_retry_state *state)
{
	state->failures = 0;
}

static inline unsigned int hx_raw_retry_failed(struct hx_raw_retry_state *state)
{
	static const unsigned int delays[] = { 200, 500, 1000, 1500, 2000 };

	if (state->failures != ~0U)
		state->failures++;
	return delays[state->failures >= 5 ? 4 : state->failures - 1];
}

/* Physical state is passed to the solver even if the firmware write failed.
 * A failed/partial write makes the applied state unknown, requiring retry.
 */
struct hx_raw_charger_state {
	bool connected;
	bool applied;
	bool applied_valid;
};

static inline bool hx_raw_charger_needs_write(const struct hx_raw_charger_state *state)
{
	return !state->applied_valid || state->applied != state->connected;
}

static inline void hx_raw_charger_write_result(struct hx_raw_charger_state *state,
					       bool connected, int result)
{
	state->applied_valid = !result;
	if (!result)
		state->applied = connected;
}

#endif
