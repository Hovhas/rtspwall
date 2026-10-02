/*
 * rtspwall — RTSP camera streams, each on its own hardware plane, without X.
 *
 * Background. On the Raspberry Pi 3, omxplayer decoded in the VideoCore
 * hardware and handed the frame to a DispmanX layer; the hardware video
 * scaler (HVS) composited the layers during scanout. No frame ever touched
 * the CPU or GPU. Under KMS, DispmanX is gone, and the hardware planes can
 * only be used by *one* display client at a time. Several independent
 * player processes therefore cannot each get a plane of their own — which
 * is why going through X and XVideo costs a copy and a format conversion
 * per frame and stream.
 *
 * This program is that one client. It becomes DRM master itself, puts each
 * camera on its own overlay plane and lets the HVS do the work:
 *
 *     RTSP (libavformat, demux only)
 *       -> V4L2 M2M decoder /dev/video10 (bcm2835-codec, hardware)
 *       -> dmabuf straight out of the decoder's buffer
 *       -> drmModeAddFB2 -> atomic commit on an overlay plane
 *
 * The frame is never copied. It is written by the decoder and read by the
 * display.
 *
 * On a Raspberry Pi 4 the HDMI CRTCs accept many overlay planes that take
 * NV12 (LINEAR and BROADCOM_SAND128), and /dev/video10 can produce NV12 and
 * NC12 (= NV12/SAND128). This version uses NV12 LINEAR. That is still zero
 * copy — the decoder just writes untiled instead of tiled. SAND128 would
 * save a little memory bandwidth internally but drags in an awkward stride
 * convention.
 *
 * Smooth playback: pts from the stream is carried through the decoder, each
 * camera has a jitter buffer (pacing.c) instead of a single "latest wins"
 * slot, and the compositor picks frames driven by vblank instead of a fixed
 * sleep. Some recorders stamp 30 fps streams in pairs (pts delta
 * alternating ~7/59.7 ms instead of an even 33/33 ms); raw pts then gave up
 * to 50 % dropped frames because both frames of a pair became ripe at the
 * same vblank. pacing_pll (pacing.h) regulates that away — the target time
 * is computed on a regulated timeline instead of raw pts. A synthetic
 * timestamp (empty arrival queue) is never fed into anchor/PLL/histogram,
 * see collect_decoded.
 *
 * Rotation: cameras with EXACTLY the same tile (width/height/x/y after the
 * layout is applied — in grid mode: the same cell) form a rotation group
 * (pacing_build_groups, run once in build_rotation()). All cameras decode
 * ALL THE TIME, whether shown or not — only which of a group's planes is
 * ATTACHED rotates (pacing_rotation_update, called once per group and
 * vblank in compositor()). A switch detaches the outgoing member's plane
 * and attaches the incoming one in the SAME atomic commit — the same
 * FB_ID=0/CRTC_ID=0 mechanism (in_flight == -2, plane_attached, confirmed
 * in complete_flip) used for a clean detach on reconnect. A camera with a
 * unique tile forms its own one-member group and never rotates.
 *
 * Here: main(), command line, signal handling and the three small helpers
 * (log_msg/xioctl/monotonic_us) every other file uses. See rtspwall.h for
 * the file map.
 *
 * Run: sudo rtspwall [/etc/rtspwall/cameras.conf]
 *      (requires that no X server/compositor runs — only one client can be
 *      DRM master)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libavformat/avformat.h>
#include <libavutil/log.h>

#include "rtspwall.h"

#ifndef VERSION
#define VERSION "0.1.0-dev"
#endif

#define DEFAULT_CONFIG "/etc/rtspwall/cameras.conf"

/* ------------------------------------------------------------------ logging */

volatile sig_atomic_t quit = 0;

/* Thread-safe: the camera threads, the compositor and the FFmpeg log
 * callback all log concurrently. The whole line is formatted into a local
 * buffer (localtime_r, no shared static state) and written with a single
 * write(2), so lines never interleave. Over-long lines are cut with "...". */
void log_msg(const char *fmt, ...)
{
	char line[2048];
	struct timespec ts;
	struct tm tm;
	va_list ap;
	size_t n;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	n = strftime(line, sizeof line, "[%H:%M:%S] ", &tm);

	va_start(ap, fmt);
	int r = vsnprintf(line + n, sizeof line - n - 1, fmt, ap);   /* -1: room for '\n' */
	va_end(ap);
	if (r < 0)
		r = 0;
	if ((size_t)r >= sizeof line - n - 1) {
		n = sizeof line - 2;
		memcpy(line + n - 3, "...", 3);
	} else {
		n += (size_t)r;
	}
	line[n++] = '\n';

	const char *p = line;
	while (n > 0) {
		ssize_t w = write(STDERR_FILENO, p, n);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		p += w;
		n -= (size_t)w;
	}
}

static void signal_handler(int sig)
{
	(void)sig;
	quit = 1;
}

int64_t monotonic_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

/* ioctl that retries on EINTR — V4L2 is interrupted by signals all the time. */
int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);
	return r;
}

/* FFmpeg's own log output goes through here: filtered by FFMPEG_LOGLEVEL
 * (default errors only), every URL in the line masked (libav messages can
 * quote the URL it was given, credentials included), and control
 * characters replaced so a stream or server cannot inject terminal escape
 * sequences or fake lines into the journal. */
static void av_log_masked(void *avcl, int level, const char *fmt, va_list vl)
{
	char line[1024], masked[1024];
	int print_prefix = 1;

	if (level > av_log_get_level())
		return;
	av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &print_prefix);
	layout_mask_urls_in_text(line, masked, sizeof masked);

	size_t n = strlen(masked);
	while (n > 0 && (masked[n - 1] == '\n' || masked[n - 1] == '\r'))
		masked[--n] = '\0';
	layout_sanitize_log_text(masked);
	if (n)
		log_msg("ffmpeg: %s", masked);
}

static int ffmpeg_level(enum layout_ffmpeg_log l)
{
	switch (l) {
	case LAYOUT_FFMPEG_LOG_QUIET:   return AV_LOG_QUIET;
	case LAYOUT_FFMPEG_LOG_ERROR:   return AV_LOG_ERROR;
	case LAYOUT_FFMPEG_LOG_WARNING: return AV_LOG_WARNING;
	case LAYOUT_FFMPEG_LOG_INFO:    return AV_LOG_INFO;
	}
	return AV_LOG_ERROR;
}

/* ------------------------------------------------------------- command line */

static void usage(FILE *out)
{
	fprintf(out,
		"Usage: rtspwall [CONFIG]\n"
		"       rtspwall --check-config [--mode WxH] [CONFIG]\n"
		"       rtspwall --help | --version\n"
		"\n"
		"Shows RTSP camera streams on a Raspberry Pi 4 display, each on its own\n"
		"hardware plane (V4L2 M2M decoder -> dmabuf -> DRM/KMS, zero copy).\n"
		"\n"
		"  CONFIG             config file (default " DEFAULT_CONFIG ")\n"
		"  --check-config     validate CONFIG and print the resulting layout,\n"
		"                     without opening DRM, V4L2 or the network\n"
		"  --mode WxH         screen size for --check-config (default 1920x1080)\n"
		"  -h, --help         show this help\n"
		"  -V, --version      show the version\n");
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "help",         no_argument,       NULL, 'h' },
		{ "version",      no_argument,       NULL, 'V' },
		{ "check-config", no_argument,       NULL, 'c' },
		{ "mode",         required_argument, NULL, 'm' },
		{ NULL, 0, NULL, 0 },
	};
	bool check = false;
	const char *mode_arg = NULL;
	int opt;

	while ((opt = getopt_long(argc, argv, "hV", opts, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(stdout);
			return 0;
		case 'V':
			printf("rtspwall %s\n", VERSION);
			return 0;
		case 'c':
			check = true;
			break;
		case 'm':
			mode_arg = optarg;
			break;
		default:
			usage(stderr);
			return 2;
		}
	}
	if (argc - optind > 1) {
		fprintf(stderr, "rtspwall: too many arguments\n");
		usage(stderr);
		return 2;
	}
	const char *conf = optind < argc ? argv[optind] : DEFAULT_CONFIG;

	if (mode_arg && !check) {
		fprintf(stderr, "rtspwall: --mode is only valid with --check-config\n");
		return 2;
	}
	if (check) {
		int w = 1920, h = 1080;
		if (mode_arg && layout_parse_size(mode_arg, &w, &h) < 0) {
			fprintf(stderr, "rtspwall: --mode expects WxH (e.g. 1920x1080), got \"%s\"\n",
				mode_arg);
			return 2;
		}
		return check_config(conf, w, h);
	}

	static struct wall v;
	int r = 1;

	v.drmfd = -1;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
	signal(SIGPIPE, SIG_IGN);

	log_msg("rtspwall %s starting, config %s", VERSION, conf);
	av_log_set_level(AV_LOG_ERROR);
	av_log_set_callback(av_log_masked);
	avformat_network_init();

	if (load_config(&v, conf) < 0)
		return 1;
	av_log_set_level(ffmpeg_level(v.cfg.ffmpeg_loglevel));

	if (open_drm_device(&v) < 0)
		return 1;

	if (drmSetClientCap(v.drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)
	    || drmSetClientCap(v.drmfd, DRM_CLIENT_CAP_ATOMIC, 1)) {
		log_msg("driver lacks atomic/universal planes: %s", strerror(errno));
		goto out;
	}

	if (drmSetMaster(v.drmfd)) {
		log_msg("cannot become DRM master: %s", strerror(errno));
		log_msg("is an X server or another compositor running? Stop it first - only one client at a time.");
		goto out;
	}

	/* Order matters: the display mode must be known before the grid tiles
	 * can be computed, and the tiles before the rotation groups. */
	if (find_display(&v) < 0 || apply_layout(&v) < 0)
		goto out;
	build_rotation(&v);

	if (read_plane_props(&v) < 0 || create_primary_fb(&v) < 0 || initial_commit(&v) < 0)
		goto out;

	v.last_vblank_us = monotonic_us();

	for (int i = 0; i < v.count; i++) {
		struct thread_arg *ta = malloc(sizeof *ta);
		if (!ta) {
			log_msg("out of memory");
			goto out;
		}
		ta->v = &v;
		ta->k = &v.cam[i];
		if (pthread_create(&v.cam[i].thread, NULL, camera_thread, ta)) {
			log_msg("pthread_create %s failed", v.cam[i].name);
			free(ta);
			goto out;
		}
		v.cam[i].thread_started = true;
	}

	log_msg("running - %d cameras, each on its own hardware plane", v.count);
	compositor(&v);
	r = 0;

out:
	quit = 1;
	/* A camera thread waiting in teardown_stream (pthread_cond_timedwait on
	 * `detached`) would otherwise only wake after the full 2-second cap on
	 * exit — needless delay of a shutdown already in progress. Broadcast
	 * here (NOT in the signal handler — pthread_cond_broadcast is not
	 * async-signal-safe) so every waiting thread immediately sees that
	 * quit is set and ends its wait (see the !quit check in the wait
	 * condition and the check after it in teardown_stream). Harmless for
	 * cameras that are not waiting at all. */
	for (int i = 0; i < v.count; i++)
		pthread_cond_broadcast(&v.cam[i].detached);
	for (int i = 0; i < v.count; i++)
		if (v.cam[i].thread_started)
			pthread_join(v.cam[i].thread, NULL);

	/* Leaked buffers: by now ALL camera threads have been joined — nobody
	 * writes k->leaked any more, no lock needed. Clean up whatever never
	 * got confirmed by a flip (see .leaked in struct camera and
	 * complete_flip). */
	for (int i = 0; i < v.count; i++) {
		struct camera *k = &v.cam[i];
		for (int j = 0; j < k->n_leaked; j++) {
			if (k->leaked[j].fb)
				drmModeRmFB(v.drmfd, k->leaked[j].fb);
			if (k->leaked[j].dmafd >= 0)
				close(k->leaked[j].dmafd);
		}
		if (k->n_leaked)
			log_msg("%s: cleaned up %d leaked buffer(s) on exit", k->name, k->n_leaked);
		k->n_leaked = 0;
	}

	if (v.mode_blob)
		drmModeDestroyPropertyBlob(v.drmfd, v.mode_blob);
	if (v.primary_fb)
		drmModeRmFB(v.drmfd, v.primary_fb);
	if (v.drmfd >= 0) {
		drmDropMaster(v.drmfd);
		close(v.drmfd);
	}
	log_msg("exited");
	return r;
}
