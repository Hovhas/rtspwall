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

/* ---------------------------------------------------------------- whole file */

static int handle_global(struct layout_config *cfg, const struct config_global *g, int line,
			 layout_warn_fn warn, void *ctx, char *err, size_t errlen)
{
	int v;

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
	} else {
		warnf(warn, ctx, line, "unknown key %s, ignored", g->key);
	}
	return 0;
}

static int handle_camera(struct layout_config *cfg, const struct config_fields *f, int line,
			 char *err, size_t errlen)
{
	if (f->count > CONFIG_MAX_FIELDS) {
		set_err(err, errlen, line,
			"too many fields (%d); expected name|url|cell[|delay_ms] or "
			"name|url|width|height|x|y[|delay_ms]", f->count);
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
	memset(cfg, 0, sizeof *cfg);
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
			if (handle_global(cfg, &g, lineno, warn, warn_ctx, err, errlen) < 0)
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
		set_err(err, errlen, 0, "no cameras in config");
		return -1;
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
};

static void sb_putn(struct sbuf *b, const char *s, size_t n)
{
	while (n-- && b->len + 1 < b->cap)
		b->p[b->len++] = *s++;
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
	struct sbuf b = { out, 0, outlen };

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
	struct sbuf b = { out, 0, outlen };

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
