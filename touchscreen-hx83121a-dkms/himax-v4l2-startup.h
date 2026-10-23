/* SPDX-License-Identifier: GPL-2.0 */
#ifndef HIMAX_V4L2_STARTUP_H
#define HIMAX_V4L2_STARTUP_H

/* op_lock is held at every entry. Never wait here for the IRQ that needs it.
 * This is the master-only adaptation of Init's three GetFrame attempts and
 * IntOpen-on-timeout. Successful SPI packets count without checksum gating.
 */
#define HIMAX_STARTUP_READ_TIMEOUT_MS 200U

static inline void himax_startup_finish(struct himax_ts_data *ts)
{
	int ret;

	hx_raw_startup_cancel(&ts->startup);
	ts->startup_deadline_ns = 0;
	ts->afe_command_slot = 0;
	ret = himax_afe_command(ts, HX_RAW_CALIBRATE, 0);
	if (ret)
		dev_warn(ts->dev, "initial calibration transport: %d\n", ret);
	ret = himax_afe_command(ts, HX_RAW_FORCE_RATE, 0);
	if (ret)
		dev_warn(ts->dev, "initial scan-rate transport: %d\n", ret);
	ts->read_errors = 0;
	himax_state_event(ts, true);
	mod_delayed_work(system_wq, &ts->charger_work, 0);
	dev_info(ts->dev, "startup master queue: discarded=%u timeouts=%u\n",
		 ts->startup.discarded, ts->startup.timeouts);
}

static inline void himax_startup_wait(struct himax_ts_data *ts)
{
	ts->startup_deadline_ns = ktime_get_ns() +
		HIMAX_STARTUP_READ_TIMEOUT_MS * NSEC_PER_MSEC;
	mod_delayed_work(system_wq, &ts->panel_reinit_work,
			 msecs_to_jiffies(HIMAX_STARTUP_READ_TIMEOUT_MS));
}

/* True means this successful producer packet was consumed by startup, so it
 * must not also be published as the first solver packet.
 */
static inline bool himax_startup_frame(struct himax_ts_data *ts)
{
	if (!ts->startup.active)
		return false;
	ts->startup.discarded++;
	if (--ts->startup.remaining)
		himax_startup_wait(ts);
	else
		himax_startup_finish(ts);
	return true;
}

static inline void himax_startup_begin(struct himax_ts_data *ts)
{
	u64 now;
	int ret;

	ts->startup.active = true;
	ts->startup.remaining = 3;
	ts->startup.discarded = 0;
	ts->startup.timeouts = 0;
	ts->read_errors = 0;
	ts->power.controller_initialized = true;
	now = ktime_get_ns();
	ts->last_frame_ns = now;
	himax_int_enable(ts, true);
	/* IntOpen submits one producer read even before the first IRQ. A
	 * transfer failure is not a queued packet; retain the bounded wait.
	 */
	ret = himax_read_raw(ts);
	if (!ret) {
		ts->last_poll_done_ns = ktime_get_ns();
		himax_startup_frame(ts);
	} else {
		himax_startup_wait(ts);
	}
}

/* A timeout consumes one attempt, then requests a producer read as IntOpen
 * does. A successful kick may satisfy the NEXT consume immediately. After the
 * final timeout that packet belongs to the normal stream, not a fourth drain.
 * Return true only when the caller must publish this saved complete packet.
 */
static inline bool himax_startup_timeout(struct himax_ts_data *ts)
{
	u64 now = ktime_get_ns();
	int ret;

	if (!ts->startup.active)
		return false;
	if (!hx_raw_can_read(&ts->power)) {
		hx_raw_startup_cancel(&ts->startup);
		ts->startup_deadline_ns = 0;
		return false;
	}
	if (now < ts->startup_deadline_ns) {
		unsigned int delay = (ts->startup_deadline_ns - now +
				      NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;

		mod_delayed_work(system_wq, &ts->panel_reinit_work,
				 msecs_to_jiffies(delay));
		return false;
	}
	ts->startup.timeouts++;
	ts->startup.remaining--;
	ret = himax_read_raw(ts);
	if (!ret) {
		ts->last_frame_ns = now;
		ts->last_poll_done_ns = ktime_get_ns();
	}
	if (!ts->startup.remaining) {
		himax_startup_finish(ts);
		return !ret;
	}
	if (!ret)
		himax_startup_frame(ts);
	else
		himax_startup_wait(ts);
	return false;
}

#endif
