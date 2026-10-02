/*
 * doctor.c — `rtspwall doctor [--fix] [--yes] [--report] [--summary]
 *                            [--no-hardware] [--no-probe] [--config PATH]`.
 *
 * One PASS/WARN/FAIL/INFO line per check, each problem with a fix to copy.
 * Exit status: 0 = everything passes, 1 = at least one WARN, 2 = at least
 * one FAIL (INFO never changes it).
 *
 * --fix only ever changes one thing: gpu_mem=256 in config.txt, only when
 * the configured cameras need it, only after asking on /dev/tty (or with
 * --yes), and with a backup of config.txt. It never reboots.
 *
 * --report prints one block for bug reports (versions, board, OS, gpu_mem,
 * displays/modes/planes, V4L2 devices, filtered dmesg, the last 15 minutes
 * of the unit journal, --check-config output, the check results). Every
 * line goes through report_append_masked (clilogic.c): all scheme:// URLs
 * are masked with layout_mask_urls_in_text, plus password/token=VALUE pairs.
 *
 * --no-hardware skips everything that needs the Pi itself (for CI).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <linux/videodev2.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "cli.h"

#ifndef VERSION
#define VERSION "0.1.0-dev"
#endif

#define SERVICE_USER "rtspwall"
#define GPU_MEM_DEFAULT_MB 76      /* Pi 4 firmware default */

/* Kernels the wall has been verified on (MMP-11 fills this in; one entry
 * per tested kernel, e.g. "6.6.51+rpt-rpi-v8"). */
static const char *const tested_kernels[] = { NULL };

enum level { L_PASS, L_INFO, L_WARN, L_FAIL };
static const char *const level_name[] = { "PASS", "INFO", "WARN", "FAIL" };

struct check {
	enum level level;
	char name[16];
	char msg[512];
	char fix[512];
};

#define MAX_CHECKS 64      /* ~20 checks + one unifi line per camera */

struct doctor {
	struct check checks[MAX_CHECKS];
	int n;

	const char *config;
	struct layout_config cfg;
	bool cfg_ok;
	bool hardware, probe;

	/* gathered for --fix and --report */
	int  gpu_needed;              /* MB, 0 = default is enough / unknown */
	int  gpu_effective;           /* MB */
	int  gpu_configtxt;           /* MB in config.txt, -1 = unset */
	struct configtxt_gpu gpu_info;  /* which key, filters doctor cannot evaluate */
	char configtxt_path[64];
	char display_text[4096];
	char model[128];
};

static void add_check(struct doctor *d, enum level l, const char *name, const char *fix,
		      const char *fmt, ...) __attribute__((format(printf, 5, 6)));

static void add_check(struct doctor *d, enum level l, const char *name, const char *fix,
		      const char *fmt, ...)
{
	if (d->n >= MAX_CHECKS)
		return;
	struct check *c = &d->checks[d->n++];
	char raw[512];
	va_list ap;

	c->level = l;
	snprintf(c->name, sizeof c->name, "%s", name);
	va_start(ap, fmt);
	vsnprintf(raw, sizeof raw, fmt, ap);
	va_end(ap);
	/* Messages may quote config lines or logs: mask like the report. */
	report_mask_line(raw, c->msg, sizeof c->msg);
	report_mask_line(fix ? fix : "", c->fix, sizeof c->fix);
}

static void read_small(const char *path, char *out, size_t outlen)
{
	out[0] = '\0';
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	ssize_t n = read(fd, out, outlen - 1);
	close(fd);
	if (n <= 0) {
		out[0] = '\0';
		return;
	}
	out[n] = '\0';
	/* device-tree strings are NUL-terminated; sysfs ends in '\n' */
	out[strcspn(out, "\n")] = '\0';
}

/* ------------------------------------------------------------- the checks */

static void check_board(struct doctor *d)
{
	read_small("/proc/device-tree/model", d->model, sizeof d->model);
	switch (board_classify(d->model)) {
	case BOARD_PI4:
		add_check(d, L_PASS, "board", NULL, "%s", d->model);
		break;
	case BOARD_PI4_FAMILY:
		add_check(d, L_INFO, "board", NULL, "%s: same SoC as the Pi 4, untested "
			  "(reports welcome)", d->model);
		break;
	case BOARD_PI5:
		add_check(d, L_FAIL, "board",
			  "rtspwall needs a Raspberry Pi 4 (see docs/faq.md for Pi 5 alternatives)",
			  "%s: no H.264 hardware decoder", d->model);
		break;
	case BOARD_OLDER_PI:
		add_check(d, L_FAIL, "board", "rtspwall needs a Raspberry Pi 4",
			  "%s: not supported (different decoder/display stack)", d->model);
		break;
	case BOARD_UNKNOWN:
		add_check(d, L_INFO, "board", NULL, "%s",
			  d->model[0] ? d->model : "not a Raspberry Pi (no device-tree model)");
		break;
	}
}

static void check_userland(struct doctor *d)
{
	struct utsname u;
	uname(&u);
	if (sizeof(void *) == 8)
		add_check(d, L_PASS, "userland", NULL, "64-bit (kernel %s)", u.machine);
	else
		add_check(d, L_FAIL, "userland",
			  "reflash with Raspberry Pi OS Lite (64-bit) - Raspberry Pi Imager, "
			  "\"Raspberry Pi OS (other)\"",
			  "32-bit userland (kernel %s): the .deb is built for arm64", u.machine);
}

static void check_decoder(struct doctor *d)
{
	const char *dev = d->cfg_ok ? d->cfg.decoder : LAYOUT_DEFAULT_DECODER;
	int fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		if (errno == ENOENT)
			add_check(d, L_FAIL, "decoder",
				  "on a Pi 4: sudo modprobe bcm2835-codec, and check that "
				  "config.txt does not disable it; a Pi 5 has no H.264 decoder",
				  "%s does not exist: no H.264 hardware decoder", dev);
		else
			add_check(d, L_FAIL, "decoder", "run doctor with sudo",
				  "cannot open %s: %s", dev, strerror(errno));
		return;
	}
	struct v4l2_capability cap;
	memset(&cap, 0, sizeof cap);
	if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
		add_check(d, L_FAIL, "decoder", NULL, "VIDIOC_QUERYCAP on %s failed: %s", dev,
			  strerror(errno));
		close(fd);
		return;
	}
	bool h264 = false;
	struct v4l2_fmtdesc f;
	for (unsigned i = 0; i < 64; i++) {
		memset(&f, 0, sizeof f);
		f.index = i;
		f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		if (ioctl(fd, VIDIOC_ENUM_FMT, &f) < 0)
			break;
		if (f.pixelformat == V4L2_PIX_FMT_H264)
			h264 = true;
	}
	close(fd);

	if (strcmp((const char *)cap.driver, "bcm2835-codec") == 0 && h264)
		add_check(d, L_PASS, "decoder", NULL, "%s: %s (%s), H.264", dev,
			  (const char *)cap.driver, (const char *)cap.card);
	else if (!h264)
		add_check(d, L_FAIL, "decoder", "set DECODER= to the bcm2835-codec decode node "
			  "(usually /dev/video10)",
			  "%s (%s, %s) does not decode H.264", dev, (const char *)cap.driver,
			  (const char *)cap.card);
	else
		add_check(d, L_WARN, "decoder", NULL, "%s is %s (%s), not bcm2835-codec: untested",
			  dev, (const char *)cap.driver, (const char *)cap.card);
}

static void find_configtxt(struct doctor *d)
{
	static const char *paths[] = { "/boot/firmware/config.txt", "/boot/config.txt" };
	d->configtxt_path[0] = '\0';
	for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
		if (access(paths[i], F_OK) == 0) {
			snprintf(d->configtxt_path, sizeof d->configtxt_path, "%s", paths[i]);
			return;
		}
}

static int vcgencmd_gpu_mem(void)
{
	char out[128];
	char *argv[] = { "vcgencmd", "get_mem", "gpu", NULL };
	if (cli_run(argv, out, sizeof out, 5000) != 0)
		return -1;
	const char *eq = strchr(out, '=');
	return eq ? atoi(eq + 1) : -1;
}

static void check_gpu_mem(struct doctor *d)
{
	find_configtxt(d);
	int conf_mb = -1;
	struct configtxt_gpu *gi = &d->gpu_info;
	memset(gi, 0, sizeof *gi);
	gi->value = -1;
	gi->key = "";
	if (d->configtxt_path[0]) {
		char *t = cli_read_file(d->configtxt_path, 256 * 1024);
		if (t) {
			configtxt_gpu_mem_info(t, gi);
			conf_mb = gi->value;
			free(t);
		}
	}
	d->gpu_configtxt = conf_mb;
	int live_mb = vcgencmd_gpu_mem();
	d->gpu_effective = live_mb > 0 ? live_mb : conf_mb > 0 ? conf_mb : GPU_MEM_DEFAULT_MB;

	char have[200];
	snprintf(have, sizeof have, "gpu_mem %d MB (%s%s%s%s)", d->gpu_effective,
		 live_mb > 0 ? "vcgencmd" : conf_mb > 0 ? "config.txt " : "firmware default",
		 live_mb <= 0 && conf_mb > 0 ? gi->key : "",
		 conf_mb > 0 && live_mb > 0 && conf_mb != live_mb ? ", config.txt says " : "",
		 conf_mb > 0 && live_mb > 0 && conf_mb != live_mb ? "a different value" : "");

	/* A gpu_mem line under a filter doctor cannot evaluate ([HDMI:0],
	 * [board-type=...], [EDID=...], ...) may or may not be what the
	 * firmware uses: say so rather than PASS on a guess. */
	char uncertain_fix[300] = "";
	if (gi->uncertain)
		snprintf(uncertain_fix, sizeof uncertain_fix,
			 "check the gpu_mem lines under %s in %s by hand (doctor cannot tell "
			 "whether that filter matches this Pi), or move gpu_mem to [all]",
			 gi->filter, d->configtxt_path);

	if (!d->cfg_ok || !d->probe) {
		const char *why = !d->cfg_ok ? "need unknown (config not valid)"
					     : "need not checked (--no-probe)";
		if (gi->uncertain)
			add_check(d, L_WARN, "gpu_mem", uncertain_fix, "%s; %s; config.txt sets gpu_mem "
				  "under %s, which may or may not apply here", have, why, gi->filter);
		else
			add_check(d, L_INFO, "gpu_mem", NULL, "%s; %s", have, why);
		return;
	}

	const char *urls[LAYOUT_MAX_CAMERAS];
	static struct probe_result res[LAYOUT_MAX_CAMERAS];
	for (int i = 0; i < d->cfg.count; i++)
		urls[i] = d->cfg.cam[i].url;
	probe_urls(urls, d->cfg.count, CLI_PROBE_TIMEOUT_MS, res);
	long total = 0;
	int measured = 0;
	for (int i = 0; i < d->cfg.count; i++) {
		long m = res[i].err == PROBE_OK && res[i].s.codec == PROBE_CODEC_H264
			 ? budget_stream_mbps(res[i].s.width, res[i].s.height, res[i].s.fps) : 0;
		if (m > 0) {
			total += m;
			measured++;
		}
	}
	if (!measured) {
		char fix[300];
		snprintf(fix, sizeof fix, "sudo rtspwall probe %.200s", d->config);
		add_check(d, L_INFO, "gpu_mem", fix, "%s; need unknown (no camera could be probed)",
			  have);
		return;
	}
	d->gpu_needed = doctor_gpu_mem_needed(total, d->cfg.count);
	if (d->gpu_needed && d->gpu_effective < d->gpu_needed && conf_mb >= d->gpu_needed) {
		add_check(d, L_WARN, "gpu_mem", "sudo reboot",
			  "%s; %s=%d is set in config.txt but not active until a reboot",
			  have, gi->key, conf_mb);
	} else if (d->gpu_needed && d->gpu_effective < d->gpu_needed) {
		char fix[300];
		if (gi->uncertain)
			snprintf(fix, sizeof fix, "%s", uncertain_fix);
		else
			snprintf(fix, sizeof fix, "sudo rtspwall doctor --fix   (sets %s=%d in %s, "
				 "then: sudo reboot)", gi->key[0] ? gi->key : "gpu_mem", d->gpu_needed,
				 d->configtxt_path[0] ? d->configtxt_path : "config.txt");
		add_check(d, L_WARN, "gpu_mem", fix,
			  "%s; the cameras use %.0f %% of the decoder%s and need %d MB%s%s%s", have,
			  budget_percent(total), measured < d->cfg.count ? " (lower bound)" : "",
			  d->gpu_needed, gi->uncertain ? " (config.txt also sets gpu_mem under " : "",
			  gi->uncertain ? gi->filter : "",
			  gi->uncertain ? ", which may or may not apply here)" : "");
	} else if (gi->uncertain) {
		add_check(d, L_WARN, "gpu_mem", uncertain_fix, "%s; enough for %.0f %% decoder load, "
			  "but config.txt sets gpu_mem under %s, which may or may not apply here "
			  "(after a reboot the value may differ)", have, budget_percent(total),
			  gi->filter);
	} else {
		add_check(d, L_PASS, "gpu_mem", NULL, "%s; enough for %.0f %% decoder load", have,
			  budget_percent(total));
	}
}

static void check_display_server(struct doctor *d)
{
	DIR *proc = opendir("/proc");
	char found[256] = "";
	if (proc) {
		struct dirent *e;
		while ((e = readdir(proc)) != NULL) {
			if (!isdigit((unsigned char)e->d_name[0]))
				continue;
			char path[64], comm[64];
			snprintf(path, sizeof path, "/proc/%.32s/comm", e->d_name);
			read_small(path, comm, sizeof comm);
			if (comm[0] && layout_is_display_server(comm) && strlen(found) < 180) {
				size_t n = strlen(found);
				snprintf(found + n, sizeof found - n, "%s%.32s (PID %.12s)", n ? ", " : "",
					 comm, e->d_name);
			}
		}
		closedir(proc);
	}
	if (found[0])
		add_check(d, L_FAIL, "desktop",
			  "sudo raspi-config nonint do_boot_behaviour B1 && sudo reboot   "
			  "(boot to the console instead of the desktop)",
			  "%s running: a desktop holds the display (DRM master)", found);
	else
		add_check(d, L_PASS, "desktop", NULL, "no desktop/display server holds the display");
}

static const char *port_hint(const char *name)
{
	if (strcmp(name, "HDMI-A-1") == 0)
		return " = HDMI0, next to the USB-C power port";
	if (strcmp(name, "HDMI-A-2") == 0)
		return " = HDMI1";
	return "";
}

static void check_displays(struct doctor *d)
{
	char *t = d->display_text;
	size_t tl = sizeof d->display_text, used = 0;
	int connected = 0, cards = 0;
	char chosen[256] = "";
	t[0] = '\0';

#define DAPPEND(...) do { if (used < tl) used += (size_t)snprintf(t + used, tl - used, __VA_ARGS__); } while (0)

	for (int card = 0; card < 16; card++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/card%d", card);
		int fd = open(path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		drmModeRes *res = drmModeGetResources(fd);
		if (!res) {
			close(fd);
			continue;
		}
		cards++;
		drmVersionPtr ver = drmGetVersion(fd);
		DAPPEND("%s: driver %s\n", path, ver && ver->name ? ver->name : "?");
		if (ver)
			drmFreeVersion(ver);

		for (int i = 0; i < res->count_connectors; i++) {
			drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
			if (!c)
				continue;
			const char *type = drmModeGetConnectorTypeName(c->connector_type);
			char name[32];
			snprintf(name, sizeof name, "%s-%u", type ? type : "Unknown", c->connector_type_id);
			bool is_conn = c->connection == DRM_MODE_CONNECTED;
			DAPPEND("  %s%s: %s, %d modes\n", name, port_hint(name),
				is_conn ? "connected" : "disconnected", c->count_modes);
			if (is_conn && c->count_modes > 0) {
				connected++;
				struct layout_mode modes[128];
				int n = c->count_modes < 128 ? c->count_modes : 128;
				for (int m = 0; m < n; m++) {
					const drmModeModeInfo *mi = &c->modes[m];
					long mhz = 0;
					if (mi->htotal && mi->vtotal)
						mhz = (long)((double)mi->clock * 1000000.0 /
							     ((double)mi->htotal * mi->vtotal) + 0.5);
					modes[m] = (struct layout_mode){ mi->hdisplay, mi->vdisplay, (int)mhz,
						(mi->flags & DRM_MODE_FLAG_INTERLACE) != 0,
						(mi->type & DRM_MODE_TYPE_PREFERRED) != 0 };
					if (m < 12)
						DAPPEND("    %dx%d@%.2f%s%s\n", modes[m].width, modes[m].height,
							modes[m].refresh_mhz / 1000.0,
							modes[m].interlaced ? "i" : "",
							modes[m].preferred ? " (preferred)" : "");
				}
				enum layout_mode_reason why;
				int want_w = d->cfg_ok ? d->cfg.mode_w : 0, want_h = d->cfg_ok ? d->cfg.mode_h : 0;
				int want_mhz = d->cfg_ok ? d->cfg.mode_mhz : 0;
				int sel = layout_select_mode(modes, n, want_w, want_h, want_mhz, &why);
				bool wanted = !d->cfg_ok || !d->cfg.connector[0] ||
					      strcmp(d->cfg.connector, name) == 0;
				if (sel >= 0 && wanted && !chosen[0])
					snprintf(chosen, sizeof chosen, "%s%s: %dx%d@%.2f (%s)", name,
						 port_hint(name), modes[sel].width, modes[sel].height,
						 modes[sel].refresh_mhz / 1000.0,
						 why == LAYOUT_MODE_EXPLICIT ? "MODE" :
						 why == LAYOUT_MODE_PREFERRED ? "preferred" :
						 why == LAYOUT_MODE_AUTO_4K ? "auto, instead of 4K" :
						 "auto, instead of < 50 Hz");
				else if (sel < 0 && wanted && !chosen[0])
					snprintf(chosen, sizeof chosen, "!%s: no mode matches MODE", name);
			}
			drmModeFreeConnector(c);
		}

		drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
		drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
		if (pr) {
			int nv12 = 0;
			for (unsigned p = 0; p < pr->count_planes; p++) {
				drmModePlane *pl = drmModeGetPlane(fd, pr->planes[p]);
				if (!pl)
					continue;
				for (unsigned f = 0; f < pl->count_formats; f++)
					if (pl->formats[f] == 0x3231564e) {   /* DRM_FORMAT_NV12 */
						nv12++;
						break;
					}
				drmModeFreePlane(pl);
			}
			DAPPEND("  planes: %u (%d take NV12)\n", pr->count_planes, nv12);
			drmModeFreePlaneResources(pr);
		}
		drmModeFreeResources(res);
		close(fd);
	}
#undef DAPPEND

	if (!cards) {
		add_check(d, L_FAIL, "display", "on a Pi 4: dtoverlay=vc4-kms-v3d in config.txt "
			  "(the Raspberry Pi OS default), then reboot",
			  "no DRM/KMS device (/dev/dri/card*)");
	} else if (!connected) {
		add_check(d, L_FAIL, "display",
			  "connect the TV to HDMI0 (next to the USB-C power port) and switch it on; "
			  "the service waits for it",
			  "no connected display");
	} else if (chosen[0] == '!') {
		add_check(d, L_FAIL, "display", "fix MODE= in the config (rtspwall --check-config "
			  "lists the modes) or use MODE=auto", "%s", chosen + 1);
	} else if (!chosen[0]) {
		add_check(d, L_FAIL, "display", "fix CONNECTOR= in the config, or remove it",
			  "the configured CONNECTOR=%s is not connected",
			  d->cfg_ok ? d->cfg.connector : "?");
	} else {
		add_check(d, L_PASS, "display", NULL, "%d connected; would use %s", connected, chosen);
	}
}

/* Groups the service runs with: the user's own groups plus the unit's
 * SupplementaryGroups= (video render in the packaged unit). */
static int service_groups(const struct passwd *pw, gid_t *groups, int cap)
{
	int n = cap;
	if (getgrouplist(pw->pw_name, pw->pw_gid, groups, &n) < 0)
		n = 0;
	char out[512];
	char *argv[] = { "systemctl", "show", "-p", "SupplementaryGroups", "--value",
			 CLI_SERVICE, NULL };
	if (cli_have_systemd() && cli_run(argv, out, sizeof out, 5000) == 0) {
		char *save = NULL;
		for (char *tok = strtok_r(out, " \t\n", &save); tok && n < cap;
		     tok = strtok_r(NULL, " \t\n", &save)) {
			struct group *g = getgrnam(tok);
			if (g)
				groups[n++] = g->gr_gid;
		}
	}
	return n;
}

static void describe_mode(const struct stat *st, char *out, size_t outlen)
{
	struct group *g = getgrgid(st->st_gid);
	struct passwd *p = getpwuid(st->st_uid);
	snprintf(out, outlen, "%04o %s:%s", (unsigned)(st->st_mode & 07777),
		 p ? p->pw_name : "?", g ? g->gr_name : "?");
}

static void check_device_access(struct doctor *d)
{
	struct passwd *pw = getpwnam(SERVICE_USER);
	if (!pw) {
		add_check(d, L_WARN, "devices", "install the rtspwall package (it creates the "
			  "user), or run install.sh",
			  "service user '%s' does not exist", SERVICE_USER);
		return;
	}
	uid_t uid = pw->pw_uid;
	gid_t gid = pw->pw_gid;
	gid_t groups[64];
	int ng = service_groups(pw, groups, 64);

	char paths[40][96];
	int np = 0;
	DIR *dir = opendir("/dev/dri");
	if (dir) {
		struct dirent *e;
		while ((e = readdir(dir)) != NULL && np < 20)
			if (strncmp(e->d_name, "card", 4) == 0)
				snprintf(paths[np++], sizeof paths[0], "/dev/dri/%.60s", e->d_name);
		closedir(dir);
	}
	snprintf(paths[np++], sizeof paths[0], "%.90s",
		 d->cfg_ok ? d->cfg.decoder : LAYOUT_DEFAULT_DECODER);
	dir = opendir("/dev/dma_heap");
	if (dir) {
		struct dirent *e;
		while ((e = readdir(dir)) != NULL && np < 40)
			if (e->d_name[0] != '.')
				snprintf(paths[np++], sizeof paths[0], "/dev/dma_heap/%.60s", e->d_name);
		closedir(dir);
	}

	char bad[400] = "";
	bool heap_bad = false, group_bad = false;
	int ok = 0;
	for (int i = 0; i < np; i++) {
		struct stat st;
		if (stat(paths[i], &st) < 0)
			continue;     /* missing nodes are reported by the decoder/display checks */
		if (perm_allows(st.st_mode, st.st_uid, st.st_gid, uid, gid, groups, ng, true)) {
			ok++;
			continue;
		}
		char m[96];
		describe_mode(&st, m, sizeof m);
		size_t n = strlen(bad);
		if (n < 300)
			snprintf(bad + n, sizeof bad - n, "%s%.90s (%.60s)", n ? ", " : "", paths[i], m);
		if (strncmp(paths[i], "/dev/dma_heap/", 14) == 0)
			heap_bad = true;
		else
			group_bad = true;
	}
	if (bad[0]) {
		const char *fix = heap_bad && !group_bad
			? "echo 'SUBSYSTEM==\"dma_heap\", GROUP=\"video\", MODE=\"0660\"' | sudo tee "
			  "/etc/udev/rules.d/60-rtspwall-dma-heap.rules && sudo udevadm trigger"
			: "sudo usermod -aG video,render rtspwall && sudo systemctl restart rtspwall";
		add_check(d, L_FAIL, "devices", fix, "user %s cannot open %s", SERVICE_USER, bad);
	} else if (ok) {
		add_check(d, L_PASS, "devices", NULL, "user %s can open %d device node%s "
			  "(DRM, decoder, dma-heap)", SERVICE_USER, ok, ok == 1 ? "" : "s");
	} else {
		add_check(d, L_INFO, "devices", NULL, "no device nodes found to check");
	}
}

/* Readable by the service user, including search permission on every
 * directory on the way. */
static bool service_user_can_read(const char *path, char *why, size_t whylen)
{
	struct passwd *pw = getpwnam(SERVICE_USER);
	if (!pw)
		return true;     /* reported by the devices check */
	gid_t groups[64];
	int ng = service_groups(pw, groups, 64);

	char buf[PATH_MAX];      /* realpath() needs PATH_MAX (FORTIFY aborts otherwise) */
	if (!realpath(path, buf))
		snprintf(buf, sizeof buf, "%s", path);
	for (char *slash = strchr(buf + 1, '/');; slash = strchr(slash + 1, '/')) {
		struct stat st;
		if (slash)
			*slash = '\0';
		bool is_file = !slash;
		if (stat(buf, &st) == 0) {
			unsigned bits = perm_bits(st.st_mode, st.st_uid, st.st_gid, pw->pw_uid,
						  pw->pw_gid, groups, ng);
			if (is_file ? !(bits & 4) : !(bits & 1)) {
				char m[96];
				describe_mode(&st, m, sizeof m);
				snprintf(why, whylen, "%.160s (%.60s)", buf, m);
				return false;
			}
		}
		if (!slash)
			break;
		*slash = '/';
	}
	return true;
}

static void check_config_file(struct doctor *d, char *cc_out, size_t cc_outlen)
{
	cc_out[0] = '\0';
	if (access(d->config, F_OK) < 0) {
		add_check(d, L_FAIL, "config", "sudo rtspwall add front-door   (asks for the URL)",
			  "%s: %s", d->config, strerror(errno));
		return;
	}
	/* Whoever can write the config decides which URLs root's probes and the
	 * service open, and can redirect them: root (and the service group
	 * reading) only. */
	struct stat cst;
	if (stat(d->config, &cst) == 0) {
		bool w_owner = (cst.st_mode & S_IWUSR) && cst.st_uid != 0;
		bool w_group = (cst.st_mode & S_IWGRP) && cst.st_gid != 0;
		bool w_other = (cst.st_mode & S_IWOTH) != 0;
		if (w_owner || w_group || w_other) {
			char m[96], fix[300];
			describe_mode(&cst, m, sizeof m);
			snprintf(fix, sizeof fix, "sudo chown root:rtspwall %s && sudo chmod 0640 %s",
				 d->config, d->config);
			add_check(d, L_FAIL, "config", fix, "%s (%s) is writable by %s: only root may "
				  "change it", d->config, m, w_other ? "every user" :
				  w_group ? "its group" : "its non-root owner");
		}
	}
	char why[256];
	if (!service_user_can_read(d->config, why, sizeof why)) {
		char fix[300];
		snprintf(fix, sizeof fix, "sudo chown root:rtspwall %s && sudo chmod 0640 %s",
			 d->config, d->config);
		add_check(d, L_FAIL, "config", fix, "user %s cannot read %s", SERVICE_USER, why);
	}

	/* --check-config, as the service user when possible (that also proves
	 * it can read the file, ACLs included). */
	char out[16384];
	int r;
	bool as_user = geteuid() == 0 && getpwnam(SERVICE_USER) != NULL;
	if (as_user) {
		char *argv[] = { "runuser", "-u", SERVICE_USER, "--", (char *)cli_self_exe(),
				 "--check-config", (char *)d->config, NULL };
		r = cli_run(argv, out, sizeof out, 20000);
		if (r < 0)
			as_user = false;
	}
	if (!as_user) {
		char *argv[] = { (char *)cli_self_exe(), "--check-config", (char *)d->config, NULL };
		r = cli_run(argv, out, sizeof out, 20000);
	}
	snprintf(cc_out, cc_outlen, "%s", out);

	if (r == 0) {
		const char *ok = strstr(out, "OK:");
		char line[256] = "";
		if (ok)
			snprintf(line, sizeof line, "%.*s", (int)strcspn(ok, "\n"), ok);
		add_check(d, L_PASS, "config", NULL, "%s passes --check-config%s%s%s", d->config,
			  as_user ? " (as user " SERVICE_USER ")" : "", line[0] ? ": " : "", line);
	} else {
		char first[400];
		const char *p = out;
		while (*p == '\n')
			p++;
		snprintf(first, sizeof first, "%.*s", (int)strcspn(p, "\n"), p);
		char fix[300];
		snprintf(fix, sizeof fix, "sudoedit %s   then: sudo rtspwall --check-config %s",
			 d->config, d->config);
		add_check(d, L_FAIL, "config", fix, "%s", first[0] ? first : "--check-config failed");
	}
}

/* The interface of the IPv4 default route (/proc/net/route), or "". */
static void default_route_iface(char *out, size_t outlen)
{
	out[0] = '\0';
	char *t = cli_read_file("/proc/net/route", 64 * 1024);
	if (!t)
		return;
	char *save = NULL;
	for (char *line = strtok_r(t, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char iface[32];
		unsigned long dest, gw, mask;
		unsigned flags;
		int refcnt, use, metric;
		if (sscanf(line, "%31s %lx %lx %x %d %d %d %lx", iface, &dest, &gw, &flags, &refcnt,
			   &use, &metric, &mask) == 8 && dest == 0 && mask == 0 && (flags & 1)) {
			snprintf(out, outlen, "%s", iface);
			break;
		}
	}
	free(t);
}

/* UNIFI_REWRITE=plain sends the UniFi access token and the video in clear
 * text: one WARN per camera it applies to. */
static void check_unifi(struct doctor *d)
{
	if (!d->cfg_ok)
		return;
	char iface[32];
	default_route_iface(iface, sizeof iface);
	bool wifi = strncmp(iface, "wlan", 4) == 0;
	for (int i = 0; i < d->cfg.count; i++) {
		const struct layout_camera *c = &d->cfg.cam[i];
		if (c->unifi != LAYOUT_UNIFI_PLAIN)
			continue;
		if (wifi)
			add_check(d, L_WARN, "unifi",
				  "remove UNIFI_REWRITE=plain from the config (the default, tls, keeps "
				  "rtsps on port 7441), or connect the Pi by Ethernet",
				  "%s: UNIFI_REWRITE=plain plays it as plain RTSP (port 7447) - the "
				  "access token and the video are unencrypted, and the default route goes "
				  "over Wi-Fi (%s): anyone in radio range of an open or shared network "
				  "can capture them", c->name, iface);
		else
			add_check(d, L_WARN, "unifi",
				  "remove UNIFI_REWRITE=plain from the config (the default, tls, keeps "
				  "rtsps on port 7441)",
				  "%s: UNIFI_REWRITE=plain plays it as plain RTSP (port 7447) - the "
				  "access token and the video cross the network unencrypted", c->name);
	}
}

static void check_service(struct doctor *d)
{
	if (!cli_have_systemd()) {
		add_check(d, L_INFO, "service", NULL, "systemd not running (container/chroot?)");
		return;
	}
	char act[64] = "", ena[64] = "";
	char *a1[] = { "systemctl", "is-active", CLI_SERVICE, NULL };
	char *a2[] = { "systemctl", "is-enabled", CLI_SERVICE, NULL };
	cli_run(a1, act, sizeof act, 5000);
	cli_run(a2, ena, sizeof ena, 5000);
	act[strcspn(act, "\n")] = '\0';
	ena[strcspn(ena, "\n")] = '\0';
	if (strcmp(act, "failed") == 0)
		add_check(d, L_WARN, "service", "journalctl -u rtspwall -b -n 30   (the last lines "
			  "say why), then: sudo systemctl restart rtspwall",
			  "%s failed (%s)", CLI_SERVICE, ena[0] ? ena : "?");
	else
		add_check(d, L_INFO, "service", NULL, "%s: %s, %s", CLI_SERVICE,
			  act[0] ? act : "unknown", ena[0] ? ena : "not installed");
}

static void check_journal(struct doctor *d)
{
	struct stat st;
	if (stat("/var/log/journal", &st) == 0 && S_ISDIR(st.st_mode))
		add_check(d, L_INFO, "journal", NULL, "persistent (logs survive reboots)");
	else
		add_check(d, L_INFO, "journal",
			  "optional: sudo mkdir -p /var/log/journal && sudo systemctl restart "
			  "systemd-journald",
			  "volatile: the log is lost at reboot");
}

static void check_kernel(struct doctor *d)
{
	struct utsname u;
	uname(&u);
	for (int i = 0; tested_kernels[i]; i++)
		if (strcmp(u.release, tested_kernels[i]) == 0) {
			add_check(d, L_INFO, "kernel", NULL, "%s (tested)", u.release);
			return;
		}
	add_check(d, L_INFO, "kernel", NULL, "%s (not in the tested list yet; reports welcome)",
		  u.release);
}

static void check_dmesg(struct doctor *d, char *filtered, size_t flen)
{
	static char out[256 * 1024];
	char *argv[] = { "dmesg", NULL };
	filtered[0] = '\0';
	if (cli_run(argv, out, sizeof out, 5000) != 0) {
		add_check(d, L_INFO, "dmesg", NULL, "cannot read the kernel log (run with sudo)");
		return;
	}
	const char *labels[3] = { NULL, NULL, NULL };
	int nl = 0;
	char last[300] = "";
	size_t fl = 0;
	static const char *keep[] = { "vc4", "drm", "hdmi", "v4l2", "bcm2835", "codec", "mmal",
				      "vb2", "cma", "gpu", "dma", "under-voltage", "throttl" };
	for (char *save = NULL, *line = strtok_r(out, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		const char *m = dmesg_match(line);
		if (m) {
			bool seen = false;
			for (int i = 0; i < nl; i++)
				seen |= labels[i] == m;
			if (!seen && nl < 3)
				labels[nl++] = m;
			snprintf(last, sizeof last, "%s", line);
		}
		bool k = m != NULL;
		for (size_t i = 0; !k && i < sizeof keep / sizeof keep[0]; i++) {
			const char *w = keep[i];
			for (const char *p = line; *p && !k; p++)
				k = strncasecmp(p, w, strlen(w)) == 0;
		}
		if (k && fl + strlen(line) + 2 < flen) {
			memcpy(filtered + fl, line, strlen(line));
			fl += strlen(line);
			filtered[fl++] = '\n';
			filtered[fl] = '\0';
		}
	}
	if (!nl) {
		add_check(d, L_PASS, "dmesg", NULL, "no decoder/GPU memory errors in the kernel log");
		return;
	}
	char msg[200] = "";
	for (int i = 0; i < nl; i++) {
		size_t n = strlen(msg);
		snprintf(msg + n, sizeof msg - n, "%s%s", i ? "; " : "", labels[i]);
	}
	add_check(d, L_WARN, "dmesg",
		  strstr(msg, "GPU mem") ? "sudo rtspwall doctor --fix (gpu_mem), then sudo reboot"
					 : "sudo reboot; if it comes back, lower the decoder load "
					   "(sudo rtspwall probe /etc/rtspwall/cameras.conf)",
		  "%s - last: %s", msg, last);
}

static void check_wifi(struct doctor *d)
{
	char state[32];
	read_small("/sys/class/net/wlan0/operstate", state, sizeof state);
	if (strcmp(state, "up") != 0)
		return;      /* no Wi-Fi in use: nothing to say */
	char out[256];
	char *argv[] = { "iw", "dev", "wlan0", "get", "power_save", NULL };
	if (cli_run(argv, out, sizeof out, 5000) != 0) {
		add_check(d, L_INFO, "wifi", "sudo apt install iw", "wlan0 is up; power save "
			  "unknown (iw not installed)");
		return;
	}
	if (strstr(out, ": on"))
		add_check(d, L_WARN, "wifi",
			  "sudo iw dev wlan0 set power_save off   (until reboot; permanently: sudo "
			  "nmcli connection modify \"<your Wi-Fi>\" wifi.powersave 2) - or use Ethernet",
			  "Wi-Fi power save is on: causes stalls and late frames");
	else
		add_check(d, L_PASS, "wifi", NULL, "Wi-Fi power save is off");
}

/* ------------------------------------------------------------------ --fix */

/* Writes `text` to `path` via a temp file in the same directory (mkostemp:
 * O_EXCL, and O_NOFOLLOW), fsyncs file and directory, renames, and
 * sync()s: /boot/firmware is FAT, where a power cut right after the
 * change must not leave a truncated config.txt. Returns 0, or -1 with
 * errno set (nothing changed). */
static int write_file_atomic(const char *path, const char *text, mode_t mode)
{
	char tmp[160];
	if (snprintf(tmp, sizeof tmp, "%s.rtspwall-XXXXXX", path) >= (int)sizeof tmp) {
		errno = ENAMETOOLONG;
		return -1;
	}
	int fd = mkostemp(tmp, O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0)
		return -1;
	(void)fchmod(fd, mode);     /* best effort: FAT has no modes */
	size_t len = strlen(text), off = 0;
	int e = 0;
	while (off < len) {
		ssize_t n = write(fd, text + off, len - off);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			e = n < 0 ? errno : EIO;
			break;
		}
		off += (size_t)n;
	}
	if (!e && fsync(fd) < 0)
		e = errno;
	if (close(fd) < 0 && !e)
		e = errno;
	if (!e && rename(tmp, path) < 0)
		e = errno;
	if (e) {
		unlink(tmp);
		errno = e;
		return -1;
	}
	char dir[160];
	snprintf(dir, sizeof dir, "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash && slash != dir)
		*slash = '\0';
	int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (dfd >= 0) {
		fsync(dfd);
		close(dfd);
	}
	sync();
	return 0;
}

static int do_fix(struct doctor *d, bool yes)
{
	if (!d->gpu_needed || d->gpu_effective >= d->gpu_needed) {
		printf("\n--fix: nothing to change (gpu_mem is enough for the configured cameras)\n");
		return 0;
	}
	if (!d->configtxt_path[0]) {
		printf("\n--fix: no config.txt found under /boot/firmware or /boot\n");
		return 1;
	}
	if (d->gpu_info.uncertain) {
		printf("\n--fix: not changing %s: it sets gpu_mem under %s, and doctor cannot "
		       "tell whether that applies to this Pi. Set gpu_mem=%d by hand (under [all])\n",
		       d->configtxt_path, d->gpu_info.filter, d->gpu_needed);
		return 1;
	}
	if (d->gpu_configtxt >= d->gpu_needed) {
		printf("\n--fix: %s=%d is already in %s; it takes effect after: sudo reboot\n",
		       d->gpu_info.key, d->gpu_configtxt, d->configtxt_path);
		return 0;
	}
	const char *key = d->gpu_info.key[0] ? d->gpu_info.key : "gpu_mem";
	char q[256];
	snprintf(q, sizeof q, "\nSet %s=%d in %s (a backup is made first)?", key, d->gpu_needed,
		 d->configtxt_path);
	if (!yes) {
		if (!cli_interactive()) {
			printf("\n--fix: not changing %s without a terminal; add --yes to apply "
			       "%s=%d\n", d->configtxt_path, key, d->gpu_needed);
			return 1;
		}
		if (!cli_ask_yes_no(q)) {
			printf("--fix: nothing changed\n");
			return 0;
		}
	}
	if (geteuid() != 0) {
		fprintf(stderr, "rtspwall: --fix must run as root: sudo rtspwall doctor --fix\n");
		return 1;
	}
	char *text = cli_read_file(d->configtxt_path, 256 * 1024);
	if (!text) {
		fprintf(stderr, "rtspwall: cannot read %s: %s\n", d->configtxt_path, strerror(errno));
		return 1;
	}
	struct stat st;
	mode_t mode = stat(d->configtxt_path, &st) == 0 ? st.st_mode & 07777 : 0644;

	char backup[140], stamp[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
	snprintf(backup, sizeof backup, "%s.rtspwall-%s.bak", d->configtxt_path, stamp);

	size_t cap = strlen(text) + 64;
	char *next = malloc(cap);
	int r = 1;
	if (!next || configtxt_set_gpu_mem(text, d->gpu_needed, next, cap) < 0) {
		fprintf(stderr, "rtspwall: out of memory\n");
	} else if (write_file_atomic(backup, text, mode) < 0) {
		fprintf(stderr, "rtspwall: cannot write the backup %s: %s (nothing changed)\n",
			backup, strerror(errno));
	} else if (write_file_atomic(d->configtxt_path, next, mode) < 0) {
		fprintf(stderr, "rtspwall: cannot write %s: %s (backup: %s)\n", d->configtxt_path,
			strerror(errno), backup);
	} else {
		printf("set %s=%d in %s (backup: %s)\n"
		       "takes effect after a reboot - not rebooting for you: sudo reboot\n"
		       "undo:     sudo cp %s %s && sudo reboot\n",
		       key, d->gpu_needed, d->configtxt_path, backup, backup, d->configtxt_path);
		r = 0;
	}
	free(next);
	free(text);
	return r;
}

/* ---------------------------------------------------------------- --report */

static void report_cmd(struct report_buf *rb, const char *title, char *const argv[])
{
	static char out[128 * 1024];
	report_section(rb, title);
	int r = cli_run(argv, out, sizeof out, 15000);
	if (r < 0)
		report_append_masked(rb, "(not available)");
	else
		report_append_masked(rb, out[0] ? out : "(empty)");
}

static void report_v4l2(struct report_buf *rb)
{
	char text[4096] = "";
	size_t n = 0;
	for (int i = 0; i < 64; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/video%d", i);
		int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		struct v4l2_capability cap;
		memset(&cap, 0, sizeof cap);
		if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 && n < sizeof text)
			n += (size_t)snprintf(text + n, sizeof text - n, "%s: %s / %s (caps 0x%08x)\n",
					      path, (const char *)cap.driver, (const char *)cap.card,
					      cap.device_caps);
		close(fd);
	}
	report_section(rb, "v4l2 devices");
	report_append_masked(rb, text[0] ? text : "(none)");
}

static void print_report(struct doctor *d, const char *cc_out, const char *dmesg_filtered)
{
	struct report_buf rb;
	char line[1024];
	struct utsname u;

	report_init(&rb);
	report_append_masked(&rb, "rtspwall doctor --report (camera URLs, passwords and tokens "
				  "are masked)");

	report_section(&rb, "versions");
	snprintf(line, sizeof line, "rtspwall %s\nFFmpeg %s, libavformat %u.%u.%u", VERSION,
		 av_version_info(), avformat_version() >> 16, (avformat_version() >> 8) & 0xff,
		 avformat_version() & 0xff);
	report_append_masked(&rb, line);

	report_section(&rb, "system");
	uname(&u);
	char os[256] = "", *osr = cli_read_file("/etc/os-release", 64 * 1024);
	if (osr) {
		const char *p = strstr(osr, "PRETTY_NAME=");
		if (p)
			snprintf(os, sizeof os, "%.*s", (int)strcspn(p + 12, "\n"), p + 12);
		free(osr);
	}
	if (!d->model[0])
		read_small("/proc/device-tree/model", d->model, sizeof d->model);
	snprintf(line, sizeof line, "model:  %s\nos:     %s\nkernel: %s %s %s",
		 d->model[0] ? d->model : "(unknown)", os[0] ? os : "(unknown)", u.release,
		 u.version, u.machine);
	report_append_masked(&rb, line);

	report_section(&rb, "checks");
	for (int i = 0; i < d->n; i++) {
		snprintf(line, sizeof line, "%s  %-9s %s", level_name[d->checks[i].level],
			 d->checks[i].name, d->checks[i].msg);
		report_append_masked(&rb, line);
	}

	if (d->hardware) {
		report_section(&rb, "gpu_mem");
		snprintf(line, sizeof line, "effective %d MB, needed %d MB (0 = default is enough)",
			 d->gpu_effective, d->gpu_needed);
		report_append_masked(&rb, line);
		if (d->configtxt_path[0]) {
			char *t = cli_read_file(d->configtxt_path, 256 * 1024);
			if (t) {
				report_section(&rb, d->configtxt_path);
				report_append_masked(&rb, t);
				free(t);
			}
		}
		report_section(&rb, "displays (connectors, modes, planes)");
		report_append_masked(&rb, d->display_text[0] ? d->display_text : "(none)");
		report_v4l2(&rb);
		report_section(&rb, "dmesg (filtered)");
		report_append_masked(&rb, dmesg_filtered[0] ? dmesg_filtered : "(nothing relevant)");
	}

	char *journal[] = { "journalctl", "--no-pager", "-o", "short-iso", "--since",
			    "15 min ago", "-u", CLI_SERVICE, "-u", CLI_DEMO_SERVICE, NULL };
	report_cmd(&rb, "journal (rtspwall, last 15 minutes)", journal);

	report_section(&rb, "--check-config");
	report_append_masked(&rb, cc_out[0] ? cc_out : "(not run)");

	if (rb.oom)
		report_append_masked(&rb, "(report truncated: out of memory)");
	fputs(rb.data ? rb.data : "", stdout);
	report_free(&rb);
}

/* ---------------------------------------------------------------- command */

static void doctor_usage(FILE *out)
{
	fprintf(out,
		"Usage: sudo rtspwall doctor [--config PATH] [--fix] [--yes] [--report]\n"
		"                            [--summary] [--no-hardware] [--no-probe]\n"
		"\n"
		"Checks the Pi, the display, the decoder and the config; prints one\n"
		"PASS/WARN/FAIL/INFO line per check with a fix to copy.\n"
		"Exit status: 0 all pass, 1 warnings, 2 failures.\n"
		"\n"
		"  --config PATH   config file (default " CLI_DEFAULT_CONFIG ")\n"
		"  --fix           offer to set gpu_mem=256 in config.txt if the cameras need it\n"
		"                  (asks first, makes a backup, never reboots)\n"
		"  --yes           answer yes to --fix (needed without a terminal)\n"
		"  --report        print one block for bug reports; URLs and tokens are masked\n"
		"  --summary       print only problems and the totals\n"
		"  --no-hardware   skip checks that need the Pi itself (for CI)\n"
		"  --no-probe      do not contact the cameras (gpu_mem need not checked)\n");
}

int cmd_doctor(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "config",      required_argument, NULL, 'c' },
		{ "fix",         no_argument,       NULL, 'f' },
		{ "yes",         no_argument,       NULL, 'y' },
		{ "report",      no_argument,       NULL, 'r' },
		{ "summary",     no_argument,       NULL, 's' },
		{ "no-hardware", no_argument,       NULL, 'H' },
		{ "no-probe",    no_argument,       NULL, 'P' },
		{ "help",        no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	static struct doctor d;
	bool fix = false, yes = false, report = false, summary = false;
	int opt;

	memset(&d, 0, sizeof d);
	d.config = CLI_DEFAULT_CONFIG;
	d.hardware = true;
	d.probe = true;
	optind = 1;
	while ((opt = getopt_long(argc, argv, "c:fyrsHPh", opts, NULL)) != -1) {
		switch (opt) {
		case 'c': d.config = optarg; break;
		case 'f': fix = true; break;
		case 'y': yes = true; break;
		case 'r': report = true; break;
		case 's': summary = true; break;
		case 'H': d.hardware = false; break;
		case 'P': d.probe = false; break;
		case 'h': doctor_usage(stdout); return 0;
		default:  doctor_usage(stderr); return 2;
		}
	}
	if (optind != argc) {
		doctor_usage(stderr);
		return 2;
	}

	char err[768];
	d.cfg_ok = cli_load_config(d.config, &d.cfg, err, sizeof err) == 0;

	static char cc_out[16384], dmesg_filtered[32768];
	if (d.hardware)
		check_board(&d);
	check_userland(&d);
	if (d.hardware) {
		check_decoder(&d);
		check_gpu_mem(&d);
		check_display_server(&d);
		check_displays(&d);
		check_device_access(&d);
	}
	check_config_file(&d, cc_out, sizeof cc_out);
	check_unifi(&d);
	check_service(&d);
	check_journal(&d);
	check_kernel(&d);
	if (d.hardware) {
		check_dmesg(&d, dmesg_filtered, sizeof dmesg_filtered);
		check_wifi(&d);
	}

	int counts[4] = { 0 };
	enum level worst = L_PASS;
	for (int i = 0; i < d.n; i++) {
		counts[d.checks[i].level]++;
		if (d.checks[i].level == L_WARN && worst < L_WARN)
			worst = L_WARN;
		if (d.checks[i].level == L_FAIL)
			worst = L_FAIL;
	}
	int status = worst == L_FAIL ? 2 : worst == L_WARN ? 1 : 0;

	if (report) {
		print_report(&d, cc_out, dmesg_filtered);
	} else {
		for (int i = 0; i < d.n; i++) {
			const struct check *c = &d.checks[i];
			if (summary && c->level != L_WARN && c->level != L_FAIL)
				continue;
			printf("%s  %-9s %s\n", level_name[c->level], c->name, c->msg);
			if (c->fix[0] && c->level != L_PASS)
				printf("      %-9s %s\n", "fix:", c->fix);
		}
		printf("%sdoctor: %d pass, %d warn, %d fail, %d info%s\n",
		       summary && (counts[L_WARN] || counts[L_FAIL]) ? "\n" : "",
		       counts[L_PASS], counts[L_WARN], counts[L_FAIL], counts[L_INFO],
		       d.hardware ? "" : " (hardware checks skipped)");
	}

	if (fix && d.hardware)
		do_fix(&d, yes);
	else if (fix)
		printf("--fix: nothing to do with --no-hardware\n");
	return status;
}
