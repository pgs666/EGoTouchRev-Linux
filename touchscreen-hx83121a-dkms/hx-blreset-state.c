// SPDX-License-Identifier: GPL-2.0
/* BLReset_BaselineStateMachine recovered from the TSACore PE jump table. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

/* Official numeric states.  Values 3/5/6/7 are explicit no-op jump-table
 * entries; state 16 is the terminal clean-baseline state and is not indexed
 * by the 0..8 table. */
#define HX_BLRST_BL_BOOTSTRAP 0
#define HX_BLRST_BL_ARMED 1
#define HX_BLRST_BL_PROTECTED 2
#define HX_BLRST_BL_RELEASE 4
#define HX_BLRST_BL_CLEAN_WAIT 8
#define HX_BLRST_BL_CAPTURED 16

bool hx_blreset_wakeup_process(struct hx_algo *algo, bool abnormal)
{
	u32 interval = max_t(u16, algo->frame_interval_ms, 1);

	/* BLReset_IsTriggered2AfeWakeUp uses a separate counter/timer from the
	 * normal reset statistics.  Its 0xa0 ms threshold is deliberately kept
	 * here instead of folded into hx_blreset_process(), whose per-reason
	 * thresholds are normally 500 ms. */
	if (abnormal) {
		algo->blreset_wake_abnormal_frames = min_t(u8,
			algo->blreset_wake_abnormal_frames + 1, U8_MAX);
		algo->blreset_wake_abnormal_elapsed_ms = min_t(u32,
			algo->blreset_wake_abnormal_elapsed_ms + interval, ~0U);
		if (algo->blreset_wake_abnormal_elapsed_ms > 160) {
			algo->blreset_wake_abnormal_elapsed_ms = 0;
			algo->blreset_wake_abnormal_frames = 0;
			algo->blreset_wake_triggered = true;
			return true;
		}
		return false;
	}
	/* The vendor decays a short false vote and clears the timer when the
	 * abnormal predicate is absent.  A clean frame therefore re-arms the
	 * detector without carrying a stale wake request into the next wake. */
	if (algo->blreset_wake_abnormal_frames > 1)
		algo->blreset_wake_abnormal_frames--;
	else
		algo->blreset_wake_abnormal_frames = 0;
	if (algo->blreset_wake_abnormal_elapsed_ms > interval * 2)
		algo->blreset_wake_abnormal_elapsed_ms -= interval * 2;
	else
		algo->blreset_wake_abnormal_elapsed_ms = 0;
	return false;
}

bool hx_blreset_dirty_process(struct hx_algo *algo, bool over_noise,
			      bool touch_protected, bool diff_dirty,
			      bool clean_matches)
{
	u32 interval = max_t(u16, algo->frame_interval_ms, 1);
	bool accumulate = over_noise && !touch_protected && diff_dirty &&
		clean_matches;

	/* BLReset_IsCurBaselineDirty updates TSAStatic+0x388 only when the
	 * current BL is abnormal while pre-CMF raw still matches the captured
	 * clean baseline.  All protection/noise-feature failure branches clear
	 * the timer rather than slowly decaying it. */
	if (!accumulate) {
		algo->blreset_dirty_elapsed_ms = 0;
		return false;
	}
	algo->blreset_dirty_elapsed_ms = algo->blreset_dirty_elapsed_ms ?
		min_t(u32, algo->blreset_dirty_elapsed_ms + interval, ~0U) : 1;
	if (algo->blreset_dirty_elapsed_ms > 300) {
		algo->blreset_dirty_elapsed_ms = 0;
		algo->blreset_dirty_triggered = true;
		return true;
	}
	return false;
}

static void hx_blreset_bl_enter(struct hx_algo *algo, u8 state)
{
	algo->blreset_baseline_state = state;
	algo->blreset_baseline_elapsed_ms = 0;
}

void hx_blreset_baseline_state_update(struct hx_algo *algo,
				      bool force_reset, bool touch_protected,
				      u16 activity_nodes)
{
	u32 interval = max_t(u16, algo->frame_interval_ms, 1);

	/* BLReset_BaselineStateUpdate resets the machine at screen-on, on the
	 * explicit reset input, and on a hardware-state transition.  The Linux
	 * screen epoch is the authoritative equivalent of those timestamps. */
	if (force_reset || algo->frame_sequence == algo->screen_on_frame_sequence) {
		algo->blreset_baseline_state = HX_BLRST_BL_BOOTSTRAP;
		algo->blreset_baseline_stable_frames = 0;
		algo->blreset_baseline_elapsed_ms = 0;
		algo->blreset_normal_baseline_ready = false;
		algo->blreset_clean_baseline_captured = false;
		return;
	}

	switch (algo->blreset_baseline_state) {
	case HX_BLRST_BL_BOOTSTRAP:
		/* TSAStatic+0x3e8 is allowed to reach six; the following frame
		 * changes state 0 -> 1 and clears it. */
		if (algo->blreset_baseline_stable_frames > 5) {
			hx_blreset_bl_enter(algo, HX_BLRST_BL_ARMED);
			algo->blreset_baseline_stable_frames = 0;
		} else if (algo->blreset_baseline_stable_frames < U8_MAX) {
			algo->blreset_baseline_stable_frames++;
		}
		break;
	case HX_BLRST_BL_ARMED:
		if (touch_protected)
			hx_blreset_bl_enter(algo, HX_BLRST_BL_PROTECTED);
		break;
	case HX_BLRST_BL_PROTECTED:
		if (!touch_protected)
			hx_blreset_bl_enter(algo, HX_BLRST_BL_RELEASE);
		break;
	case HX_BLRST_BL_RELEASE:
		if (touch_protected) {
			hx_blreset_bl_enter(algo, HX_BLRST_BL_PROTECTED);
			break;
		}
		/* The vendor uses its over-noise node count at g_tsaPrpt+0xb8:
		 * >30 restarts, <=1 advances, otherwise a 333 ms window runs. */
		if (activity_nodes > 30 ||
		    algo->blreset_baseline_elapsed_ms > 333) {
			algo->blreset_normal_baseline_ready = true;
			hx_blreset_bl_enter(algo, HX_BLRST_BL_ARMED);
		} else if (activity_nodes <= 1) {
			hx_blreset_bl_enter(algo, HX_BLRST_BL_CLEAN_WAIT);
		} else {
			algo->blreset_baseline_elapsed_ms = min_t(u32,
				algo->blreset_baseline_elapsed_ms + interval, ~0U);
		}
		break;
	case HX_BLRST_BL_CLEAN_WAIT:
		if (touch_protected) {
			hx_blreset_bl_enter(algo, HX_BLRST_BL_PROTECTED);
			break;
		}
		if (algo->blreset_baseline_elapsed_ms > 100) {
			algo->blreset_normal_baseline_ready = false;
			algo->blreset_baseline_state = HX_BLRST_BL_CAPTURED;
			memcpy(algo->normal_baseline_q8, algo->baseline_q8,
			       sizeof(algo->normal_baseline_q8));
			algo->normal_baseline_valid = true;
			algo->blreset_clean_baseline_captured = true;
		} else if (activity_nodes <= 1) {
			algo->blreset_baseline_elapsed_ms = min_t(u32,
				algo->blreset_baseline_elapsed_ms + interval, ~0U);
		} else {
			algo->blreset_normal_baseline_ready = true;
			hx_blreset_bl_enter(algo, HX_BLRST_BL_ARMED);
		}
		break;
	case 3:
	case 5:
	case 6:
	case 7:
	case HX_BLRST_BL_CAPTURED:
	default:
		break;
	}
}
