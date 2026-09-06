// SPDX-License-Identifier: GPL-2.0
/* Stable-hold baseline (BLSM_Shb*) state and raw-change detection. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

#define HX_SHB_CAPTURE 1
#define HX_SHB_SETTLED 2
#define HX_SHB_WINDOW_MS 5000

static bool hx_shb_raw_changed(const struct hx_algo *algo,
			       const u16 *raw)
{
	u16 threshold = max_t(u16, algo->wake_raw_jump_threshold / 2, 1);
	int i;

	for (i = 0; i < HX_PIXELS; i++)
		if (abs((s32)le16_to_cpup(raw + i) -
			(s32)le16_to_cpup(algo->shb_raw_capture + i)) > threshold)
			return true;
	return false;
}

void hx_blsm_shb_process(struct hx_algo *algo, const u16 *pre_cmf_raw,
			 bool reset_stage)
{
	u32 window_frames = max_t(u32, HX_SHB_WINDOW_MS /
			max_t(u16, algo->frame_interval_ms, 1), 1);
	u32 elapsed;

	/* BLSM_PreProcess clears the action word on every frame.  State and the
	 * captured grid persist, but bits 0/1 describe only this invocation. */
	algo->shb_flags = 0;
	if (reset_stage || algo->baseline_stage == HX_BLSM_RESET) {
		algo->shb_state = 0;
		algo->shb_capture_frame = 0;
	}
	if (algo->shb_state && hx_shb_raw_changed(algo, pre_cmf_raw)) {
		/* BLSM_ShbIsRawChanges restarts the hold window and clears pending
		 * actions; it does not alter the working baseline. */
		algo->shb_state = 0;
	}
	elapsed = algo->shb_state ?
		algo->frame_sequence - algo->shb_capture_frame : 0;
	if (algo->shb_state == HX_SHB_CAPTURE && elapsed >= window_frames) {
		algo->shb_state = HX_SHB_SETTLED;
		algo->shb_flags |= 1;
	} else if (algo->shb_state == HX_SHB_SETTLED &&
		   elapsed >= window_frames) {
		algo->shb_flags |= 1 | 2;
	} else if (!algo->shb_state) {
		algo->shb_state = HX_SHB_CAPTURE;
		algo->shb_flags |= 1;
	}
	/* BLSM_ShbCaptureBl executes whenever bit 0 is emitted.  In particular
	 * the five-second transition re-captures raw and restarts the second
	 * window; comparing both windows against the initial frame is not
	 * equivalent when display/VCOM drift occurs between them. */
	if (algo->shb_flags & 1) {
		memcpy(algo->shb_raw_capture, pre_cmf_raw,
		       sizeof(algo->shb_raw_capture));
		algo->shb_capture_frame = algo->frame_sequence;
	}
}

u16 hx_blsm_shb_consume_action(struct hx_algo *algo)
{
	u16 action = algo->shb_flags & 2;

	/* Bit 1 is an action notification, not a persistent BLSM stage.  Keep
	 * the raw snapshot/state for diagnostics, but consume the notification so
	 * one settled window cannot force every subsequent frame into NOISY. */
	algo->shb_flags &= ~2U;
	return action;
}
