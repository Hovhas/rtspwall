/*
 * test_pacing.c — unit tests for pacing.c.
 *
 * No framework: simple assert macros that print the line and expression on
 * failure and exit with an error code. Built and run standalone by
 * `make test` with just a C compiler (no pkg-config), so it runs on any
 * development machine as well as on the Pi.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pacing.h"
#include "test.h"

/* ------------------------------------------------------------------ anchor */

static void test_anchor_sliding_minimum(void)
{
	struct pacing_anchor a;
	pacing_anchor_init(&a, 10 * 1000000);   /* 10 s window */

	pacing_anchor_push(&a, 0, 100);
	ASSERT_EQ_I(pacing_anchor_value(&a), 100);

	pacing_anchor_push(&a, 1000, 50);        /* new lower value */
	ASSERT_EQ_I(pacing_anchor_value(&a), 50);

	pacing_anchor_push(&a, 2000, 200);       /* higher — must NOT become the minimum */
	ASSERT_EQ_I(pacing_anchor_value(&a), 50);

	pacing_anchor_push(&a, 3000, 60);
	ASSERT_EQ_I(pacing_anchor_value(&a), 50);
}

static void test_anchor_window_expiry(void)
{
	struct pacing_anchor a;
	pacing_anchor_init(&a, 10 * 1000000);   /* 10 s */

	pacing_anchor_push(&a, 0, 10);            /* low value at t=0 */
	pacing_anchor_push(&a, 1 * 1000000, 500);
	ASSERT_EQ_I(pacing_anchor_value(&a), 10);

	/* 11 s later: the t=0 value must have fallen out of the window */
	pacing_anchor_push(&a, 11 * 1000000, 300);
	ASSERT_EQ_I(pacing_anchor_value(&a), 300);
}

static void test_anchor_empty_gives_zero(void)
{
	struct pacing_anchor a;
	pacing_anchor_init(&a, 10 * 1000000);
	ASSERT_EQ_I(pacing_anchor_value(&a), 0);
}

/* --------------------------------------------------------------------- PLL */

static void test_pll_first_frame_gives_raw_pts(void)
{
	struct pacing_pll p;
	pacing_pll_init(&p);
	ASSERT_EQ_I(pacing_pll_feed(&p, 12345), 12345);
	ASSERT_EQ_I(pacing_pll_mean_delta_us(&p), 0);   /* window still empty */
}

static void test_pll_before_full_window_gives_raw_pts(void)
{
	struct pacing_pll p;
	pacing_pll_init(&p);

	int64_t pts = 0;
	/* The first call is the "anchor" (no delta to measure yet). */
	int64_t r = pacing_pll_feed(&p, pts);
	ASSERT_EQ_I(r, pts);

	/* PACING_PLL_WINDOW deltas fill the window exactly — regulation has
	 * not kicked in during ANY of these calls (see pacing_pll_feed: the
	 * "filled < WINDOW" check uses the value BEFORE the increment, so even
	 * the call that fills the last slot goes through the raw-pts branch). */
	for (int i = 0; i < PACING_PLL_WINDOW; i++) {
		pts += 33333;
		r = pacing_pll_feed(&p, pts);
		ASSERT_EQ_I(r, pts);
	}
	ASSERT_EQ_I(pacing_pll_mean_delta_us(&p), 0);   /* T̂ is computed only on the NEXT call */

	pts += 33333;
	r = pacing_pll_feed(&p, pts);
	ASSERT_EQ_I(pacing_pll_mean_delta_us(&p), 33333);
	/* Even input all the way — regulation must change nothing. */
	ASSERT_EQ_I(r, pts);
}

static void test_pll_mean_delta_clamped(void)
{
	struct pacing_pll p;
	pacing_pll_init(&p);

	/* An absurdly large delta (e.g. after a gap that was not re-anchored
	 * away) must not make T̂ run off — clamped to 200 ms. One call for the
	 * anchor + WINDOW deltas to fill the window + ONE more to actually
	 * trigger the computation (see the test above for why). */
	int64_t pts = 0;
	for (int i = 0; i < PACING_PLL_WINDOW + 2; i++) {
		pts += 5 * 1000 * 1000;   /* 5 s "delta" */
		pacing_pll_feed(&p, pts);
	}
	ASSERT_EQ_I(pacing_pll_mean_delta_us(&p), 200000);
}

/* Simulates N source frames (delta pattern in deltas[], deltas[0] unused)
 * through a pacing_pll and into a pacing_fifo, evaluated by a vblank clock
 * with the given period and phase. Network latency is assumed ~0 (the frame
 * "arrives" at its own pts) — this tests cadence/frame selection in
 * isolation from the anchor/jitter buffer, which have their own tests.
 *
 * Only frames with source index >= measure_from_frame count towards the
 * result and gaps_out/n_gaps (earlier frames still run through PLL/FIFO to
 * build up the right state — this skips the PLL warm-up, before the T̂
 * window is full, in the measurement).
 *
 * gaps_out (if not NULL, at least n slots) gets one value per SHOWN frame
 * after measure_from_frame: the number of vblank ticks since the PREVIOUS
 * shown frame (whether or not the previous one was counted). *n_gaps is set
 * to the number of slots written. */
struct sim_result {
	int shown;
	int skipped;
	int dropped_full;
};

static struct sim_result simulate_pacing(const int64_t *deltas, int n,
					 int64_t vblank_period_us,
					 int64_t vblank_phase_us,
					 int measure_from_frame,
					 int *gaps_out, int *n_gaps)
{
	struct pacing_pll pll;
	pacing_pll_init(&pll);

	struct pacing_fifo fifo;
	pacing_fifo_init(&fifo);

	struct sim_result res = { 0 };
	if (n_gaps)
		*n_gaps = 0;

	int64_t pts_us = 0;
	int64_t last_vblank_us = vblank_phase_us - vblank_period_us;
	int tick = 0;
	int last_shown_tick = -1;

	for (int i = 0; i < n; i++) {
		if (i > 0)
			pts_us += deltas[i];
		int64_t r_us = pacing_pll_feed(&pll, pts_us);
		bool measure = i >= measure_from_frame;

		struct pacing_frame dropped;
		bool ok = pacing_fifo_push(&fifo, (struct pacing_frame){
			.index = i, .target_us = r_us, .arrival_us = pts_us }, &dropped);
		if (!ok && measure)
			res.dropped_full++;

		/* Run the vblank clock forward up to and including every tick
		 * that would already have happened when this frame arrived. */
		for (;;) {
			int64_t next = pacing_next_vblank(last_vblank_us,
							  vblank_period_us,
							  last_vblank_us);
			if (next > pts_us)
				break;
			last_vblank_us = next;
			tick++;

			struct pacing_frame chosen;
			struct pacing_frame skipped[PACING_FIFO_MAX];
			int n_skipped = 0;
			bool ripe = pacing_fifo_select(&fifo, next, &chosen,
						       skipped, &n_skipped);
			if (measure)
				res.skipped += n_skipped;
			if (ripe) {
				if (measure) {
					res.shown++;
					if (last_shown_tick >= 0 && gaps_out
					    && n_gaps && *n_gaps < n)
						gaps_out[(*n_gaps)++] =
							tick - last_shown_tick;
				}
				last_shown_tick = tick;
			}
		}
	}

	return res;
}

/* α=1/16 (see PACING_PLL_ALPHA_NUM/DEN in pacing.c) was chosen because it
 * gives a LARGE safety margin against collisions (0 drops, the critical
 * property) — not because it gives exactly 2 vblanks between EVERY shown
 * frame.
 *
 * Arithmetic (confirmed both analytically and with a standalone simulation):
 * for the 7/59.7 ms pair pattern the regulated timeline's spacing converges
 * to a STATIONARY oscillation between about 32.5 ms and 34.2 ms (straddling
 * 2x16.667 ms = 33.334 ms), however small α is chosen within 1/16–1/32 —
 * the oscillation shrinks only marginally with smaller α, it does not go
 * away, because the source's own pair pattern deviates by ±26 ms from the
 * nominal half period. A larger α (empirically around 0.7–0.9) would shrink
 * the oscillation below half a vblank period, but would AT THE SAME TIME
 * shrink the short pair gap (32.5 ms at α=1/16) down to a millisecond or so
 * above a whole vblank period — i.e. reintroduce exactly the collision risk
 * (drops) the PLL exists to eliminate, for a purely cosmetic gain (cadence
 * wobbling 1/2/3 vblanks instead of always exactly 2 — the average is still
 * exactly 2, and no frame is lost).
 *
 * The test below therefore measures what is actually the hard requirement:
 * 0 drops, the gap NEVER outside {1,2,3} (no uncontrolled oscillation), and
 * the average very close to 2.0 (the nominal cadence). */
static void test_pll_sim_30fps_pairs_zero_drops_cadence_around_2(void)
{
	const int PAIRS = 100;
	const int N = PAIRS * 2 + 1;
	int64_t deltas[201];
	deltas[0] = 0;
	for (int i = 0; i < PAIRS; i++) {
		deltas[1 + i * 2] = 7000;      /* short gap within the pair */
		deltas[2 + i * 2] = 59700;     /* long gap to the next pair */
	}

	int64_t phases[] = { 0, 5000, 11000 };
	for (size_t f = 0; f < sizeof phases / sizeof phases[0]; f++) {
		int gaps[400];
		int n_gaps = 0;
		struct sim_result res = simulate_pacing(
			deltas, N, 16667, phases[f],
			PACING_PLL_WINDOW + 8, gaps, &n_gaps);

		ASSERT_EQ_I(res.dropped_full, 0);
		/* The critical property: steady state gives zero drops —
		 * against 50 % without the PLL. */
		ASSERT_EQ_I(res.skipped, 0);
		ASSERT(n_gaps > 50);   /* plenty of measurement points left */

		long long sum = 0;
		for (int i = 0; i < n_gaps; i++) {
			ASSERT(gaps[i] >= 1 && gaps[i] <= 3);   /* never an uncontrolled oscillation */
			sum += gaps[i];
		}
		double mean = (double)sum / n_gaps;
		ASSERT(mean > 1.9 && mean < 2.1);   /* cadence centers on 2 */
	}
}

static void test_pll_sim_25fps_even_unchanged(void)
{
	const int N = 300;
	int64_t deltas[300];
	deltas[0] = 0;
	for (int i = 1; i < N; i++)
		deltas[i] = 40000;   /* even 25 fps, no clumping */

	int gaps[300];
	int n_gaps = 0;
	struct sim_result res = simulate_pacing(deltas, N, 16667, 0,
						PACING_PLL_WINDOW + 8, gaps, &n_gaps);

	ASSERT_EQ_I(res.skipped, 0);
	ASSERT_EQ_I(res.dropped_full, 0);
	/* 60/25 = 2.4 vblanks/frame on average — the pattern is necessarily a
	 * mix of 2s and 3s, never anything else, exactly as without the PLL
	 * (it must be a no-op on already even input). */
	ASSERT(n_gaps > 50);
	for (int i = 0; i < n_gaps; i++)
		ASSERT(gaps[i] == 2 || gaps[i] == 3);
}

static void test_pll_sim_24fps_pairs_zero_drops(void)
{
	const int PAIRS = 100;
	const int N = PAIRS * 2 + 1;
	int64_t deltas[201];
	deltas[0] = 0;
	for (int i = 0; i < PAIRS; i++) {
		deltas[1 + i * 2] = 7000;
		deltas[2 + i * 2] = 76300;   /* 7+76.3 ≈ 2x41.67 ms (24 fps) */
	}

	int gaps[400];
	int n_gaps = 0;
	struct sim_result res = simulate_pacing(deltas, N, 16667, 0,
						PACING_PLL_WINDOW + 8, gaps, &n_gaps);

	ASSERT_EQ_I(res.dropped_full, 0);
	ASSERT_EQ_I(res.skipped, 0);   /* 0 drops in steady state */
	ASSERT(n_gaps > 50);
	/* 60/24 = 2.5 — expected mix of 2s and 3s; see the rationale above the
	 * 30 fps test for why an occasional excursion outside that is normal
	 * and harmless, not a bug. */
	for (int i = 0; i < n_gaps; i++)
		ASSERT(gaps[i] >= 1 && gaps[i] <= 4);
}

static void test_pll_sim_one_missing_frame_no_extra_drops(void)
{
	const int N = 200;
	int64_t deltas[200];
	deltas[0] = 0;
	for (int i = 1; i < N; i++)
		deltas[i] = 40000;   /* even 25 fps ... */
	deltas[100] = 80000;         /* ... except one missing source frame (2T) in the middle */

	int gaps[300];
	int n_gaps = 0;
	struct sim_result res = simulate_pacing(deltas, N, 16667, 0,
						PACING_PLL_WINDOW + 8, gaps, &n_gaps);

	ASSERT_EQ_I(res.dropped_full, 0);
	/* A single gap gives at most an occasional extra skip while the window
	 * carries the gap (never a whole lost pattern) — not the kind of
	 * structural 50 % drop the pairs gave without the PLL. */
	ASSERT(res.skipped <= 1);
	for (int i = 0; i < n_gaps; i++)
		ASSERT(gaps[i] == 2 || gaps[i] == 3 || gaps[i] == 4);
}

static void test_pll_switch_30_to_24_fps_converges_without_drift(void)
{
	struct pacing_pll p;
	pacing_pll_init(&p);

	int64_t pts_us = 0;
	for (int i = 0; i < 300; i++) {
		pts_us += (i == 0) ? 0 : (i % 2 == 1 ? 7000 : 59700);
		int64_t r = pacing_pll_feed(&p, pts_us);
		int64_t d = r - pts_us;
		if (d < 0)
			d = -d;
		ASSERT(d < 100000);   /* never more than 100 ms from raw pts */
	}
	int64_t t_hat_30 = pacing_pll_mean_delta_us(&p);
	ASSERT(t_hat_30 > 25000 && t_hat_30 < 40000);   /* close to 30 fps (33 333) */

	/* switch to the 24 fps pattern mid-stream, without re-anchoring — the
	 * same continuous pts series, just a new source behaviour. */
	for (int i = 0; i < 300; i++) {
		pts_us += (i % 2 == 0) ? 7000 : 76300;
		int64_t r = pacing_pll_feed(&p, pts_us);
		int64_t d = r - pts_us;
		if (d < 0)
			d = -d;
		ASSERT(d < 150000);   /* bounded during the whole transition */
	}
	int64_t t_hat_24 = pacing_pll_mean_delta_us(&p);
	/* T̂ must have moved towards the 24 fps nominal (41 667) and away from
	 * the 30 fps nominal (33 333) — convergence, not a lingering old state. */
	ASSERT(t_hat_24 > 38000 && t_hat_24 < 46000);
}

static void test_pll_biased_drift_bounded_over_10000_frames(void)
{
	struct pacing_pll p;
	pacing_pll_init(&p);

	int64_t pts_us = 0;
	int64_t largest_deviation = 0;
	for (int i = 0; i < 10000; i++) {
		pts_us += (i == 0) ? 0 : (i % 2 == 1 ? 7000 : 59700);
		int64_t r = pacing_pll_feed(&p, pts_us);
		int64_t d = r - pts_us;
		if (d < 0)
			d = -d;
		if (d > largest_deviation)
			largest_deviation = d;
	}
	/* Bounded = independent of the number of frames (10 000 here), not
	 * growing with N. A pair gives a momentary error on the order of half a
	 * period (~33 ms); 200 ms gives plenty of margin without being so loose
	 * that a real unbounded drift (bug) would be missed. */
	ASSERT(largest_deviation < 200000);
}

/* ---------------------------------------------------------- jitter histogram */

static void test_histogram_percentiles(void)
{
	struct pacing_histogram h;
	pacing_hist_init(&h);

	/* 100 values: 0..98 ms in steps of 1 (99 values) plus one outlier. */
	for (int i = 0; i < 99; i++)
		pacing_hist_add(&h, i);
	pacing_hist_add(&h, 600);   /* lands in over */

	ASSERT_EQ_I(h.count, 100);
	ASSERT_EQ_I(h.over, 1);

	int p50 = pacing_hist_percentile(&h, 50);
	int p95 = pacing_hist_percentile(&h, 95);
	/* p50 around the middle (49), p95 near the top but below "over" */
	ASSERT(p50 >= 45 && p50 <= 54);
	ASSERT(p95 >= 90 && p95 <= 98);
}

static void test_histogram_empty(void)
{
	struct pacing_histogram h;
	pacing_hist_init(&h);
	ASSERT_EQ_I(pacing_hist_percentile(&h, 50), -1);
}

/* ------------------------------------------------------------------- FIFO */

static void test_fifo_select_none_ripe(void)
{
	struct pacing_fifo f;
	pacing_fifo_init(&f);
	pacing_fifo_push(&f, (struct pacing_frame){ .index = 1, .target_us = 1000, .arrival_us = 0 }, NULL);

	struct pacing_frame chosen;
	struct pacing_frame skipped[PACING_FIFO_MAX];
	int n_skipped = -1;
	bool ok = pacing_fifo_select(&f, 500 /* vblank before target_us */, &chosen, skipped, &n_skipped);
	ASSERT(!ok);
	ASSERT_EQ_I(n_skipped, 0);
	ASSERT_EQ_I(pacing_fifo_count(&f), 1);   /* the frame stays */
}

static void test_fifo_select_newest_skips_older(void)
{
	struct pacing_fifo f;
	pacing_fifo_init(&f);
	pacing_fifo_push(&f, (struct pacing_frame){ .index = 1, .target_us = 1000, .arrival_us = 100 }, NULL);
	pacing_fifo_push(&f, (struct pacing_frame){ .index = 2, .target_us = 1500, .arrival_us = 200 }, NULL);
	pacing_fifo_push(&f, (struct pacing_frame){ .index = 3, .target_us = 2000, .arrival_us = 300 }, NULL);
	/* frame 4 is not yet ripe at vblank 2500 */
	pacing_fifo_push(&f, (struct pacing_frame){ .index = 4, .target_us = 3000, .arrival_us = 400 }, NULL);

	struct pacing_frame chosen;
	struct pacing_frame skipped[PACING_FIFO_MAX];
	int n_skipped = -1;
	bool ok = pacing_fifo_select(&f, 2500, &chosen, skipped, &n_skipped);
	ASSERT(ok);
	ASSERT_EQ_I(chosen.index, 3);            /* newest ripe */
	ASSERT_EQ_I(n_skipped, 2);               /* frames 1 and 2 were skipped */
	ASSERT_EQ_I(skipped[0].index, 1);        /* and their indices can be returned */
	ASSERT_EQ_I(skipped[1].index, 2);
	ASSERT_EQ_I(pacing_fifo_count(&f), 1);   /* only frame 4 left */
}

static void test_fifo_push_full_drops_oldest(void)
{
	struct pacing_fifo f;
	pacing_fifo_init(&f);
	for (int i = 0; i < PACING_FIFO_MAX + 5; i++) {
		struct pacing_frame dropped = { .index = -1 };
		bool ok = pacing_fifo_push(&f,
			(struct pacing_frame){ .index = i, .target_us = i * 1000, .arrival_us = 0 },
			&dropped);
		if (i >= PACING_FIFO_MAX - 1) {
			ASSERT(!ok);
			/* The caller MUST learn which index was dropped — otherwise
			 * the right CAPTURE buffer cannot be handed back to the
			 * decoder, and a buffer leaks. */
			ASSERT_EQ_I(dropped.index, i - (PACING_FIFO_MAX - 1));
		}
	}
	ASSERT(pacing_fifo_count(&f) <= PACING_FIFO_MAX - 1);
}

/* ------------------------------------------------------------------ vblank */

static void test_vblank_period(void)
{
	/* 1920x1080@60: classic CEA mode, clock 148500 kHz, htotal 2200, vtotal 1125 */
	int64_t period = pacing_vblank_period_us(148500, 2200, 1125);
	/* 1e6/60 ≈ 16667 µs */
	ASSERT(period >= 16600 && period <= 16700);
}

static void test_vblank_period_invalid(void)
{
	ASSERT_EQ_I(pacing_vblank_period_us(0, 2200, 1125), 0);
	ASSERT_EQ_I(pacing_vblank_period_us(148500, 0, 1125), 0);
}

static void test_next_vblank_always_strictly_after_now(void)
{
	int64_t period = 16667;
	int64_t last = 100000;

	ASSERT_EQ_I(pacing_next_vblank(last, period, last), last + period);
	ASSERT_EQ_I(pacing_next_vblank(last, period, last + 1), last + period);
	ASSERT_EQ_I(pacing_next_vblank(last, period, last + period), last + 2 * period);
	ASSERT_EQ_I(pacing_next_vblank(last, period, last + period + 1), last + 2 * period);
	/* halfway into a period */
	int64_t now = last + 2 * period + period / 2;
	int64_t next = pacing_next_vblank(last, period, now);
	ASSERT(next > now);
	ASSERT_EQ_I(next, last + 3 * period);
}

/* ------------------------------------------------------------- target time */

static void test_target_time_arithmetic(void)
{
	int64_t target = pacing_target_time(1000000, 20000, 120, 80);
	/* 1 000 000 + 20 000 + 120 000 + 80 000 */
	ASSERT_EQ_I(target, 1220000);
}

/* ---------------------------------------------------------------- rotation */

static void test_build_groups_two_groups_plus_two_fixed(void)
{
	/* Config order: cam1(0), cam2(1), cam3(2), cam4(3), cam5(4), cam6(5) —
	 * cam5 shares a tile with cam3, cam6 with cam4. */
	struct pacing_tile tiles[6] = {
		{ 960, 540, 0, 0 },     /* 0 cam1 */
		{ 960, 540, 960, 0 },   /* 1 cam2 */
		{ 960, 540, 0, 540 },   /* 2 cam3 */
		{ 960, 540, 960, 540 }, /* 3 cam4 */
		{ 960, 540, 0, 540 },   /* 4 cam5 — same tile as cam3 */
		{ 960, 540, 960, 540 }, /* 5 cam6 — same tile as cam4 */
	};
	struct pacing_group groups[8];
	int n = pacing_build_groups(tiles, 6, groups, 8);
	ASSERT_EQ_I(n, 4);

	ASSERT_EQ_I(groups[0].count, 1);
	ASSERT_EQ_I(groups[0].index[0], 0);   /* cam1 — fixed */

	ASSERT_EQ_I(groups[1].count, 1);
	ASSERT_EQ_I(groups[1].index[0], 1);   /* cam2 — fixed */

	ASSERT_EQ_I(groups[2].count, 2);
	ASSERT_EQ_I(groups[2].index[0], 2);   /* cam3 FIRST (config order) */
	ASSERT_EQ_I(groups[2].index[1], 4);   /* cam5 */

	ASSERT_EQ_I(groups[3].count, 2);
	ASSERT_EQ_I(groups[3].index[0], 3);   /* cam4 FIRST */
	ASSERT_EQ_I(groups[3].index[1], 5);   /* cam6 */
}

static void test_build_groups_four_unique_gives_four_singletons(void)
{
	/* No rotation groups — every camera its own group. */
	struct pacing_tile tiles[4] = {
		{ 960, 540, 0, 0 }, { 960, 540, 960, 0 },
		{ 960, 540, 0, 540 }, { 960, 540, 960, 540 },
	};
	struct pacing_group groups[8];
	int n = pacing_build_groups(tiles, 4, groups, 8);
	ASSERT_EQ_I(n, 4);
	for (int i = 0; i < 4; i++) {
		ASSERT_EQ_I(groups[i].count, 1);
		ASSERT_EQ_I(groups[i].index[0], i);
	}
}

static void test_rotation_switches_after_rotate_s(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	bool healthy[2] = { true, true };

	int active = pacing_rotation_update(&r, healthy, 2, 15, false, 0, NULL);
	ASSERT_EQ_I(active, 0);   /* no time has passed yet */

	active = pacing_rotation_update(&r, healthy, 2, 15, false, 14 * 1000000, NULL);
	ASSERT_EQ_I(active, 0);   /* just before 15 s */

	active = pacing_rotation_update(&r, healthy, 2, 15, false, 15 * 1000000, NULL);
	ASSERT_EQ_I(active, 1);   /* at 15 s the candidate is healthy — switch the same vblank */
}

static void test_rotation_skips_after_3s_unhealthy(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	bool healthy[2] = { true, false };   /* member 1 never healthy in this test */
	bool skipped = false;

	int active = pacing_rotation_update(&r, healthy, 2, 15, false, 15 * 1000000, &skipped);
	ASSERT_EQ_I(active, 0);
	ASSERT(!skipped);

	active = pacing_rotation_update(&r, healthy, 2, 15, false, 15 * 1000000 + 2900000, &skipped);
	ASSERT_EQ_I(active, 0);   /* 2.9 s into the wait — still waiting */
	ASSERT(!skipped);

	active = pacing_rotation_update(&r, healthy, 2, 15, false, 15 * 1000000 + 3100000, &skipped);
	ASSERT_EQ_I(active, 0);   /* > 3 s — give up this round */
	ASSERT(skipped);

	active = pacing_rotation_update(&r, healthy, 2, 15, false,
					15 * 1000000 + 3100000 + 14 * 1000000, &skipped);
	ASSERT_EQ_I(active, 0);   /* next attempt no earlier than rotate_s later */
	ASSERT(!skipped);
}

static void test_rotation_stays_if_no_other_healthy(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	bool healthy[2] = { true, false };
	int64_t now = 0;
	bool skipped;

	for (int cycle = 0; cycle < 3; cycle++) {
		now += 15 * 1000000;
		int active = pacing_rotation_update(&r, healthy, 2, 15, false, now, &skipped);
		ASSERT_EQ_I(active, 0);

		now += 3 * 1000000 + 1000;
		active = pacing_rotation_update(&r, healthy, 2, 15, false, now, &skipped);
		ASSERT_EQ_I(active, 0);
		ASSERT(skipped);
	}
}

static void test_rotation_single_member_never_rotates(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	bool healthy[1] = { true };

	for (int i = 0; i < 5; i++) {
		int64_t now = (int64_t)i * 20 * 1000000;
		int active = pacing_rotation_update(&r, healthy, 1, 15, false, now, NULL);
		ASSERT_EQ_I(active, 0);
	}
}

static void test_rotation_forced_switches_at_once_if_healthy(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	bool healthy[2] = { true, true };

	/* long before rotate_s — but forced (the active member is being torn down) */
	int active = pacing_rotation_update(&r, healthy, 2, 15, true, 2 * 1000000, NULL);
	ASSERT_EQ_I(active, 1);
}

static void test_rotation_forced_waits_without_3s_grace(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	bool healthy[2] = { true, false };
	bool skipped = false;

	int active = pacing_rotation_update(&r, healthy, 2, 15, true, 1 * 1000000, &skipped);
	ASSERT_EQ_I(active, 0);
	ASSERT(!skipped);

	/* well over 3 s — NO skipping when forced, there is no better candidate */
	active = pacing_rotation_update(&r, healthy, 2, 15, true, 10 * 1000000, &skipped);
	ASSERT_EQ_I(active, 0);
	ASSERT(!skipped);

	healthy[1] = true;
	active = pacing_rotation_update(&r, healthy, 2, 15, true, 10 * 1000000 + 1, &skipped);
	ASSERT_EQ_I(active, 1);   /* healthy now — switch at once */
}

/* Regression: in a group of 3+ where the candidate (active+1) is permanently
 * dead, rotation must still reach the next healthy member instead of falling
 * back to the exact same dead neighbour over and over (not forced), or
 * waiting on the same candidate forever (forced). See
 * pacing_rotation_update. */
static void test_rotation_steps_past_dead_neighbour_not_forced(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	/* index 0 = active (starts), index 1 = permanently dead neighbour,
	 * index 2 = healthy member rotation should reach. */
	bool healthy[3] = { true, false, true };
	bool skipped = false;

	/* rotate_s (15 s) passes — the candidate becomes (active+1)%3 = 1, dead. */
	int active = pacing_rotation_update(&r, healthy, 3, 15, false, 15 * 1000000, &skipped);
	ASSERT_EQ_I(active, 0);
	ASSERT(!skipped);

	/* > 3 s grace on candidate 1 — must step on to candidate 2, NOT give
	 * up the whole round (candidate 2 is untried and healthy). */
	active = pacing_rotation_update(&r, healthy, 3, 15, false,
					15 * 1000000 + 3100000, &skipped);
	ASSERT_EQ_I(active, 0);
	ASSERT(!skipped);   /* the round is still on — next candidate pending */

	/* The next call sees that candidate 2 is healthy — switch to it. */
	active = pacing_rotation_update(&r, healthy, 3, 15, false,
					15 * 1000000 + 3100000 + 1, &skipped);
	ASSERT_EQ_I(active, 2);   /* reached the next healthy member */
}

static void test_rotation_forced_steps_past_dead_neighbour(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	/* index 0 = active (torn down, forced=true), index 1 = permanently dead
	 * neighbour, index 2 = healthy member. */
	bool healthy[3] = { true, false, true };
	bool skipped = false;

	/* Forced — the candidate becomes 1 (dead) at once. Without stepping,
	 * rotation would wait on candidate 1 forever (black tile). */
	int active = pacing_rotation_update(&r, healthy, 3, 15, true, 1 * 1000000, &skipped);
	ASSERT_EQ_I(active, 0);
	ASSERT(!skipped);

	/* The next call (still forced) must have stepped the candidate on to
	 * 2, which is healthy — switch to it. */
	active = pacing_rotation_update(&r, healthy, 3, 15, true, 1 * 1000000 + 1, &skipped);
	ASSERT_EQ_I(active, 2);
}

/* QA addition: the group size limit was raised from 8 to 16 together with
 * the camera limit — 16 cameras on ONE tile must form ONE group (the
 * original only ever covered 2-member groups). */
static void test_build_groups_sixteen_on_one_tile(void)
{
	struct pacing_tile tiles[PACING_MAX_GROUP_SIZE + 1];
	for (int i = 0; i < PACING_MAX_GROUP_SIZE + 1; i++)
		tiles[i] = (struct pacing_tile){ 640, 360, 0, 0 };

	struct pacing_group groups[PACING_MAX_GROUP_SIZE + 1];
	int n = pacing_build_groups(tiles, PACING_MAX_GROUP_SIZE, groups,
				    PACING_MAX_GROUP_SIZE + 1);
	ASSERT_EQ_I(n, 1);
	ASSERT_EQ_I(groups[0].count, PACING_MAX_GROUP_SIZE);
	for (int i = 0; i < PACING_MAX_GROUP_SIZE; i++)
		ASSERT_EQ_I(groups[0].index[i], i);

	/* one more than fits: the overflow member becomes its own group
	 * rather than being written outside index[] */
	n = pacing_build_groups(tiles, PACING_MAX_GROUP_SIZE + 1, groups,
				PACING_MAX_GROUP_SIZE + 1);
	ASSERT_EQ_I(n, 2);
	ASSERT_EQ_I(groups[1].count, 1);
	ASSERT_EQ_I(groups[1].index[0], PACING_MAX_GROUP_SIZE);

	/* max_groups smaller than the number of groups: count is still
	 * returned, nothing written past max_groups */
	struct pacing_tile distinct[4] = {
		{ 1, 1, 0, 0 }, { 1, 1, 1, 0 }, { 1, 1, 2, 0 }, { 1, 1, 3, 0 },
	};
	struct pacing_group two[3];
	two[2].count = -7;
	ASSERT_EQ_I(pacing_build_groups(distinct, 4, two, 2), 4);
	ASSERT_EQ_I(two[2].count, -7);
}

/* QA addition: a full lap through a 16-member group where only the last
 * member is healthy — forced rotation must reach it within one lap. */
static void test_rotation_forced_reaches_last_of_sixteen(void)
{
	struct pacing_rotation r;
	pacing_rotation_init(&r, 0);
	bool healthy[16] = { false };
	healthy[15] = true;

	int active = 0;
	for (int step = 0; step < 16 && active == 0; step++)
		active = pacing_rotation_update(&r, healthy, 16, 15, true, 1000 + step, NULL);
	ASSERT_EQ_I(active, 15);
}

/* QA addition: the anchor must survive a queue that is full of strictly
 * increasing values (the "cannot happen in practice" overflow branch). */
static void test_anchor_full_queue_does_not_overflow(void)
{
	struct pacing_anchor a;
	pacing_anchor_init(&a, INT64_C(1) << 40);   /* nothing ever expires */
	for (int i = 0; i < PACING_ANCHOR_MAX * 3; i++)
		pacing_anchor_push(&a, i, i);         /* strictly increasing: all kept */
	/* The oldest were dropped to make room — the minimum is then the
	 * oldest value still held, never garbage. */
	int64_t v = pacing_anchor_value(&a);
	ASSERT(v > 0 && v < PACING_ANCHOR_MAX * 3);
}

int main(void)
{
	test_anchor_sliding_minimum();
	test_anchor_window_expiry();
	test_anchor_empty_gives_zero();

	test_pll_first_frame_gives_raw_pts();
	test_pll_before_full_window_gives_raw_pts();
	test_pll_mean_delta_clamped();
	test_pll_sim_30fps_pairs_zero_drops_cadence_around_2();
	test_pll_sim_25fps_even_unchanged();
	test_pll_sim_24fps_pairs_zero_drops();
	test_pll_sim_one_missing_frame_no_extra_drops();
	test_pll_switch_30_to_24_fps_converges_without_drift();
	test_pll_biased_drift_bounded_over_10000_frames();

	test_histogram_percentiles();
	test_histogram_empty();

	test_fifo_select_none_ripe();
	test_fifo_select_newest_skips_older();
	test_fifo_push_full_drops_oldest();

	test_vblank_period();
	test_vblank_period_invalid();
	test_next_vblank_always_strictly_after_now();

	test_target_time_arithmetic();

	test_build_groups_two_groups_plus_two_fixed();
	test_build_groups_four_unique_gives_four_singletons();
	test_rotation_switches_after_rotate_s();
	test_rotation_skips_after_3s_unhealthy();
	test_rotation_stays_if_no_other_healthy();
	test_rotation_single_member_never_rotates();
	test_rotation_forced_switches_at_once_if_healthy();
	test_rotation_forced_waits_without_3s_grace();
	test_rotation_steps_past_dead_neighbour_not_forced();
	test_rotation_forced_steps_past_dead_neighbour();

	/* QA additions */
	test_build_groups_sixteen_on_one_tile();
	test_rotation_forced_reaches_last_of_sixteen();
	test_anchor_full_queue_does_not_overflow();

	return test_summary("test_pacing");
}
