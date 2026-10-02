/*
 * config.c — reads the config file (see layout.h for the format), applies
 * the layout once the display mode is known, forms the rotation groups, and
 * implements --check-config. The parsing and geometry themselves live in
 * the pure layout.c; this file is the glue to struct wall.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtspwall.h"

#define CONFIG_MAX_BYTES (1024 * 1024)

/* Reads the whole file into a NUL-terminated heap buffer. */
static char *read_file(const char *path, char *err, size_t errlen)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		snprintf(err, errlen, "cannot read %s: %s", path, strerror(errno));
		return NULL;
	}

	char *buf = malloc(CONFIG_MAX_BYTES + 1);
	if (!buf) {
		snprintf(err, errlen, "out of memory");
		fclose(f);
		return NULL;
	}
	size_t n = fread(buf, 1, CONFIG_MAX_BYTES + 1, f);
	int read_error = ferror(f);
	fclose(f);

	if (read_error) {
		snprintf(err, errlen, "error reading %s", path);
		free(buf);
		return NULL;
	}
	if (n > CONFIG_MAX_BYTES) {
		snprintf(err, errlen, "%s is larger than %d bytes", path, CONFIG_MAX_BYTES);
		free(buf);
		return NULL;
	}
	buf[n] = '\0';
	return buf;
}

static void warn_to_log(void *ctx, const char *msg)
{
	log_msg("config %s: warning: %s", (const char *)ctx, msg);
}

static void warn_to_stderr(void *ctx, const char *msg)
{
	fprintf(stderr, "rtspwall: %s: warning: %s\n", (const char *)ctx, msg);
}

static int parse_file(struct layout_config *cfg, const char *path,
		      layout_warn_fn warn, char *err, size_t errlen)
{
	char msg[512];
	char *text = read_file(path, msg, sizeof msg);
	if (!text) {
		snprintf(err, errlen, "%s", msg);
		return -1;
	}
	int r = layout_parse(cfg, text, warn, (void *)path, msg, sizeof msg);
	free(text);
	if (r < 0)
		snprintf(err, errlen, "%s: %s", path, msg);
	return r;
}

/* ------------------------------------------------------------------ runtime */

int load_config(struct wall *v, const char *path)
{
	char err[768];

	if (parse_file(&v->cfg, path, warn_to_log, err, sizeof err) < 0) {
		log_msg("config: %s", err);
		return -1;
	}

	v->buffer_ms = v->cfg.buffer_ms;
	v->rotate_seconds = v->cfg.rotate_seconds;
	v->count = 0;

	for (int i = 0; i < v->cfg.count; i++) {
		const struct layout_camera *c = &v->cfg.cam[i];
		struct camera *k = &v->cam[v->count];

		memset(k, 0, sizeof *k);
		_Static_assert(sizeof k->name == sizeof c->name, "name size");
		_Static_assert(sizeof k->url == sizeof c->url, "url size");
		memcpy(k->name, c->name, sizeof k->name);   /* same size, NUL-terminated */
		memcpy(k->url, c->url, sizeof k->url);
		k->delay_ms = c->delay_ms;

		k->v4l2fd = -1;
		k->shown = k->in_flight = -1;
		k->r_prev_frame_us = -1;
		for (int j = 0; j < CAPTURE_BUFFERS; j++)
			k->cap[j].dmafd = -1;
		pthread_mutex_init(&k->lock, NULL);
		pthread_cond_init(&k->detached, NULL);
		pacing_fifo_init(&k->fifo);
		pacing_pll_init(&k->pll);
		pacing_hist_init(&k->m_jitter);
		pacing_hist_init(&k->m_regulated_ptsdelta);

		char masked[LAYOUT_URL_MAX];
		layout_mask_url(k->url, masked, sizeof masked);
		log_msg("camera %d: %-10s %s delay=%dms", v->count, k->name, masked, k->delay_ms);
		v->count++;
	}

	if (v->cfg.grid_cols > 0)
		log_msg("config: grid %dx%d, buffer=%dms rotate=%ds decoder=%s connector=%s drm=%s ffmpeg_log=%s",
			v->cfg.grid_cols, v->cfg.grid_rows, v->buffer_ms, v->rotate_seconds,
			v->cfg.decoder, v->cfg.connector[0] ? v->cfg.connector : "(first connected)",
			v->cfg.drm_device[0] ? v->cfg.drm_device : "(auto)",
			layout_ffmpeg_log_name(v->cfg.ffmpeg_loglevel));
	else
		log_msg("config: manual layout, buffer=%dms rotate=%ds decoder=%s connector=%s drm=%s ffmpeg_log=%s",
			v->buffer_ms, v->rotate_seconds,
			v->cfg.decoder, v->cfg.connector[0] ? v->cfg.connector : "(first connected)",
			v->cfg.drm_device[0] ? v->cfg.drm_device : "(auto)",
			layout_ffmpeg_log_name(v->cfg.ffmpeg_loglevel));
	return 0;
}

/* Computes the tiles for the actual display mode (grid mode) and copies
 * them into the cameras. Called from main() after find_display, before
 * build_rotation and before the camera threads start. */
int apply_layout(struct wall *v)
{
	char err[512];

	if (layout_apply(&v->cfg, v->mode.hdisplay, v->mode.vdisplay,
			 warn_to_log, (void *)"layout", err, sizeof err) < 0) {
		log_msg("layout: %s", err);
		return -1;
	}
	for (int i = 0; i < v->count; i++) {
		struct camera *k = &v->cam[i];
		const struct layout_camera *c = &v->cfg.cam[i];
		k->width  = c->width;
		k->height = c->height;
		k->x      = c->x;
		k->y      = c->y;
		log_msg("%-10s tile %dx%d+%d+%d", k->name, k->width, k->height, k->x, k->y);
	}
	return 0;
}

/* Forms the rotation groups from the cameras' tiles. Called ONCE from
 * main(), after apply_layout and BEFORE the camera threads start — the
 * groups and rotation states never change afterwards (only WHICH member is
 * active rotates, see pacing_rotation_update in compositor()). */
void build_rotation(struct wall *v)
{
	struct pacing_tile tiles[MAX_CAMERAS];

	for (int i = 0; i < v->count; i++) {
		tiles[i].width  = v->cam[i].width;
		tiles[i].height = v->cam[i].height;
		tiles[i].x      = v->cam[i].x;
		tiles[i].y      = v->cam[i].y;
	}

	v->n_groups = pacing_build_groups(tiles, v->count, v->groups, MAX_CAMERAS);

	int64_t now = monotonic_us();
	for (int g = 0; g < v->n_groups; g++) {
		pacing_rotation_init(&v->rotation[g], now);
		if (v->groups[g].count > 1) {
			log_msg("rotation: group %d (%s + %d more) rotates every %ds",
				g, v->cam[v->groups[g].index[0]].name,
				v->groups[g].count - 1, v->rotate_seconds);
		}
	}
}

/* -------------------------------------------------------------- --check-config */

/* Validates the config and prints the interpreted layout for a screen of
 * screen_w x screen_h, without touching DRM, V4L2 or the network. Returns
 * the process exit status (0 = valid). */
int check_config(const char *path, int screen_w, int screen_h)
{
	static struct layout_config cfg;
	char err[768];

	if (parse_file(&cfg, path, warn_to_stderr, err, sizeof err) < 0) {
		fprintf(stderr, "rtspwall: %s\n", err);
		return 1;
	}
	if (layout_apply(&cfg, screen_w, screen_h, warn_to_stderr, (void *)path,
			 err, sizeof err) < 0) {
		fprintf(stderr, "rtspwall: %s: %s\n", path, err);
		return 1;
	}

	printf("config:          %s\n", path);
	printf("screen:          %dx%d\n", screen_w, screen_h);
	if (cfg.grid_cols > 0)
		printf("layout:          grid %dx%d (%d cells)\n",
		       cfg.grid_cols, cfg.grid_rows, cfg.grid_cols * cfg.grid_rows);
	else
		printf("layout:          manual\n");
	printf("BUFFER_MS:       %d\n", cfg.buffer_ms);
	printf("ROTATE_SECONDS:  %d\n", cfg.rotate_seconds);
	printf("DECODER:         %s\n", cfg.decoder);
	printf("CONNECTOR:       %s\n", cfg.connector[0] ? cfg.connector : "(first connected)");
	printf("DRM_DEVICE:      %s\n", cfg.drm_device[0] ? cfg.drm_device : "(auto)");
	printf("FFMPEG_LOGLEVEL: %s\n", layout_ffmpeg_log_name(cfg.ffmpeg_loglevel));

	printf("\ncameras:\n");
	for (int i = 0; i < cfg.count; i++) {
		const struct layout_camera *c = &cfg.cam[i];
		char masked[LAYOUT_URL_MAX];
		char where[16] = "";
		char geometry[64];

		layout_mask_url(c->url, masked, sizeof masked);
		if (cfg.grid_cols > 0)
			snprintf(where, sizeof where, "cell %-2d  ", c->cell);
		snprintf(geometry, sizeof geometry, "%dx%d+%d+%d", c->width, c->height, c->x, c->y);
		printf("  %-12s %s%-16s delay %3d ms  %s\n", c->name, where, geometry,
		       c->delay_ms, masked);
	}

	struct pacing_tile tiles[LAYOUT_MAX_CAMERAS];
	struct pacing_group groups[LAYOUT_MAX_CAMERAS];
	for (int i = 0; i < cfg.count; i++)
		tiles[i] = (struct pacing_tile){ cfg.cam[i].width, cfg.cam[i].height,
						 cfg.cam[i].x, cfg.cam[i].y };
	int n_groups = pacing_build_groups(tiles, cfg.count, groups, LAYOUT_MAX_CAMERAS);

	int rotating = 0;
	printf("\ntiles:\n");
	for (int g = 0; g < n_groups; g++) {
		const struct layout_camera *first = &cfg.cam[groups[g].index[0]];
		char geometry[64];
		snprintf(geometry, sizeof geometry, "%dx%d+%d+%d",
			 first->width, first->height, first->x, first->y);
		if (cfg.grid_cols > 0)
			printf("  cell %-2d  ", first->cell);
		else
			printf("  ");
		printf("%-16s ", geometry);
		for (int m = 0; m < groups[g].count; m++)
			printf("%s%s", m ? ", " : "", cfg.cam[groups[g].index[m]].name);
		if (groups[g].count > 1) {
			printf("  (rotation group, every %d s)", cfg.rotate_seconds);
			rotating++;
		} else {
			printf("  (fixed)");
		}
		printf("\n");
	}

	printf("\nOK: %d camera%s, %d tile%s, %d rotation group%s; needs %d overlay plane%s "
	       "and %d concurrent decoder instance%s\n",
	       cfg.count, cfg.count == 1 ? "" : "s",
	       n_groups, n_groups == 1 ? "" : "s",
	       rotating, rotating == 1 ? "" : "s",
	       cfg.count, cfg.count == 1 ? "" : "s",
	       cfg.count, cfg.count == 1 ? "" : "s");
	return 0;
}
