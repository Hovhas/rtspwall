/*
 * probe.c — `rtspwall probe [URL | - | CONFIG]`.
 *
 * Checks a camera stream the way the wall will use it, without playing it:
 * libavformat opens the RTSP URL with initial_pause=1, so the server sees
 * DESCRIBE and SETUP but no PLAY (no frames are sent, and NVRs that share
 * one camera session between clients are not disturbed). The codec,
 * profile/level and size come from the SDP's sprop-parameter-sets SPS
 * (parsed by clilogic.c), the frame rate from the SPS VUI timing or the
 * SDP's a=framerate. Only if those are missing does it send PLAY and read
 * at most ~2 s of packets.
 *
 * Every URL that is printed is masked (layout_mask_url). A URL typed on the
 * command line works but ends up in shell history and sudo logs, so the
 * documented forms are the hidden prompt and `-` (stdin).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#include "cli.h"

static int64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* --------------------------------------------------------- libav logging */

/* libav's own messages are not printed (probe output stays one clean
 * block); the last one per thread is kept, masked, as the error detail —
 * e.g. "Failed to resolve hostname" explains an otherwise generic EIO. */
static __thread char last_av_msg[160];

static void av_log_keep(void *avcl, int level, const char *fmt, va_list vl)
{
	char line[512], masked[512];
	int prefix = 0;

	if (level > AV_LOG_ERROR)
		return;
	av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &prefix);
	layout_mask_urls_in_text(line, masked, sizeof masked);
	size_t n = strlen(masked);
	while (n && (masked[n - 1] == '\n' || masked[n - 1] == '\r'))
		masked[--n] = '\0';
	layout_sanitize_log_text(masked);
	if (n)
		snprintf(last_av_msg, sizeof last_av_msg, "%.159s", masked);
}

static void init_libav_once(void)
{
	av_log_set_level(AV_LOG_ERROR);
	av_log_set_callback(av_log_keep);
	avformat_network_init();
}

static void probe_init_libav(void)
{
	static pthread_once_t once = PTHREAD_ONCE_INIT;
	pthread_once(&once, init_libav_once);
}

/* ------------------------------------------------------------ one probe */

struct deadline {
	int64_t at_ms;
};

static int interrupt_cb(void *opaque)
{
	const struct deadline *d = opaque;
	return mono_ms() > d->at_ms;
}

static enum probe_error classify(int averr, bool interrupted)
{
	if (interrupted)
		return PROBE_ERR_TIMEOUT;
	switch (averr) {
	case AVERROR_HTTP_UNAUTHORIZED:
	case AVERROR_HTTP_FORBIDDEN:
		return PROBE_ERR_AUTH;
	case AVERROR_HTTP_NOT_FOUND:
		return PROBE_ERR_NOT_FOUND;
	case AVERROR(ECONNREFUSED):
		return PROBE_ERR_REFUSED;
	case AVERROR(ETIMEDOUT):
	case AVERROR_EXIT:
		return PROBE_ERR_TIMEOUT;
	case AVERROR(EHOSTUNREACH):
	case AVERROR(ENETUNREACH):
		return PROBE_ERR_UNREACHABLE;
	default:
		break;
	}
	/* Name resolution fails with a generic EIO; libav says why in its log. */
	if (strstr(last_av_msg, "resolve"))
		return PROBE_ERR_UNREACHABLE;
	if (strstr(last_av_msg, "401"))
		return PROBE_ERR_AUTH;
	if (strstr(last_av_msg, "404"))
		return PROBE_ERR_NOT_FOUND;
	return PROBE_ERR_OTHER;
}

static double q2d_ok(AVRational r)
{
	if (r.num <= 0 || r.den <= 0)
		return 0.0;
	double v = av_q2d(r);
	return v > 0.5 && v <= 240.0 ? v : 0.0;
}

static void fill_from_stream(const AVStream *st, bool live, struct probe_result *r)
{
	const AVCodecParameters *par = st->codecpar;

	snprintf(r->codec_name, sizeof r->codec_name, "%s", avcodec_get_name(par->codec_id));
	switch (par->codec_id) {
	case AV_CODEC_ID_H264: r->s.codec = PROBE_CODEC_H264; break;
	case AV_CODEC_ID_HEVC: r->s.codec = PROBE_CODEC_HEVC; break;
	case AV_CODEC_ID_NONE: r->s.codec = PROBE_CODEC_UNKNOWN; break;
	default:               r->s.codec = PROBE_CODEC_OTHER; break;
	}

	struct h264_sps_info sps;
	if (r->s.codec == PROBE_CODEC_H264 && par->extradata &&
	    h264_parse_extradata(par->extradata, (size_t)par->extradata_size, &sps) == 0) {
		r->s.profile_idc = sps.profile_idc;
		r->s.constraint_flags = sps.constraint_flags;
		r->s.level_idc = sps.level_idc;
		r->s.width = sps.width;
		r->s.height = sps.height;
		if (sps.fps > 0) {
			r->s.fps = sps.fps;
			r->fps_source = "VUI";
		}
	}
	if (r->s.width <= 0 || r->s.height <= 0) {
		r->s.width = par->width;
		r->s.height = par->height;
	}
	if (r->s.fps <= 0) {
		double f = q2d_ok(st->avg_frame_rate);
		if (f > 0) {
			r->s.fps = f;
			r->fps_source = live ? "SDP" : "container";
		} else if (!live && (f = q2d_ok(st->r_frame_rate)) > 0) {
			r->s.fps = f;
			r->fps_source = "container";
		}
	}
}

int probe_url(const char *url, int timeout_ms, struct probe_result *r)
{
	probe_init_libav();
	memset(r, 0, sizeof *r);
	last_av_msg[0] = '\0';

	int64_t t0 = mono_ms();
	struct deadline dl = { t0 + timeout_ms };
	bool rtsp = layout_url_is_rtsp(url);
	bool live = layout_url_is_live(url);

	AVFormatContext *ic = avformat_alloc_context();
	if (!ic) {
		r->err = PROBE_ERR_OTHER;
		snprintf(r->detail, sizeof r->detail, "out of memory");
		return -1;
	}
	ic->interrupt_callback.callback = interrupt_cb;
	ic->interrupt_callback.opaque = &dl;

	AVDictionary *opts = NULL;
	if (rtsp) {
		char us[32];
		snprintf(us, sizeof us, "%lld", (long long)timeout_ms * 1000);
		av_dict_set(&opts, "rtsp_transport", "tcp", 0);
		av_dict_set(&opts, "initial_pause", "1", 0);   /* DESCRIBE + SETUP, no PLAY */
		av_dict_set(&opts, "timeout", us, 0);          /* socket I/O, microseconds */
	}

	int e = avformat_open_input(&ic, url, NULL, &opts);   /* frees ic on failure */
	av_dict_free(&opts);
	if (e < 0) {
		bool interrupted = mono_ms() > dl.at_ms;
		r->err = classify(e, interrupted);
		char buf[128];
		av_strerror(e, buf, sizeof buf);
		if (last_av_msg[0] && !interrupted)
			snprintf(r->detail, sizeof r->detail, "%s (%s)", buf, last_av_msg);
		else
			snprintf(r->detail, sizeof r->detail, "%s", interrupted ? "no answer" : buf);
		r->elapsed_ms = (int)(mono_ms() - t0);
		return -1;
	}

	int vi = -1;
	for (unsigned i = 0; i < ic->nb_streams; i++)
		if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			vi = (int)i;
			break;
		}

	/* Without a video stream description in the SDP (unusual) a short read
	 * may still find it; the fallback below handles both cases. */
	if (vi >= 0)
		fill_from_stream(ic->streams[vi], live, r);

	bool need_more = vi < 0 ||
			 ((r->s.codec == PROBE_CODEC_H264 || r->s.codec == PROBE_CODEC_UNKNOWN) &&
			  (r->s.width <= 0 || r->s.height <= 0 || r->s.fps <= 0));
	if (need_more) {
		/* Fallback: PLAY and look at <= 2 s of packets. */
		dl.at_ms = mono_ms() + 3000;
		if (rtsp)
			av_read_play(ic);
		ic->max_analyze_duration = 2 * AV_TIME_BASE;
		if (avformat_find_stream_info(ic, NULL) >= 0) {
			if (vi < 0)
				for (unsigned i = 0; i < ic->nb_streams; i++)
					if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
						vi = (int)i;
						break;
					}
			if (vi >= 0) {
				double fps_before = r->s.fps;
				fill_from_stream(ic->streams[vi], live, r);
				if (fps_before <= 0 && r->s.fps > 0)
					r->fps_source = "packets";
			}
		}
	}

	avformat_close_input(&ic);   /* sends TEARDOWN */
	r->elapsed_ms = (int)(mono_ms() - t0);
	if (vi < 0) {
		r->err = PROBE_ERR_NO_VIDEO;
		snprintf(r->detail, sizeof r->detail, "no video stream");
		return -1;
	}
	r->err = PROBE_OK;
	return 0;
}

/* ------------------------------------------------------ parallel probing */

struct probe_job {
	const char *url;
	int timeout_ms;
	struct probe_result *res;
};

static void *probe_thread(void *arg)
{
	struct probe_job *j = arg;
	probe_url(j->url, j->timeout_ms, j->res);
	return NULL;
}

void probe_urls(const char *const *urls, int n, int timeout_ms, struct probe_result *res)
{
	pthread_t th[LAYOUT_MAX_CAMERAS];
	struct probe_job jobs[LAYOUT_MAX_CAMERAS];
	bool started[LAYOUT_MAX_CAMERAS] = { false };

	probe_init_libav();
	if (n > LAYOUT_MAX_CAMERAS)
		n = LAYOUT_MAX_CAMERAS;
	for (int i = 0; i < n; i++) {
		jobs[i] = (struct probe_job){ urls[i], timeout_ms, &res[i] };
		started[i] = pthread_create(&th[i], NULL, probe_thread, &jobs[i]) == 0;
		if (!started[i])
			probe_url(urls[i], timeout_ms, &res[i]);
	}
	for (int i = 0; i < n; i++)
		if (started[i])
			pthread_join(th[i], NULL);
}

/* ---------------------------------------------------------------- output */

static void describe_codec(const struct probe_result *r, char *out, size_t outlen)
{
	if (r->s.codec == PROBE_CODEC_H264 && r->s.profile_idc)
		snprintf(out, outlen, "H.264 %s, level %d.%d",
			 h264_profile_name(r->s.profile_idc, r->s.constraint_flags),
			 r->s.level_idc / 10, r->s.level_idc % 10);
	else if (r->s.codec == PROBE_CODEC_H264)
		snprintf(out, outlen, "H.264");
	else if (r->s.codec == PROBE_CODEC_HEVC)
		snprintf(out, outlen, "H.265 (hevc)");
	else
		snprintf(out, outlen, "%s", r->codec_name[0] ? r->codec_name : "unknown");
}

/* Verdict and hint for one probe result, errors included. */
enum budget_verdict probe_result_verdict(const struct probe_result *r, const char *masked_url,
					 char *hint, size_t hintlen)
{
	if (r->err != PROBE_OK) {
		snprintf(hint, hintlen, "%s", probe_error_hint(r->err, masked_url));
		return BUDGET_FAIL;
	}
	return probe_stream_verdict(&r->s, hint, hintlen);
}

/* The detailed block for a single probed URL. Returns the verdict. */
enum budget_verdict probe_print_one(const char *url, const struct probe_result *r)
{
	char masked[LAYOUT_URL_MAX], hint[512], codec[96];

	layout_mask_url(url, masked, sizeof masked);
	printf("probe:    %s\n", masked);
	enum budget_verdict v = probe_result_verdict(r, masked, hint, sizeof hint);

	if (r->err != PROBE_OK) {
		printf("error:    %s: %s\n", probe_error_label(r->err), r->detail);
	} else {
		describe_codec(r, codec, sizeof codec);
		printf("codec:    %s\n", codec);
		if (r->s.width > 0)
			printf("size:     %dx%d\n", r->s.width, r->s.height);
		else
			printf("size:     unknown\n");
		if (r->s.fps > 0)
			printf("fps:      %.2f (%s), budgeted as %d\n", r->s.fps,
			       r->fps_source ? r->fps_source : "?", budget_nominal_fps(r->s.fps));
		else
			printf("fps:      unknown\n");
		long mbps = budget_stream_mbps(r->s.width, r->s.height, r->s.fps);
		if (mbps > 0 && r->s.codec == PROBE_CODEC_H264)
			printf("decoder:  %.1f %% of the Pi 4 H.264 budget (%ld of %ld macroblocks/s)\n",
			       budget_percent(mbps), mbps, BUDGET_MAX_MBPS);
	}
	printf("time:     %d ms\n", r->elapsed_ms);
	printf("verdict:  %s\n", budget_verdict_name(v));
	if (hint[0])
		printf("fix:      %s\n", hint);
	return v;
}

/* Probes every camera of a config and prints the budget table. Returns the
 * worst verdict (per camera and total). */
enum budget_verdict probe_print_config(const struct layout_config *cfg, long *total_out)
{
	const char *urls[LAYOUT_MAX_CAMERAS];
	static struct probe_result res[LAYOUT_MAX_CAMERAS];
	enum budget_verdict worst = BUDGET_PASS;
	long total = 0;
	int measured = 0;

	for (int i = 0; i < cfg->count; i++)
		urls[i] = cfg->cam[i].url;
	probe_urls(urls, cfg->count, CLI_PROBE_TIMEOUT_MS, res);

	printf("%-14s %-5s %-24s %-10s %6s %7s  %s\n",
	       "CAMERA", "CELL", "CODEC", "SIZE", "FPS", "LOAD", "VERDICT");
	for (int i = 0; i < cfg->count; i++) {
		const struct layout_camera *c = &cfg->cam[i];
		const struct probe_result *r = &res[i];
		char masked[LAYOUT_URL_MAX], hint[512], codec[96], size[24] = "-", fps[16] = "-",
		     load[16] = "-", cell[8] = "-";

		layout_mask_url(c->url, masked, sizeof masked);
		enum budget_verdict v = probe_result_verdict(r, masked, hint, sizeof hint);
		if (v > worst)
			worst = v;
		if (c->cell)
			snprintf(cell, sizeof cell, "%d", c->cell);
		if (r->err == PROBE_OK) {
			describe_codec(r, codec, sizeof codec);
			if (r->s.width > 0)
				snprintf(size, sizeof size, "%dx%d", r->s.width, r->s.height);
			if (r->s.fps > 0)
				snprintf(fps, sizeof fps, "%.2f", r->s.fps);
			long m = budget_stream_mbps(r->s.width, r->s.height, r->s.fps);
			if (m > 0 && r->s.codec == PROBE_CODEC_H264) {
				snprintf(load, sizeof load, "%.1f%%", budget_percent(m));
				total += m;
				measured++;
			}
		} else {
			snprintf(codec, sizeof codec, "%s", probe_error_label(r->err));
		}
		printf("%-14s %-5s %-24s %-10s %6s %7s  %s\n", c->name, cell, codec, size, fps,
		       load, budget_verdict_name(v));
		printf("    %s\n", masked);
		if (hint[0])
			printf("    fix: %s\n", hint);
	}

	enum budget_verdict tv = budget_verdict(total);
	printf("\ntotal:    %.1f %% of the decoder budget (%ld of %ld macroblocks/s, %d of %d "
	       "camera%s measured, rotation members included): %s\n",
	       budget_percent(total), total, BUDGET_MAX_MBPS, measured, cfg->count,
	       cfg->count == 1 ? "" : "s", budget_verdict_name(tv));
	if (measured < cfg->count)
		printf("          (a lower bound: not every camera could be measured)\n");
	if (tv == BUDGET_FAIL)
		printf("fix:      too much for the Pi 4 decoder (it can wedge until a reboot): use "
		       "sub-streams, lower fps, or fewer cameras\n");
	else if (tv == BUDGET_WARN)
		printf("note:     above 90 %%: works, but with little headroom\n");
	if (tv > worst)
		worst = tv;
	if (total_out)
		*total_out = total;
	return worst;
}

/* ------------------------------------------------------------- command */

static void probe_usage(FILE *out)
{
	fprintf(out,
		"Usage: rtspwall probe            (hidden prompt for the URL)\n"
		"       rtspwall probe -          (read the URL from stdin)\n"
		"       rtspwall probe CONFIG     (probe every camera, print the budget table)\n"
		"       rtspwall probe URL        (works, but the URL ends up in shell history)\n"
		"\n"
		"Checks codec, profile, size and frame rate against the Pi 4 H.264 decoder\n"
		"without playing the stream (RTSP DESCRIBE + SETUP, no PLAY), timeout %d s.\n"
		"Exit status: 0 PASS, 1 WARN, 2 FAIL or error.\n",
		CLI_PROBE_TIMEOUT_MS / 1000);
}

static int verdict_exit(enum budget_verdict v)
{
	return v == BUDGET_PASS ? 0 : v == BUDGET_WARN ? 1 : 2;
}

static bool ends_with(const char *s, const char *suffix)
{
	size_t n = strlen(s), m = strlen(suffix);
	return n >= m && strcmp(s + n - m, suffix) == 0;
}

/* Probes one URL as typed/pasted: UniFi Protect rtsps URLs are probed the
 * way the daemon will play them (layout_unifi_rewrite). */
int probe_single(const char *url, struct probe_result *r, enum budget_verdict *v)
{
	char rewritten[LAYOUT_URL_MAX];
	const char *use = url;

	if (layout_unifi_rewrite(url, rewritten, sizeof rewritten) == 1) {
		char masked[LAYOUT_URL_MAX];
		layout_mask_url(rewritten, masked, sizeof masked);
		printf("note:     UniFi Protect URL; probed as %s (plain RTSP, as the wall "
		       "plays it unless UNIFI_REWRITE=off)\n", masked);
		use = rewritten;
	}
	probe_url(use, CLI_PROBE_TIMEOUT_MS, r);
	*v = probe_print_one(use, r);
	return verdict_exit(*v);
}

int cmd_probe(int argc, char **argv)
{
	char url[LAYOUT_URL_MAX + 2];
	struct probe_result r;
	enum budget_verdict v;

	if (argc > 2) {
		probe_usage(stderr);
		return 2;
	}
	const char *arg = argc == 2 ? argv[1] : NULL;
	if (arg && (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)) {
		probe_usage(stdout);
		return 0;
	}

	if (!arg || strcmp(arg, "-") == 0) {
		if (cli_read_url(arg != NULL, url, sizeof url) < 0)
			return 2;
		const char *why = cfg_check_url(url);
		if (why)
			fprintf(stderr, "rtspwall: note: %s\n", why);
		return probe_single(url, &r, &v);
	}

	if (strstr(arg, "://")) {
		fprintf(stderr, "rtspwall: note: a URL on the command line is saved in your shell "
				"history and sudo's log; next time use the hidden prompt "
				"(`rtspwall probe`)\n");
		return probe_single(arg, &r, &v);
	}

	/* A file: a config to probe camera by camera, or a local media file. */
	struct stat st;
	if (stat(arg, &st) < 0) {
		fprintf(stderr, "rtspwall: %s: %s\n", arg, strerror(errno));
		return 2;
	}
	static struct layout_config cfg;
	char err[768];
	if (cli_load_config(arg, &cfg, err, sizeof err) == 0) {
		long total;
		printf("config:   %s (%d camera%s)\n\n", arg, cfg.count, cfg.count == 1 ? "" : "s");
		return verdict_exit(probe_print_config(&cfg, &total));
	}
	if (ends_with(arg, ".conf")) {
		fprintf(stderr, "rtspwall: %s\n", err);
		return 2;
	}
	return probe_single(arg, &r, &v);
}
