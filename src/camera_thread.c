/*
 * camera_thread.c — the free-index ring + the whole camera thread: RTSP
 * demux (libavformat), feeding the decoder, collecting decoded frames into
 * the jitter buffer, and the full connect/read/close loop per camera. See
 * rpi4rtsp.h for the structs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
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

#include "rpi4rtsp.h"

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
		log_msg("%s: no data for %.1f s - dropping the connection",
			k->name, since / 1e6);
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
 * jump also replaces pts with the arrival time — it is no longer reliable
 * as an anchor base — AND re-anchors, just like a large forward jump
 * (> 1 s), where the pts value itself is still used but the old jitter
 * measurement says nothing about the new situation. last_pts_us is always
 * saved as exactly the value the function returns (never the discarded,
 * deviating pts value) — otherwise the next packet is compared against a
 * reference already rejected, forcing a needless extra re-anchor even if
 * the next pts is perfectly fine.
 *
 * Re-anchoring also resets the PLL (pacing_pll_init) — an old regulated
 * timeline from the previous pts base says nothing about the new one. */
static int64_t compute_pts(struct camera *k, const AVPacket *pkt,
			   AVRational tb, int64_t arrival_us)
{
	if (pkt->pts == AV_NOPTS_VALUE)
		return arrival_us;

	int64_t pts_us = av_rescale_q(pkt->pts, tb, AV_TIME_BASE_Q);
	int64_t used_us = pts_us;

	if (k->last_pts_us >= 0) {
		int64_t diff = pts_us - k->last_pts_us;
		if (diff < 0 || diff > 1000000) {
			log_msg("%s: pts %s (%.3f s) - re-anchoring",
				k->name, diff < 0 ? "went backwards" : "jumped", diff / 1e6);
			pacing_anchor_init(&k->anchor, ANCHOR_WINDOW_US);
			pacing_pll_init(&k->pll);
			k->r_prev_frame_us = -1;
			if (diff < 0)
				used_us = arrival_us;
		}
	}
	k->last_pts_us = used_us;
	return used_us;
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
				if (i > k->arrival_out)
					log_msg("%s: pacing: %u packet(s) without a matching frame (the decoder skipped a corrupt frame)",
						k->name, i - k->arrival_out);
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

/* Queues a deliberately leaked fb/dmafd (see .leaked in struct camera) for
 * later cleanup. The caller must hold `k->lock` (shared with complete_flip,
 * which drains the list). If the list is (unusually) full this is logged
 * and the entry really leaks — better visible and rare than a write outside
 * the array. */
static void camera_leak_push(struct camera *k, uint32_t fb, int dmafd)
{
	if (k->n_leaked >= MAX_LEAKED) {
		log_msg("%s: CRITICAL: leak list full (%d) - fb %u/dmafd %d leaks permanently",
			k->name, k->n_leaked, fb, dmafd);
		return;
	}
	k->leaked[k->n_leaked++] = (struct leaked_buffer){ .fb = fb, .dmafd = dmafd };
}

static void teardown_stream(struct wall *v, struct camera *k)
{
	pthread_mutex_lock(&k->lock);
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
	 * possible. 2 s cap: the compositor may stand still (e.g. on quit,
	 * when its own while (!quit) loop soon stops committing for the same
	 * reason) — then give up and destroy the FB anyway instead of hanging
	 * forever. */
	k->tearing_down = true;
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 2;
	bool gave_up = false;
	while ((k->plane_attached || k->in_flight != -1) && !quit) {
		if (pthread_cond_timedwait(&k->detached, &k->lock, &deadline) == ETIMEDOUT) {
			gave_up = true;
			break;
		}
	}
	/* The wait may also have ended because quit was set while we waited
	 * (see the broadcast at the end of main()) — same "gave up" state as
	 * ETIMEDOUT, just sooner. */
	if (quit && (k->plane_attached || k->in_flight != -1))
		gave_up = true;
	k->tearing_down = false;

	/* If we gave up while the plane may still be live (a commit in flight
	 * and/or the plane still showing a buffer), those INDICES must never be
	 * destroyed here — the exact bug class the rest of this function exists
	 * to close, just in the narrower "compositor stood still for 2 s" case
	 * (and since all cameras share the same flip gate, the WHOLE wall
	 * stalls then, not only this camera). k->in_flight and
	 * k->plane_attached are NOT adjusted based on gave_up — they are reset
	 * REGARDLESS one line down, but that is harmless: the kernel
	 * guarantees that a commit already submitted delivers its flip event
	 * sooner or later, and complete_flip then reads in_flight == -1 and
	 * becomes a no-op for this camera (neither the -2 nor the >= 0 branch
	 * matches) — plane_attached stays what it was (conservative: if it is
	 * still true the plane may still show the leaked buffer, and the NEXT
	 * teardown cycle for the same camera then attempts a new, harmless
	 * (possibly redundant) detach). The protected indices are leaked
	 * DELIBERATELY right here (neither RmFB nor close(dmafd) in this
	 * function) — rare and visible through the CRITICAL line below, better
	 * than a use-after-free against the display. The leak is NOT
	 * permanent though: fb/dmafd are saved in k->leaked (see
	 * camera_leak_push) and cleaned up by complete_flip on the next
	 * confirmed flip for the camera, or at program exit — without that the
	 * fd/GEM handle would be lost for good the next time start_capture
	 * overwrote the same index. */
	int keep_a = -1, keep_b = -1;
	if (gave_up) {
		if (k->plane_attached)
			keep_a = k->shown;
		if (k->in_flight >= 0)
			keep_b = k->in_flight;
		if (keep_a >= 0 || keep_b >= 0)
			log_msg("%s: CRITICAL: teardown gave up (shown=%d in_flight=%d plane_attached=%d) - leaking a live buffer instead of destroying it",
				k->name, k->shown, k->in_flight, k->plane_attached);
		/* Save fb/dmafd in the leak list BEFORE k->cap[keep_x] can be
		 * overwritten by the next start_capture. Done here, still under
		 * `lock` — the list is shared with complete_flip. */
		if (keep_a >= 0)
			camera_leak_push(k, k->cap[keep_a].fb, k->cap[keep_a].dmafd);
		if (keep_b >= 0 && keep_b != keep_a)
			camera_leak_push(k, k->cap[keep_b].fb, k->cap[keep_b].dmafd);
	}
	k->shown = -1;
	k->in_flight = -1;
	pthread_mutex_unlock(&k->lock);
	k->arrival_in = k->arrival_out = 0;

	if (k->v4l2fd >= 0) {
		enum v4l2_buf_type t1 = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		enum v4l2_buf_type t2 = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		xioctl(k->v4l2fd, VIDIOC_STREAMOFF, &t1);
		xioctl(k->v4l2fd, VIDIOC_STREAMOFF, &t2);
	}
	for (int i = 0; i < k->n_cap; i++) {
		if (i == keep_a || i == keep_b)
			continue;
		if (k->cap[i].fb) {
			drmModeRmFB(v->drmfd, k->cap[i].fb);
			k->cap[i].fb = 0;
		}
	}
	close_buffers(k, keep_a, keep_b);
	if (k->v4l2fd >= 0) {
		close(k->v4l2fd);
		k->v4l2fd = -1;
	}
}

void *camera_thread(void *arg)
{
	struct thread_arg *ta = arg;
	struct wall *v = ta->v;
	struct camera *k = ta->k;
	free(ta);

	while (!quit) {
		AVFormatContext *fc = NULL;
		AVDictionary *opt = NULL;
		AVBSFContext *bsf = NULL;
		int vstream = -1;

		fc = avformat_alloc_context();
		if (!fc) {
			log_msg("%s: avformat_alloc_context failed", k->name);
			goto retry;
		}
		fc->interrupt_callback.callback = interrupt_cb;
		fc->interrupt_callback.opaque = k;

		/* More generous limit during open/find_stream_info (DNS, TCP
		 * handshake, waiting for SPS/PPS) than while streaming. */
		k->watchdog_limit_us = 10 * 1000000;
		k->last_packet_us = monotonic_us();

		av_dict_set(&opt, "rtsp_transport", "tcp", 0);
		/* "stimeout" does not exist in FFmpeg 7.1 — the right option is
		 * "timeout" (µs, default 0 = no limit); older code using
		 * "stimeout" had it silently ignored. Our own watchdog above
		 * (interrupt_callback) is what actually protects us, but we still
		 * set the real option as a first line of defence. */
		av_dict_set(&opt, "timeout", "5000000", 0);        /* 5 s */
		av_dict_set(&opt, "max_delay", "500000", 0);
		av_dict_set(&opt, "fflags", "nobuffer", 0);

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
		pacing_pll_init(&k->pll);
		k->r_prev_frame_us = -1;

		int open_err = avformat_open_input(&fc, k->url, NULL, &opt);
		if (open_err < 0) {
			/* Only ever log the masked URL — it may carry credentials. */
			char masked[LAYOUT_URL_MAX];
			layout_mask_url(k->url, masked, sizeof masked);
			log_msg("%s: cannot open %s: %s", k->name, masked, av_err2str(open_err));
			av_dict_free(&opt);
			goto retry;
		}

		/* avformat_open_input removes the keys it recognises from opt —
		 * what remains are misspelt or non-existent options (like
		 * "stimeout" on FFmpeg 7.1). Silently ignored options are exactly
		 * the kind of bug that creeps back in, so log them. */
		{
			const AVDictionaryEntry *e = NULL;
			while ((e = av_dict_get(opt, "", e, AV_DICT_IGNORE_SUFFIX)))
				log_msg("%s: unknown option ignored: %s", k->name, e->key);
		}
		av_dict_free(&opt);

		if (avformat_find_stream_info(fc, NULL) < 0) {
			log_msg("%s: find_stream_info failed", k->name);
			goto retry;
		}

		/* From here on we are streaming — a healthy camera sends packets
		 * several times a second, so the limit can be tightened. */
		k->watchdog_limit_us = 5 * 1000000;
		k->last_packet_us = monotonic_us();

		for (unsigned i = 0; i < fc->nb_streams; i++)
			if (fc->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO
			    && fc->streams[i]->codecpar->codec_id == AV_CODEC_ID_H264)
				vstream = i;

		if (vstream < 0) {
			log_msg("%s: no H.264 video in the stream", k->name);
			goto retry;
		}

		AVCodecParameters *cp = fc->streams[vstream]->codecpar;
		log_msg("%s: connected, %dx%d", k->name, cp->width, cp->height);

		/* RTSP normally delivers Annex-B already. If the extradata is AVCC
		 * (starts with 1) the packets must be converted. */
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

		if (open_decoder(v->cfg.decoder, k, cp->width ? cp->width : 1024,
				 cp->height ? cp->height : 576) < 0)
			goto retry;

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

		AVPacket *pkt = av_packet_alloc();
		if (!pkt) {
			log_msg("%s: av_packet_alloc failed", k->name);
			goto retry;
		}

		while (!quit) {
			int r = av_read_frame(fc, pkt);
			if (r < 0) {
				log_msg("%s: stream broke (%s)", k->name, av_err2str(r));
				break;
			}
			k->last_packet_us = monotonic_us();   /* sign of life for the watchdog */
			if (pkt->stream_index != vstream) {
				av_packet_unref(pkt);
				continue;
			}

			int64_t arrival_us = monotonic_us();
			int64_t pts_us = compute_pts(k, pkt, fc->streams[vstream]->time_base,
						     arrival_us);

			if (bsf) {
				if (av_bsf_send_packet(bsf, pkt) == 0) {
					while (av_bsf_receive_packet(bsf, pkt) == 0) {
						if (feed_packet(v, k, pkt, pts_us, arrival_us) < 0) {
							av_packet_unref(pkt);
							goto close_stream;
						}
						av_packet_unref(pkt);
					}
				}
			} else {
				if (feed_packet(v, k, pkt, pts_us, arrival_us) < 0) {
					av_packet_unref(pkt);
					goto close_stream;
				}
			}
			av_packet_unref(pkt);

			if (!check_events(v, k))
				goto close_stream;

			if (k->capture_on) {
				collect_decoded(v, k);
				requeue_returned(k);
			}
		}

close_stream:
		av_packet_free(&pkt);
		if (bsf)
			av_bsf_free(&bsf);
		teardown_stream(v, k);
		if (fc)
			avformat_close_input(&fc);
		continue;

retry:
		if (bsf)
			av_bsf_free(&bsf);
		teardown_stream(v, k);
		if (fc)
			avformat_close_input(&fc);
		for (int i = 0; i < 10 && !quit; i++)
			usleep(100000);
	}
	return NULL;
}
