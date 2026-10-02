/*
 * compositor.c — the atomic commit loop: waits for vblank/flip, picks ripe
 * frames from each camera's jitter buffer (including the rotation
 * decision), builds ONE atomic commit per vblank and writes the 60-second
 * statistics lines. See rtspwall.h for the structs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "rtspwall.h"

/* -------------------------------------------------------- systemd notify */

void notify_systemd(const char *msg)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	size_t len;

	if (layout_notify_sockaddr(getenv("NOTIFY_SOCKET"), sa.sun_path, sizeof sa.sun_path,
				   &len) < 0)
		return;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	if (sendto(fd, msg, strlen(msg), MSG_NOSIGNAL, (struct sockaddr *)&sa,
		   (socklen_t)(offsetof(struct sockaddr_un, sun_path) + len)) < 0) {
		static bool warned;   /* benign race: at worst logged twice */
		if (!warned) {
			warned = true;
			log_msg("sd_notify: cannot send to $NOTIFY_SOCKET: %s", strerror(errno));
		}
	}
	close(fd);
}

void notify_status(const char *fmt, ...)
{
	char msg[512] = "STATUS=";
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg + 7, sizeof msg - 7, fmt, ap);
	va_end(ap);
	for (char *p = msg; *p; p++)   /* one line, no injected assignments */
		if (*p == '\n' || *p == '\r')
			*p = ' ';
	notify_systemd(msg);
}

void update_status(struct wall *v, int64_t now_us)
{
	static int64_t last_us;
	static char last[400];
	struct pacing_cam_status st[MAX_CAMERAS];
	char text[400];

	if (last_us && now_us - last_us < 1000000)
		return;
	for (int i = 0; i < v->count; i++)
		st[i] = (struct pacing_cam_status){
			.name = v->cam[i].name,
			.state = (enum pacing_cam_state)atomic_load(&v->cam[i].st_state),
			.fault = (enum pacing_fault)atomic_load(&v->cam[i].st_fault),
		};
	if (v->display_connected) {
		pacing_format_status(st, v->count, text, sizeof text);
	} else {
		int n = snprintf(text, sizeof text, "display %s disconnected (waiting); ", v->conn_name);
		if (n > 0 && (size_t)n < sizeof text)
			pacing_format_status(st, v->count, text + n, sizeof text - (size_t)n);
	}
	if (!strcmp(text, last))
		return;
	last_us = now_us;
	memcpy(last, text, sizeof last);
	notify_status("%s", text);
}

/* -------------------------------------------------------------- compositor */

/* Each page-flip commit carries a sequence number as its user data, so a
 * late event for a commit the compositor already gave up on (see the flip
 * timeout in compositor()) cannot complete a newer one. */
struct flip_data {
	bool      done;
	uintptr_t seq;        /* sequence number of the pending commit */
	int64_t   time_us;    /* the event's timestamp, set by flip_handler */
	int64_t   commit_us;  /* monotonic time of the commit */
};

static struct flip_data flip_state = { .done = true };

static void flip_handler(int fd, unsigned seq, unsigned s, unsigned us,
			 unsigned crtc, void *data)
{
	(void)fd; (void)seq; (void)crtc;
	if ((uintptr_t)data != flip_state.seq || flip_state.done)
		return;   /* stale event of an abandoned commit */
	flip_state.time_us = (int64_t)s * 1000000 + us;
	flip_state.done = true;
}

static void vblank_handler(int fd, unsigned seq, unsigned s, unsigned us, void *data)
{
	(void)fd; (void)seq;
	struct wall *v = data;
	v->last_vblank_us = v->vblank_ts_monotonic
		? (int64_t)s * 1000000 + us : monotonic_us();
}

/* Sets the mode and lights up the primary plane. Done once. */
int initial_commit(struct wall *v)
{
	if (drmModeCreatePropertyBlob(v->drmfd, &v->mode, sizeof v->mode, &v->mode_blob)) {
		log_msg("CreatePropertyBlob: %s", strerror(errno));
		return -1;
	}

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req) {
		log_msg("drmModeAtomicAlloc failed");
		return -1;
	}
	drmModeAtomicAddProperty(req, v->crtc_id, v->p_crtc_active, 1);
	drmModeAtomicAddProperty(req, v->crtc_id, v->p_crtc_mode, v->mode_blob);
	drmModeAtomicAddProperty(req, v->connector_id, v->p_conn_crtc, v->crtc_id);

	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_fb, v->primary_fb);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc, v->crtc_id);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_x, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_y, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_w, v->mode.hdisplay);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_h, v->mode.vdisplay);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_x, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_y, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_w, (uint64_t)v->mode.hdisplay << 16);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_h, (uint64_t)v->mode.vdisplay << 16);

	int r = drmModeAtomicCommit(v->drmfd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	drmModeAtomicFree(req);

	if (r) {
		log_msg("initial atomic commit: %s", strerror(errno));
		return -1;
	}
	log_msg("mode set, %ux%u", v->mode.hdisplay, v->mode.vdisplay);
	return 0;
}

/* Completes the previous commit: the old "shown" index (if one was replaced)
 * goes back to the decoder only NOW, when the hardware has confirmed that
 * the new frame is actually on screen. Doing it earlier lets the decoder
 * write into a buffer that is still being scanned out. */
static void complete_flip(struct wall *v, int64_t flip_time_us)
{
	for (int i = 0; i < v->count; i++) {
		struct camera *k = &v->cam[i];

		pthread_mutex_lock(&k->lock);
		int done = k->in_flight;
		int old = -1;
		if (done == -2) {
			/* Detach commit (teardown, see teardown_stream and the
			 * struct comment at `lock`) confirmed: the plane no longer
			 * shows any buffer. Wake the camera thread, which waits for
			 * exactly this before it destroys FB/V4L2. */
			k->plane_attached = false;
			k->in_flight = -1;
			pthread_cond_broadcast(&k->detached);
		} else if (done >= 0) {
			old = k->shown;
			k->shown = done;
			k->in_flight = -1;
			k->plane_attached = true;
		}
		if (old >= 0)
			ring_push(k, old);
		/* Indices parked by abandon_flip: any confirmed flip proves the
		 * abandoned commit has completed, so the plane shows none of
		 * them any more. */
		for (int j = 0; j < k->n_limbo; j++)
			ring_push(k, k->limbo[j]);
		k->n_limbo = 0;

		/* Leaked buffers (see .leaked in struct camera): this
		 * confirmation proves the plane can no longer reference ANY older
		 * buffer — it now shows either a NEW real frame (done >= 0) or
		 * nothing at all (done == -2, the detach confirmed). Safe to
		 * clean up the buffers teardown_stream had to leak DELIBERATELY
		 * in an earlier, faster teardown. Copy out under the lock, do
		 * the DRM/close calls outside it. */
		struct leaked_buffer leaked_copy[MAX_LEAKED];
		int n_leaked_copy = 0;
		if (done != -1 && k->n_leaked > 0) {
			for (int j = 0; j < k->n_leaked; j++)
				leaked_copy[n_leaked_copy++] = k->leaked[j];
			k->m_leaks_closed += (unsigned long)k->n_leaked;
			k->n_leaked = 0;
		}
		pthread_mutex_unlock(&k->lock);

		if (n_leaked_copy > 0) {
			for (int j = 0; j < n_leaked_copy; j++) {
				if (leaked_copy[j].fb)
					drmModeRmFB(v->drmfd, leaked_copy[j].fb);
				if (leaked_copy[j].dmafd >= 0)
					close(leaked_copy[j].dmafd);
			}
			log_msg("%s: cleaned up %d previously leaked buffer(s) after a confirmed flip",
				k->name, n_leaked_copy);
		}

		if (done < 0)
			continue;

		k->m_shown++;
		int64_t latency = flip_time_us - k->in_flight_arrival_us;
		if (latency < 0)
			latency = 0;
		k->m_latency_sum_us += (unsigned long)latency;
		k->m_latency_count++;
	}
}

/* A page flip that never got its event (see the flip timeout in
 * compositor()). Unlike complete_flip nothing is confirmed:
 *   - a detach in flight (-2) is NOT taken as done: in_flight goes back to
 *     -1 and plane_attached stays true, so the detach is made again and a
 *     waiting teardown_stream keeps waiting for a real confirmation (the
 *     plane must be confirmed off before its framebuffers are removed);
 *   - a frame in flight becomes "shown" (the plane may show it), but the
 *     previous "shown" is NOT handed back to the decoder - the plane may
 *     still show that one too. It is parked in `limbo` until a later flip
 *     is confirmed (flips on one CRTC complete in order, so a confirmed
 *     later flip proves the abandoned one is done);
 *   - leaked[] is left alone (drained only on a confirmed flip). */
static void abandon_flip(struct wall *v)
{
	for (int i = 0; i < v->count; i++) {
		struct camera *k = &v->cam[i];

		pthread_mutex_lock(&k->lock);
		if (k->in_flight == -2) {
			k->in_flight = -1;
		} else if (k->in_flight >= 0) {
			/* MAX_LIMBO == CAPTURE_BUFFERS: every index can be parked
			 * at most once, so the bound never drops one (it only
			 * guards the array). */
			if (k->shown >= 0 && k->n_limbo < MAX_LIMBO)
				k->limbo[k->n_limbo++] = k->shown;
			k->shown = k->in_flight;
			k->in_flight = -1;
			k->plane_attached = true;
		}
		pthread_mutex_unlock(&k->lock);
	}
}

/* Formats a percentile as a string — "n/a" if the histogram was empty. */
static void percentile_str(const struct pacing_histogram *h, int percentile, char *buf, size_t n)
{
	int p = pacing_hist_percentile(h, percentile);
	if (p < 0)
		snprintf(buf, n, "n/a");
	else
		snprintf(buf, n, "%d", p);
}

/* The 60-second lines: a main line + a diag line per camera, plus a global
 * diag line (see diag_* in struct wall for what it measures).
 *
 * m_decoded/m_late/m_dropped_full/m_synthetic are written only by the
 * camera thread and never reset (see the struct comment) — here the
 * window's delta is computed against the rep_* snapshots, unlocked (one
 * writer per field is enough). m_jitter, m_regulated_ptsdelta and the depth
 * are copied out and reset under `lock`, which the camera thread takes for
 * every FIFO push anyway.
 *
 * Global diag line: a warning line is written only if diag_late1_plus > 0
 * — a flip was then confirmed at least one vblank later than it aimed for
 * (a possible regression of the vblank-target computation in compositor()).
 * Per-camera diag line:
 *   - "regulated" pts delta (AFTER the PLL): a tight p5–p95 span around the
 *     nominal period (~33 ms for 30 fps, ~40/42 ms for 25/24 fps) => the
 *     source delivers evenly (or the PLL smooths a clumped/paired source).
 *   - "synthetic": number of frames since the previous line whose target
 *     time was extrapolated instead of computed because arrival_queue was
 *     empty (see collect_decoded). Should normally be 0.
 *   - "leaks_closed"/"leaks_active": leaked buffers (see .leaked in struct
 *     camera) — leaks_closed is the number of fb/dmafd cleaned up since the
 *     previous line, leaks_active how many are still waiting right now.
 *     Should normally be 0/0 — visibility should a teardown timeout (the
 *     CRITICAL line in teardown_stream) ever happen.
 *   The main line's dropped percentage is the most important receipt of
 *   smooth delivery: it should stay below 1 %. */
static void report(struct wall *v)
{
	log_msg("diag: busy_drops=%lu switches=%lu", v->diag_busy_drops, v->diag_switches);
	if (v->diag_late1_plus > 0)
		log_msg("diag: WARNING late1+=%lu flips confirmed >=1 vblank after target (regression?)",
			v->diag_late1_plus);
	v->diag_late1_plus = 0;
	v->diag_busy_drops = 0;
	v->diag_switches = 0;

	for (int i = 0; i < v->count; i++) {
		struct camera *k = &v->cam[i];

		unsigned long decoded = k->m_decoded - k->rep_decoded;
		unsigned long late    = k->m_late - k->rep_late;
		unsigned long dropped_full = k->m_dropped_full - k->rep_dropped_full;
		unsigned long synthetic = k->m_synthetic - k->rep_synthetic;
		k->rep_decoded = k->m_decoded;
		k->rep_late = k->m_late;
		k->rep_dropped_full = k->m_dropped_full;
		k->rep_synthetic = k->m_synthetic;

		unsigned long skipped = k->m_skipped;
		unsigned long dropped = dropped_full + skipped;

		/* Written only by the compositor (complete_flip runs in this
		 * thread) — no lock needed for m_leaks_closed/rep_leaks_closed
		 * themselves, same "never reset, diffed via snapshot" pattern
		 * as m_synthetic. n_leaked IS shared with the camera thread
		 * (camera_leak_push) and is read under the lock below. */
		unsigned long leaks_closed = k->m_leaks_closed - k->rep_leaks_closed;
		k->rep_leaks_closed = k->m_leaks_closed;

		pthread_mutex_lock(&k->lock);
		unsigned long depth_sum = k->m_depth_sum;
		unsigned long depth_count = k->m_depth_count;
		struct pacing_histogram jitter = k->m_jitter;
		struct pacing_histogram regulated = k->m_regulated_ptsdelta;
		int n_leaked_now = k->n_leaked;
		k->m_depth_sum = k->m_depth_count = 0;
		pacing_hist_init(&k->m_jitter);
		pacing_hist_init(&k->m_regulated_ptsdelta);
		pthread_mutex_unlock(&k->lock);

		double dec_fps = decoded / 60.0;
		double shown_fps = k->m_shown / 60.0;
		double dropped_pct = decoded ? 100.0 * (double)dropped / decoded : 0.0;
		double depth = depth_count ? (double)depth_sum / depth_count : 0.0;
		double latency_ms = k->m_latency_count
			? (double)k->m_latency_sum_us / k->m_latency_count / 1000.0 : 0.0;

		char p50s[16], p95s[16];
		percentile_str(&jitter, 50, p50s, sizeof p50s);
		percentile_str(&jitter, 95, p95s, sizeof p95s);

		log_msg("%s: 60s dec=%.1f shown=%.1f dropped=%lu(%.1f%%) late=%lu depth=%.1f jitter p50=%s p95=%sms latency=%.0fms active=%.0fs idle=%lu",
			k->name, dec_fps, shown_fps, dropped, dropped_pct, late, depth,
			p50s, p95s, latency_ms, k->m_active_us / 1e6, k->m_idle);

		/* Regulated pts delta in ms resolution (same pacing_histogram as
		 * jitter, AFTER the PLL) — see the comment above the function
		 * for how to read it. */
		char rd5s[16], rd50s[16], rd95s[16];
		percentile_str(&regulated, 5, rd5s, sizeof rd5s);
		percentile_str(&regulated, 50, rd50s, sizeof rd50s);
		percentile_str(&regulated, 95, rd95s, sizeof rd95s);

		log_msg("%s: diag regulated ptsdelta p5=%sms p50=%sms p95=%sms synthetic=%lu leaks_closed=%lu leaks_active=%d",
			k->name, rd5s, rd50s, rd95s, synthetic, leaks_closed, n_leaked_now);

		k->m_shown = 0;
		k->m_skipped = 0;
		k->m_latency_sum_us = k->m_latency_count = 0;
		k->m_active_us = 0;
		k->m_idle = 0;
	}
}

/* The compositor wakes every vblank (flip confirmation if a commit is
 * pending, otherwise a standalone vblank event — see drmWaitVBlank below)
 * and asks each camera's jitter buffer for the newest frame whose target
 * time has passed. No blind sleeping: if no camera has anything new, no
 * commit is made, but the compositor still wakes at the next vblank. */
void compositor(struct wall *v)
{
	struct flip_data *const fl = &flip_state;
	drmEventContext evctx = {
		.version = 3,
		.vblank_handler = vblank_handler,
		.page_flip_handler2 = flip_handler,
	};
	bool vblank_pending = false;
	int64_t vblank_request_us = 0;
	int64_t last_report_us = monotonic_us();
	int64_t last_flip_warn_us = 0;
	int64_t vblank_fail_since_us = 0;   /* first of a run of refused vblank waits */
	int     vblank_failures = 0;
	int64_t last_vblank_warn_us = 0;
	/* Re-modesets triggered by refused vblank waits back off 1, 2, 4 ...
	 * 60 s while they do not help (a CRTC that stays off); reset by the
	 * next vblank or flip event that arrives. */
	struct pacing_retry vblank_reprobe;
	pacing_retry_init(&vblank_reprobe, 1000000, 60 * 1000000LL);

	while (!quit) {
		int64_t loop_us = monotonic_us();

		/* A page flip that never completes (display gone, driver stuck)
		 * would freeze the wall for good. After FLIP_TIMEOUT_US give up
		 * on it WITHOUT confirming anything (abandon_flip: no buffer
		 * goes back to the decoder, a detach is redone) and let the
		 * display recovery below re-probe. */
		if (!fl->done && loop_us - fl->commit_us > FLIP_TIMEOUT_US) {
			if (!last_flip_warn_us || loop_us - last_flip_warn_us >= 60 * 1000000LL) {
				log_msg("display: no page-flip event for 2 s on %s - recovering",
					v->conn_name);
				last_flip_warn_us = loop_us;
			}
			fl->done = true;
			abandon_flip(v);
			if (v->commit_failures < 10)
				v->commit_failures = 10;
		}

		/* Hotplug / standby / failed-commit recovery, only while no flip
		 * is pending (a modeset next to a pending flip gets EBUSY). */
		if (fl->done && display_poll(v))
			vblank_pending = false;   /* the modeset ended that wait */

		/* A requested vblank event that never arrives (CRTC was off)
		 * must not stall the loop: ask again after 500 ms. */
		if (vblank_pending && loop_us - vblank_request_us > 500000)
			vblank_pending = false;

		update_status(v, loop_us);

		bool awaiting_flip = !fl->done;

		if (!awaiting_flip && !vblank_pending) {
			drmVBlank vbl = {
				.request = {
					.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT
						| ((v->crtc_pipe << DRM_VBLANK_HIGH_CRTC_SHIFT)
						   & DRM_VBLANK_HIGH_CRTC_MASK),
					.sequence = 1,
					.signal = (unsigned long)(uintptr_t)v,
				},
			};
			if (drmWaitVBlank(v->drmfd, &vbl)) {
				/* Rare (the driver refuses right now, e.g. the
				 * CRTC is off) — fall back to a short sleep
				 * instead of spinning hot or freezing the
				 * compositor. Refused for a second on end: the
				 * CRTC is most likely off, so re-probe the display
				 * and set the mode again (display_poll). */
				int e = errno;
				if (!vblank_fail_since_us) {
					vblank_fail_since_us = loop_us;
					vblank_failures = 0;
				}
				vblank_failures++;
				if (loop_us - vblank_fail_since_us >= 1000000 && v->display_connected
				    && !v->remodeset_pending
				    && pacing_retry_due(&vblank_reprobe, loop_us)) {
					if (!last_vblank_warn_us
					    || loop_us - last_vblank_warn_us >= 60 * 1000000LL) {
						log_msg("display: vblank wait refused %d times in %lld ms "
							"(%s) - re-probing %s and setting the mode again "
							"(next attempt in %lld s at the earliest if it "
							"keeps failing)",
							vblank_failures,
							(long long)(loop_us - vblank_fail_since_us) / 1000,
							strerror(e), v->conn_name,
							(long long)((vblank_reprobe.next_us - loop_us)
								    / 1000000));
						last_vblank_warn_us = loop_us;
					}
					v->remodeset_pending = true;
					vblank_fail_since_us = 0;
				}
				usleep(4000);
				continue;
			}
			vblank_fail_since_us = 0;
			vblank_pending = true;
			vblank_request_us = loop_us;
		}

		struct pollfd pfd = { .fd = v->drmfd, .events = POLLIN };
		int pr = poll(&pfd, 1, 100);
		if (pr <= 0)
			continue;   /* timeout or error — check quit, try again */
		drmHandleEvent(v->drmfd, &evctx);

		if (awaiting_flip) {
			if (!fl->done)
				continue;      /* the event was not the flip yet */
			pacing_retry_reset(&vblank_reprobe);
			int64_t flip_time_us = v->vblank_ts_monotonic
				? fl->time_us : monotonic_us();

			/* Warning (see diag_late1_plus in struct wall): how many
			 * vblanks late did the flip land compared with the vblank
			 * the commit aimed for (next_vblank_us at commit time, saved
			 * in diag_commit_target_vblank_us)? 0 = the right vblank.
			 * Guard against counting before any commit has been made
			 * (diag_commit_target_vblank_us still 0 at start). */
			if (v->vblank_period_us > 0 && v->diag_commit_target_vblank_us > 0) {
				int64_t diff = flip_time_us - v->diag_commit_target_vblank_us;
				if (diff < 0)
					diff = 0;
				int64_t steps = (diff + v->vblank_period_us / 2)
						/ v->vblank_period_us;
				if (steps >= 1)
					v->diag_late1_plus++;
			}

			v->last_vblank_us = flip_time_us;
			complete_flip(v, flip_time_us);
		} else {
			vblank_pending = false;
			pacing_retry_reset(&vblank_reprobe);   /* vblank works (again) */
			/* vblank_handler already set v->last_vblank_us. */
		}

		int64_t now = monotonic_us();

		/* v->last_vblank_us has just been updated above from a FRESH
		 * flip/vblank event, so the next target vblank is by definition
		 * one period ahead. Computing it via
		 * pacing_next_vblank(last, period, now) could get now < last
		 * (DRM's timestamp convention "end of vblank"/"start of scanout"
		 * can precede the software clock we read a moment later), and
		 * that function then returns last_vblank_us UNCHANGED (0 periods
		 * ahead) instead of the next vblank — the commit would then aim
		 * one vblank too early and every flip would land a vblank late.
		 * We already know this is the right reference point (we just
		 * handled its event), so the next target is computed directly. */
		int64_t next_vblank_us = v->vblank_period_us > 0
			? v->last_vblank_us + v->vblank_period_us
			: now;

		drmModeAtomicReq *req = drmModeAtomicAlloc();
		if (!req) {
			usleep(1000);
			continue;
		}
		int n_new = 0;

		for (int g = 0; g < v->n_groups; g++) {
			struct pacing_group *grp = &v->groups[g];
			struct pacing_rotation *rot = &v->rotation[g];

			bool ripe[PACING_MAX_GROUP_SIZE];
			bool tearing_arr[PACING_MAX_GROUP_SIZE];
			struct pacing_frame chosen[PACING_MAX_GROUP_SIZE];
			int n_skipped_arr[PACING_MAX_GROUP_SIZE];

			/* Step A: drain EVERY member's fifo this vblank — idle
			 * members too, so they stay "warm" (never a full fifo,
			 * always a fresh frame ready when rotation wants it). A
			 * camera in the middle of reconnecting (tearing_down)
			 * already has an empty fifo (teardown_stream empties it
			 * BEFORE setting tearing_down) so ripe[] is always false
			 * for it — no special case needed here. Groups with a
			 * single (fixed) member go through exactly the same code,
			 * with grp->count == 1. */
			for (int m = 0; m < grp->count; m++) {
				struct camera *k = &v->cam[grp->index[m]];
				struct pacing_frame skipped[PACING_FIFO_MAX];
				int n_skipped = 0;

				pthread_mutex_lock(&k->lock);
				tearing_arr[m] = k->tearing_down;
				ripe[m] = pacing_fifo_select(&k->fifo, next_vblank_us, &chosen[m],
							     skipped, &n_skipped);
				for (int j = 0; j < n_skipped; j++)
					ring_push(k, skipped[j].index);
				pthread_mutex_unlock(&k->lock);

				n_skipped_arr[m] = n_skipped;
			}

			/* Step B: rotation decision. count == 1 (fixed camera) is a
			 * no-op in pacing_rotation_update — active stays 0. forced
			 * = the CURRENT active member's tearing_down — a member
			 * being torn down is NEVER chosen as new_active (its
			 * healthy[] value is always false, see step A), so a forced
			 * switch jumps to ANOTHER, healthy member instead of showing
			 * black — or waits for one, see pacing_rotation_update in
			 * pacing.h. */
			int old_active = rot->active;
			bool forced = tearing_arr[old_active];
			bool skipped_round = false;
			int new_active = pacing_rotation_update(rot, ripe, grp->count,
								v->rotate_seconds, forced,
								now, &skipped_round);
			if (new_active != old_active)
				v->diag_switches++;
			if (skipped_round) {
				int candidate_cam = grp->index[(old_active + 1) % grp->count];
				log_msg("rotation: skipping %s (no fresh frame for > 3 s), staying on %s",
					v->cam[candidate_cam].name,
					v->cam[grp->index[old_active]].name);
			}

			/* Step C: one member at a time. tearing_down (whatever the
			 * role) only detaches the plane — a member being torn down
			 * is, as said, never chosen as new_active, so this is
			 * unambiguous. Otherwise: the active member (continuing or
			 * just chosen in step B) gets a real commit if it has a
			 * ripe frame; idle members are drained completely WITHOUT a
			 * commit (counted as m_idle, not as dropped). A real commit
			 * to a NEWLY chosen member always sets the FULL plane
			 * configuration (not just FB_ID) — the same code as a
			 * normal update, which is exactly right: DRM atomic wants
			 * the whole plane configuration when a plane goes from
			 * detached to attached. */
			for (int m = 0; m < grp->count; m++) {
				struct camera *k = &v->cam[grp->index[m]];
				bool emit_attach = false;
				bool emit_detach = false;
				bool was_tearing;

				pthread_mutex_lock(&k->lock);
				was_tearing = k->tearing_down;
				if (was_tearing) {
					if (k->in_flight == -1 && k->plane_attached) {
						k->in_flight = -2;
						emit_detach = true;
					}
				} else if (m == new_active) {
					if (ripe[m]) {
						k->in_flight = chosen[m].index;
						emit_attach = true;
					}
				} else {
					if (ripe[m])
						ring_push(k, chosen[m].index);
					if (k->in_flight == -1 && k->plane_attached) {
						k->in_flight = -2;
						emit_detach = true;
					}
				}
				pthread_mutex_unlock(&k->lock);

				if (emit_detach) {
					drmModeAtomicAddProperty(req, k->plane_id, k->p_fb, 0);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc, 0);
					n_new++;
				} else if (emit_attach) {
					k->in_flight_arrival_us = chosen[m].arrival_us;
					n_new++;

					drmModeAtomicAddProperty(req, k->plane_id, k->p_fb, k->cap[chosen[m].index].fb);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc, v->crtc_id);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_x, k->x);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_y, k->y);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_w, k->width);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_h, k->height);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_src_x,
								 (uint64_t)k->vis_x << 16);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_src_y,
								 (uint64_t)k->vis_y << 16);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_src_w,
								 (uint64_t)k->vis_width << 16);
					drmModeAtomicAddProperty(req, k->plane_id, k->p_src_h,
								 (uint64_t)k->vis_height << 16);
				}

				/* Statistics — independent of whether anything was
				 * committed this vblank. Nothing at all is counted
				 * while tearing_down is set (neither dropped nor idle):
				 * the fifo is empty then anyway (see step A), there is
				 * nothing to count. */
				if (!was_tearing) {
					if (m == new_active) {
						k->m_active_us += (unsigned long)v->vblank_period_us;
						if (emit_attach)
							k->m_skipped += (unsigned long)n_skipped_arr[m];
					} else {
						k->m_idle += (unsigned long)n_skipped_arr[m]
							     + (ripe[m] ? 1 : 0);
					}
				}
			}
		}

		if (!n_new) {
			drmModeAtomicFree(req);
		} else {
			fl->done = false;
			fl->seq++;
			fl->commit_us = monotonic_us();
			int r = drmModeAtomicCommit(v->drmfd, req,
						    DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
						    (void *)fl->seq);
			int commit_errno = errno;
			drmModeAtomicFree(req);

			if (r) {
				fl->done = true;
				v->commit_failures++;
				v->last_commit_errno = commit_errno;
				/* The frames were never shown — in_flight is never
				 * confirmed by a flip, so they must go straight back
				 * to the decoder instead of leaking. Also counted as
				 * dropped. A failed detach commit (in_flight == -2)
				 * has no buffer to hand back — only in_flight is
				 * reset, so compositor() retries the detach next
				 * round. */
				for (int i = 0; i < v->count; i++) {
					struct camera *k = &v->cam[i];
					int p;
					pthread_mutex_lock(&k->lock);
					p = k->in_flight;
					if (p >= 0) {
						ring_push(k, p);
						k->in_flight = -1;
					} else if (p == -2) {
						k->in_flight = -1;
					}
					pthread_mutex_unlock(&k->lock);
					if (p >= 0) {
						k->m_skipped++;
						v->diag_busy_drops++;
					}
				}
				if (commit_errno == EBUSY) {
					usleep(1000);
				} else {
					/* the first of a run, then quiet until the
					 * recovery in display_poll logs (rate-limited) */
					if (v->commit_failures == 1)
						log_msg("atomic commit: %s", strerror(commit_errno));
					usleep(100000);
				}
			} else {
				v->commit_failures = 0;
				v->diag_commit_target_vblank_us = next_vblank_us;
			}
		}

		if (now - last_report_us >= 60 * 1000000) {
			report(v);
			last_report_us = now;
		}
	}
}
