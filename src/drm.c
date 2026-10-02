/*
 * drm.c — DRM helpers: finds the display (connector, CRTC, mode) and the
 * overlay planes, reads the atomic properties, creates the black primary
 * plane. See rtspwall.h for the structs.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "rtspwall.h"

/* ------------------------------------------------------------- DRM helpers */

static uint32_t prop_id(int fd, uint32_t object, uint32_t type, const char *name)
{
	drmModeObjectProperties *props = drmModeObjectGetProperties(fd, object, type);
	uint32_t id = 0;

	if (!props)
		return 0;
	for (uint32_t i = 0; i < props->count_props && !id; i++) {
		drmModePropertyRes *p = drmModeGetProperty(fd, props->props[i]);
		if (!p)
			continue;
		if (!strcmp(p->name, name))
			id = p->prop_id;
		drmModeFreeProperty(p);
	}
	drmModeFreeObjectProperties(props);
	return id;
}

static bool plane_supports_nv12(drmModePlane *plane)
{
	for (uint32_t i = 0; i < plane->count_formats; i++)
		if (plane->formats[i] == DRM_FORMAT_NV12)
			return true;
	return false;
}

/* Connector name as the kernel shows it: type name + "-" + type id, e.g.
 * "HDMI-A-1". drmModeGetConnectorTypeName needs libdrm >= 2.4.113. */
static void connector_name(const drmModeConnector *c, char *buf, size_t n)
{
	const char *type = drmModeGetConnectorTypeName(c->connector_type);
	snprintf(buf, n, "%s-%u", type ? type : "Unknown", c->connector_type_id);
}

static const char *connection_str(drmModeConnection s)
{
	switch (s) {
	case DRM_MODE_CONNECTED:    return "connected";
	case DRM_MODE_DISCONNECTED: return "disconnected";
	default:                    return "unknown";
	}
}

static void list_connectors(int fd, const drmModeRes *res)
{
	log_msg("available connectors:");
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
		char name[32];
		if (!c)
			continue;
		connector_name(c, name, sizeof name);
		log_msg("  %-12s %s, %d mode(s)", name, connection_str(c->connection), c->count_modes);
		drmModeFreeConnector(c);
	}
}

#define MAX_DRM_CARDS 16

static void log_device_choice(struct wall *v)
{
	drmVersionPtr ver = drmGetVersion(v->drmfd);
	log_msg("display: using %s (driver %s)", v->drm_path,
		ver && ver->name ? ver->name : "unknown");
	if (ver)
		drmFreeVersion(ver);
}

/* Inspects one card for layout_pick_drm_device and writes a one-line
 * status for the error listing. */
static void probe_card(int fd, const char *wanted, struct drm_candidate *c,
		       char *status, size_t n)
{
	drmModeRes *res = drmModeGetResources(fd);
	if (!res) {
		snprintf(status, n, "not a KMS device (%s)", strerror(errno));
		return;
	}
	if (res->count_connectors < 1) {
		snprintf(status, n, "no connectors (render-only device)");
		drmModeFreeResources(res);
		return;
	}
	c->usable = true;

	/* "HDMI-A-1 connected, HDMI-A-2 disconnected" — so a misspelt
	 * CONNECTOR shows what the card actually offers. */
	size_t len = 0;
	status[0] = '\0';
	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *con = drmModeGetConnector(fd, res->connectors[i]);
		char name[32];
		if (!con)
			continue;
		connector_name(con, name, sizeof name);
		if (wanted && !strcmp(name, wanted))
			c->has_connector = true;
		if (con->connection == DRM_MODE_CONNECTED)
			c->has_connected = true;
		if (len < n) {
			int w = snprintf(status + len, n - len, "%s%s %s", len ? ", " : "",
					 name, connection_str(con->connection));
			len = w < 0 ? n : len + (size_t)w;
		}
		drmModeFreeConnector(con);
	}
	drmModeFreeResources(res);
}

/* Opens DRM_DEVICE, or with DRM_DEVICE unset ("auto") scans
 * /dev/dri/card0..card15 and picks one via layout_pick_drm_device: the
 * card numbering of the display controller (vc4) versus the 3D engine
 * (v3d) is not stable across kernels and boots. */
static int open_drm_device_q(struct wall *v, bool quiet)
{
	if (v->cfg.drm_device[0]) {
		_Static_assert(sizeof v->drm_path == sizeof v->cfg.drm_device, "path size");
		memcpy(v->drm_path, v->cfg.drm_device, sizeof v->drm_path);
		v->drmfd = open(v->drm_path, O_RDWR | O_CLOEXEC);
		if (v->drmfd < 0) {
			int e = errno;
			log_msg("DRM_DEVICE=%s: %s", v->drm_path, strerror(e));
			return e == ENOENT ? -2 : -1;
		}
		if (!quiet)
			log_device_choice(v);
		return 0;
	}

	const char *wanted = v->cfg.connector[0] ? v->cfg.connector : NULL;
	struct drm_candidate cand[MAX_DRM_CARDS];
	int fds[MAX_DRM_CARDS];
	char status[MAX_DRM_CARDS][256];
	bool exists[MAX_DRM_CARDS];

	for (int i = 0; i < MAX_DRM_CARDS; i++) {
		char path[32];
		snprintf(path, sizeof path, "/dev/dri/card%d", i);
		cand[i] = (struct drm_candidate){ 0 };
		status[i][0] = '\0';
		fds[i] = open(path, O_RDWR | O_CLOEXEC);
		exists[i] = fds[i] >= 0 || errno != ENOENT;
		if (fds[i] < 0) {
			snprintf(status[i], sizeof status[i], "cannot open: %s", strerror(errno));
			continue;
		}
		probe_card(fds[i], wanted, &cand[i], status[i], sizeof status[i]);
	}

	int pick = layout_pick_drm_device(cand, MAX_DRM_CARDS, wanted != NULL);
	for (int i = 0; i < MAX_DRM_CARDS; i++)
		if (fds[i] >= 0 && i != pick)
			close(fds[i]);

	if (pick < 0) {
		int n_exist = 0, n_usable = 0;
		for (int i = 0; i < MAX_DRM_CARDS; i++)
			n_usable += cand[i].usable;
		if (wanted)
			log_msg("no DRM device has connector %s (set DRM_DEVICE to override); scanned:",
				wanted);
		else
			log_msg("no usable DRM/KMS device found (set DRM_DEVICE to override); scanned:");
		for (int i = 0; i < MAX_DRM_CARDS; i++)
			if (exists[i]) {
				log_msg("  /dev/dri/card%d: %s", i, status[i]);
				n_exist++;
			}
		if (!n_exist)
			log_msg("  no /dev/dri/card* devices exist (is the vc4-kms-v3d overlay enabled?)");
		/* A KMS device exists but none has the configured connector: a
		 * typo in CONNECTOR, which a restart cannot fix. No usable
		 * device at all may be a boot race (driver not loaded yet). */
		return wanted && n_usable ? -2 : -1;
	}

	v->drmfd = fds[pick];
	snprintf(v->drm_path, sizeof v->drm_path, "/dev/dri/card%d", pick);
	if (!quiet)
		log_device_choice(v);
	return 0;
}

int open_drm_device(struct wall *v)
{
	return open_drm_device_q(v, false);
}

/* Sleeps `ms` in 100 ms steps; false if quit was set meanwhile. */
static bool sleep_unless_quit(int ms)
{
	for (int i = 0; i < ms / 100 && !quit; i++)
		usleep(100000);
	return !quit;
}

/* Names the desktop/display server processes (from /proc/PID/comm) that
 * typically hold DRM master: "labwc", "lightdm, Xorg". Empty if none is
 * found (or /proc is hidden by the sandbox). */
static void find_drm_holders(char *out, size_t n)
{
	DIR *d = opendir("/proc");
	struct dirent *e;
	size_t len = 0;
	int found = 0;

	out[0] = '\0';
	if (!d)
		return;
	while ((e = readdir(d)) && found < 4) {
		char path[300], comm[64];
		if (e->d_name[0] < '0' || e->d_name[0] > '9')
			continue;
		snprintf(path, sizeof path, "/proc/%s/comm", e->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		bool ok = fgets(comm, sizeof comm, f) != NULL;
		fclose(f);
		if (!ok || !layout_is_display_server(comm))
			continue;
		comm[strcspn(comm, "\n")] = '\0';
		if (strstr(out, comm))      /* already listed (e.g. two Xwayland) */
			continue;
		if (len < n) {
			int w = snprintf(out + len, n - len, "%s%s", len ? ", " : "", comm);
			len = w < 0 ? n : len + (size_t)w;
		}
		found++;
	}
	closedir(d);
}

int acquire_drm(struct wall *v)
{
	bool waiting = false;

	for (;;) {
		int r = open_drm_device_q(v, waiting);
		if (r < 0)
			return r;

		if (drmSetClientCap(v->drmfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)
		    || drmSetClientCap(v->drmfd, DRM_CLIENT_CAP_ATOMIC, 1)) {
			log_msg("driver lacks atomic/universal planes: %s", strerror(errno));
			return -1;
		}
		if (drmSetMaster(v->drmfd) == 0) {
			if (waiting)
				log_msg("display: got DRM master on %s - starting", v->drm_path);
			return 0;
		}

		int e = errno;
		if (!waiting) {
			char holders[128];
			find_drm_holders(holders, sizeof holders);
			log_msg("cannot become DRM master on %s: %s", v->drm_path, strerror(e));
			if (holders[0])
				log_msg("%s holds the display - a desktop session is running on this Pi, "
					"and only one program can drive the screen", holders);
			else
				log_msg("another program (a desktop, X server or compositor) holds the "
					"display - only one program can drive the screen");
			log_msg("fix: boot to the console instead of the desktop:");
			log_msg("  sudo raspi-config nonint do_boot_behaviour B1 && sudo reboot");
			log_msg("waiting - the wall starts by itself as soon as the display is free "
				"(checking every 2 s)");
			notify_status("waiting for the display: %s holds DRM master; fix: sudo raspi-config "
				      "nonint do_boot_behaviour B1 && sudo reboot",
				      holders[0] ? holders : "another program");
			waiting = true;
		}
		/* Re-open on every attempt: a non-root process can only take
		 * master on an fd opened while nobody else held it. */
		close(v->drmfd);
		v->drmfd = -1;
		if (!sleep_unless_quit(2000))
			return 1;
	}
}

/* A CRTC for the connector: the one its active encoder drives, otherwise the
 * first CRTC any of its encoders can drive. 0 if none. */
static uint32_t crtc_for_connector(int fd, const drmModeRes *res, const drmModeConnector *c)
{
	drmModeEncoder *e = c->encoder_id ? drmModeGetEncoder(fd, c->encoder_id) : NULL;
	uint32_t crtc = e ? e->crtc_id : 0;
	if (e)
		drmModeFreeEncoder(e);

	/* No active encoder? Take the first possible CRTC. */
	for (int j = 0; j < c->count_encoders && !crtc; j++) {
		drmModeEncoder *e2 = drmModeGetEncoder(fd, c->encoders[j]);
		if (!e2)
			continue;
		for (int k = 0; k < res->count_crtcs; k++)
			if (e2->possible_crtcs & (1u << k)) {
				crtc = res->crtcs[k];
				break;
			}
		drmModeFreeEncoder(e2);
	}
	return crtc;
}

/* Refresh rate of a mode in mHz, from the pixel clock where possible. */
static int mode_refresh_mhz(const drmModeModeInfo *m)
{
	long long total = (long long)m->htotal * m->vtotal;
	if (m->flags & DRM_MODE_FLAG_DBLSCAN)
		total *= 2;
	if (m->flags & DRM_MODE_FLAG_INTERLACE)
		total /= 2;
	if (total <= 0)
		return (int)m->vrefresh * 1000;
	return (int)(((long long)m->clock * 1000000 + total / 2) / total);
}

static void mode_str(const drmModeModeInfo *m, char *buf, size_t n)
{
	int mhz = mode_refresh_mhz(m);
	snprintf(buf, n, "%ux%u%s@%d.%02d", m->hdisplay, m->vdisplay,
		 (m->flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "", mhz / 1000, (mhz % 1000) / 10);
}

static void list_modes(const char *name, const drmModeConnector *c)
{
	char line[512];
	size_t len = 0;

	log_msg("available modes on %s (i = interlaced, never used):", name);
	line[0] = '\0';
	for (int i = 0; i < c->count_modes; i++) {
		char m[48];
		mode_str(&c->modes[i], m, sizeof m);
		if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED)
			strncat(m, "*", sizeof m - strlen(m) - 1);
		if (len + strlen(m) + 2 > 72) {
			log_msg("  %s", line);
			len = 0;
			line[0] = '\0';
		}
		int w = snprintf(line + len, sizeof line - len, "%s%s", len ? "  " : "", m);
		len = w < 0 ? len : len + (size_t)w;
	}
	if (len)
		log_msg("  %s", line);
	log_msg("  (* = the display's preferred mode; MODE=WxH or MODE=WxH@Hz picks one)");
}

#define MAX_MODES 128

/* Picks the mode for connector c according to MODE (see
 * layout_select_mode). If an explicit MODE is not offered: -2 (config
 * error, modes listed) unless fallback is set, then MODE=auto is used with
 * a warning (a running wall must not die because a different TV was
 * plugged in). quiet suppresses the log lines (retries). Returns 0 with
 * *out set, or -1/-2. */
static int pick_mode(struct wall *v, const drmModeConnector *c, bool fallback, bool quiet,
		     drmModeModeInfo *out)
{
	struct layout_mode lm[MAX_MODES];
	int n = c->count_modes < MAX_MODES ? c->count_modes : MAX_MODES;
	const struct layout_config *cfg = &v->cfg;
	enum layout_mode_reason why;
	char want[48] = "auto";

	for (int i = 0; i < n; i++)
		lm[i] = (struct layout_mode){
			.width = c->modes[i].hdisplay, .height = c->modes[i].vdisplay,
			.refresh_mhz = mode_refresh_mhz(&c->modes[i]),
			.interlaced = (c->modes[i].flags & DRM_MODE_FLAG_INTERLACE) != 0,
			.preferred = (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) != 0,
		};
	if (cfg->mode_w) {
		if (cfg->mode_mhz)
			snprintf(want, sizeof want, "%dx%d@%d.%02d", cfg->mode_w, cfg->mode_h,
				 cfg->mode_mhz / 1000, (cfg->mode_mhz % 1000) / 10);
		else
			snprintf(want, sizeof want, "%dx%d", cfg->mode_w, cfg->mode_h);
	}

	int idx = layout_select_mode(lm, n, cfg->mode_w, cfg->mode_h, cfg->mode_mhz, &why);
	if (idx < 0 && cfg->mode_w) {
		if (!quiet) {
			log_msg("MODE=%s: %s does not offer that mode", want, v->conn_name);
			list_modes(v->conn_name, c);
		}
		if (!fallback)
			return -2;
		if (!quiet)
			log_msg("MODE=%s not available on this display - using MODE=auto until it is",
				want);
		idx = layout_select_mode(lm, n, 0, 0, 0, &why);
	}
	if (idx < 0) {
		/* only interlaced modes (should not happen on HDMI) */
		if (!quiet)
			log_msg("display: %s offers no progressive mode - using its first mode",
				v->conn_name);
		idx = 0;
		why = LAYOUT_MODE_PREFERRED;
	}
	*out = c->modes[idx];
	if (quiet)
		return 0;

	char chosen[48], pref[48] = "";
	mode_str(&c->modes[idx], chosen, sizeof chosen);
	for (int i = 0; i < n && !pref[0]; i++)
		if (lm[i].preferred && !lm[i].interlaced)
			mode_str(&c->modes[i], pref, sizeof pref);
	switch (why) {
	case LAYOUT_MODE_PREFERRED:
		log_msg("display mode: %s (the display's preferred mode, MODE=%s)", chosen, want);
		break;
	case LAYOUT_MODE_AUTO_LOW_REFRESH:
		log_msg("display mode: %s instead of the preferred %s (refresh under 50 Hz); "
			"set MODE=WxH@Hz to override", chosen, pref);
		break;
	case LAYOUT_MODE_AUTO_4K:
		log_msg("display mode: %s instead of the preferred %s (4K adds nothing for "
			"cameras of 1080p or less and costs scaler bandwidth); set MODE=WxH@Hz "
			"to override", chosen, pref);
		break;
	case LAYOUT_MODE_EXPLICIT:
		log_msg("display mode: %s (MODE=%s)", chosen, want);
		break;
	}
	return 0;
}

/* Looks for the connector (by name if CONNECTOR is set, otherwise the
 * first connected one) and its CRTC, and picks the mode. Returns 0 when a
 * display is connected (v->connector_id/crtc_id/crtc_pipe/mode set), 1
 * when none is connected yet, -2 when CONNECTOR names no connector of this
 * device or MODE is not offered, -1 on errors. Lists the connectors unless
 * quiet. */
static int find_connector(struct wall *v, bool quiet)
{
	drmModeRes *res = drmModeGetResources(v->drmfd);
	if (!res) {
		log_msg("drmModeGetResources: %s", strerror(errno));
		return -1;
	}

	const char *wanted = v->cfg.connector[0] ? v->cfg.connector : NULL;
	bool name_matched = false;
	int result = 1;

	for (int i = 0; i < res->count_connectors && result == 1; i++) {
		drmModeConnector *c = drmModeGetConnector(v->drmfd, res->connectors[i]);
		char name[32];
		if (!c)
			continue;
		connector_name(c, name, sizeof name);

		if (wanted && strcmp(name, wanted)) {
			drmModeFreeConnector(c);
			continue;
		}
		if (wanted)
			name_matched = true;

		if (c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
			uint32_t crtc = crtc_for_connector(v->drmfd, res, c);
			if (crtc) {
				snprintf(v->conn_name, sizeof v->conn_name, "%s", name);
				result = pick_mode(v, c, false, false, &v->mode);
				if (result == 0) {
					v->connector_id = c->connector_id;
					v->crtc_id = crtc;
					for (int k = 0; k < res->count_crtcs; k++)
						if (res->crtcs[k] == crtc)
							v->crtc_pipe = (uint32_t)k;
				}
			}
		}
		drmModeFreeConnector(c);
	}

	if (result == 1 && wanted && !name_matched) {
		log_msg("CONNECTOR=%s: no such connector on %s", wanted, v->drm_path);
		list_connectors(v->drmfd, res);
		result = -2;
	} else if (result == 1 && !quiet) {
		if (wanted)
			log_msg("CONNECTOR=%s: not connected (or no modes/usable CRTC yet)", wanted);
		else
			log_msg("found no connected display on %s", v->drm_path);
		list_connectors(v->drmfd, res);
	}
	drmModeFreeResources(res);
	return result;
}

/* Hands out one NV12-capable overlay plane per camera, plus a primary
 * plane. Too few planes is a config problem (too many cameras). */
static int assign_planes(struct wall *v)
{
	drmModePlaneRes *pr = drmModeGetPlaneResources(v->drmfd);
	if (!pr) {
		log_msg("drmModeGetPlaneResources: %s", strerror(errno));
		return -1;
	}

	int next = 0;
	int available = 0;   /* NV12 overlay planes for this CRTC, used or not */
	for (uint32_t i = 0; i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(v->drmfd, pr->planes[i]);
		if (!p)
			continue;
		if (!(p->possible_crtcs & (1u << v->crtc_pipe))) {
			drmModeFreePlane(p);
			continue;
		}

		drmModeObjectProperties *props = drmModeObjectGetProperties(
			v->drmfd, p->plane_id, DRM_MODE_OBJECT_PLANE);
		uint64_t type = 0;
		if (props) {
			for (uint32_t j = 0; j < props->count_props; j++) {
				drmModePropertyRes *pr2 = drmModeGetProperty(v->drmfd, props->props[j]);
				if (pr2 && !strcmp(pr2->name, "type"))
					type = props->prop_values[j];
				if (pr2)
					drmModeFreeProperty(pr2);
			}
			drmModeFreeObjectProperties(props);
		}

		if (type == DRM_PLANE_TYPE_PRIMARY && !v->primary_plane) {
			v->primary_plane = p->plane_id;
		} else if (type == DRM_PLANE_TYPE_OVERLAY && plane_supports_nv12(p)) {
			available++;
			if (next < v->count) {
				v->cam[next].plane_id = p->plane_id;
				log_msg("%-10s -> plane %u", v->cam[next].name, p->plane_id);
				next++;
			}
		}
		drmModeFreePlane(p);
	}
	drmModeFreePlaneResources(pr);

	if (next < v->count) {
		log_msg("not enough overlay planes: the config has %d cameras but CRTC %u offers "
			"only %d NV12-capable overlay plane(s) - every camera needs its own "
			"plane, including the idle members of rotation groups",
			v->count, v->crtc_id, available);
		return -2;
	}
	if (!v->primary_plane) {
		log_msg("found no primary plane for CRTC %u", v->crtc_id);
		return -1;
	}
	return 0;
}

static void set_vblank_period(struct wall *v)
{
	v->vblank_period_us = pacing_vblank_period_us(v->mode.clock, v->mode.htotal, v->mode.vtotal);
}

int wait_for_display(struct wall *v)
{
	bool waiting = false;
	int r;

	while ((r = find_connector(v, waiting)) == 1) {
		if (!waiting) {
			log_msg("waiting for a display: switch the TV/monitor on or plug in HDMI "
				"(checking every 2 s)");
			notify_status("waiting for display");
			waiting = true;
		}
		if (!sleep_unless_quit(2000))
			return 1;
	}
	if (r < 0)
		return r;

	v->display_connected = true;
	log_msg("display: %s (connector %u), CRTC %u (index %u), %ux%u@%u",
		v->conn_name, v->connector_id, v->crtc_id, v->crtc_pipe,
		v->mode.hdisplay, v->mode.vdisplay, v->mode.vrefresh);
	notify_status("display %s found, starting cameras", v->conn_name);

	set_vblank_period(v);

	uint64_t cap_monotonic = 0;
	v->vblank_ts_monotonic = drmGetCap(v->drmfd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap_monotonic) == 0
				 && cap_monotonic != 0;
	log_msg("vblank: period %lld us, timestamps %s", (long long)v->vblank_period_us,
		v->vblank_ts_monotonic ? "CLOCK_MONOTONIC"
				       : "CLOCK_REALTIME (the compositor uses its own clock instead)");

	return assign_planes(v);
}

int read_plane_props(struct wall *v)
{
	for (int i = 0; i < v->count; i++) {
		struct camera *k = &v->cam[i];
		uint32_t p = k->plane_id;
#define P(field, name) \
		k->field = prop_id(v->drmfd, p, DRM_MODE_OBJECT_PLANE, name); \
		if (!k->field) { log_msg("plane %u lacks %s", p, name); return -1; }
		P(p_fb, "FB_ID") P(p_crtc, "CRTC_ID")
		P(p_crtc_x, "CRTC_X") P(p_crtc_y, "CRTC_Y")
		P(p_crtc_w, "CRTC_W") P(p_crtc_h, "CRTC_H")
		P(p_src_x, "SRC_X") P(p_src_y, "SRC_Y")
		P(p_src_w, "SRC_W") P(p_src_h, "SRC_H")
#undef P
	}

	uint32_t p = v->primary_plane;
#define Q(field, name) \
	v->field = prop_id(v->drmfd, p, DRM_MODE_OBJECT_PLANE, name); \
	if (!v->field) { log_msg("primary plane lacks %s", name); return -1; }
	Q(pp_fb, "FB_ID") Q(pp_crtc, "CRTC_ID")
	Q(pp_crtc_x, "CRTC_X") Q(pp_crtc_y, "CRTC_Y")
	Q(pp_crtc_w, "CRTC_W") Q(pp_crtc_h, "CRTC_H")
	Q(pp_src_x, "SRC_X") Q(pp_src_y, "SRC_Y")
	Q(pp_src_w, "SRC_W") Q(pp_src_h, "SRC_H")
#undef Q

	v->p_crtc_active = prop_id(v->drmfd, v->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
	v->p_crtc_mode   = prop_id(v->drmfd, v->crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
	v->p_conn_crtc   = prop_id(v->drmfd, v->connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");

	if (!v->p_crtc_active || !v->p_crtc_mode || !v->p_conn_crtc) {
		log_msg("CRTC/connector lacks atomic properties");
		return -1;
	}
	return 0;
}

/* Black background via a dumb buffer. */
int create_primary_fb(struct wall *v)
{
	struct drm_mode_create_dumb create = {
		.width = v->mode.hdisplay, .height = v->mode.vdisplay, .bpp = 32,
	};
	if (drmIoctl(v->drmfd, DRM_IOCTL_MODE_CREATE_DUMB, &create)) {
		log_msg("CREATE_DUMB: %s", strerror(errno));
		return -1;
	}

	struct drm_mode_map_dumb map = { .handle = create.handle };
	if (drmIoctl(v->drmfd, DRM_IOCTL_MODE_MAP_DUMB, &map)) {
		log_msg("MAP_DUMB: %s", strerror(errno));
		return -1;
	}

	void *p = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED,
		       v->drmfd, map.offset);
	if (p == MAP_FAILED) {
		log_msg("mmap dumb: %s", strerror(errno));
		return -1;
	}
	memset(p, 0, create.size);
	munmap(p, create.size);

	uint32_t handles[4] = { create.handle }, pitches[4] = { create.pitch }, offsets[4] = { 0 };
	if (drmModeAddFB2(v->drmfd, create.width, create.height, DRM_FORMAT_XRGB8888,
			  handles, pitches, offsets, &v->primary_fb, 0)) {
		log_msg("AddFB2 primary: %s", strerror(errno));
		return -1;
	}
	v->primary_handle = create.handle;
	return 0;
}

static void destroy_dumb(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb d = { .handle = handle };
	if (handle)
		drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
}

/* ------------------------------------------------------- hotplug recovery */

/* Rate limit for the recovery log lines: the first one, then at most one
 * a minute (with a counter), so a TV left in standby for a night does not
 * flood the journal. */
static bool recovery_log_ok(struct wall *v, int64_t now)
{
	if (v->last_recovery_log_us && now - v->last_recovery_log_us < 60 * 1000000LL)
		return false;
	v->last_recovery_log_us = now;
	return true;
}

/* Sets mode m again with ALLOW_MODESET (blocking commit; the caller makes
 * sure no page flip is pending). If the size changes, a new black primary
 * framebuffer and new tiles are made, and every attached camera plane is
 * moved in the same commit (a plane outside the new screen would make the
 * commit fail). On failure the old state is kept and -1 returned. */
static int display_remodeset(struct wall *v, const drmModeModeInfo *m)
{
	static struct layout_config tiles;   /* compositor thread only */
	bool resize = m->hdisplay != v->mode.hdisplay || m->vdisplay != v->mode.vdisplay;
	uint32_t blob = 0;
	uint32_t old_fb = v->primary_fb, old_handle = v->primary_handle;
	uint32_t fb = old_fb, handle = old_handle;
	drmModeModeInfo old_mode = v->mode;
	int64_t now = monotonic_us();

	if (drmModeCreatePropertyBlob(v->drmfd, m, sizeof *m, &blob)) {
		if (recovery_log_ok(v, now))
			log_msg("display: CreatePropertyBlob: %s", strerror(errno));
		return -1;
	}

	if (resize) {
		char err[256];
		tiles = v->cfg;
		if (layout_apply(&tiles, m->hdisplay, m->vdisplay, NULL, NULL, err, sizeof err) < 0) {
			log_msg("display: cannot lay out the wall on %ux%u: %s - keeping %ux%u",
				m->hdisplay, m->vdisplay, err, old_mode.hdisplay, old_mode.vdisplay);
			drmModeDestroyPropertyBlob(v->drmfd, blob);
			return -1;
		}
		v->mode = *m;
		int r = create_primary_fb(v);
		fb = v->primary_fb;
		handle = v->primary_handle;
		v->mode = old_mode;
		v->primary_fb = old_fb;
		v->primary_handle = old_handle;
		if (r < 0) {
			drmModeDestroyPropertyBlob(v->drmfd, blob);
			return -1;
		}
	}

	drmModeAtomicReq *req = drmModeAtomicAlloc();
	if (!req) {
		drmModeDestroyPropertyBlob(v->drmfd, blob);
		if (fb != old_fb) {
			drmModeRmFB(v->drmfd, fb);
			destroy_dumb(v->drmfd, handle);
		}
		return -1;
	}
	drmModeAtomicAddProperty(req, v->crtc_id, v->p_crtc_active, 1);
	drmModeAtomicAddProperty(req, v->crtc_id, v->p_crtc_mode, blob);
	drmModeAtomicAddProperty(req, v->connector_id, v->p_conn_crtc, v->crtc_id);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_fb, fb);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc, v->crtc_id);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_x, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_y, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_w, m->hdisplay);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_crtc_h, m->vdisplay);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_x, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_y, 0);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_w, (uint64_t)m->hdisplay << 16);
	drmModeAtomicAddProperty(req, v->primary_plane, v->pp_src_h, (uint64_t)m->vdisplay << 16);
	if (resize)
		for (int i = 0; i < v->count; i++) {
			struct camera *k = &v->cam[i];
			const struct layout_camera *t = &tiles.cam[i];
			pthread_mutex_lock(&k->lock);
			bool attached = k->plane_attached;
			pthread_mutex_unlock(&k->lock);
			if (!attached)
				continue;
			drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_x, t->x);
			drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_y, t->y);
			drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_w, t->width);
			drmModeAtomicAddProperty(req, k->plane_id, k->p_crtc_h, t->height);
		}

	int r = drmModeAtomicCommit(v->drmfd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
	int e = errno;
	drmModeAtomicFree(req);
	if (r) {
		if (recovery_log_ok(v, now))
			log_msg("display: setting the mode again failed: %s (will retry)", strerror(e));
		drmModeDestroyPropertyBlob(v->drmfd, blob);
		if (fb != old_fb) {
			drmModeRmFB(v->drmfd, fb);
			destroy_dumb(v->drmfd, handle);
		}
		return -1;
	}

	if (v->mode_blob)
		drmModeDestroyPropertyBlob(v->drmfd, v->mode_blob);
	v->mode_blob = blob;
	v->mode = *m;
	if (resize) {
		drmModeRmFB(v->drmfd, old_fb);
		destroy_dumb(v->drmfd, old_handle);
		v->primary_fb = fb;
		v->primary_handle = handle;
		v->cfg = tiles;
		for (int i = 0; i < v->count; i++) {
			struct camera *k = &v->cam[i];
			k->x = tiles.cam[i].x;
			k->y = tiles.cam[i].y;
			k->width = tiles.cam[i].width;
			k->height = tiles.cam[i].height;
			log_msg("%-10s tile %dx%d+%d+%d", k->name, k->width, k->height, k->x, k->y);
		}
	}
	set_vblank_period(v);
	v->last_vblank_us = monotonic_us();
	log_msg("display: mode set again on %s, %ux%u@%u", v->conn_name,
		m->hdisplay, m->vdisplay, m->vrefresh);
	return 0;
}

bool display_poll(struct wall *v)
{
	int64_t now = monotonic_us();
	bool force = v->commit_failures >= 10
		     && now - v->last_forced_probe_us >= 5 * 1000000LL;

	if (!force && now - v->last_display_poll_us < 1000000)
		return false;
	v->last_display_poll_us = now;

	if (force) {
		v->last_forced_probe_us = now;
		v->recoveries++;
		if (recovery_log_ok(v, now))
			log_msg("display: %d atomic commits failed in a row - re-probing %s and "
				"setting the mode again (recovery #%u)",
				v->commit_failures, v->conn_name, v->recoveries);
		v->commit_failures = 0;
	}

	/* The cached status is cheap (no EDID read) and follows the hotplug
	 * interrupt; a forced probe re-reads everything. */
	drmModeConnector *c = force ? drmModeGetConnector(v->drmfd, v->connector_id)
				    : drmModeGetConnectorCurrent(v->drmfd, v->connector_id);
	if (!c)
		return false;
	bool connected = c->connection == DRM_MODE_CONNECTED;

	if (!connected) {
		if (v->display_connected) {
			log_msg("display: %s disconnected (TV off or in standby, input switched or "
				"cable out?) - the wall keeps running and the mode is set again when "
				"it comes back", v->conn_name);
			v->display_connected = false;
		}
		drmModeFreeConnector(c);
		return false;
	}
	bool first = force;
	if (!v->display_connected) {
		log_msg("display: %s connected again - re-reading its modes and setting the mode",
			v->conn_name);
		v->display_connected = true;
		first = true;
	}
	if (first)
		v->remodeset_pending = true;
	if (!v->remodeset_pending) {
		drmModeFreeConnector(c);
		return false;
	}

	if (!force) {
		/* fresh probe: it may be a different display with other modes */
		drmModeFreeConnector(c);
		c = drmModeGetConnector(v->drmfd, v->connector_id);
		if (!c)
			return false;
	}
	if (c->count_modes < 1) {
		drmModeFreeConnector(c);
		return false;   /* EDID not readable yet; the next poll tries again */
	}

	/* pick_mode logs its choice only on the first attempt; retries after
	 * a failed modeset stay quiet (display_remodeset rate-limits its own
	 * failure lines). */
	drmModeModeInfo m;
	bool done = false;
	if (pick_mode(v, c, true, !first, &m) == 0 && display_remodeset(v, &m) == 0) {
		v->remodeset_pending = false;
		done = true;
	}
	drmModeFreeConnector(c);
	return done;
}
