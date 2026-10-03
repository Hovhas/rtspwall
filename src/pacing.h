/*
 * pacing.h — pure logic for smooth frame pacing and rotation.
 *
 * Everything here is self-contained on purpose: no Linux, DRM or FFmpeg
 * headers. That makes the module buildable and unit-testable on any machine
 * (see test_pacing.c and `make test`) without Raspberry Pi hardware, or even
 * Linux.
 *
 * The module collects logic that would otherwise be untested code in the
 * middle of the hardware-facing files:
 *   - the anchor: a sliding minimum of network jitter, used to compute a
 *     target display time for each frame
 *   - the PLL: regulates a clumped/paired pts series (a recorder that stamps
 *     two frames close together) into an even timeline, see pacing_pll_feed
 *   - a small FIFO that picks which ripe frame to show at the next vblank,
 *     and counts how many older frames were skipped
 *   - a jitter histogram for the per-minute statistics line
 *   - vblank arithmetic (period and next vblank from a DRM mode's raw
 *     numbers, passed as plain integers so the DRM header is not needed)
 *   - rotation groups: cameras sharing a tile, and the decision of which
 *     member is visible
 */
#ifndef PACING_H
#define PACING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ anchor */

#define PACING_ANCHOR_MAX 512

/* Sliding minimum of (arrival_us - pts_us) over a time window. Monotonic
 * deque: the queue is kept ascending in value, so the minimum is always at
 * the head. */
struct pacing_anchor {
	int64_t window_us;
	struct {
		int64_t time_us;
		int64_t value;
	} queue[PACING_ANCHOR_MAX];
	int head, tail;   /* circular, [head, tail) occupied */
};

void pacing_anchor_init(struct pacing_anchor *a, int64_t window_us);

/* Add a new value at time now_us. The caller is responsible for resetting
 * the anchor (calling pacing_anchor_init again) on pts jumps and reconnects
 * — this function knows nothing about pts continuity.
 *
 * IMPORTANT: the caller must also NEVER feed a synthetic or unknown
 * timestamp here (e.g. monotonic_us() used as a pts substitute because a
 * pairing queue was empty). Such a sample can become the new minimum jitter
 * and ruin the sliding minimum for the whole window (10 s) — this function
 * has no way to recognise such a sample itself; that is the caller's job
 * (see collect_decoded in camera_thread.c). */
void pacing_anchor_push(struct pacing_anchor *a, int64_t now_us, int64_t value);

/* Current sliding minimum. 0 if the anchor is empty. */
int64_t pacing_anchor_value(const struct pacing_anchor *a);

/* --------------------------------------------------------------------- PLL */

/* Why this exists: a recorder/NVR can stamp two frames close together (for
 * example 7 ms apart) and then jump further (for example 59 ms) to the next
 * pair — the source FRAME RATE is even, only the TIMESTAMPS are clumped. A
 * target time built directly on such raw pts makes both frames of a pair
 * ripe at the same vblank, and frame selection (pacing_fifo_select) drops
 * the older one — up to 50 % dropped frames on a 30 fps stream even though
 * the source never drops a single frame.
 *
 * pacing_pll removes exactly that: a regulated timeline r_n that follows
 * the source's NOMINAL rate (T̂, a sliding mean) instead of jumping with
 * every individual pts deviation:
 *
 *     r_n = r_{n-1} + T̂ + α·(pts_n − (r_{n-1} + T̂))
 *
 * With a small α (see PACING_PLL_ALPHA_NUM/DEN) the error can never drift
 * away unbounded — it is bounded by (maximum one-off deviation) x α. This
 * is DELIBERATELY free of per-frame gap detection (e.g. "if delta >
 * 1.5xT̂, assume a jump and handle it differently"): the pair pattern above
 * already produces an error of more than half a nominal period (7 ms
 * against a 33 ms period), so such detection would fire falsely on exactly
 * the pattern it is meant to fix. α provides the smoothing instead, without
 * knowing which frames "belong together".
 *
 * The anchor (sliding minimum of arrival−pts, above) does NOT touch r_n —
 * it keeps working on raw pts.
 *
 * ABOUT α: a small α (1/16–1/32) does NOT give an exactly constant vblank
 * spacing between EVERY frame — for the 7/59.7 ms pair pattern the
 * regulated timeline's spacing converges to a stationary oscillation of
 * ±~1 ms around 2x the vblank period (confirmed analytically and by
 * simulation, see test_pll_sim_30fps_pairs_... in test_pacing.c), because
 * the source's own pair pattern already deviates by more than half a
 * period. A larger α would shrink that oscillation, but would AT THE SAME
 * TIME shrink the safety margin against two frames landing in the same
 * vblank (the original bug) from plenty of margin down to a millisecond or
 * so — trading a cosmetic cadence wobble (1/2/3 vblanks instead of always
 * exactly 2, average still exactly 2, NO frame dropped) for renewed risk of
 * the structural drop bug α exists to solve. A small α is chosen on purpose
 * for LARGE margin against drops, not for minimal cadence wobble.
 *
 * ABOUT BIAS AMPLIFICATION: "the error is bounded" above applies to ONE-OFF
 * deviations (a single pts_n far from r_{n-1}+T̂), which die out
 * geometrically by a factor (1−α) per frame. A PERSISTENT bias b in
 * (pts_n − (r_{n-1}+T̂)) — e.g. if an independent error source
 * systematically pushed pts_n the same way frame after frame, not just the
 * known pair clumping — instead converges to a STATIONARY offset of
 * b·(1−α)/α. With α=1/16, (1−α)/α = 15: a small but SYSTEMATIC bias is
 * amplified 15x in the regulated timeline. This is not a problem for the
 * known pair pattern (which is symmetric around zero, not a bias), but is
 * worth remembering if the PLL is ever fed a new kind of deviating source. */

#define PACING_PLL_WINDOW 32       /* frames in the sliding T̂ window */

struct pacing_pll {
	int64_t window_us[PACING_PLL_WINDOW];
	int     filled;             /* number of filled slots, up to WINDOW */
	int     next_slot;          /* circular write index once the window is full */
	int64_t sum_us;             /* window sum — for a cheap mean */
	int64_t mean_delta_us;      /* latest (sanity-clamped) T̂, 0 = window not full */
	int64_t prev_pts_us;        /* -1 = no frame yet in this series */
	int64_t r_us;               /* latest regulated value */
};

void pacing_pll_init(struct pacing_pll *p);

/* Feeds a new frame's raw pts (µs, monotonic within a connection) and
 * returns the regulated timeline's value for that frame.
 *
 * The caller is responsible for resetting (pacing_pll_init again) at the
 * same points where the anchor is reset: pts jump > 1 s, backwards jump,
 * reconnect — this function knows nothing about pts continuity.
 *
 * Until the window holds PACING_PLL_WINDOW deltas (i.e. too little data for
 * a meaningful T̂), pts_us is returned UNCHANGED, so the transition to the
 * regulated mode is seamless (r_us is kept in phase with raw pts).
 *
 * IMPORTANT (same as for pacing_anchor_push above): the caller must NEVER
 * feed a synthetic or unknown timestamp here — a single such sample can
 * cause several seconds of error in r_us that decays geometrically over
 * dozens of frames (see collect_decoded in camera_thread.c for how that
 * case is handled: the PLL is not fed at all, the frame gets a target time
 * extrapolated from the previous regulated time + T̂ instead). */
int64_t pacing_pll_feed(struct pacing_pll *p, int64_t pts_us);

/* Latest (sanity-clamped to 10–200 ms) estimate of the nominal period T̂,
 * in µs — the same value actually used in the latest pacing_pll_feed call.
 * 0 if the window is not yet full. For diagnostics/tests. */
int64_t pacing_pll_mean_delta_us(const struct pacing_pll *p);

/* ---------------------------------------------------------- jitter histogram */

#define PACING_HIST_BINS 500   /* 0..499 ms, one bin per ms */

struct pacing_histogram {
	uint32_t bins[PACING_HIST_BINS];
	uint32_t over;    /* >= 500 ms lands here instead of overflowing the array */
	uint32_t count;
};

void pacing_hist_init(struct pacing_histogram *h);
void pacing_hist_add(struct pacing_histogram *h, int64_t value_ms);

/* Percentile 0..100 in ms (the bin the sample landed in). -1 if the
 * histogram is empty. Samples in the "over" bin give PACING_HIST_BINS
 * (i.e. "at least 500 ms"). */
int pacing_hist_percentile(const struct pacing_histogram *h, int percentile);

/* ------------------------------------------------------- frame selection (FIFO) */

struct pacing_frame {
	int     index;        /* caller's buffer index (CAPTURE index) */
	int64_t target_us;    /* target display time, monotonic clock */
	int64_t arrival_us;   /* when the frame became available to the caller */
};

#define PACING_FIFO_MAX 20   /* > CAPTURE_BUFFERS (16) with margin */

/* Circular FIFO in target-time order (same order as pts, which is monotonic
 * within a connection). One slot is always kept empty to tell empty from
 * full without a separate counter. */
struct pacing_fifo {
	struct pacing_frame frame[PACING_FIFO_MAX];
	int head, tail;
};

void pacing_fifo_init(struct pacing_fifo *f);
int  pacing_fifo_count(const struct pacing_fifo *f);

/* Appends a frame. If the FIFO was already full the oldest frame is dropped
 * (it would never be shown anyway) and the function returns false. The
 * dropped frame (including .index, so the caller can hand the buffer back
 * to the decoder instead of leaking it) is written to *dropped if dropped
 * is not NULL. */
bool pacing_fifo_push(struct pacing_fifo *f, struct pacing_frame fr,
		      struct pacing_frame *dropped);

/* Selects the NEWEST frame whose target_us <= next_vblank_us. Older ripe
 * frames are removed from the FIFO — their full pacing_frame (including
 * .index) is written to skipped[] (caller's array, at least
 * PACING_FIFO_MAX-1 slots) so the caller can return the buffers to the
 * decoder instead of leaking them. *n_skipped is set to the number of slots
 * written (0 if none). skipped and n_skipped may be NULL if the caller does
 * not care (e.g. tests that only check which frame was chosen).
 *
 * Unripe frames (target_us > next_vblank_us) are left alone.
 *
 * Returns true and writes the chosen frame to *chosen (removing it from the
 * FIFO) if any frame was ripe, otherwise false (FIFO untouched, *n_skipped
 * still set to 0). */
bool pacing_fifo_select(struct pacing_fifo *f, int64_t next_vblank_us,
			struct pacing_frame *chosen,
			struct pacing_frame *skipped, int *n_skipped);

/* ------------------------------------------------------------------ vblank */

/* Vblank period in microseconds from a DRM mode's raw numbers. clock_khz is
 * the pixel clock in kHz (drmModeModeInfo.clock), htotal/vtotal the total
 * columns/lines including blanking (mode.htotal/vtotal). 0 on invalid
 * input. */
int64_t pacing_vblank_period_us(uint32_t clock_khz, uint32_t htotal, uint32_t vtotal);

/* Smallest vblank time strictly after now_us, stepping forward in steps of
 * period_us from the last known vblank. If period_us <= 0, now_us is
 * returned unchanged (the caller has no mode to compute from yet).
 *
 * WARNING: the function does exactly what it says for a GIVEN now_us, but
 * now_us — read with monotonic_us() just after a flip/vblank event was
 * handled — can land BEFORE that event's own timestamp (DRM stamps "end of
 * vblank"/"start of scanout", which can precede the software clock read a
 * moment later). With now_us < last_vblank_us this function returns
 * last_vblank_us UNCHANGED — correct by definition ("smallest vblank
 * strictly after now_us"), but wrong for that caller: it wants the NEXT
 * vblank after the one it just received, not the same one again. The
 * compositor (compositor.c) therefore computes last_vblank_us + period_us
 * directly in that situation instead of going through this function. */
int64_t pacing_next_vblank(int64_t last_vblank_us, int64_t period_us, int64_t now_us);

/* ------------------------------------------------------------- target time */

/* target_us = pts_us + anchor_us + buffer_ms + delay_ms (the last two in
 * milliseconds, converted here). Plain arithmetic, but kept in one place so
 * the unit (µs vs ms) only has to be right once.
 *
 * pts_us is the REGULATED timeline (see pacing_pll_feed), not raw pts — the
 * caller (camera_thread.c) runs raw pts through the PLL first. The function
 * itself does not care; it is just arithmetic. */
int64_t pacing_target_time(int64_t pts_us, int64_t anchor_us, int buffer_ms, int delay_ms);

/* ---------------------------------------------------------------- rotation */

/* Generous: MAX_CAMERAS (rtspwall.h) is 16, and a rotation group can never
 * be larger than that. */
#define PACING_MAX_GROUP_SIZE 16

/* Just a tile's geometry — the caller fills in one per camera from its own
 * config (width/height/x/y). No other camera data is needed to form groups. */
struct pacing_tile {
	int width, height, x, y;
};

/* A rotation group: `index[]` holds the caller's camera array indices
 * (0-based, config order) for the members, in ASCENDING order — i.e.
 * `index[0]` is the camera that appears FIRST in the config for this tile,
 * and it is the one that starts active (see struct pacing_rotation below).
 * A camera with a unique tile forms its own group with `count == 1` and
 * never rotates. */
struct pacing_group {
	int index[PACING_MAX_GROUP_SIZE];
	int count;
};

/* Forms groups from `count` cameras' tiles (`tiles[i]` belongs to camera
 * index i). Two cameras end up in the same group if and only if their tile
 * is EXACTLY equal (width, height, x and y). Groups are written to `groups`
 * in the order their FIRST member appears in `tiles` — at most `max_groups`
 * groups are written. Returns the TOTAL number of groups identified, which
 * can exceed `max_groups` if `groups` was too small (same "write at most N,
 * return the real count" pattern as snprintf) — the callers size their
 * array by MAX_CAMERAS so that never happens in practice, but the function
 * never writes outside `groups[0..max_groups-1]`.
 *
 * `count` is clamped internally to at most 64 (far above MAX_CAMERAS) as a
 * safety net against bogus input — cameras beyond index 63 are ignored. */
int pacing_build_groups(const struct pacing_tile *tiles, int count,
			struct pacing_group *groups, int max_groups);

/* Rotation state for ONE group. The caller owns one instance per group
 * (groups with `count == 1` strictly do not need one, but it is harmless —
 * it just never rotates, see pacing_rotation_update). `active`/`candidate`
 * are GROUP indices (0..group count-1), NOT camera indices — the caller
 * looks up `group.index[active]` to get the real camera index. */
struct pacing_rotation {
	int      active;              /* group index of the active member */
	int64_t  switched_us;         /* when active last changed (or init) */
	int      candidate;           /* -1 = no switch pending, else group index being waited for */
	int64_t  candidate_since_us;  /* when candidate was set, for the 3 s grace period */
};

void pacing_rotation_init(struct pacing_rotation *r, int64_t now_us);

/* Called once per vblank per group (groups with only ONE member may skip
 * the call entirely — it is a no-op for them, see below).
 *
 * `healthy[i]` (i = group index, `count` entries) = member i has a ripe
 * frame to show RIGHT NOW — the caller has already checked its FIFO this
 * vblank (see compositor() in compositor.c, which therefore has to drain
 * idle members' FIFOs every vblank regardless of rotation state).
 * `forced` = the ACTIVE member must go NOW (teardown in progress for it,
 * see teardown_stream) — skip the rotate_s wait and try to switch to a
 * healthy member immediately. When forced there is NO 3-second grace (see
 * below) for an individual candidate — if it is not healthy the candidate
 * steps on to the next member in turn immediately (the active one is going
 * away regardless, so there is no reason to wait for one specific
 * candidate). Only if ONE FULL LAP finds no healthy member does the
 * function keep waiting on the last tried candidate until the next vblank.
 *
 * A switch becomes DUE when `forced` OR at least `rotate_s` seconds have
 * passed since the last switch. When it becomes due (and no switch is
 * already pending) the NEXT member in turn (active+1 mod count) is chosen
 * as candidate. The candidate is switched in as SOON as it is healthy
 * (possibly the same vblank it became due, or several vblanks later).
 *
 * If the candidate is NOT healthy it steps on to the next member in turn
 * instead of getting stuck on the same (dead) neighbour — otherwise, in
 * groups with 3+ members, rotation could never reach a healthy member
 * beyond a dead neighbour, and in forced mode (active torn down) the tile
 * would go black for good. When not forced, the candidate steps on once it
 * has been unhealthy for more than 3 seconds; when forced it steps on
 * immediately. If a full lap finds no healthy member (all dead) the round
 * is abandoned for this vblank: when not forced `*skipped` (if not NULL) is
 * set to true for that one call (the caller logs ONE line, see
 * compositor()) and the next attempt becomes due no earlier than
 * `rotate_s` later; when forced it keeps waiting on the last tried
 * candidate until the next vblank (no `rotate_s` pause, the attempt is
 * retried right away). Otherwise `*skipped` is set to false.
 *
 * Returns the (possibly unchanged) active group index. NEVER switches to a
 * member that is not healthy. A lone member (`count <= 1`) never rotates —
 * `active` is always 0 and no candidate is set. */
int pacing_rotation_update(struct pacing_rotation *r, const bool *healthy, int count,
			   int rotate_s, bool forced, int64_t now_us,
			   bool *skipped);

/* ----------------------------------------------------- camera fault classes
 *
 * Why a camera connection failed, decided by camera_thread.c from the
 * libavformat error (or its own checks). Deterministic faults will not go
 * away by retrying quickly (wrong password, wrong path, H.265 stream,
 * stream too large for the decoder): they are logged once with a hint and
 * retried with exponential backoff up to 60 s. Transient faults (camera or
 * NVR rebooting, network glitch, stall) are retried every 2-5 s so the
 * wall recovers quickly. */
enum pacing_fault {
	PACING_FAULT_NONE,
	PACING_FAULT_UNAUTHORIZED,   /* 401 */
	PACING_FAULT_FORBIDDEN,      /* 403 */
	PACING_FAULT_NOT_FOUND,      /* 404 / no such file */
	PACING_FAULT_CODEC,          /* video is not H.264 */
	PACING_FAULT_TOO_LARGE,      /* larger than 1920x1920 */
	PACING_FAULT_DECODER,        /* the hardware decoder could not be set up */
	PACING_FAULT_REFUSED,        /* connection refused */
	PACING_FAULT_TIMEOUT,        /* connect/read timeout */
	PACING_FAULT_EOF,            /* the server closed the stream */
	PACING_FAULT_STALL,          /* no data for several seconds (watchdog) */
	PACING_FAULT_UNREACHABLE,    /* host/network unreachable */
	PACING_FAULT_OTHER,          /* anything else: treated as transient */
};

bool pacing_fault_is_deterministic(enum pacing_fault f);

/* Short text for STATUS= and log lines, e.g. "login failed (401)". */
const char *pacing_fault_short(enum pacing_fault f);

/* What to do about it, one sentence ("" for NONE/OTHER). */
const char *pacing_fault_hint(enum pacing_fault f);

/* Delay before reconnect attempt `attempt` (1 = first failure in a row).
 * Deterministic: 5, 10, 20, 40, then 60 s. Transient: 2, 3, 4, then 5 s. */
int64_t pacing_backoff_ms(enum pacing_fault f, unsigned attempt);

/* Per-camera reconnect backoff (camera thread only).
 *
 * The attempt counter belongs to ONE fault: when the fault changes, the
 * schedule of the new fault starts from its first step. Otherwise an NVR
 * restart (refused x N, then 404 while the stream path is not ready yet)
 * would hand the 404 attempt N+1 and wait 60 s.
 *
 * Right after the camera has delivered video (mark_live), 401 and 404
 * count as transient: the credentials and the path were right before, so
 * the server is most likely still starting. That grace is bounded: only
 * the first PACING_LIVE_GRACE_ATTEMPTS 401/404 attempts and only within
 * PACING_LIVE_GRACE_US of the moment the camera was last live, whichever
 * ends first. After that a password or path changed for good backs off
 * like any deterministic fault (up to 60 s). */
#define PACING_LIVE_GRACE_ATTEMPTS 10
#define PACING_LIVE_GRACE_US       (120 * 1000000LL)

struct pacing_backoff {
	enum pacing_fault last;       /* fault of the previous failed attempt */
	unsigned          failures;   /* failed attempts in a row with `last` */
	unsigned          total;      /* failed attempts in a row, any fault */
	unsigned          since_live; /* 401/404 attempts since mark_live */
	bool              was_live;   /* delivered video at least once */
	int64_t           live_us;    /* when it was last live (mark_live) */
};

void pacing_backoff_init(struct pacing_backoff *b);
/* The camera delivered video until now_us (monotonic, us): starts a new
 * 401/404 grace. */
void pacing_backoff_mark_live(struct pacing_backoff *b, int64_t now_us);
/* A healthy period ended the run of failures (keeps was_live). */
void pacing_backoff_reset(struct pacing_backoff *b);
/* Records a failed attempt with fault f at now_us and returns the delay
 * before the next one. */
int64_t pacing_backoff_next(struct pacing_backoff *b, enum pacing_fault f, int64_t now_us);
/* Failed attempts in a row, whatever the fault (for "after N attempts"). */
unsigned pacing_backoff_total(const struct pacing_backoff *b);
/* pacing_fault_is_deterministic, except 401/404 during the grace after
 * the camera was live (for the next attempt at now_us). */
bool pacing_backoff_is_deterministic(const struct pacing_backoff *b, enum pacing_fault f,
				     int64_t now_us);

/* True if a libavformat error line (as logged, "[tcp @ 0x...]" prefix and
 * all) reports a failed host name lookup. libavformat returns a generic
 * AVERROR(EIO) for those ("Failed to resolve hostname X: Name or service
 * not known"), so only the text tells them apart. NULL/"" -> false. */
bool pacing_text_is_dns_failure(const char *av_msg);

/* The fault class from the error code, refined by libavformat's last error
 * line: an unclassified error (OTHER) whose line is a DNS failure becomes
 * UNREACHABLE (transient, like `rtspwall probe` reports it). Any other
 * class is returned unchanged. */
enum pacing_fault pacing_fault_refine(enum pacing_fault f, const char *av_msg);

/* ----------------------------------------------------- exponential retry
 *
 * Rate limit for a recovery action that may not help (compositor.c: a
 * re-modeset after refused vblank waits): the first call is due at once,
 * then the gaps double from min_us up to max_us. pacing_retry_reset (the
 * condition went away) starts over at min_us. */
struct pacing_retry {
	int64_t min_us, max_us;
	int64_t delay_us;    /* gap after the next due action */
	int64_t next_us;     /* not due before this; 0 = due at once */
	bool    armed;       /* false = nothing done since init/reset */
};

void pacing_retry_init(struct pacing_retry *r, int64_t min_us, int64_t max_us);
void pacing_retry_reset(struct pacing_retry *r);
/* True if the action is due at now_us; schedules the next one. */
bool pacing_retry_due(struct pacing_retry *r, int64_t now_us);

/* ------------------------------------------------------- follow the cable
 *
 * With CONNECTOR unset the wall follows a cable moved to another port
 * (drm.c, display_poll). Called about once a second with the cached
 * connector states:
 *   current_connected - the wall's connector right now
 *   moved             - the wall is not on the connector chosen at start
 *   home_connected    - that start ("home") connector right now (only
 *                       looked at when moved)
 * MOVE_HOME once home has been connected for stable_us in a row (the
 * wall goes back, even from a working connector); otherwise MOVE_AWAY
 * once the current connector has been disconnected for stable_us in a row
 * (a short hotplug pulse at TV power-on or an input switch never moves
 * the wall). The caller calls pacing_conn_follow_moved after a move. */
enum pacing_conn_action {
	PACING_CONN_STAY,
	PACING_CONN_MOVE_AWAY,
	PACING_CONN_MOVE_HOME,
};

struct pacing_conn_follow {
	int64_t down_since_us;      /* current disconnected since, 0 = connected */
	int64_t home_up_since_us;   /* home connected since (while moved), 0 = not */
};

void pacing_conn_follow_init(struct pacing_conn_follow *f);
void pacing_conn_follow_moved(struct pacing_conn_follow *f);
enum pacing_conn_action pacing_conn_follow_update(struct pacing_conn_follow *f,
						  bool current_connected, bool moved,
						  bool home_connected, int64_t now_us,
						  int64_t stable_us);

/* ------------------------------------------------------ teardown shortcut */

/* teardown_stream (camera_thread.c) normally waits up to TEARDOWN_WAIT_S
 * for the plane to be confirmed detached. With the display disconnected
 * that confirmation may never come, so the wait is skipped and the indices
 * that may still be on screen (`n_protect`: shown, in flight, parked in
 * limbo) are leaked through the leak list instead - but only while that
 * list (n_leaked of max_leaked used) has room for all of them; otherwise
 * the normal wait applies. */
bool pacing_teardown_skip_wait(bool display_down, int n_leaked, int n_protect, int max_leaked);

/* ------------------------------------------------------------ log collapse
 *
 * Collapses repeated identical log lines: the first occurrence of a line is
 * emitted, repeats within `interval_us` of the last emitted copy are
 * suppressed and counted, and the next occurrence after the interval is
 * emitted together with the count ("repeated N times"). A few slots so
 * that lines alternating between two or three texts collapse as well. */
#define PACING_DEDUP_SLOTS    16
#define PACING_DEDUP_KEY_MAX 192

struct pacing_dedup_slot {
	char     key[PACING_DEDUP_KEY_MAX];
	bool     used;
	unsigned suppressed;
	int64_t  last_emit_us, last_seen_us;
};

struct pacing_dedup {
	struct pacing_dedup_slot slot[PACING_DEDUP_SLOTS];
	int64_t interval_us;
};

void pacing_dedup_init(struct pacing_dedup *d, int64_t interval_us);
void pacing_dedup_reset(struct pacing_dedup *d);

/* Returns true if the line should be logged now; *repeats is then the
 * number of copies suppressed since it was last logged (0 = none). Keys
 * longer than PACING_DEDUP_KEY_MAX - 1 are compared on their prefix. When
 * all slots are taken the least recently seen one is reused. */
bool pacing_dedup_check(struct pacing_dedup *d, const char *key, int64_t now_us,
			unsigned *repeats);

/* Copies `in` to `out` with every hexadecimal pointer ("0x55d4c2a1b2c0",
 * as in FFmpeg's "[rtsp @ 0x...]" prefix) cut down to "0x", so lines from
 * different contexts collapse. Always NUL-terminates. */
void pacing_dedup_normalize(const char *in, char *out, size_t outlen);

/* ------------------------------------------------------------ STATUS text */

enum pacing_cam_state {
	PACING_CAM_CONNECTING,
	PACING_CAM_LIVE,
	PACING_CAM_FAILING,
};

struct pacing_cam_status {
	const char           *name;
	enum pacing_cam_state state;
	enum pacing_fault     fault;
};

/* "5/6 live; garage: login failed (401)" — the live count, then up to
 * three cameras that are not live ("connecting" or their fault), then
 * "+N more". Never contains a URL. Always NUL-terminates. */
void pacing_format_status(const struct pacing_cam_status *s, int n, char *out, size_t outlen);

/* ------------------------------------------------- pts continuity, file pace */

enum pacing_pts_event {
	PACING_PTS_OK,          /* continues the series (or the first pts) */
	PACING_PTS_BACKWARDS,   /* went backwards: new series (loop, camera restart) */
	PACING_PTS_JUMP,        /* jumped forward more than 1 s */
};

/* How far pts may step back and still continue the series: packets
 * arrive in DECODE order, so with B-frames (I0 P3 B1 B2 ...) pts goes
 * back by a few frame periods all the time. 500 ms covers two B-frames
 * even at 5 fps; a looped clip or a camera restarting its clock goes back
 * by far more. */
#define PACING_PTS_REORDER_US 500000

/* Classifies pts_us against the previous pts (last_us < 0 = none yet).
 * A step back of up to PACING_PTS_REORDER_US is reordering, not a new
 * series. The caller re-anchors on anything but OK and then uses pts_us
 * itself as the new reference, so a looped clip re-anchors exactly once. */
enum pacing_pts_event pacing_pts_classify(int64_t last_us, int64_t pts_us);

/* Reads a local file at its natural speed (like ffmpeg -re): the wall
 * clock time a packet is due is base_wall + (pts - base_pts). Re-based on
 * the first packet, when pts goes backwards (loop), when a packet would be
 * due more than 2 s from now (forward jump), or when reading has fallen
 * more than 1 s behind. So the caller never waits more than 2 s. */
struct pacing_filepace {
	bool    init;
	int64_t base_wall_us, base_pts_us;
};

void pacing_filepace_reset(struct pacing_filepace *p);

/* Wall clock time (same clock as now_us) at which the packet with pts_us
 * is due; <= now_us means "feed it now". */
int64_t pacing_filepace_due(struct pacing_filepace *p, int64_t pts_us, int64_t now_us);

/* ------------------------------------------------ unmatched packets (pts) */

enum pacing_unmatched {
	PACING_UNMATCHED_NONE,      /* every fed packet produced a frame */
	PACING_UNMATCHED_STARTUP,   /* before the first frame: normal at connect */
	PACING_UNMATCHED_SKIPPED,   /* after the first frame: decoder skipped one */
};

/* In pts mode, a decoded frame that matches an arrival-queue entry past
 * the oldest one means n_unmatched packets never produced a frame. Before
 * the first decoded frame of a connection that is expected (the decoder
 * drops packets ahead of the first IDR and before its CAPTURE queue is
 * set up), so it must not be reported as a corrupt frame. */
enum pacing_unmatched pacing_unmatched_classify(bool first_frame_seen, unsigned n_unmatched);

#endif
