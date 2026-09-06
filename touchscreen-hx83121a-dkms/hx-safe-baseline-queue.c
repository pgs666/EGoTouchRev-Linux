// SPDX-License-Identifier: GPL-2.0
/* Chronological SafeBaseline queue, matching the vendor ring semantics. */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/string.h>
#endif

#include "hx-algo-internal.h"

static bool hx_safe_queue_compatible(const s32 *first_q8,
				     const s32 *second_q8)
{
	s64 sum = 0;
	s32 common;
	u16 divergent = 0;
	int i;

	for (i = 0; i < HX_PIXELS; i++)
		sum += (first_q8[i] - second_q8[i]) >>
			HX_BASELINE_FRACTION_BITS;
	common = (s32)(sum / HX_PIXELS);
	if (abs(common) > HX_BASELINE_CLEAN_LOCAL_THRESHOLD)
		return false;
	for (i = 0; i < HX_PIXELS; i++) {
		s32 local = ((first_q8[i] - second_q8[i]) >>
			HX_BASELINE_FRACTION_BITS) - common;

		if (abs(local) > HX_BASELINE_CLEAN_LOCAL_THRESHOLD &&
		    ++divergent > HX_BASELINE_CLEAN_MAX_NODES)
			return false;
	}
	return true;
}

void hx_safe_baseline_queue_reset(struct hx_algo *algo)
{
	int i;

	algo->safe_queue_head = 0;
	algo->safe_queue_tail = 0;
	algo->safe_queue_count = 0;
	algo->safe_queue_full_pushes = 0;
	algo->safe_baseline_pushes = 0;
	algo->safe_baseline_count = 0;
	algo->safe_baseline_next = 0;
	for (i = 0; i < HX_SAFE_BASELINE_SLOTS; i++)
		algo->safe_queue_order[i] = 0;
}

u8 hx_safe_baseline_queue_oldest(const struct hx_algo *algo)
{
	if (!algo->safe_queue_count)
		return HX_SAFE_BASELINE_SLOTS;
	return algo->safe_queue_order[algo->safe_queue_tail];
}

u8 hx_safe_baseline_queue_latest(const struct hx_algo *algo)
{
	u8 index;

	if (!algo->safe_queue_count)
		return HX_SAFE_BASELINE_SLOTS;
	index = (algo->safe_queue_head + HX_SAFE_BASELINE_SLOTS - 1) %
		HX_SAFE_BASELINE_SLOTS;
	return algo->safe_queue_order[index];
}

bool hx_safe_baseline_queue_trustable(const struct hx_algo *algo)
{
	u32 first_epoch = ~0U;
	u8 distinct = 0;
	u8 i;

	/* SafeBaseline_IsTrustableEnoughCommon requires at least two queue
	 * units from distinct screen-on epochs.  Do not count reset/invalid
	 * entries, and do not infer trust from confidence alone. */
	for (i = 0; i < algo->safe_queue_count; i++) {
		u8 index = (algo->safe_queue_tail + i) % HX_SAFE_BASELINE_SLOTS;
		const struct hx_safe_baseline_entry *entry =
			&algo->safe_baselines[algo->safe_queue_order[index]];

		if (!entry->valid || entry->reset_pending)
			continue;
		if (first_epoch == ~0U || entry->screen_epoch != first_epoch) {
			first_epoch = entry->screen_epoch;
			if (++distinct >= 2)
				return true;
		}
	}
	return false;
}

bool hx_safe_baseline_queue_double_checked(const struct hx_algo *algo)
{
	u32 first_epoch = ~0U;
	u8 i;

	/* SafeBaseline_IsDoubleChecked is intentionally weaker than
	 * IsTrustableEnoughCommon: it only asks whether at least one valid history
	 * item predates the current screen epoch (or, equivalently, whether two
	 * queue timestamps differ).  Replacement must use this predicate rather
	 * than the stronger confidence gate. */
	for (i = 0; i < algo->safe_queue_count; i++) {
		u8 index = (algo->safe_queue_tail + i) % HX_SAFE_BASELINE_SLOTS;
		const struct hx_safe_baseline_entry *entry =
			&algo->safe_baselines[algo->safe_queue_order[index]];

		if (!entry->valid || entry->reset_pending)
			continue;
		if (first_epoch == ~0U)
			first_epoch = entry->screen_epoch;
		else if (entry->screen_epoch != first_epoch)
			return true;
	}
	return false;
}

bool hx_safe_baseline_queue_all_valid(const struct hx_algo *algo)
{
	u8 i;

	/* SafeBaseline_IsValid checks every normal-queue unit against the
	 * current working baseline.  A single incompatible unit invalidates the
	 * queue for reset decisions; this is intentionally stricter than slot
	 * selection, which may still choose a spatially fitting unit. */
	for (i = 0; i < algo->safe_queue_count; i++) {
		u8 index = (algo->safe_queue_tail + i) % HX_SAFE_BASELINE_SLOTS;
		const struct hx_safe_baseline_entry *entry =
			&algo->safe_baselines[algo->safe_queue_order[index]];

		if (!entry->valid || entry->reset_pending ||
		    !hx_safe_queue_compatible(entry->baseline_q8,
					       algo->baseline_q8))
			return false;
	}
	/* The vendor SafeBaseline_IsValid() returns true for an empty queue;
	 * emptiness means there is no stale unit to invalidate. */
	return true;
}

void hx_safe_baseline_queue_record(struct hx_algo *algo, u8 slot)
{
	u8 i;

	if (slot >= HX_SAFE_BASELINE_SLOTS)
		return;
	/* Push commonly reuses the physical unit returned as the oldest item.
	 * Treat that as a pop-then-push, rather than deduplicating it in place. */
	if (algo->safe_queue_count == HX_SAFE_BASELINE_SLOTS &&
	    algo->safe_queue_order[algo->safe_queue_tail] == slot) {
		algo->safe_queue_tail = (algo->safe_queue_tail + 1) %
			HX_SAFE_BASELINE_SLOTS;
		algo->safe_queue_order[algo->safe_queue_head] = slot;
		algo->safe_queue_head = (algo->safe_queue_head + 1) %
			HX_SAFE_BASELINE_SLOTS;
		algo->safe_baseline_next = algo->safe_queue_head;
		return;
	}
	/* A deduplicated baseline remains at its original queue position. */
	for (i = 0; i < algo->safe_queue_count; i++) {
		u8 index = (algo->safe_queue_tail + i) % HX_SAFE_BASELINE_SLOTS;

		if (algo->safe_queue_order[index] == slot)
			return;
	}
	if (algo->safe_queue_count == HX_SAFE_BASELINE_SLOTS) {
		u8 old = algo->safe_queue_order[algo->safe_queue_tail];

		algo->safe_baselines[old].valid = false;
		algo->safe_baselines[old].reset_pending = false;
		algo->safe_baselines[old].state = HX_SAFE_SLOT_EMPTY;
		algo->safe_queue_tail = (algo->safe_queue_tail + 1) %
			HX_SAFE_BASELINE_SLOTS;
		algo->safe_queue_count--;
	}
	algo->safe_queue_order[algo->safe_queue_head] = slot;
	algo->safe_queue_head = (algo->safe_queue_head + 1) %
		HX_SAFE_BASELINE_SLOTS;
	algo->safe_queue_count++;
	algo->safe_baseline_count = algo->safe_queue_count;
	algo->safe_baseline_next = algo->safe_queue_head;
}

void hx_safe_baseline_queue_remove(struct hx_algo *algo, u8 slot)
{
	u8 i;

	for (i = 0; i < algo->safe_queue_count; i++) {
		u8 index = (algo->safe_queue_tail + i) % HX_SAFE_BASELINE_SLOTS;
		u8 next;

		if (algo->safe_queue_order[index] != slot)
			continue;
		/* Compact the logical FIFO order; physical slot storage remains
		 * untouched so callers can mark the unit RESET before removal. */
		for (; i + 1 < algo->safe_queue_count; i++) {
			next = (algo->safe_queue_tail + i + 1) % HX_SAFE_BASELINE_SLOTS;
			algo->safe_queue_order[index] = algo->safe_queue_order[next];
			index = next;
		}
		algo->safe_queue_count--;
		algo->safe_queue_head = (algo->safe_queue_tail +
			algo->safe_queue_count) % HX_SAFE_BASELINE_SLOTS;
		algo->safe_baseline_count = algo->safe_queue_count;
		algo->safe_baseline_next = algo->safe_queue_head;
		return;
	}
}

void hx_safe_baseline_temp_reset(struct hx_algo *algo)
{
	memset(algo->safe_temp_baselines, 0,
	       sizeof(algo->safe_temp_baselines));
	memset(algo->safe_temp_queue_order, 0,
	       sizeof(algo->safe_temp_queue_order));
	algo->safe_temp_queue_head = 0;
	algo->safe_temp_queue_tail = 0;
	algo->safe_temp_queue_count = 0;
}

bool hx_safe_baseline_temp_observe(struct hx_algo *algo,
				   const s32 *baseline_q8)
{
	struct hx_safe_baseline_entry *entry;
	u8 slot;
	u8 i;
	u8 distinct_epochs = 0;
	u32 previous_epoch = ~0U;

	/* SafeBaseline_IsNewBaselineTrustableEnough first validates all temp
	 * units against the current baseline.  One incompatible unit resets the
	 * temporary queue before the current observation is pushed. */
	for (i = 0; i < algo->safe_temp_queue_count; i++) {
		u8 index = (algo->safe_temp_queue_tail + i) %
			HX_SAFE_BASELINE_SLOTS;
		entry = &algo->safe_temp_baselines[
			algo->safe_temp_queue_order[index]];
		if (!entry->valid ||
		    !hx_safe_queue_compatible(entry->baseline_q8, baseline_q8)) {
			hx_safe_baseline_temp_reset(algo);
			break;
		}
	}

	/* Do not manufacture an independent confirmation from repeated frames
	 * in one screen-on epoch. */
	for (i = 0; i < algo->safe_temp_queue_count; i++) {
		u8 index = (algo->safe_temp_queue_tail + i) %
			HX_SAFE_BASELINE_SLOTS;
		entry = &algo->safe_temp_baselines[
			algo->safe_temp_queue_order[index]];
		if (entry->screen_epoch == algo->screen_epoch)
			goto count_epochs;
	}
	if (algo->safe_temp_queue_count < HX_SAFE_BASELINE_SLOTS) {
		slot = algo->safe_temp_queue_head;
		algo->safe_temp_queue_count++;
	} else {
		slot = algo->safe_temp_queue_order[algo->safe_temp_queue_tail];
		algo->safe_temp_queue_tail = (algo->safe_temp_queue_tail + 1) %
			HX_SAFE_BASELINE_SLOTS;
	}
	entry = &algo->safe_temp_baselines[slot];
	memcpy(entry->baseline_q8, baseline_q8, sizeof(entry->baseline_q8));
	entry->valid = true;
	entry->screen_epoch = algo->screen_epoch;
	entry->captured_frame = algo->frame_sequence;
	entry->generation = ++algo->safe_baseline_generation;
	algo->safe_temp_queue_order[algo->safe_temp_queue_head] = slot;
	algo->safe_temp_queue_head = (algo->safe_temp_queue_head + 1) %
		HX_SAFE_BASELINE_SLOTS;

count_epochs:
	for (i = 0; i < algo->safe_temp_queue_count; i++) {
		u8 index = (algo->safe_temp_queue_tail + i) %
			HX_SAFE_BASELINE_SLOTS;
		entry = &algo->safe_temp_baselines[
			algo->safe_temp_queue_order[index]];
		if (!entry->valid)
			continue;
		if (previous_epoch == ~0U || entry->screen_epoch != previous_epoch) {
			previous_epoch = entry->screen_epoch;
			if (++distinct_epochs >= 2)
				return true;
		}
	}
	return false;
}
