/*
 * layout.h — pure config parsing and screen layout.
 *
 * Like pacing.h this module has no Linux, DRM or FFmpeg dependencies, so the
 * whole config syntax, its validation and the tile geometry are unit-tested
 * by `make test` (see test_layout.c) on any machine.
 *
 * Config file format (one item per line, '#' starts a comment line):
 *
 *   KEY=VALUE                       global setting, may appear anywhere
 *   name|url|cell[|delay_ms]        grid mode (requires GRID=COLSxROWS)
 *   name|url|width|height|x|y[|delay_ms]   manual mode (no GRID key)
 *
 * The two camera line formats cannot be mixed in one file. Cameras with
 * the same tile (same cell in grid mode, or identical width/height/x/y in
 * manual mode) form a rotation group.
 */
#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

#define LAYOUT_MAX_CAMERAS             16
#define LAYOUT_MAX_GRID                 8     /* max columns and max rows */
#define LAYOUT_MAX_LINE              1024
#define LAYOUT_NAME_MAX                64
#define LAYOUT_URL_MAX                512
#define LAYOUT_PATH_MAX               256

/* 120 ms gave 0.7–0.9 % late frames on a 30 fps camera and ~9 % at 24 fps;
 * 160 ms brought that down to 0–1 late frame per minute on the same
 * cameras, at the cost of +40 ms extra latency. */
#define LAYOUT_DEFAULT_BUFFER_MS      160
#define LAYOUT_DEFAULT_ROTATE_SECONDS  15
#define LAYOUT_DEFAULT_DECODER        "/dev/video10"
#define LAYOUT_MAX_DELAY_MS         10000     /* |delay_ms| limit */

/* Process exit codes with a meaning for the service manager. The systemd
 * unit lists 2 and 3 in RestartPreventExitStatus=, so a problem that a
 * restart cannot fix ends the service with a readable status instead of a
 * restart loop. */
#define RTSPWALL_EXIT_CONFIG          2   /* config error, no cameras, bad usage */
#define RTSPWALL_EXIT_NO_DECODER      3   /* no usable H.264 hardware decoder */

/* FFMPEG_LOGLEVEL: how much of libav's own logging reaches the log. */
enum layout_ffmpeg_log {
	LAYOUT_FFMPEG_LOG_QUIET,
	LAYOUT_FFMPEG_LOG_ERROR,     /* default */
	LAYOUT_FFMPEG_LOG_WARNING,
	LAYOUT_FFMPEG_LOG_INFO,
};

/* "quiet", "error", "warning" or "info" for a level. */
const char *layout_ffmpeg_log_name(enum layout_ffmpeg_log level);

/* ------------------------------------------------------------ single line */

enum config_line_type {
	CONFIG_LINE_EMPTY,     /* blank line or comment — nothing to do */
	CONFIG_LINE_GLOBAL,    /* KEY=VALUE */
	CONFIG_LINE_CAMERA,    /* '|'-separated fields */
	CONFIG_LINE_INVALID,   /* neither of the above */
};

struct config_global {
	char key[32];
	char value[LAYOUT_PATH_MAX];
	bool key_overlong;     /* key did not fit in key[] (stored truncated) */
	bool value_overlong;   /* value did not fit in value[] (stored truncated) */
};

#define CONFIG_MAX_FIELDS 7

struct config_fields {
	int  count;   /* number of fields on the line; may exceed CONFIG_MAX_FIELDS
	               * (the extra fields are counted but not stored) */
	char field[CONFIG_MAX_FIELDS][LAYOUT_URL_MAX];
	int  overlong;   /* 1-based number of the first field that did not fit
	                  * in field[] (stored truncated), 0 = none */
};

/* Classifies and splits one config line. `line` is not modified. Fields
 * and key/value are trimmed of surrounding whitespace (including '\r').
 * Empty fields are kept (so "a||b" has three fields, the middle one empty).
 * CONFIG_LINE_GLOBAL is written to *global and CONFIG_LINE_CAMERA to
 * *fields — only if the respective pointer is not NULL. Camera lines are
 * recognised by '|' BEFORE '=' is considered, because an RTSP URL may well
 * contain '=' (query parameters/tokens). Deciding whether a key is known,
 * or whether the field count is right, is the caller's job. */
enum config_line_type config_parse_line(const char *line, struct config_global *global,
					struct config_fields *fields);

/* ---------------------------------------------------------------- whole file */

struct layout_camera {
	char name[LAYOUT_NAME_MAX];
	char url[LAYOUT_URL_MAX];
	int  line;                    /* config line number, for messages */
	int  cell;                    /* grid mode: 1-based cell, row-major; 0 in manual mode */
	int  width, height, x, y;     /* tile on screen; set by layout_apply in grid mode */
	int  delay_ms;                /* optional last field, default 0 */
	int  n_numbers;               /* internal: numeric fields seen on the line */
	bool unifi_rewritten;         /* url was rewritten by layout_unifi_rewrite */
};

struct layout_config {
	int  buffer_ms;               /* BUFFER_MS */
	int  rotate_seconds;          /* ROTATE_SECONDS */
	int  grid_cols, grid_rows;    /* GRID=COLSxROWS; 0/0 = manual mode */
	char decoder[LAYOUT_PATH_MAX];   /* DECODER */
	char connector[32];           /* CONNECTOR; "" = first connected */
	char drm_device[LAYOUT_PATH_MAX];  /* DRM_DEVICE; "" = auto-detect */
	enum layout_ffmpeg_log ffmpeg_loglevel;   /* FFMPEG_LOGLEVEL */
	int  mode_w, mode_h;          /* MODE=WxH[@Hz]; 0/0 = auto */
	int  mode_mhz;                /* refresh in mHz, 0 = any */
	bool unifi_rewrite;           /* UNIFI_REWRITE=auto (true, default) | off */

	struct layout_camera cam[LAYOUT_MAX_CAMERAS];
	int  count;
};

/* Called for non-fatal problems (unknown key, tile outside the screen). */
typedef void (*layout_warn_fn)(void *ctx, const char *msg);

/* Parses a whole config file held in `text` (NUL-terminated). Returns 0 on
 * success. On error returns -1 and writes a message of the form
 * "line N: ..." (or a file-level message without a line number) to `err`.
 * `warn` may be NULL. After success in grid mode the tiles are not yet
 * known — call layout_apply once the screen size is known. */
int layout_parse(struct layout_config *cfg, const char *text,
		 layout_warn_fn warn, void *warn_ctx, char *err, size_t errlen);

/* Flags for layout_parse_flags. */
#define LAYOUT_PARSE_STRICT  0x1u   /* an unknown key is an error, not a warning
				     * (--check-config); the daemon stays lenient
				     * so an upgrade never bricks a running wall */

/* layout_parse with flags. layout_parse(...) == layout_parse_flags(..., 0, ...).
 *
 * Always errors, in both modes:
 *   - a placeholder (see layout_has_placeholder) in a camera field or a
 *     global value, reported with its line number;
 *   - a file without a single camera line: "no cameras configured: ..."
 *     (file-level message, no line number).
 * Unknown keys: warning (default) or error (LAYOUT_PARSE_STRICT), with
 * "did you mean X?" from layout_suggest_key when a known key is close.
 * When UNIFI_REWRITE is auto (the default) every camera URL is passed
 * through layout_unifi_rewrite after the whole file has been read;
 * rewritten cameras get unifi_rewritten = true (the caller logs it). */
int layout_parse_flags(struct layout_config *cfg, const char *text, unsigned flags,
		       layout_warn_fn warn, void *warn_ctx, char *err, size_t errlen);

/* True if `s` contains a template placeholder: "CHANGE_ME" (any case) or
 * "<something>" (a '<', at least one character, then '>'). */
bool layout_has_placeholder(const char *s);

/* Case-insensitive Levenshtein distance (edit distance). Strings longer
 * than 63 characters are compared on their first 63. */
int layout_levenshtein(const char *a, const char *b);

/* The known config key closest to `key`, or NULL if none is close enough
 * (distance <= 1 for keys up to 5 characters, otherwise <= 2; a key that
 * differs only in case always matches). */
const char *layout_suggest_key(const char *key);

/* ------------------------------------------------------------- display mode */

/* Parses MODE: "auto" (w = h = mhz = 0), "WxH" (mhz = 0) or "WxH@Hz" with
 * an integer or decimal refresh ("59.94" -> 59940). Returns 0 or -1. */
int layout_parse_mode(const char *s, int *w, int *h, int *mhz);

/* One connector mode, as the caller found it. */
struct layout_mode {
	int  width, height;
	int  refresh_mhz;     /* refresh rate in mHz */
	bool interlaced;
	bool preferred;       /* DRM_MODE_TYPE_PREFERRED */
};

enum layout_mode_reason {
	LAYOUT_MODE_PREFERRED,        /* auto: the display's preferred mode kept */
	LAYOUT_MODE_AUTO_LOW_REFRESH, /* auto: preferred refresh < 50 Hz, replaced */
	LAYOUT_MODE_AUTO_4K,          /* auto: preferred width >= 3840, replaced */
	LAYOUT_MODE_EXPLICIT,         /* MODE=WxH[@Hz] matched */
};

/* Picks a mode. Interlaced modes are never chosen.
 *
 * Auto (want_w == 0): the preferred mode (the first progressive mode with
 * the preferred flag, else the first progressive mode) is kept unless its
 * refresh is under 50 Hz or its width is 3840 or more. Then the
 * progressive mode with the same aspect ratio (1 % tolerance), width <=
 * 1920 and refresh >= min(preferred refresh, 49.5 Hz) is chosen: the
 * highest refresh up to 60 Hz first (a 100/120 Hz mode gains nothing for
 * cameras and multiplies the commits), then the largest area, then the
 * list order. If no such mode exists the preferred mode is kept.
 *
 * Explicit (want_w/want_h set): same size and, if want_mhz != 0, a refresh
 * within 1 Hz (the closest wins); without a refresh the highest refresh up
 * to 60 Hz, else the highest.
 *
 * Returns the index into m[], or -1 if nothing matches (the caller lists
 * the available modes). */
int layout_select_mode(const struct layout_mode *m, int n, int want_w, int want_h,
		       int want_mhz, enum layout_mode_reason *reason);

/* ------------------------------------------------------------ URL helpers */

/* UniFi Protect shows its streams as rtsps://HOST:7441/TOKEN?enableSrtp.
 * If `url` is such a URL (scheme rtsps, any case, port 7441) writes the
 * plain-RTSP equivalent to `out`: scheme rtsp, port 7447, userinfo/host/
 * path kept, every "enableSrtp" query parameter removed (the '?' too if
 * nothing is left). Returns 1 if rewritten, 0 if `url` is not a UniFi
 * Protect rtsps URL (out = copy of url), -1 if `out` is too small (out is
 * NUL-terminated but incomplete). */
int layout_unifi_rewrite(const char *url, char *out, size_t outlen);

/* True for a network stream ("scheme://..." with any scheme but file),
 * false for a local file (a plain path or a file: URL). Local files are
 * read paced by pts and looped (demo clips); live streams never are. */
bool layout_url_is_live(const char *url);

/* True for rtsp:// and rtsps:// (any case) — only those get RTSP options. */
bool layout_url_is_rtsp(const char *url);

/* ------------------------------------------------------------ sd_notify */

/* Converts $NOTIFY_SOCKET to the sun_path bytes of a sockaddr_un: an
 * absolute path is copied (NUL-terminated); "@name" is the abstract
 * namespace, written as a leading NUL followed by name. *len is the number
 * of significant bytes in sun_path (for the sockaddr length: offsetof
 * (struct sockaddr_un, sun_path) + *len). Returns 0, or -1 if env is NULL,
 * empty, relative, a bare "@" or does not fit in cap - 1 bytes. */
int layout_notify_sockaddr(const char *env, char *sun_path, size_t cap, size_t *len);

/* True if `comm` (a /proc/PID/comm value, trailing newline allowed) is a
 * desktop/display server process that typically holds DRM master. */
bool layout_is_display_server(const char *comm);

/* Computes each camera's tile for a screen of screen_w x screen_h pixels
 * (grid mode), or checks the manual tiles against the screen (manual mode:
 * a tile reaching outside the screen is a warning, not an error). Returns 0
 * on success, -1 with a message in `err` on failure. */
int layout_apply(struct layout_config *cfg, int screen_w, int screen_h,
		 layout_warn_fn warn, void *warn_ctx, char *err, size_t errlen);

/* Geometry of one grid cell (1-based, row-major: 1 = top left, `cols` = top
 * right, cols+1 = first cell of the second row).
 *
 * Rounding: column edges are placed at floor(i * screen_w / cols) and row
 * edges at floor(j * screen_h / rows). The tiles therefore cover the whole
 * screen with no gaps or overlap; when the size does not divide evenly the
 * tiles differ by at most one pixel, with the extra pixels going to the
 * later columns/rows (e.g. 1366 / 3 gives widths 455, 455, 456).
 *
 * Returns 0, or -1 if the cell or grid is out of range. */
int layout_grid_cell(int cols, int rows, int cell, int screen_w, int screen_h,
		     int *x, int *y, int *width, int *height);

/* Parses "WxH" (e.g. "1920x1080"). Returns 0, or -1 on bad input. */
int layout_parse_size(const char *s, int *w, int *h);

/* Copies `url` to `out` with anything credential-like replaced by "***":
 *
 *   - the password in the userinfo part:
 *       rtsp://user:secret@host/path  ->  rtsp://user:***@host/path
 *   - the whole query string (and fragment):
 *       realmonitor?channel=1&subtype=0  ->  realmonitor?***
 *   - every path segment that looks like a token, e.g. the UniFi segment
 *     in rtsp://host:7447/aB3dE5fG7hJ9kL1m becomes "***".
 *
 * A path segment counts as a token if it is either
 *   (a) at least 16 characters, all [A-Za-z0-9] — UniFi Protect tokens are
 *       16 random alphanumerics, and a random token may happen to contain
 *       no digit, so no "must mix letters and digits" requirement; or
 *   (b) at least 32 characters, all [A-Za-z0-9_-] — long base64url-style
 *       tokens.
 * Camera path names such as "stream1", "Streaming/Channels/101",
 * "h264Preview_01_main" (has '_', under 32) or "front_door" are left alone.
 * A false positive only makes a log line less informative; a false negative
 * leaks a credential, so the rules lean towards masking. Always
 * NUL-terminates. */
void layout_mask_url(const char *url, char *out, size_t outlen);

/* Copies free text (e.g. an FFmpeg log line) to `out`, running every
 * embedded "scheme://..." URL (up to whitespace or a quote/bracket)
 * through layout_mask_url. Always NUL-terminates. */
void layout_mask_urls_in_text(const char *in, char *out, size_t outlen);

/* Replaces control characters (< 0x20 except '\t', and 0x7f) in place with
 * '?', so text from an external source (FFmpeg log lines, stream metadata)
 * cannot inject terminal escape sequences or fake extra lines into the log.
 * Strip the trailing newline before calling. */
void layout_sanitize_log_text(char *s);

/* ------------------------------------------------------- DRM device choice */

/* What the caller found out about one /dev/dri/cardN, in scan order. */
struct drm_candidate {
	bool usable;          /* opened, drmModeGetResources ok, >= 1 connector */
	bool has_connector;   /* the configured CONNECTOR exists on this device */
	bool has_connected;   /* at least one connector is connected */
};

/* Picks the DRM device to use when DRM_DEVICE is auto. With CONNECTOR set:
 * the first usable device that has that connector (connected or not — the
 * display search then gives the precise error). Without: the first usable
 * device with a connected connector, falling back to the first usable
 * device (so the display search can list its connectors). Returns the
 * index, or -1 if nothing fits. */
int layout_pick_drm_device(const struct drm_candidate *c, int n, bool connector_set);

#endif
