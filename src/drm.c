/*
 * drm.c — DRM helpers: finds the display (connector, CRTC, mode) and the
 * overlay planes, reads the atomic properties, creates the black primary
 * plane. See rpi4rtsp.h for the structs.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include "rpi4rtsp.h"

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
int open_drm_device(struct wall *v)
{
	if (v->cfg.drm_device[0]) {
		_Static_assert(sizeof v->drm_path == sizeof v->cfg.drm_device, "path size");
		memcpy(v->drm_path, v->cfg.drm_device, sizeof v->drm_path);
		v->drmfd = open(v->drm_path, O_RDWR | O_CLOEXEC);
		if (v->drmfd < 0) {
			log_msg("DRM_DEVICE=%s: %s", v->drm_path, strerror(errno));
			return -1;
		}
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
		int n_exist = 0;
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
		return -1;
	}

	v->drmfd = fds[pick];
	snprintf(v->drm_path, sizeof v->drm_path, "/dev/dri/card%d", pick);
	log_device_choice(v);
	return 0;
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

/* Finds the connector (by name if CONNECTOR is set, otherwise the first
 * connected one), its CRTC and the preferred mode, then hands out one
 * NV12-capable overlay plane per camera plus a primary plane. */
int find_display(struct wall *v)
{
	drmModeRes *res = drmModeGetResources(v->drmfd);
	if (!res) {
		log_msg("drmModeGetResources: %s", strerror(errno));
		return -1;
	}

	const char *wanted = v->cfg.connector[0] ? v->cfg.connector : NULL;
	int crtc_index = -1;
	char found_name[32] = "";
	bool name_matched = false;

	for (int i = 0; i < res->count_connectors; i++) {
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
				v->connector_id = c->connector_id;
				v->crtc_id = crtc;
				v->mode = c->modes[0];   /* modes[0] is the preferred mode */
				for (int k = 0; k < res->count_crtcs; k++)
					if (res->crtcs[k] == crtc)
						crtc_index = k;
				v->crtc_pipe = (uint32_t)crtc_index;
				snprintf(found_name, sizeof found_name, "%s", name);
				drmModeFreeConnector(c);
				break;
			}
		}
		drmModeFreeConnector(c);
	}

	if (crtc_index < 0) {
		if (wanted && !name_matched)
			log_msg("CONNECTOR=%s: no such connector", wanted);
		else if (wanted)
			log_msg("CONNECTOR=%s: not connected, no modes or no usable CRTC", wanted);
		else
			log_msg("found no connected display");
		list_connectors(v->drmfd, res);
		drmModeFreeResources(res);
		return -1;
	}

	log_msg("display: %s (connector %u), CRTC %u (index %d), %ux%u@%u",
		found_name, v->connector_id, v->crtc_id, crtc_index,
		v->mode.hdisplay, v->mode.vdisplay, v->mode.vrefresh);

	v->vblank_period_us = pacing_vblank_period_us(v->mode.clock, v->mode.htotal, v->mode.vtotal);

	uint64_t cap_monotonic = 0;
	v->vblank_ts_monotonic = drmGetCap(v->drmfd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap_monotonic) == 0
				 && cap_monotonic != 0;
	log_msg("vblank: period %lld us, timestamps %s", (long long)v->vblank_period_us,
		v->vblank_ts_monotonic ? "CLOCK_MONOTONIC"
				       : "CLOCK_REALTIME (the compositor uses its own clock instead)");

	/* Hand out one overlay plane per camera, plus a primary plane. */
	drmModePlaneRes *pr = drmModeGetPlaneResources(v->drmfd);
	if (!pr) {
		log_msg("drmModeGetPlaneResources: %s", strerror(errno));
		drmModeFreeResources(res);
		return -1;
	}

	int next = 0;
	int available = 0;   /* NV12 overlay planes for this CRTC, used or not */
	for (uint32_t i = 0; i < pr->count_planes; i++) {
		drmModePlane *p = drmModeGetPlane(v->drmfd, pr->planes[i]);
		if (!p)
			continue;
		if (!(p->possible_crtcs & (1u << crtc_index))) {
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
	drmModeFreeResources(res);

	if (next < v->count) {
		log_msg("not enough overlay planes: the config has %d cameras but CRTC %u offers "
			"only %d NV12-capable overlay plane(s) - every camera needs its own "
			"plane, including the idle members of rotation groups",
			v->count, v->crtc_id, available);
		return -1;
	}
	if (!v->primary_plane) {
		log_msg("found no primary plane for CRTC %u", v->crtc_id);
		return -1;
	}
	return 0;
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
	return 0;
}
