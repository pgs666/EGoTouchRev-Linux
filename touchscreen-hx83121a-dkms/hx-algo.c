// SPDX-License-Identifier: GPL-2.0
/*
 * Himax HX83121A touch algorithm implementation.
 *
 * This file contains the pure signal-processing pipeline:
 *   Phase 1: preprocessing  (baseline subtraction, CMF, IIR)
 *   Phase 2: touch solving  (macro-zone BFS, palm rejection, peak detection)
 *   Phase 3: tracking       (greedy distance-matching)
 *
 * No SPI, no IRQ, no input_dev — all driver glue lives in himax-spi-core.c.
 */

#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>

#include "hx-algo.h"

/* Raw baseline value output by the firmware when no touch is present. */
#define HX_BASELINE  0x7ffe

/*
 * Default tracking constants — exposed through hx_algo fields so they can
 * be overridden at runtime via sysfs without reloading the module.
 */
#define HIMAX_TRACK_MATCH_DIST2   (420 * 420)
#define HIMAX_TRACK_LOST_FRAMES   3
#define HIMAX_NEW_TOUCH_DEBOUNCE  2

/* ======================================================================== */
/* Initialisation                                                            */
/* ======================================================================== */

void hx_algo_init_defaults(struct hx_algo *algo)
{
	algo->cmf_enabled        = true;
	algo->cmf_exclusion      = 250;
	algo->cmf_max_correction = 500;
	algo->iir_enabled        = true;
	algo->iir_decay_weight   = 200;
	algo->iir_decay_step     = 80;
	algo->iir_noise_floor    = 5;
	algo->iir_gate_floor     = 200;
	algo->iir_gate_ratio_q8  = 26;
	algo->gauss_enabled      = false;
	algo->gauss_center_weight = 20;
	algo->macro_threshold    = 800;
	algo->peak_threshold     = 800;
	algo->palm_enabled       = true;
	algo->palm_area_threshold    = 50;
	algo->palm_signal_threshold  = 80000;
	algo->palm_density_low       = 400;
	algo->track_dist2_max   = HIMAX_TRACK_MATCH_DIST2;
	algo->track_lost_frames = HIMAX_TRACK_LOST_FRAMES;
	algo->debounce_base     = HIMAX_NEW_TOUCH_DEBOUNCE;
}

/* ======================================================================== */
/* Phase 1A — baseline subtraction                                          */
/* ======================================================================== */

static void hx_prepare_frame_baseline(struct hx_algo *algo, const u16 *raw)
{
	int r, c;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 sample = (s32)le16_to_cpup(raw + idx) - HX_BASELINE;

			algo->frame[r][c] = clamp_t(s32, sample, SHRT_MIN, SHRT_MAX);
		}
	}

	/* pixel [0][0] is always invalid on this panel layout */
	algo->frame[0][0] = 0;
}

/* ======================================================================== */
/* Phase 1B — CMF (Common Mode Filter)                                      */
/*                                                                           */
/* Removes charger-induced common-mode noise by subtracting per-row and     */
/* per-column offsets computed from "quiet" pixels (|val| < exclusion).     */
/* DualDim mode: rows first, then columns.                                   */
/* ======================================================================== */

static void hx_apply_cmf(struct hx_algo *algo)
{
	int r, c;

	/* Row pass */
	for (r = 0; r < HX_ROWS; r++) {
		s32 sum = 0, count = 0, offset;

		for (c = 0; c < HX_COLS; c++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (c = 0; c < HX_COLS; c++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}

	/* Column pass */
	for (c = 0; c < HX_COLS; c++) {
		s32 sum = 0, count = 0, offset;

		for (r = 0; r < HX_ROWS; r++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (r = 0; r < HX_ROWS; r++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}
}

/* ======================================================================== */
/* Phase 1C — GridIIR temporal filter                                       */
/*                                                                           */
/* Per-pixel exponential decay for noise suppression.  Pixels above a       */
/* dynamic threshold (proportional to the frame maximum) bypass the filter  */
/* so real touch signals are never attenuated.                               */
/* ======================================================================== */

static void hx_apply_iir(struct hx_algo *algo)
{
	int r, c;
	s32 frame_max = 0;
	s32 dyn_threshold;
	u16 decay_weight, decay_step;

	if (!algo->iir_enabled) {
		memcpy(algo->iir_history, algo->frame, sizeof(algo->frame));
		algo->iir_initialized = true;
		return;
	}

	if (!algo->iir_initialized) {
		memcpy(algo->iir_history, algo->frame, sizeof(algo->frame));
		algo->iir_initialized = true;
		return;
	}

	for (r = 0; r < HX_ROWS; r++)
		for (c = 0; c < HX_COLS; c++)
			frame_max = max(frame_max, abs((int)algo->frame[r][c]));

	dyn_threshold = max((frame_max * algo->iir_gate_ratio_q8) >> 8,
			    (s32)algo->iir_gate_floor);
	decay_weight  = min_t(u16, algo->iir_decay_weight, 256);
	decay_step    = algo->iir_decay_step;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			s32 cur = algo->frame[r][c];
			s32 output;

			if (cur >= dyn_threshold) {
				output = cur;
			} else {
				s32 hist  = algo->iir_history[r][c];
				s32 mixed = decay_weight * cur +
					    (256 - decay_weight) * hist;

				output = mixed >> 8;
				output = max(0, output - (s32)decay_step);
				if (output < algo->iir_noise_floor)
					output = 0;
			}

			algo->frame[r][c]       = clamp_t(s32, output, SHRT_MIN, SHRT_MAX);
			algo->iir_history[r][c] = algo->frame[r][c];
		}
	}
}

/* ======================================================================== */
/* Phase 1 entry point                                                       */
/* ======================================================================== */

void hx_preprocess_frame(struct hx_algo *algo, const u16 *raw)
{
	hx_prepare_frame_baseline(algo, raw);

	if (algo->cmf_enabled)
		hx_apply_cmf(algo);

	hx_apply_iir(algo);
}

/* ======================================================================== */
/* Helpers                                                                   */
/* ======================================================================== */

/* Debug helper — kept for bring-up and signal-quality checks. */
static void __maybe_unused dump_frame(const struct hx_algo *algo)
{
	if (!IS_ENABLED(CONFIG_DYNAMIC_DEBUG))
		return;

	char buf[1024];

	pr_warn("Frame start\n");
	for (int i = 0, offset; i < HX_PIXELS; i++) {
		if (i % HX_COLS == 0) {
			if (i)
				pr_info("%s\n", buf);
			offset = sprintf(buf, "%04x:", i);
		}
		offset += sprintf(buf + offset, " %04x",
				  (u16)max_t(s16, 0,
					     algo->frame[i / HX_COLS][i % HX_COLS]));
	}
}

/*
 * Clamp-to-zero accessor: returns 0 for out-of-bounds or negative values so
 * neighbour lookups near the grid edge never need special-casing.
 */
static inline s16 hx_frame_at(const struct hx_algo *algo, int r, int c)
{
	s16 val;

	if (r < 0 || r >= HX_ROWS || c < 0 || c >= HX_COLS)
		return 0;
	val = algo->frame[r][c];
	return val > 0 ? val : 0;
}

/* ======================================================================== */
/* Phase 2A — macro-zone detection (8-connected BFS)                        */
/* ======================================================================== */

void hx_detect_macro_zones(struct hx_algo *algo)
{
	static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
	static const int dc[] = {-1,  0,  1, -1, 1, -1, 0, 1};
	u16 head, tail;
	int r, c, d;

	memset(algo->visited, 0, sizeof(algo->visited));
	algo->zone_count = 0;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			struct hx_macro_zone *zone;

			if (algo->visited[idx])
				continue;
			if (algo->frame[r][c] < algo->macro_threshold)
				continue;
			if (algo->zone_count >= HX_MAX_ZONES)
				return;

			zone = &algo->zones[algo->zone_count];
			zone->area       = 0;
			zone->signal_sum = 0;
			zone->min_r = r;  zone->max_r = r;
			zone->min_c = c;  zone->max_c = c;

			/* Ring-buffer BFS using the pre-allocated queue. */
			head = 0;
			tail = 0;
			algo->bfs_queue[tail++] = idx;
			algo->visited[idx] = 1;

			while (head != tail) {
				int ci = algo->bfs_queue[head++];
				int cr = ci / HX_COLS;
				int cc = ci % HX_COLS;
				s16 sig = algo->frame[cr][cc];

				if (zone->area < HX_ZONE_PX_MAX)
					zone->pixels[zone->area] = ci;
				zone->area++;
				if (sig > 0)
					zone->signal_sum += sig;

				if (cr < zone->min_r) zone->min_r = cr;
				if (cr > zone->max_r) zone->max_r = cr;
				if (cc < zone->min_c) zone->min_c = cc;
				if (cc > zone->max_c) zone->max_c = cc;

				for (d = 0; d < 8; d++) {
					int nr = cr + dr[d];
					int nc = cc + dc[d];
					int ni;

					if (nr < 0 || nr >= HX_ROWS ||
					    nc < 0 || nc >= HX_COLS)
						continue;
					ni = nr * HX_COLS + nc;
					if (algo->visited[ni])
						continue;
					if (algo->frame[nr][nc] < algo->macro_threshold)
						continue;
					algo->visited[ni] = 1;
					algo->bfs_queue[tail++] = ni;
				}
			}

			algo->zone_count++;
		}
	}
}

/* ======================================================================== */
/* Phase 2B — palm rejection                                                 */
/*                                                                           */
/* Four integer-only rules.  Any zone matching a rule is discarded.         */
/* Rule 1: area >= palm_area_threshold                (large footprint)     */
/* Rule 2: signal_sum >= palm_signal_threshold         (strong integrated)  */
/* Rule 3: area >= 20 && density < palm_density_low   (spread low signal)  */
/* Rule 4: area >= 10 && aspect-ratio >= 4:1           (elongated shape)   */
/* ======================================================================== */

void hx_reject_palms(struct hx_algo *algo)
{
	u8 dst = 0;
	u8 i;

	if (!algo->palm_enabled)
		return;

	for (i = 0; i < algo->zone_count; i++) {
		struct hx_macro_zone *z = &algo->zones[i];
		u16 bbox_w, bbox_h, max_side, min_side;
		bool reject = false;

		/* Rule 1 */
		if (z->area >= algo->palm_area_threshold) {
			reject = true;
			goto next;
		}

		/* Rule 2 */
		if (z->signal_sum >= algo->palm_signal_threshold) {
			reject = true;
			goto next;
		}

		/* Rule 3: density = signal_sum / area < palm_density_low
		 *         Rewritten without division: signal_sum < low * area */
		if (z->area >= 20 &&
		    z->signal_sum < (s32)algo->palm_density_low * z->area) {
			reject = true;
			goto next;
		}

		/* Rule 4: aspect ratio — fixed-point: max*256 >= 4*min*256 */
		bbox_w   = z->max_c - z->min_c + 1;
		bbox_h   = z->max_r - z->min_r + 1;
		max_side = max(bbox_w, bbox_h);
		min_side = min(bbox_w, bbox_h);
		if (z->area >= 10 && min_side > 0 &&
		    (u32)max_side * 256 >= 1024u * min_side)
			reject = true;

next:
		if (!reject) {
			if (dst != i)
				algo->zones[dst] = *z;
			dst++;
		}
	}

	algo->zone_count = dst;
}

/* ======================================================================== */
/* Phase 2C — peak detection within surviving zones                         */
/* ======================================================================== */

/*
 * Asymmetric local-maximum test.
 *
 * "Before" neighbours (up + left in scan order) must be strictly less;
 * "after" neighbours (down + right) may be equal.  This breaks ties on
 * flat ridges so exactly one peak is produced per finger plateau.
 */
static bool hx_is_asymmetric_peak(const struct hx_algo *algo, int r, int c)
{
	s16 v = algo->frame[r][c];
	int dr, dc;

	for (dr = -1; dr <= 1; dr++) {
		for (dc = -1; dc <= 1; dc++) {
			int nr, nc;
			s16 nv;
			bool after;

			if (dr == 0 && dc == 0)
				continue;
			nr = r + dr;
			nc = c + dc;
			if (nr < 0 || nr >= HX_ROWS || nc < 0 || nc >= HX_COLS)
				continue;
			nv    = algo->frame[nr][nc];
			after = (dr > 0) || (dr == 0 && dc > 0);
			if (after) {
				if (nv > v) return false;
			} else {
				if (nv >= v) return false;
			}
		}
	}
	return true;
}

/*
 * Pressure-drift detector.
 *
 * A flat palm press produces a nearly-uniform row of elevated pixels with
 * low cross-row gradient.  Returns true when the peak signal falls in the
 * drift range [3/8, 3/4] of peak_threshold and the row gradient is low
 * while the row signal sum is high relative to the peak.
 */
static bool hx_detect_pressure_drift(const struct hx_algo *algo, int r, int c)
{
	s16 peak_sig  = algo->frame[r][c];
	s16 limit3_4  = (algo->peak_threshold * 3) >> 2;
	s16 limit3_8  = (algo->peak_threshold * 3) >> 3;
	int grad_sum  = 0;
	int row_sum   = 0;
	int col;

	if (peak_sig > limit3_4 || peak_sig < limit3_8)
		return false;

	for (col = 1; col < HX_COLS - 1; col++) {
		int grad = abs((int)hx_frame_at(algo, r, col + 1) -
			       (int)hx_frame_at(algo, r, col - 1));

		if (grad > algo->peak_threshold / 3)
			return false;   /* sharp spike → not drift */
		grad_sum += grad;
		if (algo->frame[r][col] > 0)
			row_sum += algo->frame[r][col];
	}

	return (row_sum >= peak_sig * 9 / 2) &&
	       (peak_sig * 6 >= grad_sum);
}

/*
 * Insert a peak into the fixed-size peak array.  When the array is full,
 * replace the weakest existing entry if the new peak is stronger.
 */
static void hx_insert_peak(struct hx_algo *algo, const struct hx_peak *p)
{
	int k, weakest;

	if (algo->peak_count < HX_MAX_PEAKS) {
		algo->peaks[algo->peak_count++] = *p;
		return;
	}

	weakest = 0;
	for (k = 1; k < HX_MAX_PEAKS; k++) {
		if (algo->peaks[k].z < algo->peaks[weakest].z)
			weakest = k;
	}
	if (p->z > algo->peaks[weakest].z)
		algo->peaks[weakest] = *p;
}

void hx_detect_peaks(struct hx_algo *algo)
{
	u8 zi;

	algo->peak_count = 0;

	/* --- Asymmetric local-max scan within each surviving zone --- */
	for (zi = 0; zi < algo->zone_count; zi++) {
		struct hx_macro_zone *zone = &algo->zones[zi];
		u16 px_limit = min_t(u16, zone->area, HX_ZONE_PX_MAX);
		u16 pi;

		for (pi = 0; pi < px_limit; pi++) {
			int idx = zone->pixels[pi];
			int r = idx / HX_COLS;
			int c = idx % HX_COLS;
			s16 v = algo->frame[r][c];
			struct hx_peak peak;
			int dr, dc;
			s32 nbr_sum = 0;

			if (v < algo->peak_threshold)
				continue;
			if (!hx_is_asymmetric_peak(algo, r, c))
				continue;
			if (hx_detect_pressure_drift(algo, r, c))
				continue;

			for (dr = -1; dr <= 1; dr++)
				for (dc = -1; dc <= 1; dc++) {
					if (dr == 0 && dc == 0)
						continue;
					nbr_sum += hx_frame_at(algo, r + dr, c + dc);
				}

			peak = (struct hx_peak){
				.r         = r,
				.c         = c,
				.z         = v,
				.nbr_sum   = nbr_sum,
				.zone_area = zone->area,
			};
			hx_insert_peak(algo, &peak);
		}
	}

	/* --- Z8 isolation filter: (z >> 5) > nbr_sum → isolated spike --- */
	{
		u8 dst = 0, i;

		for (i = 0; i < algo->peak_count; i++) {
			if ((algo->peaks[i].z >> 5) <= algo->peaks[i].nbr_sum)
				algo->peaks[dst++] = algo->peaks[i];
		}
		algo->peak_count = dst;
	}

	/* --- Zone minimum-area filter: area < 2 → reject --- */
	{
		u8 dst = 0, i;

		for (i = 0; i < algo->peak_count; i++) {
			if (algo->peaks[i].zone_area >= 2)
				algo->peaks[dst++] = algo->peaks[i];
		}
		algo->peak_count = dst;
	}

	/* --- Edge peak filter: weak edge peaks < max_sig * 5/8 --- */
	{
		int edge;

		for (edge = 0; edge < 4; edge++) {
			s16 max_sig = 0, cutoff;
			u8 dst = 0, i;

			for (i = 0; i < algo->peak_count; i++) {
				bool on_edge;

				switch (edge) {
				case 0:  on_edge = algo->peaks[i].r == 0; break;
				case 1:  on_edge = algo->peaks[i].r == HX_ROWS - 1; break;
				case 2:  on_edge = algo->peaks[i].c == 0; break;
				default: on_edge = algo->peaks[i].c == HX_COLS - 1; break;
				}
				if (on_edge && algo->peaks[i].z > max_sig)
					max_sig = algo->peaks[i].z;
			}
			if (max_sig == 0)
				continue;

			cutoff = (max_sig >> 3) * 5;
			for (i = 0; i < algo->peak_count; i++) {
				bool on_edge;

				switch (edge) {
				case 0:  on_edge = algo->peaks[i].r == 0; break;
				case 1:  on_edge = algo->peaks[i].r == HX_ROWS - 1; break;
				case 2:  on_edge = algo->peaks[i].c == 0; break;
				default: on_edge = algo->peaks[i].c == HX_COLS - 1; break;
				}
				if (!(on_edge && algo->peaks[i].z < cutoff))
					algo->peaks[dst++] = algo->peaks[i];
			}
			algo->peak_count = dst;
		}
	}

	/* --- Sort ascending by signal (selection sort, ≤20 elements) --- */
	{
		u8 i, j;

		for (i = 0; i + 1 < algo->peak_count; i++) {
			u8 min_idx = i;

			for (j = i + 1; j < algo->peak_count; j++) {
				if (algo->peaks[j].z < algo->peaks[min_idx].z)
					min_idx = j;
			}
			if (min_idx != i)
				swap(algo->peaks[i], algo->peaks[min_idx]);
		}
	}
}

/* ======================================================================== */
/* Peak → position conversion                                               */
/*                                                                           */
/* 4-point weighted interpolation.  Output range [0, 65535].               */
/* ======================================================================== */

void hx_peaks_to_positions(struct hx_algo *algo,
			    struct input_mt_pos *pos, int *cnt)
{
	int i;

	*cnt = min_t(int, algo->peak_count, HIMAX_MAX_TOUCH);

	for (i = 0; i < *cnt; i++) {
		int r = algo->peaks[i].r;
		int c = algo->peaks[i].c;
		s16 vc = hx_frame_at(algo, r, c);
		s16 vl = hx_frame_at(algo, r, c - 1);
		s16 vr = hx_frame_at(algo, r, c + 1);
		s16 vu = hx_frame_at(algo, r - 1, c);
		s16 vd = hx_frame_at(algo, r + 1, c);
		s64 tmp_x, tmp_y, num;

		tmp_x = (s64)(vc + vr) * (vc + vl);
		if (tmp_x > 0) {
			num = (2 * c + 1) * tmp_x + 2 * vc * (vr - vl);
			pos[i].x = clamp_val(div_s64(64 * num, 3 * tmp_x),
					     0, SZ_64K - 1);
		} else {
			pos[i].x = clamp_val((s64)c * SZ_64K / HX_COLS,
					     0, SZ_64K - 1);
		}

		tmp_y = (s64)(vc + vu) * (vc + vd);
		if (tmp_y > 0) {
			num = (2 * r + 1) * tmp_y + 2 * vc * (vd - vu);
			pos[i].y = clamp_val(div_s64(20 * num, tmp_y),
					     0, SZ_64K - 1);
		} else {
			pos[i].y = clamp_val((s64)r * SZ_64K / HX_ROWS,
					     0, SZ_64K - 1);
		}
	}
}

/* ======================================================================== */
/* Phase 3A — greedy distance-matching tracker                              */
/* ======================================================================== */

static inline s64 hx_dist2(const struct input_mt_pos *a,
			    const struct hx_track *b)
{
	s32 dx = a->x - b->x;
	s32 dy = a->y - b->y;

	return (s64)dx * dx + (s64)dy * dy;
}

struct hx_match_candidate {
	u8  track_idx;
	u8  det_idx;
	s64 dist2;
};

static void hx_reset_track(struct hx_track *trk)
{
	memset(trk, 0, sizeof(*trk));
}

void hx_track_contacts(struct hx_algo *algo,
		       struct input_mt_pos *det, int det_cnt)
{
	bool det_used[HIMAX_MAX_TOUCH]     = { false };
	bool track_matched[HIMAX_MAX_TOUCH] = { false };
	struct hx_match_candidate cand[HIMAX_MAX_TOUCH * HIMAX_MAX_TOUCH];
	int cand_cnt = 0;
	int i, j, k;

	/*
	 * Build a candidate list of (track, detection) pairs within the
	 * maximum allowed squared distance.
	 */
	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		struct hx_track *trk = &algo->tracks[i];

		if (!trk->active)
			continue;

		for (j = 0; j < det_cnt; j++) {
			s64 d2 = hx_dist2(&det[j], trk);

			if (d2 > algo->track_dist2_max)
				continue;

			cand[cand_cnt].track_idx = i;
			cand[cand_cnt].det_idx   = j;
			cand[cand_cnt].dist2     = d2;
			cand_cnt++;
		}
	}

	/* Selection-sort candidates by distance (max 100 entries). */
	for (i = 0; i < cand_cnt; i++) {
		int best = i;

		for (j = i + 1; j < cand_cnt; j++) {
			if (cand[j].dist2 < cand[best].dist2 ||
			    (cand[j].dist2 == cand[best].dist2 &&
			     cand[j].track_idx < cand[best].track_idx) ||
			    (cand[j].dist2 == cand[best].dist2 &&
			     cand[j].track_idx == cand[best].track_idx &&
			     cand[j].det_idx < cand[best].det_idx))
				best = j;
		}
		if (best != i)
			swap(cand[i], cand[best]);
	}

	/* Greedy assignment: take the shortest-distance uncontested pair. */
	for (k = 0; k < cand_cnt; k++) {
		struct hx_match_candidate *m = &cand[k];
		struct hx_track *trk;

		if (track_matched[m->track_idx] || det_used[m->det_idx])
			continue;

		trk = &algo->tracks[m->track_idx];
		if (!trk->active)
			continue;

		/* Mild temporal smoothing to reduce jitter. */
		trk->vx = det[m->det_idx].x - trk->x;
		trk->vy = det[m->det_idx].y - trk->y;
		trk->x  = (trk->x * 3 + det[m->det_idx].x) / 4;
		trk->y  = (trk->y * 3 + det[m->det_idx].y) / 4;
		trk->missed = 0;
		if (trk->age < U8_MAX)
			trk->age++;
		if (trk->debounce > 0)
			trk->debounce--;

		track_matched[m->track_idx] = true;
		det_used[m->det_idx]        = true;
	}

	/* Age or release unmatched tracks. */
	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		struct hx_track *trk = &algo->tracks[i];

		if (!trk->active || track_matched[i])
			continue;

		/*
		 * Before the first stable touch is established, drop stray
		 * tracks immediately to prevent noise from being reported.
		 */
		if (!algo->touch_active) {
			hx_reset_track(trk);
			continue;
		}

		trk->missed++;
		if (trk->missed > algo->track_lost_frames)
			hx_reset_track(trk);
	}

	/* Create new slots for unmatched detections. */
	for (j = 0; j < det_cnt; j++) {
		struct hx_track *trk = NULL;

		if (det_used[j])
			continue;

		for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
			if (!algo->tracks[i].active) {
				trk = &algo->tracks[i];
				break;
			}
		}
		if (!trk)
			continue;

		trk->active   = true;
		trk->age      = 1;
		trk->missed   = 0;
		trk->debounce = algo->debounce_base;
		trk->x        = det[j].x;
		trk->y        = det[j].y;
	}
}

int hx_count_stable_tracks(struct hx_algo *algo)
{
	int i, cnt = 0;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo->tracks[i].active && algo->tracks[i].debounce == 0)
			cnt++;
	}
	return cnt;
}
