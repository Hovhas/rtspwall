/*
 * layout.c — pure config parsing and tile geometry. See layout.h for the
 * format. Only the standard C library is used.
 */
#include "layout.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ----------------------------------------------------------------- helpers */

static char *trim(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
	return s;
}

/* Copies src into dst[cap] with an explicit length check (no snprintf, so
 * no -Wformat-truncation at any optimisation level). Returns false — and
 * stores a truncated, NUL-terminated copy — if src does not fit. */
static bool copy_str(char *dst, size_t cap, const char *src)
{
	size_t n = strlen(src);
	bool fits = n < cap;

	if (!cap)
		return false;
	if (!fits)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
	return fits;
}

/* Strict integer: optional sign, digits, nothing else (surrounding
 * whitespace is already trimmed by the caller). */
static bool parse_int(const char *s, int *out)
{
	char *end;
	long v;

	if (!*s)
		return false;
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || *end || v < INT_MIN || v > INT_MAX)
		return false;
	*out = (int)v;
	return true;
}

static void set_err(char *err, size_t errlen, int line, const char *fmt, ...)
{
	char msg[512];
	va_list ap;

	if (!err || !errlen)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (line > 0)
		snprintf(err, errlen, "line %d: %s", line, msg);
	else
		snprintf(err, errlen, "%s", msg);
}

static void warnf(layout_warn_fn warn, void *ctx, int line, const char *fmt, ...)
{
	char msg[512], full[600];
	va_list ap;

	if (!warn)
		return;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	if (line > 0)
		snprintf(full, sizeof full, "line %d: %s", line, msg);
	else
		snprintf(full, sizeof full, "%s", msg);
	warn(ctx, full);
}

int layout_parse_size(const char *s, int *w, int *h)
{
	char buf[64];
	char *x;

	if (!copy_str(buf, sizeof buf, s))
		return -1;
	x = strchr(buf, 'x');
	if (!x)
		x = strchr(buf, 'X');
	if (!x)
		return -1;
	*x = '\0';
	if (!parse_int(trim(buf), w) || !parse_int(trim(x + 1), h))
		return -1;
	if (*w <= 0 || *h <= 0)
		return -1;
	return 0;
}

/* ------------------------------------------------------------ single line */

enum config_line_type config_parse_line(const char *line_in, struct config_global *global,
					struct config_fields *fields)
{
	char line[LAYOUT_MAX_LINE];
	/* The caller splits at LAYOUT_MAX_LINE and rejects longer lines, so
	 * this cannot truncate in practice. */
	copy_str(line, sizeof line, line_in);

	char *p = trim(line);
	if (*p == '#' || *p == '\0')
		return CONFIG_LINE_EMPTY;

	if (strchr(p, '|')) {
		if (!fields)
			return CONFIG_LINE_CAMERA;
		fields->count = 0;
		fields->overlong = 0;
		for (char *start = p;;) {
			char *bar = strchr(start, '|');
			if (bar)
				*bar = '\0';
			if (fields->count < CONFIG_MAX_FIELDS
			    && !copy_str(fields->field[fields->count], sizeof fields->field[0],
					 trim(start))
			    && !fields->overlong)
				fields->overlong = fields->count + 1;
			fields->count++;
			if (!bar)
				break;
			start = bar + 1;
		}
		return CONFIG_LINE_CAMERA;
	}

	char *eq = strchr(p, '=');
	if (!eq)
		return CONFIG_LINE_INVALID;

	*eq = '\0';
	char *key = trim(p);
	char *value = trim(eq + 1);
	if (!*key)
		return CONFIG_LINE_INVALID;

	if (global) {
		global->key_overlong = !copy_str(global->key, sizeof global->key, key);
		global->value_overlong = !copy_str(global->value, sizeof global->value, value);
	}
	return CONFIG_LINE_GLOBAL;
}

/* ------------------------------------------------- placeholders and keys */

/* Finds the first placeholder in s: "CHANGE_ME" (any case) or "<x...>".
 * Returns its start and stores its length, or NULL. */
static const char *find_placeholder(const char *s, size_t *len)
{
	static const char change_me[] = "CHANGE_ME";
	const size_t n_cm = sizeof change_me - 1;

	for (const char *p = s; *p; p++) {
		if (strncasecmp(p, change_me, n_cm) == 0) {
			*len = n_cm;
			return p;
		}
		if (*p == '<') {
			const char *close = strchr(p + 1, '>');
			const char *next_open = strchr(p + 1, '<');
			if (close && close > p + 1 && (!next_open || next_open > close)) {
				*len = (size_t)(close - p) + 1;
				return p;
			}
		}
	}
	return NULL;
}

bool layout_has_placeholder(const char *s)
{
	size_t len;
	return find_placeholder(s, &len) != NULL;
}

/* Copies the placeholder found in s (at most 40 characters) into out. */
static bool placeholder_text(const char *s, char *out, size_t cap)
{
	size_t len;
	const char *p = find_placeholder(s, &len);
	if (!p)
		return false;
	if (len > 40)
		len = 40;
	if (len >= cap)
		len = cap - 1;
	memcpy(out, p, len);
	out[len] = '\0';
	return true;
}

static const char *const known_keys[] = {
	"BUFFER_MS", "ROTATE_SECONDS", "GRID", "DECODER", "DRM_DEVICE",
	"CONNECTOR", "FFMPEG_LOGLEVEL", "MODE", "UNIFI_REWRITE",
};

#define LEV_MAX 64

int layout_levenshtein(const char *a, const char *b)
{
	size_t na = strlen(a), nb = strlen(b);
	int prev[LEV_MAX], cur[LEV_MAX];

	if (na > LEV_MAX - 1)
		na = LEV_MAX - 1;
	if (nb > LEV_MAX - 1)
		nb = LEV_MAX - 1;
	for (size_t j = 0; j <= nb; j++)
		prev[j] = (int)j;
	for (size_t i = 1; i <= na; i++) {
		cur[0] = (int)i;
		for (size_t j = 1; j <= nb; j++) {
			int cost = toupper((unsigned char)a[i - 1])
				   != toupper((unsigned char)b[j - 1]);
			int best = prev[j] + 1;
			if (cur[j - 1] + 1 < best)
				best = cur[j - 1] + 1;
			if (prev[j - 1] + cost < best)
				best = prev[j - 1] + cost;
			cur[j] = best;
		}
		memcpy(prev, cur, (nb + 1) * sizeof prev[0]);
	}
	return prev[nb];
}

const char *layout_suggest_key(const char *key)
{
	size_t n = strlen(key);
	int limit = n <= 5 ? 1 : 2;
	const char *best = NULL;
	int best_d = limit + 1;

	if (!n)
		return NULL;
	for (size_t i = 0; i < sizeof known_keys / sizeof known_keys[0]; i++) {
		int d = layout_levenshtein(key, known_keys[i]);
		if (d < best_d) {
			best_d = d;
			best = known_keys[i];
		}
	}
	return best;
}

/* ---------------------------------------------------------------- whole file */

static int handle_global(struct layout_config *cfg, const struct config_global *g, int line,
			 unsigned flags, layout_warn_fn warn, void *ctx, char *err, size_t errlen)
{
	int v;
	char ph[48];

	if (g->key_overlong) {
		set_err(err, errlen, line, "key longer than %d characters",
			(int)sizeof g->key - 1);
		return -1;
	}
	if (g->value_overlong) {
		set_err(err, errlen, line, "value of %s longer than %d characters",
			g->key, (int)sizeof g->value - 1);
		return -1;
	}
	if (placeholder_text(g->value, ph, sizeof ph)) {
		set_err(err, errlen, line,
			"placeholder \"%s\" in the value of %s - replace it with a real value",
			ph, g->key);
		return -1;
	}

	if (!strcmp(g->key, "BUFFER_MS")) {
		if (!parse_int(g->value, &v) || v < 0 || v > 10000) {
			set_err(err, errlen, line, "BUFFER_MS must be an integer 0-10000, got \"%s\"",
				g->value);
			return -1;
		}
		cfg->buffer_ms = v;
	} else if (!strcmp(g->key, "ROTATE_SECONDS")) {
		if (!parse_int(g->value, &v) || v < 1 || v > 86400) {
			set_err(err, errlen, line,
				"ROTATE_SECONDS must be an integer 1-86400, got \"%s\"", g->value);
			return -1;
		}
		cfg->rotate_seconds = v;
	} else if (!strcmp(g->key, "GRID")) {
		int c, r;
		if (layout_parse_size(g->value, &c, &r) < 0
		    || c > LAYOUT_MAX_GRID || r > LAYOUT_MAX_GRID) {
			set_err(err, errlen, line,
				"GRID must be COLSxROWS with 1-%d columns and rows (e.g. 2x2), got \"%s\"",
				LAYOUT_MAX_GRID, g->value);
			return -1;
		}
		cfg->grid_cols = c;
		cfg->grid_rows = r;
	} else if (!strcmp(g->key, "DECODER")) {
		if (!*g->value) {
			set_err(err, errlen, line, "DECODER must be a device path");
			return -1;
		}
		if (!copy_str(cfg->decoder, sizeof cfg->decoder, g->value)) {
			set_err(err, errlen, line, "DECODER longer than %d characters",
				(int)sizeof cfg->decoder - 1);
			return -1;
		}
	} else if (!strcmp(g->key, "DRM_DEVICE")) {
		if (!*g->value) {
			set_err(err, errlen, line, "DRM_DEVICE must be a device path or \"auto\"");
			return -1;
		}
		if (!strcmp(g->value, "auto")) {
			cfg->drm_device[0] = '\0';
		} else if (!copy_str(cfg->drm_device, sizeof cfg->drm_device, g->value)) {
			set_err(err, errlen, line, "DRM_DEVICE longer than %d characters",
				(int)sizeof cfg->drm_device - 1);
			return -1;
		}
	} else if (!strcmp(g->key, "CONNECTOR")) {
		if (!copy_str(cfg->connector, sizeof cfg->connector, g->value)) {
			set_err(err, errlen, line, "CONNECTOR longer than %d characters",
				(int)sizeof cfg->connector - 1);
			return -1;
		}
	} else if (!strcmp(g->key, "FFMPEG_LOGLEVEL")) {
		enum layout_ffmpeg_log l;
		for (l = LAYOUT_FFMPEG_LOG_QUIET; l <= LAYOUT_FFMPEG_LOG_INFO; l++)
			if (!strcmp(g->value, layout_ffmpeg_log_name(l)))
				break;
		if (l > LAYOUT_FFMPEG_LOG_INFO) {
			set_err(err, errlen, line,
				"FFMPEG_LOGLEVEL must be quiet, error, warning or info, got \"%s\"",
				g->value);
			return -1;
		}
		cfg->ffmpeg_loglevel = l;
	} else if (!strcmp(g->key, "MODE")) {
		if (layout_parse_mode(g->value, &cfg->mode_w, &cfg->mode_h, &cfg->mode_mhz) < 0) {
			set_err(err, errlen, line,
				"MODE must be auto, WxH or WxH@Hz (e.g. 1920x1080@60), got \"%s\"",
				g->value);
			return -1;
		}
	} else if (!strcmp(g->key, "UNIFI_REWRITE")) {
		if (!strcmp(g->value, "auto")) {
			cfg->unifi_rewrite = true;
		} else if (!strcmp(g->value, "off")) {
			cfg->unifi_rewrite = false;
		} else {
			set_err(err, errlen, line, "UNIFI_REWRITE must be auto or off, got \"%s\"",
				g->value);
			return -1;
		}
	} else {
		const char *near = layout_suggest_key(g->key);
		char hint[64] = "";
		if (near)
			snprintf(hint, sizeof hint, " (did you mean %s?)", near);
		if (flags & LAYOUT_PARSE_STRICT) {
			set_err(err, errlen, line, "unknown key %s%s", g->key, hint);
			return -1;
		}
		warnf(warn, ctx, line, "unknown key %s, ignored%s", g->key, hint);
	}
	return 0;
}

/* A '|' inside a password splits the URL in two: the URL field ends inside
 * the authority (no '@'), and a later field carries the "...@host" rest. */
static bool pipe_in_password(const struct config_fields *f)
{
	int n = f->count < CONFIG_MAX_FIELDS ? f->count : CONFIG_MAX_FIELDS;

	if (n < 3 || !strstr(f->field[1], "://") || strchr(f->field[1], '@'))
		return false;
	for (int i = 2; i < n; i++)
		if (strchr(f->field[i], '@'))
			return true;
	return false;
}

#define PIPE_HINT " - a '|' in the URL splits the line: encode | in passwords as %7C"

static int handle_camera(struct layout_config *cfg, const struct config_fields *f, int line,
			 char *err, size_t errlen)
{
	const char *pipe_hint = pipe_in_password(f) ? PIPE_HINT : "";
	char ph[48];

	if (f->count > CONFIG_MAX_FIELDS) {
		set_err(err, errlen, line,
			"too many fields (%d); expected name|url|cell[|delay_ms] or "
			"name|url|width|height|x|y[|delay_ms]%s", f->count, pipe_hint);
		return -1;
	}
	if (f->count < 3) {
		set_err(err, errlen, line,
			"too few fields (%d); expected name|url|cell[|delay_ms] or "
			"name|url|width|height|x|y[|delay_ms]", f->count);
		return -1;
	}
	if (f->overlong) {
		set_err(err, errlen, line, "field %d longer than %d characters",
			f->overlong, (int)sizeof f->field[0] - 1);
		return -1;
	}
	if (cfg->count >= LAYOUT_MAX_CAMERAS) {
		set_err(err, errlen, line, "too many cameras (max %d)", LAYOUT_MAX_CAMERAS);
		return -1;
	}
	if (!*f->field[0]) {
		set_err(err, errlen, line, "empty camera name");
		return -1;
	}
	if (!*f->field[1]) {
		set_err(err, errlen, line, "empty URL for camera %s", f->field[0]);
		return -1;
	}
	if (placeholder_text(f->field[0], ph, sizeof ph)) {
		set_err(err, errlen, line,
			"placeholder \"%s\" as the camera name - replace it with a short name",
			ph);
		return -1;
	}
	if (placeholder_text(f->field[1], ph, sizeof ph)) {
		set_err(err, errlen, line,
			"placeholder \"%s\" in the URL of camera %s - replace it with the real "
			"value (user, password, address or stream path)", ph, f->field[0]);
		return -1;
	}
	for (int i = 2; i < f->count; i++)
		if (placeholder_text(f->field[i], ph, sizeof ph)) {
			set_err(err, errlen, line,
				"placeholder \"%s\" in field %d of camera %s - replace it with a number",
				ph, i + 1, f->field[0]);
			return -1;
		}

	struct layout_camera *c = &cfg->cam[cfg->count];
	memset(c, 0, sizeof *c);
	if (!copy_str(c->name, sizeof c->name, f->field[0])) {
		set_err(err, errlen, line, "camera name longer than %d characters",
			(int)sizeof c->name - 1);
		return -1;
	}
	if (!copy_str(c->url, sizeof c->url, f->field[1])) {
		set_err(err, errlen, line, "URL longer than %d characters",
			(int)sizeof c->url - 1);
		return -1;
	}
	c->line = line;
	c->n_numbers = f->count - 2;

	int num[CONFIG_MAX_FIELDS - 2] = { 0 };
	for (int i = 0; i < c->n_numbers; i++) {
		if (!parse_int(f->field[2 + i], &num[i])) {
			if (*pipe_hint)
				set_err(err, errlen, line, "field %d is not an integer%s",
					3 + i, pipe_hint);
			else
				set_err(err, errlen, line, "field %d (\"%s\") is not an integer",
					3 + i, f->field[2 + i]);
			return -1;
		}
	}

	/* Interpretation depends on the mode, which is only known once the
	 * whole file has been read (GRID may appear anywhere). Store the raw
	 * numbers in both interpretations' slots; layout_parse sorts it out. */
	if (c->n_numbers <= 2) {
		c->cell = num[0];
		c->delay_ms = c->n_numbers == 2 ? num[1] : 0;
	} else if (c->n_numbers >= 4) {
		c->width  = num[0];
		c->height = num[1];
		c->x      = num[2];
		c->y      = num[3];
		c->delay_ms = c->n_numbers == 5 ? num[4] : 0;
	}
	if (c->delay_ms < -LAYOUT_MAX_DELAY_MS || c->delay_ms > LAYOUT_MAX_DELAY_MS) {
		set_err(err, errlen, line, "delay_ms must be %d..%d, got %d",
			-LAYOUT_MAX_DELAY_MS, LAYOUT_MAX_DELAY_MS, c->delay_ms);
		return -1;
	}
	cfg->count++;
	return 0;
}

static bool is_grid_line(const struct layout_camera *c)
{
	return c->n_numbers == 1 || c->n_numbers == 2;
}

static bool is_manual_line(const struct layout_camera *c)
{
	return c->n_numbers == 4 || c->n_numbers == 5;
}

int layout_parse(struct layout_config *cfg, const char *text,
		 layout_warn_fn warn, void *warn_ctx, char *err, size_t errlen)
{
	return layout_parse_flags(cfg, text, 0, warn, warn_ctx, err, errlen);
}

int layout_parse_flags(struct layout_config *cfg, const char *text, unsigned flags,
		       layout_warn_fn warn, void *warn_ctx, char *err, size_t errlen)
{
	memset(cfg, 0, sizeof *cfg);
	cfg->unifi_rewrite = true;
	cfg->buffer_ms = LAYOUT_DEFAULT_BUFFER_MS;
	cfg->rotate_seconds = LAYOUT_DEFAULT_ROTATE_SECONDS;
	copy_str(cfg->decoder, sizeof cfg->decoder, LAYOUT_DEFAULT_DECODER);
	cfg->ffmpeg_loglevel = LAYOUT_FFMPEG_LOG_ERROR;
	if (err && errlen)
		err[0] = '\0';

	int grid_line = 0;
	int lineno = 0;
	const char *p = text;

	/* A UTF-8 byte order mark (e.g. "UTF-8 with BOM" from Windows
	 * editors) is not part of the first line. */
	if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB
	    && (unsigned char)p[2] == 0xBF)
		p += 3;

	/* ~3.5 KB — kept off the stack. */
	struct config_fields *fields = malloc(sizeof *fields);
	if (!fields) {
		set_err(err, errlen, 0, "out of memory");
		return -1;
	}

	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		char line[LAYOUT_MAX_LINE];

		lineno++;
		if (len >= sizeof line) {
			set_err(err, errlen, lineno, "line longer than %d characters",
				LAYOUT_MAX_LINE - 1);
			goto fail;
		}
		memcpy(line, p, len);
		line[len] = '\0';
		p = nl ? nl + 1 : p + len;

		struct config_global g;
		switch (config_parse_line(line, &g, fields)) {
		case CONFIG_LINE_EMPTY:
			break;
		case CONFIG_LINE_INVALID:
			set_err(err, errlen, lineno,
				"not understood (expected KEY=VALUE or a '|'-separated camera line)");
			goto fail;
		case CONFIG_LINE_GLOBAL:
			if (handle_global(cfg, &g, lineno, flags, warn, warn_ctx, err, errlen) < 0)
				goto fail;
			if (!strcmp(g.key, "GRID"))
				grid_line = lineno;
			break;
		case CONFIG_LINE_CAMERA:
			if (handle_camera(cfg, fields, lineno, err, errlen) < 0)
				goto fail;
			break;
		}
	}
	free(fields);
	fields = NULL;

	if (!cfg->count) {
		set_err(err, errlen, 0,
			"no cameras configured: every camera line is commented out or missing - "
			"add at least one, e.g. front-door|rtsp://user:password@192.168.1.10:554/stream1|1 "
			"with GRID=2x2 (see the comments in the example config)");
		return -1;
	}

	/* UniFi Protect rtsps URLs, once UNIFI_REWRITE is known (it may come
	 * after the camera lines). Never longer than the input, so it fits. */
	if (cfg->unifi_rewrite)
		for (int i = 0; i < cfg->count; i++) {
			char out[LAYOUT_URL_MAX];
			if (layout_unifi_rewrite(cfg->cam[i].url, out, sizeof out) == 1) {
				copy_str(cfg->cam[i].url, sizeof cfg->cam[i].url, out);
				cfg->cam[i].unifi_rewritten = true;
			}
		}

	if (cfg->grid_cols > 0) {
		int cells = cfg->grid_cols * cfg->grid_rows;
		for (int i = 0; i < cfg->count; i++) {
			struct layout_camera *c = &cfg->cam[i];
			if (is_manual_line(c)) {
				set_err(err, errlen, c->line,
					"manual-mode camera line (name|url|width|height|x|y) in a "
					"GRID config (GRID set on line %d); use name|url|cell[|delay_ms] "
					"or remove GRID - the two formats cannot be mixed", grid_line);
				return -1;
			}
			if (!is_grid_line(c)) {
				set_err(err, errlen, c->line,
					"expected name|url|cell[|delay_ms] in GRID mode, got %d fields",
					c->n_numbers + 2);
				return -1;
			}
			if (c->cell < 1 || c->cell > cells) {
				set_err(err, errlen, c->line,
					"cell %d is outside the %dx%d grid (valid cells: 1-%d)",
					c->cell, cfg->grid_cols, cfg->grid_rows, cells);
				return -1;
			}
		}
	} else {
		int first_manual = 0;
		for (int i = 0; i < cfg->count && !first_manual; i++)
			if (is_manual_line(&cfg->cam[i]))
				first_manual = cfg->cam[i].line;
		for (int i = 0; i < cfg->count; i++) {
			struct layout_camera *c = &cfg->cam[i];
			if (is_grid_line(c)) {
				if (first_manual)
					set_err(err, errlen, c->line,
						"grid-mode camera line (name|url|cell) mixed with "
						"manual-mode lines (first on line %d); the two formats "
						"cannot be mixed", first_manual);
				else
					set_err(err, errlen, c->line,
						"name|url|cell lines require GRID=COLSxROWS "
						"(e.g. GRID=2x2)");
				return -1;
			}
			if (!is_manual_line(c)) {
				set_err(err, errlen, c->line,
					"expected name|url|width|height|x|y[|delay_ms], got %d fields",
					c->n_numbers + 2);
				return -1;
			}
			if (c->width <= 0 || c->height <= 0 || c->x < 0 || c->y < 0) {
				set_err(err, errlen, c->line,
					"width and height must be > 0 and x, y >= 0 (got %dx%d+%d+%d)",
					c->width, c->height, c->x, c->y);
				return -1;
			}
		}
	}
	return 0;

fail:
	free(fields);
	return -1;
}

/* ---------------------------------------------------------------- geometry */

int layout_grid_cell(int cols, int rows, int cell, int screen_w, int screen_h,
		     int *x, int *y, int *width, int *height)
{
	if (cols < 1 || rows < 1 || cell < 1 || cell > cols * rows
	    || screen_w < cols || screen_h < rows)
		return -1;

	int col = (cell - 1) % cols;
	int row = (cell - 1) / cols;

	/* int64 arithmetic: col * screen_w cannot overflow for sane sizes,
	 * but it costs nothing to be sure. */
	int x0 = (int)((long long)col * screen_w / cols);
	int x1 = (int)((long long)(col + 1) * screen_w / cols);
	int y0 = (int)((long long)row * screen_h / rows);
	int y1 = (int)((long long)(row + 1) * screen_h / rows);

	*x = x0;
	*y = y0;
	*width = x1 - x0;
	*height = y1 - y0;
	return 0;
}

int layout_apply(struct layout_config *cfg, int screen_w, int screen_h,
		 layout_warn_fn warn, void *warn_ctx, char *err, size_t errlen)
{
	if (screen_w <= 0 || screen_h <= 0) {
		set_err(err, errlen, 0, "invalid screen size %dx%d", screen_w, screen_h);
		return -1;
	}

	for (int i = 0; i < cfg->count; i++) {
		struct layout_camera *c = &cfg->cam[i];

		if (cfg->grid_cols > 0) {
			if (layout_grid_cell(cfg->grid_cols, cfg->grid_rows, c->cell,
					     screen_w, screen_h,
					     &c->x, &c->y, &c->width, &c->height) < 0) {
				set_err(err, errlen, c->line,
					"cell %d does not fit a %dx%d grid on a %dx%d screen",
					c->cell, cfg->grid_cols, cfg->grid_rows, screen_w, screen_h);
				return -1;
			}
		} else if ((long long)c->x + c->width > screen_w
			   || (long long)c->y + c->height > screen_h) {
			warnf(warn, warn_ctx, c->line,
			      "tile %dx%d+%d+%d of camera %s reaches outside the %dx%d screen",
			      c->width, c->height, c->x, c->y, c->name, screen_w, screen_h);
		}
	}
	return 0;
}

const char *layout_ffmpeg_log_name(enum layout_ffmpeg_log level)
{
	switch (level) {
	case LAYOUT_FFMPEG_LOG_QUIET:   return "quiet";
	case LAYOUT_FFMPEG_LOG_ERROR:   return "error";
	case LAYOUT_FFMPEG_LOG_WARNING: return "warning";
	case LAYOUT_FFMPEG_LOG_INFO:    return "info";
	}
	return "error";
}

/* -------------------------------------------------------------- URL masking */

/* Bounded appender: never writes past outlen-1, always NUL-terminated. */
struct sbuf {
	char   *p;
	size_t  len, cap;
	bool    overflow;   /* something did not fit */
};

static void sb_putn(struct sbuf *b, const char *s, size_t n)
{
	while (n && b->len + 1 < b->cap) {
		b->p[b->len++] = *s++;
		n--;
	}
	if (n)
		b->overflow = true;
	if (b->cap)
		b->p[b->len] = '\0';
}

static void sb_puts(struct sbuf *b, const char *s)
{
	sb_putn(b, s, strlen(s));
}

static bool segment_is_token(const char *s, size_t n)
{
	bool alnum_only = true;

	for (size_t i = 0; i < n; i++) {
		unsigned char ch = (unsigned char)s[i];
		if (isalnum(ch))
			continue;
		if (ch == '_' || ch == '-') {
			alnum_only = false;
			continue;
		}
		return false;   /* '.', '%', ... — a name, not a token */
	}
	return (alnum_only && n >= 16) || n >= 32;
}

static void mask_url_into(struct sbuf *b, const char *url, size_t url_len)
{
	const char *end = url + url_len;
	const char *scheme = NULL;

	for (const char *q = url; q + 3 <= end; q++)
		if (q[0] == ':' && q[1] == '/' && q[2] == '/') {
			scheme = q;
			break;
		}

	const char *auth = scheme ? scheme + 3 : url;
	const char *auth_end = auth;
	while (auth_end < end && *auth_end != '/' && *auth_end != '?' && *auth_end != '#')
		auth_end++;

	/* The LAST '@' inside the authority ends the userinfo — a password may
	 * itself contain an (unescaped) '@'. */
	const char *at = NULL, *colon = NULL;
	for (const char *q = auth; q < auth_end; q++)
		if (*q == '@')
			at = q;
	if (at)
		for (const char *q = auth; q < at; q++)
			if (*q == ':') {
				colon = q;
				break;
			}

	if (colon) {
		sb_putn(b, url, (size_t)(colon + 1 - url));
		sb_puts(b, "***");
		sb_putn(b, at, (size_t)(auth_end - at));
	} else {
		sb_putn(b, url, (size_t)(auth_end - url));
	}

	/* Path, one segment at a time. */
	const char *p = auth_end;
	while (p < end && *p == '/') {
		sb_putn(b, p, 1);
		p++;
		const char *seg = p;
		while (p < end && *p != '/' && *p != '?' && *p != '#')
			p++;
		if (segment_is_token(seg, (size_t)(p - seg)))
			sb_puts(b, "***");
		else
			sb_putn(b, seg, (size_t)(p - seg));
	}

	/* Query string / fragment: masked wholesale. */
	if (p < end) {
		sb_putn(b, p, 1);
		sb_puts(b, "***");
	}
}

void layout_mask_url(const char *url, char *out, size_t outlen)
{
	struct sbuf b = { out, 0, outlen, false };

	if (!outlen)
		return;
	out[0] = '\0';
	mask_url_into(&b, url, strlen(url));
}

/* Characters that may appear in a URL (RFC 3986: unreserved, reserved and
 * '%'), minus the ones that in practice wrap a URL in running text: quotes,
 * parentheses and angle brackets. */
static bool url_char(char c)
{
	if (isalnum((unsigned char)c))
		return true;
	return c && strchr("-._~:/?#[]@!$&*+,;=%", c) != NULL;
}

static bool scheme_char(char c)
{
	return isalnum((unsigned char)c) || c == '+' || c == '-' || c == '.';
}

void layout_mask_urls_in_text(const char *in, char *out, size_t outlen)
{
	struct sbuf b = { out, 0, outlen, false };

	if (!outlen)
		return;
	out[0] = '\0';

	const char *p = in;
	while (*p) {
		const char *sep = strstr(p, "://");
		if (!sep) {
			sb_puts(&b, p);
			return;
		}
		/* Walk back over the scheme letters. */
		const char *start = sep;
		while (start > p && scheme_char(start[-1]))
			start--;

		/* The URL ends at the first character a URL cannot contain ... */
		const char *stop = sep + 3;
		while (url_char(*stop))
			stop++;

		/* ... or where a second "scheme://" begins inside the same run
		 * ("rtsp://u:pw@h/x;rtsp://a:b@c"), which then gets its own pass. */
		for (const char *q = sep + 3; q + 3 <= stop; q++) {
			if (q[0] != ':' || q[1] != '/' || q[2] != '/')
				continue;
			const char *s2 = q;
			while (s2 > sep + 3 && scheme_char(s2[-1]))
				s2--;
			if (s2 < q) {
				stop = s2;
				break;
			}
		}

		/* Punctuation that ends a sentence or introduces the next part of
		 * a message ("...m1L: Connection refused", "..., retrying") is not
		 * part of the URL. */
		while (stop > sep + 3 && strchr(":,.;!?]", stop[-1]))
			stop--;

		sb_putn(&b, p, (size_t)(start - p));
		mask_url_into(&b, start, (size_t)(stop - start));
		p = stop;   /* stop >= sep + 3 > p: always progresses */
	}
}

void layout_sanitize_log_text(char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c < 0x20 && c != '\t') || c == 0x7f)
			*s = '?';
	}
}

/* -------------------------------------------------------- DRM device choice */

int layout_pick_drm_device(const struct drm_candidate *c, int n, bool connector_set)
{
	int first_usable = -1;

	for (int i = 0; i < n; i++) {
		if (!c[i].usable)
			continue;
		if (connector_set) {
			if (c[i].has_connector)
				return i;
			continue;
		}
		if (c[i].has_connected)
			return i;
		if (first_usable < 0)
			first_usable = i;
	}
	return connector_set ? -1 : first_usable;
}

/* ------------------------------------------------------------- display mode */

int layout_parse_mode(const char *s, int *w, int *h, int *mhz)
{
	char buf[64];

	if (!strcmp(s, "auto")) {
		*w = *h = *mhz = 0;
		return 0;
	}
	if (!copy_str(buf, sizeof buf, s))
		return -1;

	int hz_milli = 0;
	char *at = strchr(buf, '@');
	if (at) {
		*at = '\0';
		const char *p = at + 1;
		long whole = 0;
		int frac = 0, frac_digits = 0;

		if (!isdigit((unsigned char)*p))
			return -1;
		while (isdigit((unsigned char)*p) && whole < 100000)
			whole = whole * 10 + (*p++ - '0');
		if (*p == '.') {
			p++;
			while (isdigit((unsigned char)*p) && frac_digits < 3) {
				frac = frac * 10 + (*p++ - '0');
				frac_digits++;
			}
			if (!frac_digits)
				return -1;
		}
		if (*p)
			return -1;
		while (frac_digits++ < 3)
			frac *= 10;
		if (whole < 1 || whole > 240)
			return -1;
		hz_milli = (int)whole * 1000 + frac;
	}

	int ww, hh;
	if (layout_parse_size(buf, &ww, &hh) < 0 || ww > 8192 || hh > 8192)
		return -1;
	*w = ww;
	*h = hh;
	*mhz = hz_milli;
	return 0;
}

#define MODE_CAP_MHZ      60500   /* "up to 60 Hz" (59.94/60 count) */
#define MODE_LOW_MHZ      49500   /* "under 50 Hz" (49.95/50 do not count) */
#define MODE_TOLERANCE_MHZ 1000   /* explicit refresh match: +-1 Hz */

static bool same_aspect(const struct layout_mode *a, const struct layout_mode *b)
{
	long long l = (long long)a->width * b->height;
	long long r = (long long)a->height * b->width;
	long long d = l > r ? l - r : r - l;
	return d * 100 <= l;
}

/* True if candidate c is a better pick than the current best b (auto and
 * explicit-without-refresh): <= 60 Hz first, then refresh, then area. */
static bool mode_better(const struct layout_mode *c, const struct layout_mode *b)
{
	bool c_cap = c->refresh_mhz <= MODE_CAP_MHZ, b_cap = b->refresh_mhz <= MODE_CAP_MHZ;

	if (c_cap != b_cap)
		return c_cap;
	if (c->refresh_mhz != b->refresh_mhz)
		return c_cap ? c->refresh_mhz > b->refresh_mhz : c->refresh_mhz < b->refresh_mhz;
	return (long long)c->width * c->height > (long long)b->width * b->height;
}

int layout_select_mode(const struct layout_mode *m, int n, int want_w, int want_h,
		       int want_mhz, enum layout_mode_reason *reason)
{
	int best = -1;

	if (want_w > 0) {
		int best_diff = 0;
		*reason = LAYOUT_MODE_EXPLICIT;
		for (int i = 0; i < n; i++) {
			if (m[i].interlaced || m[i].width != want_w || m[i].height != want_h)
				continue;
			if (want_mhz) {
				int diff = abs(m[i].refresh_mhz - want_mhz);
				if (diff > MODE_TOLERANCE_MHZ)
					continue;
				if (best < 0 || diff < best_diff) {
					best = i;
					best_diff = diff;
				}
			} else if (best < 0 || mode_better(&m[i], &m[best])) {
				best = i;
			}
		}
		return best;
	}

	*reason = LAYOUT_MODE_PREFERRED;
	int base = -1;
	for (int i = 0; i < n && base < 0; i++)
		if (!m[i].interlaced && m[i].preferred)
			base = i;
	for (int i = 0; i < n && base < 0; i++)
		if (!m[i].interlaced)
			base = i;
	if (base < 0)
		return -1;

	const struct layout_mode *b = &m[base];
	bool low = b->refresh_mhz < MODE_LOW_MHZ;
	if (!low && b->width < 3840)
		return base;

	int min_mhz = b->refresh_mhz < MODE_LOW_MHZ ? b->refresh_mhz : MODE_LOW_MHZ;
	for (int i = 0; i < n; i++) {
		if (m[i].interlaced || m[i].width > 1920 || m[i].refresh_mhz < min_mhz
		    || !same_aspect(&m[i], b))
			continue;
		if (best < 0 || mode_better(&m[i], &m[best]))
			best = i;
	}
	if (best < 0 || best == base)
		return base;
	*reason = low ? LAYOUT_MODE_AUTO_LOW_REFRESH : LAYOUT_MODE_AUTO_4K;
	return best;
}

/* ------------------------------------------------------------ URL helpers */

/* Length of the "scheme" in "scheme://...", or 0 if url does not start
 * with one. */
static size_t url_scheme_len(const char *url)
{
	size_t n = 0;

	if (!isalpha((unsigned char)url[0]))
		return 0;
	while (scheme_char(url[n]))
		n++;
	return strncmp(url + n, "://", 3) == 0 ? n : 0;
}

bool layout_url_is_live(const char *url)
{
	size_t n = url_scheme_len(url);
	return n > 0 && !(n == 4 && strncasecmp(url, "file", 4) == 0);
}

bool layout_url_is_rtsp(const char *url)
{
	size_t n = url_scheme_len(url);
	return (n == 4 && strncasecmp(url, "rtsp", 4) == 0)
	       || (n == 5 && strncasecmp(url, "rtsps", 5) == 0);
}

int layout_unifi_rewrite(const char *url, char *out, size_t outlen)
{
	static const char port_in[] = "7441";

	if (!outlen)
		return -1;
	if (strncasecmp(url, "rtsps://", 8) != 0) {
		copy_str(out, outlen, url);
		return 0;
	}

	const char *auth = url + 8;
	const char *auth_end = auth + strcspn(auth, "/?#");

	const char *host = auth;
	for (const char *q = auth; q < auth_end; q++)
		if (*q == '@')
			host = q + 1;

	const char *colon = NULL;
	if (*host == '[') {
		const char *rb = memchr(host, ']', (size_t)(auth_end - host));
		if (rb && rb + 1 < auth_end && rb[1] == ':')
			colon = rb + 1;
	} else {
		for (const char *q = host; q < auth_end; q++)
			if (*q == ':')
				colon = q;
	}
	if (!colon || (size_t)(auth_end - colon - 1) != sizeof port_in - 1
	    || strncmp(colon + 1, port_in, sizeof port_in - 1) != 0) {
		copy_str(out, outlen, url);
		return 0;
	}

	struct sbuf b = { out, 0, outlen, false };
	out[0] = '\0';
	sb_puts(&b, "rtsp://");
	sb_putn(&b, auth, (size_t)(colon - auth));
	sb_puts(&b, ":7447");

	const char *path_end = auth_end + strcspn(auth_end, "?#");
	sb_putn(&b, auth_end, (size_t)(path_end - auth_end));

	const char *p = path_end;
	if (*p == '?') {
		bool first = true;
		p++;
		while (*p && *p != '#') {
			size_t len = strcspn(p, "&#");
			size_t key_len = strcspn(p, "=&#");
			bool drop = key_len == 10 && strncasecmp(p, "enableSrtp", 10) == 0;
			if (!drop && len > 0) {
				sb_puts(&b, first ? "?" : "&");
				sb_putn(&b, p, len);
				first = false;
			}
			p += len;
			if (*p == '&')
				p++;
		}
	}
	if (*p == '#')
		sb_puts(&b, p);
	return b.overflow ? -1 : 1;
}

/* ------------------------------------------------------------ sd_notify */

int layout_notify_sockaddr(const char *env, char *sun_path, size_t cap, size_t *len)
{
	if (!env || !*env || !cap)
		return -1;
	if (env[0] == '@') {
		size_t n = strlen(env + 1);
		if (!n || n + 1 >= cap)
			return -1;
		sun_path[0] = '\0';
		memcpy(sun_path + 1, env + 1, n);
		*len = n + 1;
		return 0;
	}
	if (env[0] != '/')
		return -1;
	size_t n = strlen(env);
	if (n >= cap)
		return -1;
	memcpy(sun_path, env, n + 1);
	*len = n;
	return 0;
}

bool layout_is_display_server(const char *comm)
{
	static const char *const names[] = {
		"labwc", "Xorg", "X", "Xwayland", "wayfire", "lightdm", "gdm", "gdm3",
		"gdm-x-session", "gdm-wayland-ses", "sddm", "weston", "kodi", "kodi.bin",
		"kodi-standalone", "gnome-shell", "kwin_wayland", "sway", "cage",
		"mutter", "plymouthd",
	};
	size_t n = strcspn(comm, "\n");

	for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
		if (strlen(names[i]) == n && strncmp(comm, names[i], n) == 0)
			return true;
	return false;
}
