/*
 * camera_thread.c — the free-index ring + the whole camera thread: RTSP
 * demux (libavformat), feeding the decoder, collecting decoded frames into
 * the jitter buffer, and the full connect/read/close loop per camera. See
 * rtspwall.h for the structs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <xf86drmMode.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>

#include "avstream.h"
#include "rtspwall.h"

/* -------------------------------------------------------- free-index ring */

void ring_push(struct camera *k, int index)
{
	int n = (k->ring_head + 1) % (CAPTURE_BUFFERS + 1);
	if (n == k->ring_tail)
		return;              /* full — cannot happen, but drop rather than overwrite */
	k->returned[k->ring_head] = index;
	k->ring_head = n;
}

int ring_pop(struct camera *k)
{
	if (k->ring_tail == k->ring_head)
		return -1;
	int i = k->returned[k->ring_tail];
	k->ring_tail = (k->ring_tail + 1) % (CAPTURE_BUFFERS + 1);
	return i;
}

/* ----------------------------------------------------------- camera thread */

/* Without this, av_read_frame gets stuck in a blocking read when we want to
 * quit, and pthread_join waits forever.
 *
 * Also carries our own RTSP watchdog (see the comment at last_packet_us in
 * struct camera): "stimeout" does not exist in FFmpeg 7.1, so a stream that
 * stalls without the TCP connection breaking would otherwise never be
 * noticed — av_read_frame blocks silently, no EAGAIN, no EOF. */
static int interrupt_cb(void *arg)
{
	struct camera *k = arg;

	if (quit)
		return 1;
	if (!k || !k->last_packet_us)
		return 0;

	int64_t since = monotonic_us() - k->last_packet_us;
	if (since > k->watchdog_limit_us) {
		/* logged by camera_thread as PACING_FAULT_STALL (collapsed) */
		k->stalled = true;
		return 1;
	}
	return 0;
}

static void collect_decoded(struct wall *v, struct camera *k);
static void requeue_returned(struct camera *k);

/* Reads the decoder's event queue. SOURCE_CHANGE means it has parsed the
 * SPS and knows what size it will deliver — only then may CAPTURE be set
 * up, and only after that does it start consuming OUTPUT again. */
static bool check_events(struct wall *v, struct camera *k)
{
	struct v4l2_event ev;

	while (xioctl(k->v4l2fd, VIDIOC_DQEVENT, &ev) == 0) {
		if (ev.type != V4L2_EVENT_SOURCE_CHANGE)
			continue;
		if (!(ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION))
			continue;
		if (!k->capture_on && start_capture(v, k) < 0)
			return false;
	}
	return true;
}

/* Gets a free OUTPUT index. We must NOT just block here: it is exactly
 * during this wait that SOURCE_CHANGE shows up. */
static int get_free_output(struct wall *v, struct camera *k)
{
	for (int round = 0; round < 500 && !quit; round++) {
		if (k->n_free > 0)
			return k->free_out[--k->n_free];

		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.length = 1, .m.planes = &pl,
		};
		if (xioctl(k->v4l2fd, VIDIOC_DQBUF, &b) == 0)
			return b.index;
		if (errno != EAGAIN) {
			log_msg("%s: DQBUF OUTPUT: %s", k->name, strerror(errno));
			return -1;
		}

		if (!check_events(v, k))
			return -1;
		if (k->capture_on) {
			collect_decoded(v, k);
			requeue_returned(k);
		}

		struct pollfd pfd = {
			.fd = k->v4l2fd,
			.events = POLLOUT | POLLIN | POLLPRI,
		};
		poll(&pfd, 1, 20);
	}
	if (!quit)
		log_msg("%s: no free OUTPUT buffer for 10 s", k->name);
	return -1;
}

/* Converts pkt->pts to microseconds and handles anomalies. A missing pts
 * (AV_NOPTS_VALUE) is replaced by the packet's arrival time directly, no
 * re-anchoring (just a gap in the pts series, nothing broken). A backwards
 * jump (a looped file, a camera that restarted its RTP clock) or a large
 * forward jump (> 1 s) starts a new pts series: anchor and PLL are reset
 * and the NEW pts becomes both the returned value and the reference for
 * the next packet (pacing_pts_classify). An earlier version returned and
 * stored the arrival time on a backwards jump; the next packet was then
 * compared against a monotonic-clock value and every later packet counted
 * as "went backwards" again — one re-anchor and log line per packet.
 *
 * Re-anchoring also resets the PLL (pacing_pll_init) — an old regulated
 * timeline from the previous pts base says nothing about the new one.
 * Local files loop every few seconds, so their re-anchors are not logged;
 * a live camera's are, collapsed through k->logdd. */
static int64_t compute_pts(struct camera *k, const AVPacket *pkt,
			   AVRational tb, int64_t arrival_us, bool log_reanchor)
{
	if (pkt->pts == AV_NOPTS_VALUE)
		return arrival_us;

	int64_t pts_us = av_rescale_q(pkt->pts, tb, AV_TIME_BASE_Q);
	enum pacing_pts_event ev = pacing_pts_classify(k->last_pts_us, pts_us);

	if (ev != PACING_PTS_OK) {
		const char *what = ev == PACING_PTS_BACKWARDS ? "went backwards" : "jumped";
		char key[96];
		unsigned rep;
		snprintf(key, sizeof key, "pts %s - re-anchoring", what);
		if (log_reanchor && pacing_dedup_check(&k->logdd, key, arrival_us, &rep))
			log_msg("%s: pts %s (%.3f s) - re-anchoring%s", k->name, what,
				(pts_us - k->last_pts_us) / 1e6,
				rep ? " (and more times since the last such line)" : "");
		pacing_anchor_init(&k->anchor, ANCHOR_WINDOW_US);
		pacing_pll_init(&k->pll);
		k->r_prev_frame_us = -1;
	}
	k->last_pts_us = pts_us;
	return pts_us;
}

/* pts_us: set in the OUTPUT buffer's timestamp field, which bcm2835-codec
 * copies back to the corresponding CAPTURE buffer if it supports that (see
 * collect_decoded, which checks V4L2_BUF_FLAG_TIMESTAMP_COPY).
 * arrival_us: network arrival time; cannot travel in the timestamp field
 * (it holds only one number) so it goes into arrival_queue instead. -1
 * means "do not queue" — used for the SPS/PPS priming packet, which never
 * produces a CAPTURE frame of its own and must therefore never be paired
 * with anything. */
static int feed_packet(struct wall *v, struct camera *k, AVPacket *pkt,
		       int64_t pts_us, int64_t arrival_us)
{
	int i = get_free_output(v, k);

	if (i < 0)
		return -1;

	if ((size_t)pkt->size > k->out[i].length) {
		log_msg("%s: packet of %d B larger than buffer of %zu B - dropped",
			k->name, pkt->size, k->out[i].length);
		k->free_out[k->n_free++] = i;
		return 0;
	}

	memcpy(k->out[i].map, pkt->data, pkt->size);

	struct v4l2_plane pl = { .bytesused = pkt->size };
	struct v4l2_buffer b = {
		.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
		.memory = V4L2_MEMORY_MMAP,
		.index = i, .length = 1, .m.planes = &pl,
		.timestamp.tv_sec  = (long)(pts_us / 1000000),
		.timestamp.tv_usec = (long)(pts_us % 1000000),
	};
	if (xioctl(k->v4l2fd, VIDIOC_QBUF, &b)) {
		log_msg("%s: QBUF OUTPUT: %s", k->name, strerror(errno));
		k->free_out[k->n_free++] = i;
		return -1;
	}

	if (arrival_us >= 0) {
		unsigned slot = k->arrival_in % ARRIVAL_QUEUE_SIZE;
		k->arrival_queue[slot].pts_us = pts_us;
		k->arrival_queue[slot].arrival_us = arrival_us;
		k->arrival_in++;
	}
	return 0;
}

static void collect_decoded(struct wall *v, struct camera *k)
{
	for (;;) {
		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.length = 1, .m.planes = &pl,
		};
		if (xioctl(k->v4l2fd, VIDIOC_DQBUF, &b)) {
			if (errno != EAGAIN)
				log_msg("%s: DQBUF CAPTURE: %s", k->name, strerror(errno));
			return;
		}

		k->m_decoded++;

		int64_t now = monotonic_us();
		if (!k->live_since_us) {
			k->live_since_us = now;
			atomic_store(&k->st_state, PACING_CAM_LIVE);
			atomic_store(&k->st_fault, PACING_FAULT_NONE);
		}
		int64_t pts_captured_us = (int64_t)b.timestamp.tv_sec * 1000000
					  + b.timestamp.tv_usec;

		/* Decided once per connection: does the decoder really copy the
		 * OUTPUT timestamp back to the CAPTURE buffer? */
		if (!k->pts_mode_logged) {
			bool copies = (b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK)
				      == V4L2_BUF_FLAG_TIMESTAMP_COPY;
			k->pts_mode = copies ? PTS_MODE_SOURCE : PTS_MODE_ARRIVAL;
			log_msg("%s: pacing: %s", k->name, copies
				? "pts through the decoder (TIMESTAMP_COPY)"
				: "arrival time (the decoder does not copy timestamps)");
			k->pts_mode_logged = true;
		}

		int64_t pts_us, arrival_us;
		bool synthetic = false;

		if (k->pts_mode == PTS_MODE_SOURCE) {
			/* Look up by captured pts instead of relying on queue
			 * order: if the decoder drops a corrupt frame, order-based
			 * pairing would be shifted permanently for the rest of the
			 * connection. Older, unmatched entries (packets that never
			 * produced a CAPTURE frame of their own) are discarded. */
			unsigned i = k->arrival_out;
			while (i < k->arrival_in
			       && k->arrival_queue[i % ARRIVAL_QUEUE_SIZE].pts_us != pts_captured_us)
				i++;

			if (i < k->arrival_in) {
				unsigned slot = i % ARRIVAL_QUEUE_SIZE;
				pts_us = k->arrival_queue[slot].pts_us;
				arrival_us = k->arrival_queue[slot].arrival_us;
				unsigned n = i - k->arrival_out;
				switch (pacing_unmatched_classify(k->first_frame_seen, n)) {
				case PACING_UNMATCHED_STARTUP:
					/* Packets ahead of the first IDR, or fed
					 * before the CAPTURE queue was set up, are
					 * dropped by the decoder — not corruption. */
					log_msg("%s: pacing: first frame after %u packet(s) (normal at connect)",
						k->name, n);
					break;
				case PACING_UNMATCHED_SKIPPED:
					log_msg("%s: pacing: %u packet(s) without a matching frame (the decoder skipped a corrupt frame)",
						k->name, n);
					break;
				case PACING_UNMATCHED_NONE:
					break;
				}
				k->arrival_out = i + 1;
			} else if (k->arrival_out < k->arrival_in) {
				/* No match — queue out of sync. Queue order is all
				 * that is left to fall back on. */
				unsigned slot = k->arrival_out % ARRIVAL_QUEUE_SIZE;
				pts_us = k->arrival_queue[slot].pts_us;
				arrival_us = k->arrival_queue[slot].arrival_us;
				k->arrival_out++;
			} else {
				/* Queue completely empty — no arrival entry to pair
				 * with. A SYNTHETIC timestamp (monotonic clock, not
				 * the source's pts timeline) must NEVER be fed into
				 * anchor, PLL or jitter histogram: a single such
				 * sample gave the PLL several seconds of error that
				 * decayed over ~70 frames, and can become the new
				 * minimum jitter in the anchor's 10 s window. See the
				 * synthetic branch further down, which skips all three
				 * filters and extrapolates a target time instead. */
				pts_us = arrival_us = now;
				synthetic = true;
			}
		} else {
			/* Arrival mode: the captured pts is meaningless (the
			 * decoder does not copy timestamps) — only queue order is
			 * left to pair with. */
			if (k->arrival_out < k->arrival_in) {
				unsigned slot = k->arrival_out % ARRIVAL_QUEUE_SIZE;
				pts_us = k->arrival_queue[slot].pts_us;
				arrival_us = k->arrival_queue[slot].arrival_us;
				k->arrival_out++;
			} else {
				pts_us = arrival_us = now;
			}
		}
		k->first_frame_seen = true;

		int64_t target_us;
		bool have_jitter = false;
		int64_t jitter_ms = 0;
		bool have_regulated_delta = false;
		int64_t regulated_delta_ms = 0;
		if (k->pts_mode == PTS_MODE_ARRIVAL) {
			/* No usable pts to anchor to — show shortly after arrival
			 * instead of pretending to compensate network jitter we
			 * cannot measure. The PLL (which regulates pts series) is
			 * meaningless here too. */
			target_us = pacing_target_time(arrival_us, 0, v->buffer_ms, k->delay_ms);
		} else if (synthetic) {
			/* Synthetic timestamp (see above): does NOT feed anchor,
			 * PLL or jitter histogram — but DOES read the anchor's
			 * current value (pacing_anchor_value), just like the normal
			 * branch below does with its measurement. Reading the
			 * anchor is harmless; only feeding it a synthetic sample
			 * is forbidden. Still give the frame a sensible target
			 * time that follows the timeline: the previous frame's
			 * REGULATED time + the latest known T̂ if we have one
			 * (otherwise T̂ is 0, i.e. the window is not full yet and
			 * the base not reliable), else arrival time + buffer — the
			 * same fallback as arrival mode above, no extrapolation.
			 * r_prev_frame_us is updated with the extrapolated value
			 * in the first case, so the NEXT synthetic frame (if there
			 * are several in a row) continues the timeline instead of
			 * recomputing from the same old base every time. */
			int64_t t_hat = pacing_pll_mean_delta_us(&k->pll);
			if (k->r_prev_frame_us >= 0 && t_hat > 0) {
				k->r_prev_frame_us += t_hat;
				target_us = pacing_target_time(k->r_prev_frame_us,
							       pacing_anchor_value(&k->anchor),
							       v->buffer_ms, k->delay_ms);
			} else {
				target_us = pacing_target_time(arrival_us, 0, v->buffer_ms, k->delay_ms);
			}
			k->m_synthetic++;
		} else {
			int64_t jitter = arrival_us - pts_us;
			pacing_anchor_push(&k->anchor, now, jitter);
			int64_t anchor = pacing_anchor_value(&k->anchor);

			/* The target time is computed on the REGULATED timeline
			 * (r_us), not raw pts — see pacing_pll_feed in pacing.h
			 * for why. The anchor above does NOT touch this; it keeps
			 * working on raw pts (sliding minimum of network jitter,
			 * independent of any clumping of the source timestamps). */
			int64_t r_us = pacing_pll_feed(&k->pll, pts_us);
			if (k->r_prev_frame_us >= 0) {
				regulated_delta_ms = (r_us - k->r_prev_frame_us) / 1000;
				have_regulated_delta = true;
			}
			k->r_prev_frame_us = r_us;

			target_us = pacing_target_time(r_us, anchor, v->buffer_ms, k->delay_ms);
			jitter_ms = (jitter - anchor) / 1000;
			have_jitter = true;
		}

		/* "Late" = the target time had already passed when the frame
		 * came out of the decoder (target_us < now) — not compared with
		 * the arrival time, which is network timing and says nothing
		 * about whether the compositor could show the frame in time.
		 * Written only by this thread (see the comment at m_decoded),
		 * no lock needed. */
		if (target_us < now)
			k->m_late++;

		/* fifo, m_dropped_full, m_depth_*, m_jitter and
		 * m_regulated_ptsdelta are shared with the compositor (report()
		 * reads/resets the last four) — all under the same `lock` the
		 * FIFO push needs anyway. */
		pthread_mutex_lock(&k->lock);
		struct pacing_frame dropped;
		bool kept = pacing_fifo_push(&k->fifo, (struct pacing_frame){
			.index = b.index, .target_us = target_us, .arrival_us = arrival_us }, &dropped);
		if (!kept) {
			ring_push(k, dropped.index);
			k->m_dropped_full++;
		}
		k->m_depth_sum += (unsigned long)pacing_fifo_count(&k->fifo);
		k->m_depth_count++;
		if (have_jitter)
			pacing_hist_add(&k->m_jitter, jitter_ms);
		if (have_regulated_delta)
			pacing_hist_add(&k->m_regulated_ptsdelta, regulated_delta_ms);
		pthread_mutex_unlock(&k->lock);
	}
}

static void requeue_returned(struct camera *k)
{
	for (;;) {
		pthread_mutex_lock(&k->lock);
		int i = ring_pop(k);
		pthread_mutex_unlock(&k->lock);
		if (i < 0)
			return;

		struct v4l2_plane pl = { 0 };
		struct v4l2_buffer b = {
			.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
			.memory = V4L2_MEMORY_MMAP,
			.index = i, .length = 1, .m.planes = &pl,
		};
		if (xioctl(k->v4l2fd, VIDIOC_QBUF, &b))
			log_msg("%s: QBUF requeue %d: %s", k->name, i, strerror(errno));
	}
}

/* Queues a deliberately leaked fb/handle/dmafd (see .leaked in struct
 * camera) for later cleanup. The caller must hold `k->lock` (shared with
 * complete_flip, which drains the list). If the list is (unusually) full
 * this is logged and the entry really leaks — better visible and rare than
 * a write outside the array. */
static void camera_leak_push(struct camera *k, uint32_t fb, uint32_t handle, int dmafd)
{
	if (k->n_leaked >= MAX_LEAKED) {
		log_msg("%s: CRITICAL: leak list full (%d) - fb %u/handle %u/dmafd %d leaks permanently",
			k->name, k->n_leaked, fb, handle, dmafd);
		return;
	}
	k->leaked[k->n_leaked++] = (struct leaked_buffer){
		.fb = fb, .handle = handle, .dmafd = dmafd,
	};
}

static void teardown_stream(struct wall *v, struct camera *k)
{
	pthread_mutex_lock(&k->lock);
	/* A frame the compositor popped from the fifo (its step A) but has not
	 * committed yet (step C) belongs to the buffers torn down below: the
	 * new generation makes step C drop it instead of committing its fb or
	 * pushing its index into the next connection's ring (see generation
	 * in struct camera). */
	k->generation++;
	pacing_fifo_init(&k->fifo);       /* contents released without QBUF, the queue is torn down anyway */
	k->ring_head = k->ring_tail = 0;

	/* Ask the compositor to detach the plane BEFORE we touch
	 * k->cap[i].fb — see the struct comment at `lock`/tearing_down/
	 * plane_attached/detached for the full rule. In short: set
	 * tearing_down, then wait (same lock, condvar) until the compositor
	 * has confirmed that no commit is in flight for the plane AND that the
	 * plane no longer shows any buffer. If the condition already holds
	 * (plane never attached, or already detached) the wait never runs —
	 * the while condition is false at once under the same lock
	 * tearing_down was set under, so no race with the compositor is
	 * possible. TEARDOWN_WAIT_S cap: the compositor may stand still (e.g.
	 * on quit, when its own while (!quit) loop soon stops committing for
	 * the same reason) — then give up and destroy the FB anyway instead of
	 * hanging forever. The cap is longer than the compositor's flip
	 * timeout (FLIP_TIMEOUT_US): an abandoned detach is NOT taken as
	 * confirmed (see abandon_flip) but made again, and this wait must
	 * outlast that second attempt. `detached` is broadcast whenever the
	 * compositor clears what we wait for: a confirmed detach
	 * (complete_flip), a failed commit that resets in_flight, a connector
	 * switch (planes_detached). A timeout re-checks the condition before
	 * giving up — it may have cleared exactly at the deadline, and giving
	 * up then would leak buffers for nothing. */
	k->tearing_down = true;
	/* Display disconnected (TV off, cable out): the detach may never be
	 * confirmed, and waiting TEARDOWN_WAIT_S would only stretch every
	 * reconnect. Skip the wait and leak what may still be on screen
	 * instead (below) - as long as the leak list has room for all of it,
	 * otherwise wait as usual. */
	int n_protect = (k->shown >= 0) + (k->in_flight >= 0) + k->n_limbo;
	bool skip_wait = (k->plane_attached || k->in_flight != -1)
			 && pacing_teardown_skip_wait(atomic_load(&v->display_down), k->n_leaked,
						      n_protect, MAX_LEAKED);
	bool gave_up = skip_wait;
	if (!skip_wait) {
		struct timespec deadline;
		clock_gettime(CLOCK_REALTIME, &deadline);
		deadline.tv_sec += TEARDOWN_WAIT_S;
		while ((k->plane_attached || k->in_flight != -1) && !quit) {
			if (pthread_cond_timedwait(&k->detached, &k->lock, &deadline) == ETIMEDOUT) {
				gave_up = k->plane_attached || k->in_flight != -1;
				break;
			}
		}
	}
	/* The wait may also have ended because quit was set while we waited
	 * (see the broadcast at the end of main()) — same "gave up" state as
	 * ETIMEDOUT, just sooner, and handled the same safe way below
	 * (keep[]/leak list, freed by main() after the threads are joined).
	 * That is a normal stop, though, not a fault: on quit the leak is not
	 * logged (stopping = true), so a clean shutdown never prints the
	 * CRITICAL line users are told to report. */
	bool stopping = quit;
	if (stopping && (k->plane_attached || k->in_flight != -1))
		gave_up = true;
	k->tearing_down = false;

	/* If we gave up while the plane may still be live (a commit in flight
	 * and/or the plane still showing a buffer, or indices parked in limbo
	 * by an abandoned flip), those INDICES must never be destroyed here —
	 * the exact bug class the rest of this function exists to close, just
	 * in the narrower "compositor stood still for 2 s" case (and since all
	 * cameras share the same flip gate, the WHOLE wall stalls then, not
	 * only this camera). k->in_flight is reset REGARDLESS a few lines
	 * down: the kernel guarantees that a commit already submitted
	 * delivers its flip event sooner or later, and complete_flip then
	 * reads in_flight == -1 and becomes a no-op for this camera (neither
	 * the -2 nor the >= 0 branch matches). plane_attached is therefore
	 * left true if it was (the plane may still show the leaked buffer;
	 * the NEXT teardown cycle, a rotation that idles the camera or a
	 * connector switch then makes a new, harmless (possibly redundant)
	 * detach) — and SET if a real frame was in flight: once that flip
	 * lands the plane shows it, but complete_flip, seeing -1, would never
	 * record that. Left false, nothing would ever detach the plane, and a
	 * connector switch (display_remodeset) would free the leaked buffer
	 * while it is still scanned out. The protected indices are leaked
	 * DELIBERATELY right here (no release_buffer for them in this
	 * function) — rare and visible through the log line below (except
	 * on a normal stop), better than a use-after-free against the
	 * display. The leak is NOT permanent though: fb/handle/dmafd are
	 * saved in k->leaked (see camera_leak_push) and cleaned up by
	 * complete_flip on the next confirmed flip for the camera, or at
	 * program exit — without that the fd/GEM handle would be lost for good the next time
	 * start_capture overwrote the same index. Limbo indices count as "may
	 * be on screen" exactly like shown/in_flight: the abandoned flip that
	 * parked them was never confirmed. */
	bool keep[CAPTURE_BUFFERS] = { false };
	int n_keep = 0;
	if (gave_up) {
		int cand[2 + MAX_LIMBO];
		int n_cand = 0;
		if (k->plane_attached && k->shown >= 0)
			cand[n_cand++] = k->shown;
		if (k->in_flight >= 0)
			cand[n_cand++] = k->in_flight;
		for (int j = 0; j < k->n_limbo && j < MAX_LIMBO; j++)
			cand[n_cand++] = k->limbo[j];
		for (int j = 0; j < n_cand; j++) {
			int c = cand[j];
			if (c < 0 || c >= CAPTURE_BUFFERS || c >= k->n_cap || keep[c])
				continue;
			keep[c] = true;
			n_keep++;
			/* Save fb/handle/dmafd in the leak list BEFORE k->cap[c]
			 * can be overwritten by the next start_capture. Done here,
			 * still under `lock` — the list is shared with
			 * complete_flip. From here on the list owns them:
			 * close_buffers skips index c (keep[]), and bufs_held is
			 * unchanged because the handle is still held. */
			camera_leak_push(k, k->cap[c].fb, k->cap[c].handle, k->cap[c].dmafd);
		}
		/* Not logged on a normal stop (stopping, see above). */
		bool log_leak = n_keep && !stopping;
		if (log_leak && skip_wait)
			log_msg("%s: display disconnected - not waiting for the plane to detach; %d "
				"buffer(s) that may still be on screen are freed after the next "
				"confirmed flip", k->name, n_keep);
		else if (log_leak)
			log_msg("%s: CRITICAL: teardown gave up (shown=%d in_flight=%d limbo=%d plane_attached=%d) - leaking %d live buffer(s) instead of destroying them",
				k->name, k->shown, k->in_flight, k->n_limbo, k->plane_attached, n_keep);
	}
	/* A real frame still in flight will be on screen once its flip lands
	 * (see above). Only after the candidates are chosen: a stale "shown"
	 * of a confirmed-detached plane must not be leaked for nothing. */
	if (k->in_flight >= 0)
		k->plane_attached = true;
	k->shown = -1;
	k->in_flight = -1;
	k->n_limbo = 0;   /* the indices die with the buffers below */
	/* complete_flip may have handed indices back while we waited (an old
	 * "shown", parked limbo indices): they belong to the buffers torn
	 * down below and must not be queued into the next connection. */
	k->ring_head = k->ring_tail = 0;
	pthread_mutex_unlock(&k->lock);
	k->arrival_in = k->arrival_out = 0;

	if (k->v4l2fd >= 0) {
		enum v4l2_buf_type t1 = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		enum v4l2_buf_type t2 = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(k->v4l2fd, VIDIOC_STREAMOFF, &t1);
		xioctl(k->v4l2fd, VIDIOC_STREAMOFF, &t2);
	}
	/* RmFB + close GEM handle + close dmafd for every index not kept. */
	close_buffers(v->drmfd, k, keep);
	if (k->v4l2fd >= 0) {
		close(k->v4l2fd);
		k->v4l2fd = -1;
	}
}

/* Maps a libavformat error to a fault class. */
static enum pacing_fault fault_from_averror(const struct camera *k, int err)
{
	if (k->stalled)
		return PACING_FAULT_STALL;
	if (err == AVERROR_HTTP_UNAUTHORIZED)
		return PACING_FAULT_UNAUTHORIZED;
	if (err == AVERROR_HTTP_FORBIDDEN || err == AVERROR(EACCES) || err == AVERROR(EPERM))
		return PACING_FAULT_FORBIDDEN;
	if (err == AVERROR_HTTP_NOT_FOUND || err == AVERROR(ENOENT))
		return PACING_FAULT_NOT_FOUND;
	if (err == AVERROR(ECONNREFUSED))
		return PACING_FAULT_REFUSED;
	if (err == AVERROR(ETIMEDOUT))
		return PACING_FAULT_TIMEOUT;
	if (err == AVERROR(EHOSTUNREACH) || err == AVERROR(ENETUNREACH)
	    || err == AVERROR(EHOSTDOWN) || err == AVERROR(ENETDOWN))
		return PACING_FAULT_UNREACHABLE;
	if (err == AVERROR_EOF || err == AVERROR(ECONNRESET) || err == AVERROR(EPIPE))
		return PACING_FAULT_EOF;
	if (err == AVERROR_EXIT)
		return PACING_FAULT_STALL;   /* interrupt callback, not by quit */
	/* A failed host name lookup comes back as a generic EIO; libavformat's
	 * error line in this thread says what it was (as in `rtspwall probe`). */
	return pacing_fault_refine(PACING_FAULT_OTHER, av_last_error());
}

/* Logs a failure through the camera's log collapse: the first occurrence
 * of a line, then at most one copy per LOG_COLLAPSE_US with the number of
 * suppressed repeats. `next_ms` (the backoff) is not part of the collapse
 * key, so a growing backoff does not defeat it. */
#define LOG_COLLAPSE_US (300 * 1000000LL)

static void log_fault(struct camera *k, enum pacing_fault fault, const char *what,
		      int64_t next_ms)
{
	char key[PACING_DEDUP_KEY_MAX + 128];
	const char *hint = pacing_fault_hint(fault);
	unsigned rep;

	if (fault == PACING_FAULT_OTHER || fault == PACING_FAULT_NONE)
		snprintf(key, sizeof key, "%s", what);
	else
		snprintf(key, sizeof key, "%s: %s%s%s", pacing_fault_short(fault), what,
			 *hint ? " - " : "", hint);
	if (!pacing_dedup_check(&k->logdd, key, monotonic_us(), &rep))
		return;

	char again[64] = "";
	if (rep)
		snprintf(again, sizeof again, " [repeated %u times in the last %d min]",
			 rep, (int)(LOG_COLLAPSE_US / 60000000));
	if (next_ms > 0)
		log_msg("%s: %s; next attempt in %lld s%s", k->name, key,
			(long long)((next_ms + 999) / 1000), again);
	else
		log_msg("%s: %s; reconnecting%s", k->name, key, again);
}

/* File inputs (demo clips) only: waits until the packet is due at the
 * clip's natural speed (like ffmpeg -re), keeping the decoder serviced
 * meanwhile. Uses dts (monotonic in decode order), else pts. */
static void pace_file_packet(struct wall *v, struct camera *k, struct pacing_filepace *fp,
			     const AVPacket *pkt, AVRational tb)
{
	int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

	if (ts == AV_NOPTS_VALUE)
		return;   /* no timing at all: nothing to pace by */
	int64_t due = pacing_filepace_due(fp, av_rescale_q(ts, tb, AV_TIME_BASE_Q),
					  monotonic_us());
	for (;;) {
		int64_t now = monotonic_us();
		if (quit || now >= due)
			break;
		if (k->capture_on) {
			collect_decoded(v, k);
			requeue_returned(k);
		}
		int64_t wait = due - now;
		usleep((useconds_t)(wait > 5000 ? 5000 : wait));
	}
	k->last_packet_us = monotonic_us();   /* the wait is not a stall */
}

/* libavformat whitelists (see camera_thread). "sdp"/"rtp" for RTSP's
 * internal RTP handling; the file demuxer names match the demuxers' name
 * lists ("mov,mp4,m4a,3gp,3g2,mj2", "matroska,webm"). */
#define PROTOCOLS_NET "rtsp,rtsps,tcp,udp,tls,rtp,srtp,crypto"
#define FORMATS_NET   "rtsp,sdp,rtp"
#define FORMATS_FILE  "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm"

/* Rewinds a file input to its start for the next loop. */
static bool rewind_file(AVFormatContext *fc, int vstream)
{
	int64_t start = fc->streams[vstream]->start_time;
	if (start == AV_NOPTS_VALUE)
		start = 0;
	return av_seek_frame(fc, vstream, start, AVSEEK_FLAG_BACKWARD) >= 0;
}

void *camera_thread(void *arg)
{
	struct thread_arg *ta = arg;
	struct wall *v = ta->v;
	struct camera *k = ta->k;
	free(ta);

	/* Decided once: a local file is paced and looped, a network stream
	 * never is; RTSP options only for rtsp(s)://. */
	const bool live_src = layout_url_is_live(k->url);
	const bool rtsp = layout_url_is_rtsp(k->url);
	char masked[LAYOUT_URL_MAX];
	layout_mask_url(k->url, masked, sizeof masked);

	struct pacing_backoff backoff;  /* failed attempts in a row, per fault */
	pacing_backoff_init(&backoff);
	pacing_dedup_init(&k->logdd, LOG_COLLAPSE_US);
	atomic_store(&k->st_state, PACING_CAM_CONNECTING);
	if (!live_src)
		log_msg("%s: local file - played at its natural speed and looped", k->name);

	while (!quit) {
		AVFormatContext *fc = NULL;
		AVDictionary *opt = NULL;
		AVBSFContext *bsf = NULL;
		AVPacket *pkt = NULL;
		int vstream = -1;
		enum pacing_fault fault = PACING_FAULT_NONE;
		char what[LAYOUT_URL_MAX + 160] = "";
		struct pacing_filepace fp;

		pacing_filepace_reset(&fp);
		k->stalled = false;
		k->live_since_us = 0;

		fc = avformat_alloc_context();
		if (!fc) {
			fault = PACING_FAULT_OTHER;
			snprintf(what, sizeof what, "avformat_alloc_context failed");
			goto retry;
		}
		fc->interrupt_callback.callback = interrupt_cb;
		fc->interrupt_callback.opaque = k;

		/* More generous limit during open/find_stream_info (DNS, TCP
		 * handshake, waiting for SPS/PPS) than while streaming. */
		k->watchdog_limit_us = 10 * 1000000;
		k->last_packet_us = monotonic_us();

		if (rtsp) {
			av_dict_set(&opt, "rtsp_transport", "tcp", 0);
			/* "stimeout" does not exist in FFmpeg 7.1 — the right
			 * option is "timeout" (µs, default 0 = no limit); older
			 * code using "stimeout" had it silently ignored. Our own
			 * watchdog above (interrupt_callback) is what actually
			 * protects us, but we still set the real option as a
			 * first line of defence. */
			av_dict_set(&opt, "timeout", "5000000", 0);        /* 5 s */
			/* Video only: no SETUP for the audio track. Besides
			 * saving bandwidth this keeps the codec whitelist below
			 * from failing (and logging) on every camera with audio
			 * while avformat_find_stream_info probes it, which also
			 * cost ~4 s per connect. */
			av_dict_set(&opt, "allowed_media_types", "video", 0);
		}
		if (live_src) {
			av_dict_set(&opt, "max_delay", "500000", 0);
			av_dict_set(&opt, "fflags", "nobuffer", 0);
		}
		/* Defence in depth: libavformat would otherwise follow whatever
		 * a URL or a server hands it (other protocols, any demuxer, any
		 * decoder during probing). Only what a camera wall needs: RTSP(S)
		 * over TCP/UDP/TLS for network streams, local files for the demo
		 * clips, the MP4/Matroska demuxers for those, H.264 only. */
		if (live_src) {
			av_dict_set(&opt, "protocol_whitelist", PROTOCOLS_NET, 0);
			av_dict_set(&opt, "format_whitelist", FORMATS_NET, 0);
		} else {
			av_dict_set(&opt, "protocol_whitelist", "file", 0);
			av_dict_set(&opt, "format_whitelist", FORMATS_FILE, 0);
		}
		av_dict_set(&opt, "codec_whitelist", "h264", 0);

		k->connections++;

		/* New connection, new pacing: a pts series only belongs together
		 * within one connection, and an old anchor/PLL measurement (or
		 * the detection of whether the decoder copies timestamps) from
		 * the previous stream says nothing about the new one.
		 * arrival_in/arrival_out are NOT touched here — see the comment
		 * at them in struct camera: they have already been reset by
		 * teardown_stream (run just before the loop brought us back here)
		 * or by load_config (first round). */
		pacing_anchor_init(&k->anchor, ANCHOR_WINDOW_US);
		k->last_pts_us = -1;
		k->pts_mode = PTS_MODE_UNKNOWN;
		k->pts_mode_logged = false;
		k->first_frame_seen = false;
		pacing_pll_init(&k->pll);
		k->r_prev_frame_us = -1;

		av_last_error_clear();
		int r = avformat_open_input(&fc, k->url, NULL, &opt);
		if (r < 0) {
			/* Only ever log the masked URL — it may carry credentials. */
			fault = fault_from_averror(k, r);
			snprintf(what, sizeof what, "cannot open %s (%s)", masked, av_err2str(r));
			av_dict_free(&opt);
			goto retry;
		}

		/* avformat_open_input removes the keys it recognises from opt —
		 * what remains are misspelt or non-existent options (like
		 * "stimeout" on FFmpeg 7.1). Silently ignored options are exactly
		 * the kind of bug that creeps back in, so log them (once). */
		{
			const AVDictionaryEntry *e = NULL;
			while ((e = av_dict_get(opt, "", e, AV_DICT_IGNORE_SUFFIX)))
				if (k->connections == 1)
					log_msg("%s: unknown option ignored: %s", k->name, e->key);
		}
		av_dict_free(&opt);

		/* A local file's other streams (AAC audio, a second video stream
		 * in another codec) are never used, but avformat_find_stream_info
		 * would try to open their decoders - and log "Codec (aac) not on
		 * whitelist" errors on every loop/reconnect. The container already
		 * names each stream's codec, so pick the stream now (the same
		 * choice as below), then discard the others and hide their codec:
		 * probing looks at the chosen stream only (the probe itself stays,
		 * it is what gives Matroska packets their dts). */
		if (!live_src) {
			int pick = av_pick_video_stream(fc);
			for (unsigned i = 0; i < fc->nb_streams; i++)
				if ((int)i != pick) {
					fc->streams[i]->discard = AVDISCARD_ALL;
					fc->streams[i]->codecpar->codec_id = AV_CODEC_ID_NONE;
				}
		}
		av_last_error_clear();
		r = avformat_find_stream_info(fc, NULL);
		if (r < 0) {
			fault = fault_from_averror(k, r);
			snprintf(what, sizeof what, "no stream information from %s (%s)", masked,
				 av_err2str(r));
			goto retry;
		}

		/* From here on we are streaming — a healthy camera sends packets
		 * several times a second, so the limit can be tightened. */
		k->watchdog_limit_us = 5 * 1000000;
		k->last_packet_us = monotonic_us();

		/* The same choice as `rtspwall probe` (avstream.h): the first
		 * H.264 video stream, else the first video stream. */
		vstream = av_pick_video_stream(fc);
		if (vstream < 0 || fc->streams[vstream]->codecpar->codec_id != AV_CODEC_ID_H264) {
			if (vstream >= 0) {
				fault = PACING_FAULT_CODEC;
				snprintf(what, sizeof what, "the stream is %s, not H.264",
					 avcodec_get_name(fc->streams[vstream]->codecpar->codec_id));
			} else {
				fault = PACING_FAULT_OTHER;
				snprintf(what, sizeof what, "no video in the stream from %s", masked);
			}
			goto retry;
		}

		AVCodecParameters *cp = fc->streams[vstream]->codecpar;
		if (cp->width > 1920 || cp->height > 1920) {
			fault = PACING_FAULT_TOO_LARGE;
			snprintf(what, sizeof what, "the stream is %dx%d", cp->width, cp->height);
			goto retry;
		}
		unsigned failures = pacing_backoff_total(&backoff);
		if (failures)
			log_msg("%s: connected, %dx%d (after %u failed attempt%s)", k->name,
				cp->width, cp->height, failures, failures == 1 ? "" : "s");
		else
			log_msg("%s: connected, %dx%d", k->name, cp->width, cp->height);

		/* RTSP normally delivers Annex-B already. If the extradata is AVCC
		 * (starts with 1) the packets must be converted (MP4 files). */
		if (cp->extradata_size > 0 && cp->extradata[0] == 1) {
			const AVBitStreamFilter *f = av_bsf_get_by_name("h264_mp4toannexb");
			if (f && av_bsf_alloc(f, &bsf) == 0) {
				avcodec_parameters_copy(bsf->par_in, cp);
				if (av_bsf_init(bsf) < 0) {
					av_bsf_free(&bsf);
					bsf = NULL;
				}
			}
		}

		if (open_decoder(v->cfg.decoder, k) < 0) {
			fault = PACING_FAULT_DECODER;
			snprintf(what, sizeof what, "the hardware decoder %s could not be set up",
				 v->cfg.decoder);
			goto retry;
		}

		/* SPS/PPS first, otherwise the decoder knows nothing. Never goes
		 * into arrival_queue (arrival_us = -1) — that packet never
		 * produces a CAPTURE frame of its own to pair it with. */
		if (!bsf && cp->extradata_size > 0) {
			AVPacket *ex = av_packet_alloc();
			if (ex && av_new_packet(ex, cp->extradata_size) == 0) {
				memcpy(ex->data, cp->extradata, cp->extradata_size);
				feed_packet(v, k, ex, 0, -1);
			}
			av_packet_free(&ex);
		}

		pkt = av_packet_alloc();
		if (!pkt) {
			fault = PACING_FAULT_OTHER;
			snprintf(what, sizeof what, "av_packet_alloc failed");
			goto retry;
		}

		while (!quit) {
			r = av_read_frame(fc, pkt);
			if (r < 0) {
				/* A clip loops: rewind and continue in the same
				 * connection (compute_pts re-anchors on the
				 * backwards pts, the file pacing re-bases). */
				if (!live_src && r == AVERROR_EOF && rewind_file(fc, vstream))
					continue;
				fault = fault_from_averror(k, r);
				if (fault == PACING_FAULT_STALL)
					snprintf(what, sizeof what, "no data for %lld s",
						 (long long)(k->watchdog_limit_us / 1000000));
				else
					snprintf(what, sizeof what, "stream broke (%s)", av_err2str(r));
				break;
			}
			k->last_packet_us = monotonic_us();   /* sign of life for the watchdog */
			if (pkt->stream_index != vstream) {
				av_packet_unref(pkt);
				continue;
			}

			if (!live_src)
				pace_file_packet(v, k, &fp, pkt, fc->streams[vstream]->time_base);

			int64_t arrival_us = monotonic_us();
			int64_t pts_us = compute_pts(k, pkt, fc->streams[vstream]->time_base,
						     arrival_us, live_src);

			if (bsf) {
				if (av_bsf_send_packet(bsf, pkt) == 0) {
					while (av_bsf_receive_packet(bsf, pkt) == 0) {
						if (feed_packet(v, k, pkt, pts_us, arrival_us) < 0) {
							av_packet_unref(pkt);
							goto decoder_failed;
						}
						av_packet_unref(pkt);
					}
				}
			} else {
				if (feed_packet(v, k, pkt, pts_us, arrival_us) < 0) {
					av_packet_unref(pkt);
					goto decoder_failed;
				}
			}
			av_packet_unref(pkt);

			if (!check_events(v, k))
				goto decoder_failed;

			if (k->capture_on) {
				collect_decoded(v, k);
				requeue_returned(k);
			}
		}
		goto retry;

decoder_failed:
		fault = PACING_FAULT_OTHER;
		snprintf(what, sizeof what, "decoder error while streaming");

retry:
		av_packet_free(&pkt);
		if (bsf)
			av_bsf_free(&bsf);
		teardown_stream(v, k);
		if (fc)
			avformat_close_input(&fc);
		if (quit)
			break;

		/* A stream that was live for a while and then broke reconnects
		 * at once (fast recovery after a camera/NVR blip), and a long
		 * healthy period also forgets the collapsed log lines. Anything
		 * else counts as a failed attempt and backs off by fault class. */
		int64_t now = monotonic_us();
		bool was_healthy = k->live_since_us && now - k->live_since_us >= 10 * 1000000LL;
		if (k->live_since_us)
			pacing_backoff_mark_live(&backoff, now);   /* 401/404 grace starts */
		if (k->live_since_us && now - k->live_since_us >= 60 * 1000000LL)
			pacing_dedup_reset(&k->logdd);
		k->live_since_us = 0;

		if (fault == PACING_FAULT_NONE)
			fault = PACING_FAULT_OTHER;
		atomic_store(&k->st_fault, fault);
		atomic_store(&k->st_state, PACING_CAM_FAILING);

		if (was_healthy) {
			pacing_backoff_reset(&backoff);
			log_fault(k, fault, what, 0);
			continue;
		}
		int64_t delay_ms = pacing_backoff_next(&backoff, fault, now);
		log_fault(k, fault, what, delay_ms);
		for (int64_t waited = 0; waited < delay_ms && !quit; waited += 100)
			usleep(100000);
	}
	return NULL;
}
