/*
 * rtspwall.h — shared structs and function declarations for rtspwall.
 *
 *   main.c           main(), command line, log/xioctl/monotonic_us, signals
 *   drm.c            DRM helpers (display, connector, planes, properties,
 *                    primary plane)
 *   v4l2.c           V4L2 helpers (the decoder, its buffers)
 *   camera_thread.c  the free-index ring + the whole per-camera thread
 *   compositor.c     the atomic commit loop + the 60-second statistics
 *   config.c         reading the config file, applying the layout, forming
 *                    rotation groups, --check-config
 *   layout.{c,h}     pure config parsing and tile geometry (testable)
 *   pacing.{c,h}     pure pacing/rotation logic (testable)
 */
#ifndef RTSPWALL_H
#define RTSPWALL_H

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <xf86drmMode.h>

#include "layout.h"
#include "pacing.h"

#define MAX_CAMERAS      LAYOUT_MAX_CAMERAS
#define OUTPUT_BUFFERS    8      /* compressed H.264 buffers into the decoder */
#define CAPTURE_BUFFERS  16      /* decoded NV12 buffers out of the decoder.
                                  * Raised from 10: the jitter buffer holds
                                  * several decoded frames queued while they
                                  * wait for their target time instead of a
                                  * single "latest wins" slot, so more buffers
                                  * can be busy at once. PACING_FIFO_MAX
                                  * (pacing.h) is 20, with margin over this. */
#define OUTPUT_BUFFER_SIZE  (1024 * 1024)

/* Indices parked by the compositor after an abandoned page flip (see
 * abandon_flip in compositor.c): the previous "shown" per abandoned flip,
 * drained by the next confirmed flip. An index can be parked only once
 * (it leaves limbo only through the ring), so CAPTURE_BUFFERS slots can
 * never overflow, however many flips in a row are abandoned. */
#define MAX_LIMBO    CAPTURE_BUFFERS

/* Leaked buffers (see struct camera .leaked below): a teardown that gives
 * up (or skips the wait because the display is disconnected) leaks every
 * index that may still be on screen - shown, in flight and the limbo
 * indices, at most CAPTURE_BUFFERS distinct ones. Twice that leaves room
 * for a second such teardown before a confirmed flip drains the list. */
#define MAX_LEAKED   (2 * CAPTURE_BUFFERS)

/* A page flip without an event for this long is abandoned (compositor.c). */
#define FLIP_TIMEOUT_US   (2 * 1000000LL)

/* teardown_stream waits this long for the plane to be confirmed detached.
 * Longer than FLIP_TIMEOUT_US plus a second detach attempt, so a flip
 * that times out does not make the camera thread give up (and leak) at
 * the same moment the compositor retries the detach. Not waited at all
 * while the display is disconnected (wall.display_down, see
 * pacing_teardown_skip_wait). */
#define TEARDOWN_WAIT_S   5

#define ANCHOR_WINDOW_US     (10 * 1000000)   /* sliding minimum over 10 s */
#define ARRIVAL_QUEUE_SIZE   32                /* > OUTPUT_BUFFERS, margin */

/* ---------------------------------------------------------------- signal */

/* Set by the signal handler (main.c) and the broadcast at the end of main()
 * — read by camera_thread()/teardown_stream (camera_thread.c) and
 * compositor() (compositor.c) to leave their loops/waits. */
extern volatile sig_atomic_t quit;

/* -------------------------------------------------------------- data structures */

/* CAPTURE buffers own three DRM/dmabuf resources, released together by
 * release_buffer (v4l2.c): fb == 0, handle == 0 and dmafd == -1 each mean
 * "nothing to do" for that part. The GEM handle matters most: it holds a
 * reference to the imported dmabuf (and the decoder's CMA buffer behind
 * it) that neither drmModeRmFB nor close(dmafd) drops — only
 * drmCloseBufferHandle does, or closing the DRM fd. */
struct buffer {
	void     *map;          /* mmap'ed address (only OUTPUT needs it) */
	size_t    length;
	int       dmafd;        /* dmabuf fd (only CAPTURE), -1 = none */
	uint32_t  handle;       /* GEM handle from drmPrimeFDToHandle, 0 = none */
	uint32_t  fb;           /* DRM framebuffer built from the dmabuf, 0 = none */
};

/* A DELIBERATELY leaked fb/handle/dmafd (see .leaked in struct camera).
 * Same "nothing to do" convention as struct buffer above. */
struct leaked_buffer {
	uint32_t  fb;
	uint32_t  handle;
	int       dmafd;
};

struct camera {
	char      name[LAYOUT_NAME_MAX];
	char      url[LAYOUT_URL_MAX];
	int       x, y, width, height;    /* the tile on screen */

	/* V4L2 */
	int              v4l2fd;
	struct buffer    out[OUTPUT_BUFFERS];
	struct buffer    cap[CAPTURE_BUFFERS];
	int              n_out, n_cap;
	int              free_out[OUTPUT_BUFFERS];   /* stack of free OUTPUT indices */
	int              n_free;
	unsigned         fb_width, fb_height;        /* decoder buffer, padded */
	unsigned         vis_x, vis_y;               /* visible part of the buffer ... */
	unsigned         vis_width, vis_height;      /* ... e.g. 1080 of 1088 lines */
	bool             capture_on;

	/* DRM */
	uint32_t  plane_id;
	uint32_t  p_fb, p_crtc, p_crtc_x, p_crtc_y, p_crtc_w, p_crtc_h;
	uint32_t  p_src_x, p_src_y, p_src_w, p_src_h;

	/* Hand-over between the camera thread and the compositor, plus the
	 * statistics fields the camera thread fills in at the same moment it
	 * takes `lock` for the FIFO push anyway (m_jitter,
	 * m_regulated_ptsdelta, m_depth_sum/count — see collect_decoded and
	 * report).
	 *
	 * fifo      — jitter buffer: decoded frames waiting for their target
	 *             time (pacing_frame.target_us), in target-time order
	 *             (same order as pts, which is monotonic within a
	 *             connection)
	 * shown     — buffer index currently on screen, -1 = none
	 * in_flight — what the pending, unconfirmed commit is about:
	 *             >= 0  buffer index, a real frame is on its way up; it
	 *                   becomes "shown" only when the flip event confirms
	 *                   it is actually on screen (see complete_flip) — that
	 *                   step closes the window where the decoder could
	 *                   otherwise write into a buffer still being scanned
	 *                   out.
	 *             -2    detach commit (FB_ID=0/CRTC_ID=0) in flight, see
	 *                   tearing_down/plane_attached/detached below.
	 *             -1    nothing in flight.
	 * returned  — ring of indices the compositor has released and the
	 *             camera thread should queue back to the decoder
	 *
	 * The camera thread owns all V4L2 calls. The compositor never touches
	 * v4l2fd — it hands indices back through the ring instead. fifo,
	 * shown, in_flight, the ring, leaked/n_leaked and m_jitter/
	 * m_regulated_ptsdelta/m_depth_sum/m_depth_count are shared between
	 * the two threads and all protected by `lock`. The remaining m_ and
	 * diag_ statistics fields (further down) have only ONE writing thread
	 * each and therefore need no lock.
	 *
	 * Teardown race against k->cap[i].fb: `lock` protects fifo/shown/
	 * in_flight/the ring, but NOT the k->cap[i].fb array itself, which
	 * teardown_stream destroys on every reconnect. The rules below make
	 * sure an RmFB can never hit a buffer that is shown or part of a
	 * commit in flight:
	 *
	 * tearing_down   — set by teardown_stream (camera thread) under `lock`
	 *                  BEFORE the fifo is emptied. While set, the
	 *                  compositor NEVER picks a new frame from this
	 *                  camera's fifo (it is empty anyway) and instead, as
	 *                  soon as nothing else is in flight (in_flight == -1)
	 *                  and the plane still shows something
	 *                  (plane_attached), commits FB_ID=0/CRTC_ID=0 for the
	 *                  plane — in the SAME atomic commit as the other
	 *                  cameras (compositor() still builds only ONE atomic
	 *                  commit per vblank; rotation reuses the same
	 *                  mechanism to detach/attach planes).
	 * plane_attached — the plane is currently showing a real buffer (the
	 *                  latest confirmed commit had FB_ID != 0). Set true
	 *                  in complete_flip when a real frame is confirmed,
	 *                  false when the detach commit is confirmed. Also
	 *                  set by teardown_stream when it gives up with a
	 *                  real frame in flight (that flip lands unrecorded,
	 *                  see teardown_stream): true means "may show
	 *                  something", which is what every detach decision
	 *                  needs.
	 * detached       — condvar complete_flip signals when the detach
	 *                  commit is confirmed (also broadcast when a failed
	 *                  commit resets in_flight, and by planes_detached
	 *                  after a connector switch). teardown_stream waits
	 *                  (pthread_cond_timedwait, `lock` held, TEARDOWN_WAIT_S
	 *                  cap) until
	 *                  NEITHER in_flight != -1 NOR plane_attached holds —
	 *                  i.e. no commit in flight and the plane off — before
	 *                  it destroys the FBs/closes V4L2. The wait is capped
	 *                  so the camera thread can never hang forever if the
	 *                  compositor stands still (e.g. on quit, where
	 *                  compositor()'s own while (!quit) loop soon stops
	 *                  committing for the same reason). If the plane was
	 *                  already detached or never attached (e.g. the first
	 *                  connection attempt died before a single frame was
	 *                  shown) the wait condition is false at once — no
	 *                  commit, no wait.
	 * generation     — bumped by teardown_stream under `lock` in the same
	 *                  critical section that sets tearing_down. Closes the
	 *                  window between compositor() step A (a frame is
	 *                  popped from the fifo, `lock` dropped) and step C
	 *                  (`lock` taken again, the frame committed or handed
	 *                  back): with nothing in flight and the plane not
	 *                  attached (first frame of a connection, an idle
	 *                  rotation member) teardown_stream does not wait and
	 *                  can run from start to end inside that window,
	 *                  destroying the frame's fb and resetting the ring.
	 *                  Step A records the generation with the frame; step
	 *                  C uses the frame only if it is unchanged (and
	 *                  tearing_down is not set), reading cap[i].fb while
	 *                  still holding `lock` — otherwise the frame belongs
	 *                  to a torn-down connection and is simply dropped
	 *                  (neither committed nor ring_push()ed). Once step C
	 *                  has set in_flight under `lock`, a teardown waits for
	 *                  it (or keeps its fb alive in leaked[]), so the fb id
	 *                  read there stays valid until the commit.
	 *
	 * leaked/n_leaked — when teardown_stream gives up and DELIBERATELY
	 *                  leaks a protected index (shown/in flight/limbo,
	 *                  see teardown_stream and close_buffers) its
	 *                  fb/handle/dmafd is saved HERE
	 *                  instead of just being left in k->cap[i] — where
	 *                  start_capture would otherwise overwrite it without
	 *                  closing/destroying on the next connection, losing
	 *                  the fd and GEM handle for good. Drained
	 *                  (release_buffer) by complete_flip on the next CONFIRMED flip
	 *                  for this camera (the plane then provably can no
	 *                  longer reference them, whether it was a real frame
	 *                  or a detach that was confirmed) or at program exit
	 *                  (main(), after all camera threads are joined).
	 *                  m_leaks_closed counts the total (same "never reset,
	 *                  diffed via rep_ snapshot" pattern as m_synthetic).
	 *
	 * bufs_held      — number of imported GEM handles (decoder CAPTURE
	 *                  buffers) currently held for this camera: the live
	 *                  k->cap[] of the current connection plus the
	 *                  entries in leaked[]. Incremented by start_capture
	 *                  (camera thread) on every successful import,
	 *                  decremented wherever such a handle is closed —
	 *                  start_capture/close_buffers (camera thread),
	 *                  complete_flip/planes_detached (compositor thread)
	 *                  and main() at exit. Moving an index to leaked[] does
	 *                  not change it (the handle is still held). More than
	 *                  one writing thread, so it is an atomic rather than
	 *                  a lock-protected field: no lock needed. report()
	 *                  prints it as bufs=N; while streaming it should be
	 *                  the decoder's buffer count (at most CAPTURE_BUFFERS)
	 *                  plus any deliberately leaked entries still waiting
	 *                  (leaks_active) — a value that grows from reconnect
	 *                  to reconnect is a buffer leak.
	 */
	pthread_mutex_t  lock;
	struct pacing_fifo fifo;
	int              shown;
	int              in_flight;
	int64_t          in_flight_arrival_us;   /* only the compositor touches it */
	int              returned[CAPTURE_BUFFERS + 1];
	int              ring_head, ring_tail;

	struct leaked_buffer leaked[MAX_LEAKED];
	int                  n_leaked;
	int                  limbo[MAX_LIMBO];   /* under `lock`, see abandon_flip */
	int                  n_limbo;
	unsigned long        m_leaks_closed, rep_leaks_closed;
	_Atomic int          bufs_held;          /* see the comment above */

	bool             tearing_down;    /* teardown_stream wants the plane detached */
	bool             plane_attached;  /* the plane shows a real buffer right now */
	pthread_cond_t   detached;        /* signalled when the detach is confirmed */
	unsigned         generation;      /* bumped per teardown, see above */

	pthread_t        thread;
	bool             thread_started;

	int              delay_ms;        /* optional per-camera config field, default 0 */

	/* RTSP watchdog. "stimeout" does not exist in FFmpeg 7.1 — the option
	 * is called "timeout" and defaults to 0 (no limit at all). Without our
	 * own watchdog a stream that stalls without the TCP connection breaking
	 * would freeze the tile forever: last_packet_us (CLOCK_MONOTONIC) is
	 * updated on every packet read, and interrupt_callback aborts if too
	 * long has passed since the last sign of life. The limit is more
	 * generous during open/find_stream_info (DNS, TCP handshake, SPS/PPS
	 * can take a while) than while streaming. */
	int64_t          last_packet_us;
	int64_t          watchdog_limit_us;

	/* Pacing: pts through the decoder + sliding anchor for network jitter.
	 *
	 * pts_mode is decided once per connection, from the flag on the FIRST
	 * captured buffer: if the decoder copies back the timestamp we set in
	 * the OUTPUT buffer (V4L2_BUF_FLAG_TIMESTAMP_COPY) we use pts,
	 * otherwise we fall back to arrival time. Logged once per connection
	 * (see pts_mode_logged). */
	enum { PTS_MODE_UNKNOWN, PTS_MODE_SOURCE, PTS_MODE_ARRIVAL } pts_mode;
	bool             pts_mode_logged;

	/* Set once the first decoded frame of the connection has been paired
	 * with the arrival queue; reset per connection next to
	 * pts_mode_logged. Before it, packets without a matching frame are
	 * normal (dropped ahead of the first IDR), not a corrupt frame — see
	 * pacing_unmatched_classify. Written by the camera thread only. */
	bool             first_frame_seen;

	int64_t          last_pts_us;      /* -1 = no pts yet in this connection */
	struct pacing_anchor anchor;

	/* Pairs each fed packet (arrival time at av_read_frame) with the
	 * decoded frame that comes out on the other side. The V4L2 buffer's
	 * timestamp field carries only ONE timestamp (the one we use for pts),
	 * so the arrival time has to be tracked separately.
	 *
	 * In pts mode the queue is looked up by the captured pts value (see
	 * collect_decoded) instead of relying on queue order: if the decoder
	 * drops a corrupt frame, order-based pairing would be shifted
	 * permanently for the rest of the connection. In arrival mode (the
	 * decoder does not copy timestamps) queue order is all that is left.
	 *
	 * arrival_in/arrival_out are reset in teardown_stream (run on every
	 * close/retry before camera_thread() loops back and redoes the
	 * anchor/PLL reset a few lines further down) — not in that reset
	 * block. They are therefore already 0 there, just like on the first
	 * connection (load_config zeroes the whole struct). */
	struct { int64_t pts_us, arrival_us; } arrival_queue[ARRIVAL_QUEUE_SIZE];
	unsigned         arrival_in, arrival_out;

	/* Statistics for the 60-second line (report()).
	 *
	 * m_decoded, m_late, m_dropped_full and m_synthetic are written ONLY
	 * by the camera thread and are NEVER reset — they only count up since
	 * start (same principle as `connections` further down). report() (the
	 * compositor thread) reads them unlocked — safe since each has only
	 * ONE writer — and computes the window's delta against
	 * rep_decoded/rep_late/rep_dropped_full/rep_synthetic, a snapshot only
	 * report() itself updates.
	 *
	 * m_jitter and m_depth_sum/m_depth_count are written by the camera
	 * thread AND read/reset by report() — they MUST therefore be protected
	 * by `lock` (see the comment at lock above).
	 *
	 * m_shown, m_skipped, m_latency_sum_us/m_latency_count, m_idle and
	 * m_active_us are written and reset only by the compositor — no other
	 * thread touches them, so no lock is needed. m_idle counts ripe frames
	 * released WITHOUT a commit because the camera was an idle member of
	 * its rotation group that vblank (NOT dropped frames — drops are only
	 * counted during active time, see m_skipped); m_active_us accumulates
	 * how long (µs, one vblank_period_us at a time) the camera was the
	 * group's active member during the reporting window. For a camera with
	 * a unique tile (no rotation) m_idle is always 0 and m_active_us is
	 * always the whole window. */
	unsigned long    m_decoded, m_late, m_dropped_full;
	unsigned long    rep_decoded, rep_late, rep_dropped_full;

	/* Synthetic timestamp: number of frames whose target time was
	 * extrapolated instead of computed via anchor/PLL, because
	 * arrival_queue was completely empty (no posted arrival time to pair
	 * with) — see collect_decoded. Should normally be 0; here for
	 * visibility should it ever happen. Same pattern (never reset, diffed
	 * via rep_ snapshot) as m_decoded above. */
	unsigned long    m_synthetic, rep_synthetic;

	unsigned long    m_depth_sum, m_depth_count;
	struct pacing_histogram m_jitter;

	unsigned long    m_shown, m_skipped;
	unsigned long    m_latency_sum_us, m_latency_count;
	unsigned long    m_idle;            /* see the comment above */
	unsigned long    m_active_us;       /* see the comment above */

	/* Regulated pts delta (see pacing_pll_feed in pacing.h and
	 * collect_decoded): measures the source frame rate AFTER PLL
	 * regulation, for the drift diagnostics in report(). Written by the
	 * camera thread in collect_decoded under `lock` (like m_jitter),
	 * read/reset by report() under the same lock. Millisecond resolution
	 * (same pacing_histogram as jitter) is plenty. NEVER fed a synthetic
	 * timestamp (see m_synthetic above and collect_decoded). */
	struct pacing_pll pll;
	int64_t          r_prev_frame_us;          /* -1 = none yet, for diagnostics */
	struct pacing_histogram m_regulated_ptsdelta;  /* under `lock`, like m_jitter */

	/* statistics accumulated since start (never reset) */
	unsigned long    connections;

	/* Health, for STATUS= (sd_notify) and the log. st_state/st_fault are
	 * written by the camera thread and read by the compositor's status
	 * update — atomics, no lock. stalled is set by interrupt_cb (camera
	 * thread) when the watchdog fires. logdd collapses the camera thread's
	 * repeated failure lines (camera thread only). */
	_Atomic int      st_state;          /* enum pacing_cam_state */
	_Atomic int      st_fault;          /* enum pacing_fault */
	int64_t          live_since_us;     /* camera thread only; 0 = not live */
	bool             stalled;
	struct pacing_dedup logdd;
};

struct wall {
	int              drmfd;
	char             drm_path[LAYOUT_PATH_MAX];   /* the DRM device actually opened */
	uint32_t         crtc_id;
	uint32_t         crtc_pipe;   /* the CRTC's index in res->crtcs — needed
	                               * for the vblank wait's high-crtc bits on
	                               * systems with more than one CRTC */
	uint32_t         connector_id;
	drmModeModeInfo  mode;
	uint32_t         mode_blob;

	int64_t          vblank_period_us;    /* period at the current mode */
	int64_t          last_vblank_us;      /* from the latest vblank/flip event */
	bool             vblank_ts_monotonic; /* DRM_CAP_TIMESTAMP_MONOTONIC */

	uint32_t         p_crtc_active, p_crtc_mode;
	uint32_t         p_conn_crtc;

	/* black primary plane — some drivers reject a CRTC without one */
	uint32_t         primary_plane;
	uint32_t         primary_fb;
	uint32_t         primary_handle;   /* dumb buffer behind primary_fb */
	uint32_t         pp_fb, pp_crtc, pp_crtc_x, pp_crtc_y, pp_crtc_w, pp_crtc_h;
	uint32_t         pp_src_x, pp_src_y, pp_src_w, pp_src_h;

	struct camera    cam[MAX_CAMERAS];
	int              count;

	/* The parsed config (globals + camera list). In grid mode the tiles
	 * are only known after the display mode is (apply_layout). */
	struct layout_config cfg;

	/* Global config values (KEY=VALUE lines, see layout.h). */
	int              buffer_ms;
	int              rotate_seconds;

	/* Rotation: groups[0..n_groups-1] are formed ONCE by build_rotation()
	 * (called from main() after the layout is applied, before the threads
	 * start) from the cameras' tiles — never changed afterwards. A camera
	 * with a unique tile forms its own group with one member (`count ==
	 * 1`, never rotates, see pacing_rotation_update in pacing.h).
	 * rotation[g] is the rotation state of group g, owned and updated ONLY
	 * by the compositor — no lock needed (same principle as diag_* below). */
	struct pacing_group    groups[MAX_CAMERAS];
	int                    n_groups;
	struct pacing_rotation rotation[MAX_CAMERAS];

	/* Diagnostics, trimmed to what is needed in operation. Written only by
	 * the compositor (compositor()), no lock needed — same principle as
	 * m_shown/m_skipped in struct camera.
	 *
	 * diag_commit_target_vblank_us — next_vblank_us at the latest
	 *   successful commit; compared against the flip event's timestamp to
	 *   compute diag_late1_plus.
	 * diag_late1_plus — how many flips were confirmed AT LEAST one vblank
	 *   later than the vblank the commit aimed at. Should normally be 0 —
	 *   visibility should the vblank-target computation ever regress, see
	 *   the warning line in report().
	 * diag_busy_drops — in_flight frames handed back to the decoder
	 *   because the atomic commit failed (usually EBUSY). Counted both in
	 *   the camera's m_skipped (the main line's dropped column) AND here,
	 *   for visibility in the diag line.
	 * diag_switches — number of times ANY rotation group's active member
	 *   changed (pacing_rotation_update returned a different group index
	 *   than before), summed over all groups. 0 if there are no rotation
	 *   groups.
	 */
	/* Display health, owned by the compositor thread after startup (by
	 * main() before). See display_poll in drm.c. */
	char             conn_name[32];        /* e.g. "HDMI-A-1" */
	bool             display_connected;
	_Atomic bool     display_down;         /* !display_connected, for the camera
					        * threads (teardown_stream) */
	bool             remodeset_pending;    /* reconnected, mode not yet set again */
	int              commit_failures;      /* failed atomic commits in a row */
	int              last_commit_errno;    /* errno of the latest failed commit */
	bool             mode_fallback;        /* explicit MODE not offered, or only
					        * reserve modes: running on a fallback
					        * mode (see pick_mode) */
	bool             mode_reserve_only;    /* the modes at the latest pick were only
					        * the driver's reserve set */
	/* Following a moved cable (CONNECTOR unset), see follow_cable in drm.c */
	uint32_t         home_connector_id;    /* the connector chosen at start */
	struct pacing_conn_follow follow;
	struct pacing_retry conn_retry;        /* failed moves back off 1 ... 60 s */
	int64_t          last_cantmove_log_us;
	int64_t          last_fallback_probe_us;
	int64_t          last_display_poll_us;
	int64_t          last_forced_probe_us;
	int64_t          last_recovery_log_us;
	unsigned         recoveries;           /* forced re-probes, for the log */

	int64_t          diag_commit_target_vblank_us;
	unsigned long    diag_late1_plus;
	unsigned long    diag_busy_drops;
	unsigned long    diag_switches;
};

/* Argument to camera_thread() (camera_thread.c) — malloc'ed by main(),
 * freed by camera_thread() itself. */
struct thread_arg {
	struct wall   *v;
	struct camera *k;
};

/* ------------------------------------------------------------------ main.c */

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int xioctl(int fd, unsigned long req, void *arg);
int64_t monotonic_us(void);
/* The latest error line libav logged in the calling thread (URLs masked;
 * "" if none since av_last_error_clear), whatever FFMPEG_LOGLEVEL is. */
const char *av_last_error(void);
void av_last_error_clear(void);

/* ------------------------------------------------------------------- drm.c */

/* Return convention for the startup steps below: 0 = ok, -1 = error (exit
 * 1, systemd restarts), -2 = config error (exit RTSPWALL_EXIT_CONFIG),
 * 1 = quit was requested while waiting (exit 0). */
/* Opens the DRM device, sets the atomic caps and becomes DRM master. If
 * DRM_DEVICE does not exist (yet) it waits for it; if another program (a
 * desktop) holds master: names it, prints the fix once and keeps retrying
 * every 2 s (re-opening the device each time). */
int acquire_drm(struct wall *v);
/* Waits (logging once, polling every 2 s) until a display is connected,
 * then picks the mode (MODE; falls back to auto with a warning if the
 * display does not offer it), waits up to 60 s for a mode the manual
 * tiles fit, and hands out the planes. */
int wait_for_display(struct wall *v);
int read_plane_props(struct wall *v);
int create_primary_fb(struct wall *v);
/* Compositor thread, only while no page flip is pending: notices a
 * connector going away/coming back (cheap cached status, at most once a
 * second) and then re-reads the modes and sets the mode again. ~10 failed
 * commits in a row (forced re-probe, at most every 5 s) set the mode again
 * only if the display's modes changed or a commit of the primary plane
 * alone fails too. Running on the MODE=auto fallback, it checks every 30 s
 * whether the explicit MODE is offered again. Returns true if a modeset
 * was made. */
bool display_poll(struct wall *v);

/* ------------------------------------------------------------------ v4l2.c */

/* Releases one CAPTURE buffer's resources on `drmfd`, in this order:
 * drmModeRmFB (if fb), drmCloseBufferHandle (if handle), close (if
 * dmafd >= 0). Any part may be "none" (see struct buffer). */
void release_buffer(int drmfd, uint32_t fb, uint32_t handle, int dmafd);
/* Unmaps the camera's OUTPUT buffers and releases its CAPTURE buffers
 * (release_buffer on `drmfd`), except the CAPTURE indices marked in
 * keep[CAPTURE_BUFFERS] (may be NULL), which are left alone. */
void close_buffers(int drmfd, struct camera *k, const bool *keep);
int open_decoder(const char *device, struct camera *k, unsigned width, unsigned height);
/* VIDIOC_QUERYCAP + ENUM_FMT on the decoder before any camera starts.
 * Waits up to 15 s for the device node to appear and up to 30 s (in all)
 * for permission to open it (boot races with the driver and udev).
 * Returns 0, RTSPWALL_EXIT_NO_DECODER if there is no usable H.264 M2M
 * decoder there (the node is still missing, still not accessible, or
 * answers QUERYCAP without being an H.264 M2M decoder - restarting will
 * not help), or 1 for other errors (QUERYCAP itself failing included). */
int decoder_preflight(const char *device);
int start_capture(struct wall *v, struct camera *k);

/* --------------------------------------------------------- camera_thread.c */

void ring_push(struct camera *k, int index);
int ring_pop(struct camera *k);
void *camera_thread(void *arg);

/* ------------------------------------------------------------ compositor.c */

int initial_commit(struct wall *v);
void compositor(struct wall *v);

/* sd_notify without libsystemd: sends `msg` (e.g. "READY=1") to
 * $NOTIFY_SOCKET; does nothing when not run by systemd. Thread-safe. */
void notify_systemd(const char *msg);
/* notify_systemd("STATUS=...") with printf formatting. Never pass a URL. */
void notify_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* Sends STATUS= with the cameras' live count and problems (plus the
 * display state) if it changed, at most once a second. */
void update_status(struct wall *v, int64_t now_us);

/* ---------------------------------------------------------------- config.c */

int load_config(struct wall *v, const char *path);
int apply_layout(struct wall *v);
void build_rotation(struct wall *v);
int check_config(const char *path, int screen_w, int screen_h);

#endif
