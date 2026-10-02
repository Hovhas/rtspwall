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
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/log.h>

#include "cli.h"

/* How long the PLAY fallback may read packets. */
#define PROBE_FALLBACK_MS 3000

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
			snprintf(r->fps_source, sizeof r->fps_source, "VUI");
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
			snprintf(r->fps_source, sizeof r->fps_source, "%s", live ? "SDP" : "container");
		} else if (!live && (f = q2d_ok(st->r_frame_rate)) > 0) {
			r->s.fps = f;
			snprintf(r->fps_source, sizeof r->fps_source, "container");
		}
	}
}

int probe_url(const char *url, int timeout_ms, struct probe_result *r)
{
	probe_urls(&url, 1, timeout_ms, r);
	return r->err == PROBE_OK ? 0 : -1;
}

/* What libav may touch. The local-file case reads through our own
 * AVIOContext on an fd the parent opened, so it needs no protocol at all
 * ("none" matches no protocol, so a demuxer cannot open nested files). */
#define NET_PROTOCOLS  "rtsp,rtsps,tcp,udp,tls,rtp,srtp,crypto"
#define NET_FORMATS    "rtsp,sdp,rtp"
#define FILE_PROTOCOLS "none"
#define FILE_FORMATS   "mov,mp4,matroska"
#define CODECS         "h264"

/* A probe target: a network URL, or a local file already opened (and
 * checked to be a regular file) by the caller. */
struct probe_target {
	const char *url;
	int fd;               /* local file, or -1 */
	bool skip;            /* result already filled in (could not open) */
};

struct fd_io {
	int fd;
};

static int fd_read(void *opaque, uint8_t *buf, int size)
{
	const struct fd_io *io = opaque;
	ssize_t n;
	do
		n = read(io->fd, buf, (size_t)size);
	while (n < 0 && errno == EINTR);
	if (n == 0)
		return AVERROR_EOF;
	return n < 0 ? AVERROR(errno) : (int)n;
}

static int64_t fd_seek(void *opaque, int64_t off, int whence)
{
	const struct fd_io *io = opaque;
	if (whence & AVSEEK_SIZE) {
		struct stat st;
		return fstat(io->fd, &st) == 0 ? (int64_t)st.st_size : AVERROR(errno);
	}
	off_t r = lseek(io->fd, (off_t)off, whence & ~AVSEEK_FORCE);
	return r < 0 ? AVERROR(errno) : (int64_t)r;
}

/* The probe itself. Runs in the sandboxed child when root. */
static void probe_do(const struct probe_target *t, int fd, int timeout_ms, struct probe_result *r)
{
	probe_init_libav();
	memset(r, 0, sizeof *r);
	r->uid = (int)getuid();
	last_av_msg[0] = '\0';

	int64_t t0 = mono_ms();
	struct deadline dl = { t0 + timeout_ms };
	bool file = fd >= 0;
	bool rtsp = !file && layout_url_is_rtsp(t->url);
	bool live = !file;

	if (!file && !rtsp) {
		r->err = PROBE_ERR_OTHER;
		snprintf(r->detail, sizeof r->detail, "only rtsp://, rtsps:// and local files can "
			 "be probed");
		return;
	}

	AVFormatContext *ic = avformat_alloc_context();
	AVIOContext *pb = NULL;
	struct fd_io io = { fd };
	if (ic && file) {
		unsigned char *buf = av_malloc(32768);
		pb = buf ? avio_alloc_context(buf, 32768, 0, &io, fd_read, NULL, fd_seek) : NULL;
		if (!pb)
			av_free(buf);
		else
			ic->pb = pb;
	}
	if (!ic || (file && !pb)) {
		avformat_free_context(ic);
		r->err = PROBE_ERR_OTHER;
		snprintf(r->detail, sizeof r->detail, "out of memory");
		return;
	}
	ic->interrupt_callback.callback = interrupt_cb;
	ic->interrupt_callback.opaque = &dl;

	AVDictionary *opts = NULL;
	av_dict_set(&opts, "protocol_whitelist", file ? FILE_PROTOCOLS : NET_PROTOCOLS, 0);
	av_dict_set(&opts, "format_whitelist", file ? FILE_FORMATS : NET_FORMATS, 0);
	av_dict_set(&opts, "codec_whitelist", CODECS, 0);
	if (rtsp) {
		char us[32];
		snprintf(us, sizeof us, "%lld", (long long)timeout_ms * 1000);
		av_dict_set(&opts, "rtsp_transport", "tcp", 0);
		av_dict_set(&opts, "initial_pause", "1", 0);   /* DESCRIBE + SETUP, no PLAY */
		av_dict_set(&opts, "timeout", us, 0);          /* socket I/O, microseconds */
	}

	int e = avformat_open_input(&ic, t->url, NULL, &opts);   /* frees ic on failure */
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
		if (pb) {
			av_freep(&pb->buffer);
			avio_context_free(&pb);
		}
		return;
	}

	int vi = -1;
	for (unsigned i = 0; i < ic->nb_streams; i++)
		if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			vi = (int)i;
			break;
		}

	/* The codec comes from the stream description (SDP / container): an
	 * H.265 stream is recognised here, before any decoder exists. */
	if (vi >= 0)
		fill_from_stream(ic->streams[vi], live, r);

	/* Without a video stream description in the SDP (unusual) a short read
	 * may still find it. Only H.264 (or not yet known) streams get here,
	 * and codec_whitelist=h264 keeps libavcodec from opening any other
	 * decoder. */
	bool need_more = vi < 0 ||
			 ((r->s.codec == PROBE_CODEC_H264 || r->s.codec == PROBE_CODEC_UNKNOWN) &&
			  (r->s.width <= 0 || r->s.height <= 0 || r->s.fps <= 0));
	if (need_more) {
		/* Fallback: PLAY and look at <= 2 s of packets. */
		dl.at_ms = mono_ms() + PROBE_FALLBACK_MS;
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
					snprintf(r->fps_source, sizeof r->fps_source, "packets");
			}
		}
	}

	avformat_close_input(&ic);   /* sends TEARDOWN */
	if (pb) {
		av_freep(&pb->buffer);
		avio_context_free(&pb);
	}
	r->elapsed_ms = (int)(mono_ms() - t0);
	if (vi < 0) {
		r->err = PROBE_ERR_NO_VIDEO;
		snprintf(r->detail, sizeof r->detail, "no video stream");
		return;
	}
	r->err = PROBE_OK;
}

/* ------------------------------------------------------- probe targets */

/* "file:PATH" / "file://PATH" -> PATH. */
static const char *file_path(const char *url)
{
	if (strncasecmp(url, "file:", 5) != 0)
		return url;
	url += 5;
	if (url[0] == '/' && url[1] == '/')
		url += 2;
	return url;
}

/* Opens a local file for probing: as the user running rtspwall (root
 * reads root-only files), and only if it is a regular file — never a
 * FIFO, device or directory. The child gets the fd, not the path. */
static void target_open(const char *url, struct probe_target *t, struct probe_result *r)
{
	t->url = url;
	t->fd = -1;
	t->skip = false;
	if (layout_url_is_live(url))
		return;

	const char *path = file_path(url);
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
	struct stat st;
	if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
		memset(r, 0, sizeof *r);
		r->err = PROBE_ERR_OTHER;
		r->uid = (int)getuid();
		snprintf(r->detail, sizeof r->detail, "%s", fd < 0 ? strerror(errno) :
			 "not a regular file (only regular files are probed)");
		if (fd >= 0)
			close(fd);
		t->skip = true;
		return;
	}
	t->fd = fd;
}

/* -------------------------------------------------------- the sandbox */

struct probe_job {
	const struct probe_target *t;
	int timeout_ms;
	struct probe_result *res;
};

static void probe_child(void *ctx, int fd, void *out)
{
	const struct probe_job *j = ctx;
	probe_do(j->t, fd, j->timeout_ms, out);
}

static bool known_fps_source(const char *s)
{
	static const char *const ok[] = { "", "VUI", "SDP", "container", "packets" };
	for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++)
		if (strcmp(s, ok[i]) == 0)
			return true;
	return false;
}

/* The child's result is untrusted input too: check every field before
 * anything prints or adds it up. */
static bool result_valid(struct probe_result *r, uid_t expected_uid)
{
	r->detail[sizeof r->detail - 1] = '\0';
	r->codec_name[sizeof r->codec_name - 1] = '\0';
	r->fps_source[sizeof r->fps_source - 1] = '\0';
	layout_sanitize_log_text(r->detail);
	layout_sanitize_log_text(r->codec_name);
	const struct probe_stream *s = &r->s;
	return (unsigned)r->err <= PROBE_ERR_OTHER &&
	       (unsigned)s->codec <= PROBE_CODEC_OTHER &&
	       s->width >= 0 && s->width <= 65536 && s->height >= 0 && s->height <= 65536 &&
	       s->fps >= 0.0 && s->fps <= 240.0 &&
	       s->profile_idc >= 0 && s->profile_idc <= 255 &&
	       s->constraint_flags >= 0 && s->constraint_flags <= 255 &&
	       s->level_idc >= 0 && s->level_idc <= 255 &&
	       r->elapsed_ms >= 0 && known_fps_source(r->fps_source) &&
	       r->uid == (int)expected_uid;
}

static void sandbox_failed(struct probe_result *r, const struct sandbox_job *j,
			   const struct sandbox_user *u, int limit_ms)
{
	memset(r, 0, sizeof *r);
	r->uid = -1;
	r->elapsed_ms = j->status == SANDBOX_TIMEOUT ? limit_ms : 0;
	switch (j->status) {
	case SANDBOX_TIMEOUT:
		r->err = PROBE_ERR_TIMEOUT;
		snprintf(r->detail, sizeof r->detail, "no answer (probe process stopped after %d s)",
			 limit_ms / 1000);
		break;
	case SANDBOX_CRASHED:
		r->err = PROBE_ERR_OTHER;
		snprintf(r->detail, sizeof r->detail, "the probe process died (signal %d, %s) while "
			 "reading the stream: a malformed stream, or a bug - please report it",
			 j->signo, strsignal(j->signo));
		break;
	case SANDBOX_NOPRIV:
		r->err = PROBE_ERR_OTHER;
		snprintf(r->detail, sizeof r->detail, "could not drop root privileges to user %s "
			 "for the probe; not probing as root", u->name);
		break;
	default:
		r->err = PROBE_ERR_OTHER;
		snprintf(r->detail, sizeof r->detail, "the probe process %s",
			 sandbox_status_text(j->status));
		break;
	}
}

static void probe_sandboxed(const struct probe_target *t, int n, int timeout_ms,
			    struct probe_result *res)
{
	struct probe_job pj[LAYOUT_MAX_CAMERAS];
	struct sandbox_job sj[LAYOUT_MAX_CAMERAS];
	int idx[LAYOUT_MAX_CAMERAS], nj = 0;
	const struct sandbox_user *u = sandbox_user();
	/* The child stops itself after timeout + fallback; this is the hard
	 * limit for a child that hangs anyway. */
	int limit_ms = timeout_ms + PROBE_FALLBACK_MS + 2000;

	for (int i = 0; i < n; i++) {
		if (t[i].skip)
			continue;
		pj[nj] = (struct probe_job){ &t[i], timeout_ms, &res[i] };
		sj[nj] = (struct sandbox_job){ .fn = probe_child, .ctx = &pj[nj], .fd = t[i].fd,
					       .out = &res[i], .outlen = sizeof res[i] };
		idx[nj++] = i;
	}
	sandbox_run(sj, nj, limit_ms);

	bool debug = getenv("RTSPWALL_DEBUG") != NULL;
	for (int k = 0; k < nj; k++) {
		struct probe_result *r = &res[idx[k]];
		if (sj[k].status != SANDBOX_OK) {
			sandbox_failed(r, &sj[k], u, limit_ms);
		} else if (!result_valid(r, u->uid)) {
			memset(r, 0, sizeof *r);
			r->err = PROBE_ERR_OTHER;
			r->uid = -1;
			snprintf(r->detail, sizeof r->detail, "the probe process returned an invalid "
				 "result");
		}
		if (debug) {
			char masked[LAYOUT_URL_MAX];
			layout_mask_url(t[idx[k]].url, masked, sizeof masked);
			fprintf(stderr, "debug: probe of %s: pid %d, ran as uid %d (%s), %s\n", masked,
				(int)sj[k].pid, r->uid, u->name, sandbox_status_text(sj[k].status));
		}
	}
}

/* ------------------------------------------------------ parallel probing */

static void *probe_thread(void *arg)
{
	struct probe_job *j = arg;
	probe_do(j->t, j->t->fd, j->timeout_ms, j->res);
	return NULL;
}

void probe_urls(const char *const *urls, int n, int timeout_ms, struct probe_result *res)
{
	struct probe_target t[LAYOUT_MAX_CAMERAS];

	if (n > LAYOUT_MAX_CAMERAS)
		n = LAYOUT_MAX_CAMERAS;
	for (int i = 0; i < n; i++)
		target_open(urls[i], &t[i], &res[i]);

	if (geteuid() == 0) {
		/* Untrusted stream data is never parsed as root. */
		probe_sandboxed(t, n, timeout_ms, res);
	} else {
		pthread_t th[LAYOUT_MAX_CAMERAS];
		struct probe_job jobs[LAYOUT_MAX_CAMERAS];
		bool started[LAYOUT_MAX_CAMERAS] = { false };

		probe_init_libav();
		for (int i = 0; i < n; i++) {
			if (t[i].skip)
				continue;
			jobs[i] = (struct probe_job){ &t[i], timeout_ms, &res[i] };
			started[i] = pthread_create(&th[i], NULL, probe_thread, &jobs[i]) == 0;
			if (!started[i])
				probe_do(&t[i], t[i].fd, timeout_ms, &res[i]);
		}
		for (int i = 0; i < n; i++)
			if (started[i])
				pthread_join(th[i], NULL);
	}
	for (int i = 0; i < n; i++)
		if (t[i].fd >= 0)
			close(t[i].fd);
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
			       r->fps_source[0] ? r->fps_source : "?", budget_nominal_fps(r->s.fps));
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
		if (c->unifi == LAYOUT_UNIFI_PLAIN)
			printf("    WARNING: UNIFI_REWRITE=plain: plain RTSP on port 7447, the token "
			       "and the video are unencrypted\n");
		else if (c->unifi == LAYOUT_UNIFI_KEPT_TLS)
			printf("    note: UniFi Protect over rtsps (TLS kept)\n");
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
		"       rtspwall probe FILE       (a local video file: .mp4/.mov/.mkv)\n"
		"       rtspwall probe URL        (only a URL without password, token or query:\n"
		"                                  the command line ends up in shell history,\n"
		"                                  `ps` and sudo's log)\n"
		"\n"
		"  --insecure-argv   accept a URL with a password/token on the command line\n"
		"                    anyway\n"
		"\n"
		"Checks codec, profile, size and frame rate against the Pi 4 H.264 decoder\n"
		"without playing the stream (RTSP DESCRIBE + SETUP, no PLAY), timeout %d s.\n"
		"Run as root, each probe runs as the unprivileged user " SANDBOX_USER ".\n"
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

/* Prints what UNIFI_REWRITE does with a UniFi Protect URL (nothing for
 * other URLs). `use` is the URL the wall will play. */
void probe_unifi_note(enum layout_unifi_result res, const char *use)
{
	char masked[LAYOUT_URL_MAX];
	layout_mask_url(use, masked, sizeof masked);
	switch (res) {
	case LAYOUT_UNIFI_KEPT_TLS:
		printf("note:     UniFi Protect URL: the wall plays it as %s - rtsps, TLS is kept "
		       "(UNIFI_REWRITE=tls, the default; only ?enableSrtp is dropped)\n", masked);
		break;
	case LAYOUT_UNIFI_PLAIN:
		printf("WARNING:  UNIFI_REWRITE=plain: the wall plays this camera as %s - plain "
		       "RTSP on port 7447, so the access token and the video cross the network "
		       "UNENCRYPTED. Remove UNIFI_REWRITE=plain (default: tls) to keep TLS.\n",
		       masked);
		break;
	case LAYOUT_UNIFI_OFF:
		printf("note:     UniFi Protect URL, UNIFI_REWRITE=off: used exactly as written "
		       "(rtsps, TLS)\n");
		break;
	case LAYOUT_UNIFI_NOT_UNIFI:
		break;
	}
}

/* UNIFI_REWRITE of the installed config, for a URL probed on its own;
 * the default (tls) when there is none. */
enum layout_unifi_mode probe_default_unifi_mode(void)
{
	static struct layout_config cfg;
	char err[256];
	if (access(CLI_DEFAULT_CONFIG, R_OK) == 0 &&
	    cli_load_config(CLI_DEFAULT_CONFIG, &cfg, err, sizeof err) == 0)
		return cfg.unifi_mode;
	return LAYOUT_UNIFI_MODE_TLS;
}

/* Probes one URL as typed/pasted, the way the daemon will play it with
 * UNIFI_REWRITE=`mode` (UniFi Protect rtsps URLs). */
int probe_single(const char *url, enum layout_unifi_mode mode, struct probe_result *r,
		 enum budget_verdict *v)
{
	char played[LAYOUT_URL_MAX];
	enum layout_unifi_result res = LAYOUT_UNIFI_NOT_UNIFI;
	const char *use = url;

	if (layout_unifi_apply(url, mode, played, sizeof played, &res) == 0 &&
	    res != LAYOUT_UNIFI_NOT_UNIFI) {
		probe_unifi_note(res, played);
		use = played;
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
	bool insecure_argv = false;
	const char *arg = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			probe_usage(stdout);
			return 0;
		}
		if (strcmp(argv[i], "--insecure-argv") == 0) {
			insecure_argv = true;
		} else if (arg || (argv[i][0] == '-' && argv[i][1])) {
			probe_usage(stderr);
			return 2;
		} else {
			arg = argv[i];
		}
	}

	if (!arg || strcmp(arg, "-") == 0) {
		if (cli_read_url(arg != NULL, url, sizeof url) < 0)
			return 2;
		const char *why = cfg_check_url(url);
		if (why)
			fprintf(stderr, "rtspwall: note: %s\n", why);
		return probe_single(url, probe_default_unifi_mode(), &r, &v);
	}

	if (strstr(arg, "://")) {
		const char *secret = url_secret_reason(arg);
		if (secret && !insecure_argv) {
			fprintf(stderr,
				"rtspwall: not probing: this URL contains %s, and a command line is "
				"saved in your shell history, visible in `ps` and logged by sudo.\n"
				"Give the URL without the command line instead:\n"
				"  sudo rtspwall probe              (hidden prompt; paste the URL)\n"
				"  sudo rtspwall probe - < url.txt  (read it from stdin)\n"
				"or add --insecure-argv to use it from the command line anyway.\n",
				secret);
			return 2;
		}
		fprintf(stderr, "rtspwall: note: a URL on the command line is saved in your shell "
				"history and sudo's log; next time use the hidden prompt "
				"(`rtspwall probe`)\n");
		return probe_single(arg, probe_default_unifi_mode(), &r, &v);
	}

	/* A file: a config to probe camera by camera, or a local media file.
	 * Only regular files: a FIFO or device would block or be read as
	 * root. */
	struct stat st;
	if (stat(arg, &st) < 0) {
		fprintf(stderr, "rtspwall: %s: %s\n", arg, strerror(errno));
		return 2;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "rtspwall: %s: not a regular file (probe takes a URL, a config "
				"file or a video file)\n", arg);
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
	return probe_single(arg, probe_default_unifi_mode(), &r, &v);
}
