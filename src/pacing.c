/*
 * pacing.c — implementation of the pure pacing logic. See pacing.h for the
 * intent behind each part. No Linux/DRM/FFmpeg headers, only the standard
 * library.
 */
#include "pacing.h"

#include <string.h>

/* ------------------------------------------------------------------ anchor */

void pacing_anchor_init(struct pacing_anchor *a, int64_t window_us)
{
	a->window_us = window_us;
	a->head = a->tail = 0;
}

void pacing_anchor_push(struct pacing_anchor *a, int64_t now_us, int64_t value)
{
	/* Drop what has fallen out of the window at the front. */
	while (a->head != a->tail
	       && a->queue[a->head].time_us < now_us - a->window_us)
		a->head = (a->head + 1) % PACING_ANCHOR_MAX;

	/* Keep the queue monotonically ascending from the back: anything >= the
	 * new, fresher value can never become the minimum before it, so it can
	 * be removed. */
	while (a->head != a->tail) {
		int last = (a->tail - 1 + PACING_ANCHOR_MAX) % PACING_ANCHOR_MAX;
		if (a->queue[last].value >= value)
			a->tail = last;
		else
			break;
	}

	int n = (a->tail + 1) % PACING_ANCHOR_MAX;
	if (n == a->head)
		/* Full — cannot happen in practice (the window holds far fewer
		 * distinct minimum candidates than PACING_ANCHOR_MAX), but drop
		 * the oldest rather than write outside the array. */
		a->head = (a->head + 1) % PACING_ANCHOR_MAX;

	a->queue[a->tail].time_us = now_us;
	a->queue[a->tail].value = value;
	a->tail = n;
}

int64_t pacing_anchor_value(const struct pacing_anchor *a)
{
	if (a->head == a->tail)
		return 0;
	return a->queue[a->head].value;
}

/* --------------------------------------------------------------------- PLL */

#define PACING_PLL_T_MIN_US   (10 * 1000)
#define PACING_PLL_T_MAX_US   (200 * 1000)
#define PACING_PLL_ALPHA_NUM  1
#define PACING_PLL_ALPHA_DEN  16   /* α = 1/16, see the rationale in pacing.h */

void pacing_pll_init(struct pacing_pll *p)
{
	memset(p, 0, sizeof *p);
	p->prev_pts_us = -1;
}

int64_t pacing_pll_feed(struct pacing_pll *p, int64_t pts_us)
{
	if (p->prev_pts_us < 0) {
		/* First frame in this series — no delta to measure yet. */
		p->prev_pts_us = pts_us;
		p->r_us = pts_us;
		return p->r_us;
	}

	int64_t delta_us = pts_us - p->prev_pts_us;
	p->prev_pts_us = pts_us;

	if (p->filled < PACING_PLL_WINDOW) {
		/* Window still filling — pass raw pts through, but keep r_us in
		 * phase so the transition to regulated mode is seamless. */
		p->window_us[p->filled] = delta_us;
		p->sum_us += delta_us;
		p->filled++;
		p->next_slot = p->filled % PACING_PLL_WINDOW;
		p->r_us = pts_us;
		return p->r_us;
	}

	/* Window full — ring buffer: replace the oldest delta, keep the sum up
	 * to date without re-summing the whole window every time. */
	p->sum_us -= p->window_us[p->next_slot];
	p->window_us[p->next_slot] = delta_us;
	p->sum_us += delta_us;
	p->next_slot = (p->next_slot + 1) % PACING_PLL_WINDOW;

	int64_t mean_us = p->sum_us / PACING_PLL_WINDOW;
	if (mean_us < PACING_PLL_T_MIN_US)
		mean_us = PACING_PLL_T_MIN_US;
	else if (mean_us > PACING_PLL_T_MAX_US)
		mean_us = PACING_PLL_T_MAX_US;
	p->mean_delta_us = mean_us;

	int64_t expected_us = p->r_us + mean_us;
	int64_t deviation_us = pts_us - expected_us;
	p->r_us = expected_us + deviation_us * PACING_PLL_ALPHA_NUM / PACING_PLL_ALPHA_DEN;
	return p->r_us;
}

int64_t pacing_pll_mean_delta_us(const struct pacing_pll *p)
{
	return p->mean_delta_us;
}

/* ---------------------------------------------------------- jitter histogram */

void pacing_hist_init(struct pacing_histogram *h)
{
	memset(h, 0, sizeof *h);
}

void pacing_hist_add(struct pacing_histogram *h, int64_t value_ms)
{
	if (value_ms < 0)
		value_ms = 0;
	if (value_ms >= PACING_HIST_BINS)
		h->over++;
	else
		h->bins[value_ms]++;
	h->count++;
}

int pacing_hist_percentile(const struct pacing_histogram *h, int percentile)
{
	if (!h->count)
		return -1;

	uint64_t target = ((uint64_t)h->count * (uint64_t)percentile + 99) / 100;
	if (target < 1)
		target = 1;

	uint64_t acc = 0;
	for (int i = 0; i < PACING_HIST_BINS; i++) {
		acc += h->bins[i];
		if (acc >= target)
			return i;
	}
	return PACING_HIST_BINS;   /* landed in "over" */
}

/* ------------------------------------------------------- frame selection (FIFO) */

void pacing_fifo_init(struct pacing_fifo *f)
{
	f->head = f->tail = 0;
}

int pacing_fifo_count(const struct pacing_fifo *f)
{
	return (f->tail - f->head + PACING_FIFO_MAX) % PACING_FIFO_MAX;
}

static struct pacing_frame pacing_fifo_pop(struct pacing_fifo *f)
{
	struct pacing_frame fr = f->frame[f->head];
	f->head = (f->head + 1) % PACING_FIFO_MAX;
	return fr;
}

bool pacing_fifo_push(struct pacing_fifo *f, struct pacing_frame fr,
		      struct pacing_frame *dropped)
{
	bool full = pacing_fifo_count(f) >= PACING_FIFO_MAX - 1;

	if (full) {
		struct pacing_frame old = pacing_fifo_pop(f);   /* drop the oldest */
		if (dropped)
			*dropped = old;
	}
	f->frame[f->tail] = fr;
	f->tail = (f->tail + 1) % PACING_FIFO_MAX;
	return !full;
}

bool pacing_fifo_select(struct pacing_fifo *f, int64_t next_vblank_us,
			struct pacing_frame *chosen,
			struct pacing_frame *skipped, int *n_skipped)
{
	int ripe = -1;   /* offset (0-based from head) of the latest ripe frame */
	int count = pacing_fifo_count(f);

	if (n_skipped)
		*n_skipped = 0;

	/* The FIFO is in target_us order (pts is monotonic within a
	 * connection), so it is enough to walk forward until the first unripe
	 * frame. */
	for (int k = 0; k < count; k++) {
		int idx = (f->head + k) % PACING_FIFO_MAX;
		if (f->frame[idx].target_us <= next_vblank_us)
			ripe = k;
		else
			break;
	}

	if (ripe < 0)
		return false;

	for (int k = 0; k < ripe; k++) {
		struct pacing_frame fr = pacing_fifo_pop(f);
		if (skipped)
			skipped[k] = fr;
		if (n_skipped)
			(*n_skipped)++;
	}
	*chosen = pacing_fifo_pop(f);
	return true;
}

/* ------------------------------------------------------------------ vblank */

int64_t pacing_vblank_period_us(uint32_t clock_khz, uint32_t htotal, uint32_t vtotal)
{
	if (!clock_khz || !htotal || !vtotal)
		return 0;
	/* refresh_hz = clock_khz*1000 / (htotal*vtotal); period_us = 1e6/refresh_hz
	 * = htotal*vtotal*1000 / clock_khz. int64 so htotal*vtotal cannot
	 * overflow (typically < 3 million, plenty of margin). */
	return (int64_t)htotal * (int64_t)vtotal * 1000 / (int64_t)clock_khz;
}

/* Floor division (towards minus infinity) — C's built-in / truncates towards
 * zero, which gives the wrong answer for a negative diff. We want
 * ceil(diff/period) + 1 steps forward, and the safest way is floor division
 * plus one. */
static int64_t floor_div(int64_t a, int64_t b)
{
	int64_t q = a / b;
	int64_t r = a % b;
	if (r != 0 && ((r < 0) != (b < 0)))
		q--;
	return q;
}

int64_t pacing_next_vblank(int64_t last_vblank_us, int64_t period_us, int64_t now_us)
{
	if (period_us <= 0)
		return now_us;
	int64_t diff = now_us - last_vblank_us;
	int64_t steps = floor_div(diff, period_us) + 1;
	return last_vblank_us + steps * period_us;
}

/* ------------------------------------------------------------- target time */

int64_t pacing_target_time(int64_t pts_us, int64_t anchor_us, int buffer_ms, int delay_ms)
{
	return pts_us + anchor_us + (int64_t)buffer_ms * 1000 + (int64_t)delay_ms * 1000;
}

/* ---------------------------------------------------------------- rotation */

int pacing_build_groups(const struct pacing_tile *tiles, int count,
			struct pacing_group *groups, int max_groups)
{
	bool used[64] = { false };
	int n_groups = 0;

	if (count > 64)
		count = 64;   /* safety net — far above MAX_CAMERAS */
	if (count < 0)
		count = 0;

	for (int i = 0; i < count; i++) {
		if (used[i])
			continue;

		struct pacing_group g = { .count = 0 };
		g.index[g.count++] = i;
		used[i] = true;

		for (int j = i + 1; j < count && g.count < PACING_MAX_GROUP_SIZE; j++) {
			if (used[j])
				continue;
			if (tiles[j].width == tiles[i].width && tiles[j].height == tiles[i].height
			    && tiles[j].x == tiles[i].x && tiles[j].y == tiles[i].y) {
				g.index[g.count++] = j;
				used[j] = true;
			}
		}

		if (n_groups < max_groups)
			groups[n_groups] = g;
		n_groups++;
	}
	return n_groups;
}

void pacing_rotation_init(struct pacing_rotation *r, int64_t now_us)
{
	r->active = 0;
	r->switched_us = now_us;
	r->candidate = -1;
	r->candidate_since_us = 0;
}

#define PACING_ROTATION_GRACE_US (3 * 1000000)

int pacing_rotation_update(struct pacing_rotation *r, const bool *healthy, int count,
			   int rotate_s, bool forced, int64_t now_us,
			   bool *skipped)
{
	if (skipped)
		*skipped = false;

	if (count <= 1) {
		r->active = 0;
		r->candidate = -1;
		return r->active;
	}

	bool due = forced || (now_us - r->switched_us >= (int64_t)rotate_s * 1000000);

	if (due && r->candidate < 0) {
		r->candidate = (r->active + 1) % count;
		r->candidate_since_us = now_us;
	}

	if (r->candidate >= 0) {
		if (healthy[r->candidate]) {
			r->active = r->candidate;
			r->switched_us = now_us;
			r->candidate = -1;
		} else if (forced) {
			/* The active member MUST go NOW. Waiting on this one
			 * candidate forever would leave the tile black for good as
			 * soon as the candidate happens to be a dead neighbour in a
			 * group with 3+ members — step straight on to the next
			 * member in turn instead. If a full lap finds no healthy
			 * member (all dead) there is nothing better to do than
			 * keep waiting on the last tried candidate until the next
			 * vblank. */
			int next = (r->candidate + 1) % count;
			if (next != r->active) {
				r->candidate = next;
				r->candidate_since_us = now_us;
			}
		} else if (now_us - r->candidate_since_us > PACING_ROTATION_GRACE_US) {
			/* No healthy candidate within 3 s — step on to the next
			 * member in turn instead of falling back to the exact same
			 * (dead) neighbour, which in groups with 3+ members would
			 * never reach the next healthy member. Only once a full lap
			 * finds nothing (next would be the active member itself) is
			 * the round abandoned: the next attempt becomes due no
			 * earlier than rotate_s later. */
			int next = (r->candidate + 1) % count;
			if (next == r->active) {
				r->candidate = -1;
				r->switched_us = now_us;
				if (skipped)
					*skipped = true;
			} else {
				r->candidate = next;
				r->candidate_since_us = now_us;
			}
		}
	}
	return r->active;
}
