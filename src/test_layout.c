/*
 * test_layout.c — unit tests for layout.c: config line parsing, whole-file
 * validation (grid and manual mode), grid geometry, URL masking and DRM
 * device choice.
 */
#include <stdio.h>
#include <string.h>

#include "layout.h"
#include "pacing.h"
#include "test.h"

static char err[512];

static int warnings;
static char last_warning[512];

static void count_warning(void *ctx, const char *msg)
{
	(void)ctx;
	warnings++;
	snprintf(last_warning, sizeof last_warning, "%s", msg);
}

static int parse(struct layout_config *cfg, const char *text)
{
	warnings = 0;
	last_warning[0] = '\0';
	err[0] = '\0';
	return layout_parse(cfg, text, count_warning, NULL, err, sizeof err);
}

/* ------------------------------------------------------------- single line */

static void test_line_empty(void)
{
	ASSERT_EQ_I(config_parse_line("", NULL, NULL), CONFIG_LINE_EMPTY);
	ASSERT_EQ_I(config_parse_line("\n", NULL, NULL), CONFIG_LINE_EMPTY);
	ASSERT_EQ_I(config_parse_line("   \n", NULL, NULL), CONFIG_LINE_EMPTY);
	ASSERT_EQ_I(config_parse_line("# a comment\n", NULL, NULL), CONFIG_LINE_EMPTY);
	ASSERT_EQ_I(config_parse_line("  # indented comment\n", NULL, NULL), CONFIG_LINE_EMPTY);
}

static void test_line_global(void)
{
	struct config_global g = { 0 };
	enum config_line_type t = config_parse_line("BUFFER_MS=120\n", &g, NULL);
	ASSERT_EQ_I(t, CONFIG_LINE_GLOBAL);
	ASSERT(strcmp(g.key, "BUFFER_MS") == 0);
	ASSERT(strcmp(g.value, "120") == 0);

	struct config_global g2 = { 0 };
	t = config_parse_line("  GRID = 2x2 \r\n", &g2, NULL);
	ASSERT_EQ_I(t, CONFIG_LINE_GLOBAL);
	ASSERT(strcmp(g2.key, "GRID") == 0);
	ASSERT(strcmp(g2.value, "2x2") == 0);

	/* unknown key — the line parser passes no judgement, it only harvests
	 * key/value. Warning about it is the caller's job. */
	struct config_global g3 = { 0 };
	t = config_parse_line("SOMETHING_UNKNOWN=42\n", &g3, NULL);
	ASSERT_EQ_I(t, CONFIG_LINE_GLOBAL);
	ASSERT(strcmp(g3.key, "SOMETHING_UNKNOWN") == 0);

	ASSERT_EQ_I(config_parse_line("=42\n", NULL, NULL), CONFIG_LINE_INVALID);
	ASSERT_EQ_I(config_parse_line("just words\n", NULL, NULL), CONFIG_LINE_INVALID);
}

static void test_line_camera_six_fields(void)
{
	struct config_fields f;
	enum config_line_type t = config_parse_line(
		"cam1|rtsp://example/token|960|540|0|0\n", NULL, &f);
	ASSERT_EQ_I(t, CONFIG_LINE_CAMERA);
	ASSERT_EQ_I(f.count, 6);
	ASSERT(strcmp(f.field[0], "cam1") == 0);
	ASSERT(strcmp(f.field[1], "rtsp://example/token") == 0);
	ASSERT(strcmp(f.field[5], "0") == 0);
}

static void test_line_camera_url_with_equals_is_camera(void)
{
	/* '|' wins over '=' — a URL may carry query parameters. */
	struct config_fields f;
	enum config_line_type t = config_parse_line(
		"cam1|rtsp://h/s?token=abc&x=1|2\n", NULL, &f);
	ASSERT_EQ_I(t, CONFIG_LINE_CAMERA);
	ASSERT_EQ_I(f.count, 3);
	ASSERT(strcmp(f.field[1], "rtsp://h/s?token=abc&x=1") == 0);
}

static void test_line_camera_empty_fields_kept(void)
{
	struct config_fields f;
	config_parse_line("a||b\n", NULL, &f);
	ASSERT_EQ_I(f.count, 3);
	ASSERT(strcmp(f.field[1], "") == 0);
}

/* ------------------------------------------------------------- manual mode */

static void test_manual_six_and_seven_fields(void)
{
	struct layout_config cfg;
	int r = parse(&cfg,
		"cam1|rtsp://example/token|960|540|0|0\n"
		"cam2|rtsp://example/token2|960|540|960|0|80\n");
	ASSERT_EQ_I(r, 0);
	ASSERT_EQ_I(cfg.count, 2);
	ASSERT_EQ_I(cfg.grid_cols, 0);
	ASSERT(strcmp(cfg.cam[0].name, "cam1") == 0);
	ASSERT(strcmp(cfg.cam[0].url, "rtsp://example/token") == 0);
	ASSERT_EQ_I(cfg.cam[0].width, 960);
	ASSERT_EQ_I(cfg.cam[0].height, 540);
	ASSERT_EQ_I(cfg.cam[0].x, 0);
	ASSERT_EQ_I(cfg.cam[0].y, 0);
	ASSERT_EQ_I(cfg.cam[0].delay_ms, 0);   /* missing — default 0 */
	ASSERT_EQ_I(cfg.cam[1].x, 960);
	ASSERT_EQ_I(cfg.cam[1].delay_ms, 80);
}

static void test_manual_too_few_fields_is_error(void)
{
	struct layout_config cfg;
	/* five fields — y missing */
	ASSERT_EQ_I(parse(&cfg, "cam3|rtsp://x|960|540|0\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);
}

static void test_defaults_and_globals(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT_EQ_I(cfg.buffer_ms, 160);
	ASSERT_EQ_I(cfg.rotate_seconds, 15);
	ASSERT(strcmp(cfg.decoder, "/dev/video10") == 0);
	ASSERT(strcmp(cfg.connector, "") == 0);

	ASSERT_EQ_I(parse(&cfg,
		"BUFFER_MS=120\n"
		"ROTATE_SECONDS=30\n"
		"DECODER=/dev/video11\n"
		"CONNECTOR=HDMI-A-2\n"
		"cam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT_EQ_I(cfg.buffer_ms, 120);
	ASSERT_EQ_I(cfg.rotate_seconds, 30);
	ASSERT(strcmp(cfg.decoder, "/dev/video11") == 0);
	ASSERT(strcmp(cfg.connector, "HDMI-A-2") == 0);
	ASSERT_EQ_I(warnings, 0);
}

static void test_unknown_key_is_warning_not_error(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "SOMETHING=1\ncam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT_EQ_I(warnings, 1);
	ASSERT(strstr(last_warning, "line 1:") != NULL);
	ASSERT(strstr(last_warning, "SOMETHING") != NULL);
}

static void test_bad_global_values_are_errors(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0\nBUFFER_MS=fast\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
	ASSERT_EQ_I(parse(&cfg, "ROTATE_SECONDS=0\ncam1|rtsp://x|960|540|0|0\n"), -1);
	ASSERT_EQ_I(parse(&cfg, "GRID=2\ncam1|rtsp://x|1\n"), -1);
	ASSERT_EQ_I(parse(&cfg, "GRID=0x2\ncam1|rtsp://x|1\n"), -1);
	ASSERT_EQ_I(parse(&cfg, "GRID=9x9\ncam1|rtsp://x|1\n"), -1);
}

static void test_non_integer_field_is_error(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "# header\ncam1|rtsp://x|960|wide|0|0\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
	ASSERT(strstr(err, "not an integer") != NULL);
}

static void test_no_cameras_is_error(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "# nothing here\nBUFFER_MS=100\n"), -1);
	ASSERT(strstr(err, "no cameras") != NULL);
}

static void test_too_many_cameras_is_error(void)
{
	struct layout_config cfg;
	char text[4096] = "GRID=4x4\n";
	for (int i = 0; i < LAYOUT_MAX_CAMERAS + 1; i++) {
		char line[64];
		snprintf(line, sizeof line, "cam%d|rtsp://x|%d\n", i + 1, i % 16 + 1);
		strcat(text, line);
	}
	ASSERT_EQ_I(parse(&cfg, text), -1);
	ASSERT(strstr(err, "too many cameras") != NULL);
}

/* --------------------------------------------------------------- grid mode */

static void test_grid_2x2_on_1080p(void)
{
	struct layout_config cfg;
	int r = parse(&cfg,
		"GRID=2x2\n"
		"cam1|rtsp://x/1|1\n"
		"cam2|rtsp://x/2|2\n"
		"cam3|rtsp://x/3|3\n"
		"cam4|rtsp://x/4|4|40\n");
	ASSERT_EQ_I(r, 0);
	ASSERT_EQ_I(cfg.grid_cols, 2);
	ASSERT_EQ_I(cfg.grid_rows, 2);
	ASSERT_EQ_I(cfg.cam[3].cell, 4);
	ASSERT_EQ_I(cfg.cam[3].delay_ms, 40);

	ASSERT_EQ_I(layout_apply(&cfg, 1920, 1080, NULL, NULL, err, sizeof err), 0);
	int expect[4][4] = {
		{ 960, 540, 0, 0 }, { 960, 540, 960, 0 },
		{ 960, 540, 0, 540 }, { 960, 540, 960, 540 },
	};
	for (int i = 0; i < 4; i++) {
		ASSERT_EQ_I(cfg.cam[i].width, expect[i][0]);
		ASSERT_EQ_I(cfg.cam[i].height, expect[i][1]);
		ASSERT_EQ_I(cfg.cam[i].x, expect[i][2]);
		ASSERT_EQ_I(cfg.cam[i].y, expect[i][3]);
	}
}

static void test_grid_3x3_on_1080p(void)
{
	int x, y, w, h;
	/* cell 1: top left */
	ASSERT_EQ_I(layout_grid_cell(3, 3, 1, 1920, 1080, &x, &y, &w, &h), 0);
	ASSERT_EQ_I(x, 0); ASSERT_EQ_I(y, 0); ASSERT_EQ_I(w, 640); ASSERT_EQ_I(h, 360);
	/* cell 3: top right */
	ASSERT_EQ_I(layout_grid_cell(3, 3, 3, 1920, 1080, &x, &y, &w, &h), 0);
	ASSERT_EQ_I(x, 1280); ASSERT_EQ_I(y, 0);
	/* cell 5: center */
	ASSERT_EQ_I(layout_grid_cell(3, 3, 5, 1920, 1080, &x, &y, &w, &h), 0);
	ASSERT_EQ_I(x, 640); ASSERT_EQ_I(y, 360); ASSERT_EQ_I(w, 640); ASSERT_EQ_I(h, 360);
	/* cell 9: bottom right */
	ASSERT_EQ_I(layout_grid_cell(3, 3, 9, 1920, 1080, &x, &y, &w, &h), 0);
	ASSERT_EQ_I(x, 1280); ASSERT_EQ_I(y, 720); ASSERT_EQ_I(w, 640); ASSERT_EQ_I(h, 360);
}

static void test_grid_uneven_division_covers_screen(void)
{
	/* 1366x768 in 3x3: 1366/3 is not whole. Edges at floor(i*W/cols):
	 * 0, 455, 910, 1366 -> widths 455, 455, 456 (extra pixel to the last
	 * column). 768/3 = 256 exactly. */
	int x, y, w, h;
	int widths[3];
	for (int c = 1; c <= 3; c++) {
		ASSERT_EQ_I(layout_grid_cell(3, 3, c, 1366, 768, &x, &y, &w, &h), 0);
		widths[c - 1] = w;
		ASSERT_EQ_I(h, 256);
	}
	ASSERT_EQ_I(widths[0], 455);
	ASSERT_EQ_I(widths[1], 455);
	ASSERT_EQ_I(widths[2], 456);

	/* No gaps or overlap, the last tile ends exactly at the screen edge —
	 * for a range of awkward sizes and grids. */
	int sizes[][2] = { { 1366, 768 }, { 1280, 1024 }, { 1920, 1080 }, { 1000, 999 } };
	for (size_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
		for (int n = 1; n <= LAYOUT_MAX_GRID; n++) {
			int expect_x = 0;
			for (int c = 1; c <= n; c++) {
				ASSERT_EQ_I(layout_grid_cell(n, n, c, sizes[s][0], sizes[s][1],
							     &x, &y, &w, &h), 0);
				ASSERT_EQ_I(x, expect_x);
				ASSERT(w == sizes[s][0] / n || w == sizes[s][0] / n + 1);
				expect_x = x + w;
			}
			ASSERT_EQ_I(expect_x, sizes[s][0]);

			int expect_y = 0;
			for (int r = 0; r < n; r++) {
				ASSERT_EQ_I(layout_grid_cell(n, n, r * n + 1, sizes[s][0], sizes[s][1],
							     &x, &y, &w, &h), 0);
				ASSERT_EQ_I(y, expect_y);
				expect_y = y + h;
			}
			ASSERT_EQ_I(expect_y, sizes[s][1]);
		}
	}
}

static void test_grid_720p(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\ncam1|rtsp://x|4\n"), 0);
	ASSERT_EQ_I(layout_apply(&cfg, 1280, 720, NULL, NULL, err, sizeof err), 0);
	ASSERT_EQ_I(cfg.cam[0].width, 640);
	ASSERT_EQ_I(cfg.cam[0].height, 360);
	ASSERT_EQ_I(cfg.cam[0].x, 640);
	ASSERT_EQ_I(cfg.cam[0].y, 360);
}

static void test_grid_shared_cell_forms_rotation_group(void)
{
	struct layout_config cfg;
	int r = parse(&cfg,
		"GRID=2x2\n"
		"cam1|rtsp://x/1|1\n"
		"cam2|rtsp://x/2|2\n"
		"cam3|rtsp://x/3|3\n"
		"cam4|rtsp://x/4|4\n"
		"cam5|rtsp://x/5|3\n"
		"cam6|rtsp://x/6|4\n");
	ASSERT_EQ_I(r, 0);
	ASSERT_EQ_I(layout_apply(&cfg, 1920, 1080, NULL, NULL, err, sizeof err), 0);

	struct pacing_tile tiles[LAYOUT_MAX_CAMERAS];
	for (int i = 0; i < cfg.count; i++)
		tiles[i] = (struct pacing_tile){ cfg.cam[i].width, cfg.cam[i].height,
						 cfg.cam[i].x, cfg.cam[i].y };
	struct pacing_group groups[LAYOUT_MAX_CAMERAS];
	int n = pacing_build_groups(tiles, cfg.count, groups, LAYOUT_MAX_CAMERAS);

	ASSERT_EQ_I(n, 4);
	ASSERT_EQ_I(groups[2].count, 2);
	ASSERT_EQ_I(groups[2].index[0], 2);   /* cam3 first, starts active */
	ASSERT_EQ_I(groups[2].index[1], 4);   /* cam5 */
	ASSERT_EQ_I(groups[3].count, 2);
	ASSERT_EQ_I(groups[3].index[0], 3);   /* cam4 */
	ASSERT_EQ_I(groups[3].index[1], 5);   /* cam6 */
}

static void test_grid_cell_outside_grid_is_error(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\ncam1|rtsp://x|1\ncam2|rtsp://x|5\n"), -1);
	ASSERT(strstr(err, "line 3:") != NULL);
	ASSERT(strstr(err, "outside the 2x2 grid") != NULL);

	ASSERT_EQ_I(parse(&cfg, "GRID=3x3\ncam1|rtsp://x|0\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);

	int x, y, w, h;
	ASSERT_EQ_I(layout_grid_cell(2, 2, 5, 1920, 1080, &x, &y, &w, &h), -1);
	ASSERT_EQ_I(layout_grid_cell(2, 2, 0, 1920, 1080, &x, &y, &w, &h), -1);
}

static void test_grid_key_after_cameras_still_counts(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|2\nGRID=2x1\n"), 0);
	ASSERT_EQ_I(layout_apply(&cfg, 1920, 1080, NULL, NULL, err, sizeof err), 0);
	ASSERT_EQ_I(cfg.cam[0].x, 960);
	ASSERT_EQ_I(cfg.cam[0].width, 960);
	ASSERT_EQ_I(cfg.cam[0].height, 1080);
}

static void test_mixed_modes_are_errors(void)
{
	struct layout_config cfg;

	/* manual line in a GRID config */
	ASSERT_EQ_I(parse(&cfg,
		"GRID=2x2\n"
		"cam1|rtsp://x|1\n"
		"cam2|rtsp://x|960|540|960|0\n"), -1);
	ASSERT(strstr(err, "line 3:") != NULL);
	ASSERT(strstr(err, "cannot be mixed") != NULL);

	/* grid line among manual lines, no GRID */
	ASSERT_EQ_I(parse(&cfg,
		"cam1|rtsp://x|960|540|0|0\n"
		"cam2|rtsp://x|2\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
	ASSERT(strstr(err, "cannot be mixed") != NULL);

	/* grid lines only, but GRID missing */
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|1\n"), -1);
	ASSERT(strstr(err, "require GRID") != NULL);
}

static void test_manual_tile_outside_screen_warns(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|960|540\n"), 0);
	warnings = 0;
	ASSERT_EQ_I(layout_apply(&cfg, 1280, 720, count_warning, NULL, err, sizeof err), 0);
	ASSERT_EQ_I(warnings, 1);
	warnings = 0;
	ASSERT_EQ_I(layout_apply(&cfg, 1920, 1080, count_warning, NULL, err, sizeof err), 0);
	ASSERT_EQ_I(warnings, 0);
}

static void test_parse_size(void)
{
	int w, h;
	ASSERT_EQ_I(layout_parse_size("1280x720", &w, &h), 0);
	ASSERT_EQ_I(w, 1280);
	ASSERT_EQ_I(h, 720);
	ASSERT_EQ_I(layout_parse_size("1920X1080", &w, &h), 0);
	ASSERT_EQ_I(layout_parse_size("1920", &w, &h), -1);
	ASSERT_EQ_I(layout_parse_size("0x1080", &w, &h), -1);
	ASSERT_EQ_I(layout_parse_size("axb", &w, &h), -1);
}

/* -------------------------------------------------------------- URL masking */

static void expect_mask(const char *in, const char *want)
{
	char out[512];
	layout_mask_url(in, out, sizeof out);
	test_checks++;
	if (strcmp(out, want)) {
		fprintf(stderr, "FAIL mask_url(\"%s\") = \"%s\", want \"%s\"\n", in, out, want);
		test_failures++;
	}
}

static void test_mask_url_userinfo(void)
{
	expect_mask("rtsp://admin:s3cret@192.168.1.10:554/stream1",
		    "rtsp://admin:***@192.168.1.10:554/stream1");
	/* password containing '@' — the LAST '@' in the authority counts */
	expect_mask("rtsp://user:p@ss@host/path", "rtsp://user:***@host/path");
	/* '@' in the path is not userinfo */
	expect_mask("rtsp://host/a:b@c", "rtsp://host/a:b@c");
	/* no credentials / user only: unchanged */
	expect_mask("rtsp://192.168.1.10:554/stream1", "rtsp://192.168.1.10:554/stream1");
	expect_mask("rtsp://user@host/x", "rtsp://user@host/x");
}

static void test_mask_url_unifi(void)
{
	/* UniFi Protect: the 16-character alphanumeric path segment IS the
	 * credential. Also without any digit (a random token may lack one). */
	expect_mask("rtsp://192.168.1.1:7447/aB3dE5fG7hJ9kL1m",
		    "rtsp://192.168.1.1:7447/***");
	expect_mask("rtsp://192.168.1.1:7447/AbCdEfGhIjKlMnOp",
		    "rtsp://192.168.1.1:7447/***");
	expect_mask("rtsps://192.168.1.1:7441/aB3dE5fG7hJ9kL1m?enableSrtp",
		    "rtsps://192.168.1.1:7441/***?***");
}

static void test_mask_url_common_cameras_untouched_paths(void)
{
	/* Reolink */
	expect_mask("rtsp://admin:pw@192.168.1.20:554/h264Preview_01_main",
		    "rtsp://admin:***@192.168.1.20:554/h264Preview_01_main");
	/* Hikvision */
	expect_mask("rtsp://admin:pw@192.168.1.21:554/Streaming/Channels/101",
		    "rtsp://admin:***@192.168.1.21:554/Streaming/Channels/101");
	/* Dahua: the query string is masked wholesale */
	expect_mask("rtsp://admin:pw@192.168.1.22:554/cam/realmonitor?channel=1&subtype=0",
		    "rtsp://admin:***@192.168.1.22:554/cam/realmonitor?***");
	/* go2rtc */
	expect_mask("rtsp://192.168.1.5:8554/front_door", "rtsp://192.168.1.5:8554/front_door");
	expect_mask("rtsp://192.168.1.5:8554/front_door?video=h264",
		    "rtsp://192.168.1.5:8554/front_door?***");
}

static void test_mask_url_long_tokens_with_separators(void)
{
	/* >= 32 characters of [A-Za-z0-9_-]: a long base64url-style token */
	expect_mask("rtsp://host/live/Zm9vYmFyYmF6cXV4LXF1dXgtY29yZ2VfZ3JhdWx0",
		    "rtsp://host/live/***");
	/* 15 alphanumerics: too short to be treated as a secret */
	expect_mask("rtsp://host/abcdefghijklmn1", "rtsp://host/abcdefghijklmn1");
	/* human-readable name with separators, under 32 characters */
	expect_mask("rtsp://host/back-garden-camera-main", "rtsp://host/back-garden-camera-main");
}

static void test_mask_url_truncation(void)
{
	char out[10];
	layout_mask_url("rtsp://admin:s3cret@host/x", out, sizeof out);
	ASSERT_EQ_I(strlen(out), 9);
}

static void test_mask_urls_in_text(void)
{
	char out[512];
	layout_mask_urls_in_text(
		"Connection to rtsp://u:pw@10.0.0.1:7447/aB3dE5fG7hJ9kL1m failed: 401\n",
		out, sizeof out);
	ASSERT(strcmp(out, "Connection to rtsp://u:***@10.0.0.1:7447/*** failed: 401\n") == 0);

	layout_mask_urls_in_text("'rtsp://h/cam/realmonitor?channel=1' and tcp://h:554?timeout=5",
				 out, sizeof out);
	ASSERT(strcmp(out, "'rtsp://h/cam/realmonitor?***' and tcp://h:554?***") == 0);

	layout_mask_urls_in_text("no url here", out, sizeof out);
	ASSERT(strcmp(out, "no url here") == 0);
}

/* ---------------------------------------------- QA: added edge-case coverage */

static void test_mask_url_edge_cases(void)
{
	/* IPv6 literal host: the ':' inside [...] must not be taken as the
	 * userinfo separator, and the password is still masked. */
	expect_mask("rtsp://user:pw@[::1]:554/stream", "rtsp://user:***@[::1]:554/stream");
	expect_mask("rtsp://[fe80::1]:554/stream1", "rtsp://[fe80::1]:554/stream1");
	expect_mask("rtsp://[::1]:7447/aB3dE5fG7hJ9kL1m", "rtsp://[::1]:7447/***");
	/* no path at all */
	expect_mask("rtsp://user:pw@host", "rtsp://user:***@host");
	expect_mask("rtsp://user:pw@host:554", "rtsp://user:***@host:554");
	/* query directly after the authority */
	expect_mask("rtsp://user:pw@host?x=1", "rtsp://user:***@host?***");
	/* empty user */
	expect_mask("rtsp://:pw@host/x", "rtsp://:***@host/x");
	/* fragment */
	expect_mask("rtsp://user:pw@host/x#frag", "rtsp://user:***@host/x#***");
	/* token segment followed by a trailing slash */
	expect_mask("rtsp://host/aB3dE5fG7hJ9kL1m/", "rtsp://host/***/");
	/* empty input */
	expect_mask("", "");
}

static void test_mask_url_truncation_never_leaks(void)
{
	/* Whatever the output size, the result is a prefix of the full masked
	 * URL and never contains any part of the secret. */
	const char *url = "rtsp://admin:s3cretPW@192.168.1.10:7447/aB3dE5fG7hJ9kL1m?token=xyz";
	char full[512];
	layout_mask_url(url, full, sizeof full);
	for (size_t n = 1; n <= strlen(full) + 1; n++) {
		char out[512];
		memset(out, 'Z', sizeof out);
		layout_mask_url(url, out, n);
		ASSERT(strlen(out) == n - 1 || strlen(out) == strlen(full));
		ASSERT(strncmp(out, full, strlen(out)) == 0);
		ASSERT(strstr(out, "s3c") == NULL);
		ASSERT(strstr(out, "aB3") == NULL);
		ASSERT(strstr(out, "xyz") == NULL);
		ASSERT(out[n] == 'Z' || n == sizeof out);   /* nothing written past outlen */
	}
	/* same for the free-text variant */
	for (size_t n = 1; n < 80; n++) {
		char out[128];
		memset(out, 'Z', sizeof out);
		layout_mask_urls_in_text("err: rtsp://u:s3cret@h/x", out, n);
		ASSERT(strlen(out) < n);
		ASSERT(strstr(out, "s3c") == NULL);
		ASSERT(out[n] == 'Z');
	}
}

/* Regression (QA, fixed): a token followed by punctuation that commonly ends a URL in a
 * log sentence (':' before an error message, ',' or '.') is not masked,
 * because the whole run up to whitespace is taken as the URL and the last
 * path segment then contains a non-token character. */
static void test_mask_urls_in_text_token_before_punctuation(void)
{
	char out[512];

	layout_mask_urls_in_text("rtsp://h:7447/aB3dE5fG7hJ9kL1m: Connection refused",
				 out, sizeof out);
	ASSERT(strstr(out, "aB3dE5fG7hJ9kL1m") == NULL);

	layout_mask_urls_in_text("url=rtsp://h:7447/aB3dE5fG7hJ9kL1m, retrying", out, sizeof out);
	ASSERT(strstr(out, "aB3dE5fG7hJ9kL1m") == NULL);

	layout_mask_urls_in_text("failed to open rtsp://h:7447/aB3dE5fG7hJ9kL1m.", out, sizeof out);
	ASSERT(strstr(out, "aB3dE5fG7hJ9kL1m") == NULL);
}

static void test_crlf_whole_file(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg,
		"# edited on Windows\r\n"
		"GRID=2x2\r\n"
		"BUFFER_MS=120\r\n"
		"cam1|rtsp://x/1|1\r\n"
		"cam2|rtsp://x/2|2|40\r\n"), 0);
	ASSERT_EQ_I(cfg.count, 2);
	ASSERT_EQ_I(cfg.buffer_ms, 120);
	ASSERT_EQ_I(cfg.cam[1].delay_ms, 40);
	ASSERT(strcmp(cfg.cam[1].url, "rtsp://x/2") == 0);
	ASSERT_EQ_I(warnings, 0);

	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0\r\n"), 0);
	ASSERT_EQ_I(cfg.cam[0].y, 0);
}

/* Regression (QA, fixed): a UTF-8 BOM (Windows Notepad "UTF-8 with BOM") on the first line
 * turns the first key into "\xEF\xBB\xBFBUFFER_MS" — an unknown-key warning
 * and the setting is silently ignored; a BOM before a '#' comment is a hard
 * error; a BOM before a camera line ends up in the camera name. */
static void test_utf8_bom_is_ignored(void)
{
	struct layout_config cfg;

	ASSERT_EQ_I(parse(&cfg, "\xEF\xBB\xBF" "BUFFER_MS=120\ncam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT_EQ_I(cfg.buffer_ms, 120);
	ASSERT_EQ_I(warnings, 0);

	ASSERT_EQ_I(parse(&cfg, "\xEF\xBB\xBF" "# comment\ncam1|rtsp://x|960|540|0|0\n"), 0);

	ASSERT_EQ_I(parse(&cfg, "\xEF\xBB\xBF" "cam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT(strcmp(cfg.cam[0].name, "cam1") == 0);
}

/* Regression (QA, fixed): a URL longer than LAYOUT_URL_MAX-1 is silently truncated and
 * accepted, so the program connects to a different URL than configured —
 * contrary to "strict parsing: an error instead of a silent skip". Same for
 * DECODER/DRM_DEVICE longer than LAYOUT_PATH_MAX-1. */
static void test_overlong_url_and_paths_are_errors(void)
{
	struct layout_config cfg;
	char text[LAYOUT_MAX_LINE];
	char url[700];

	memset(url, 'a', sizeof url);
	memcpy(url, "rtsp://h/", 9);
	url[600] = '\0';
	snprintf(text, sizeof text, "cam1|%s|960|540|0|0\n", url);
	ASSERT_EQ_I(parse(&cfg, text), -1);

	char path[400];
	memset(path, 'a', sizeof path);
	memcpy(path, "/dev/", 5);
	path[300] = '\0';
	snprintf(text, sizeof text, "DECODER=%s\ncam1|rtsp://x|960|540|0|0\n", path);
	ASSERT_EQ_I(parse(&cfg, text), -1);
}

static void test_line_length_boundary(void)
{
	struct layout_config cfg;
	static char text[4 * LAYOUT_MAX_LINE];
	/* exactly LAYOUT_MAX_LINE-1 characters before '\n': accepted */
	int n = snprintf(text, sizeof text, "# ");
	while (n < LAYOUT_MAX_LINE - 1)
		text[n++] = 'x';
	snprintf(text + n, sizeof text - (size_t)n, "\ncam1|rtsp://x|960|540|0|0\n");
	ASSERT_EQ_I(parse(&cfg, text), 0);

	/* one more: error with the line number */
	n = snprintf(text, sizeof text, "# ");
	while (n < LAYOUT_MAX_LINE)
		text[n++] = 'x';
	snprintf(text + n, sizeof text - (size_t)n, "\ncam1|rtsp://x|960|540|0|0\n");
	ASSERT_EQ_I(parse(&cfg, text), -1);
	ASSERT(strstr(err, "line 1:") != NULL);

	/* last line without a trailing newline */
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0"), 0);
	ASSERT_EQ_I(cfg.count, 1);
}

static void test_field_count_limits(void)
{
	struct layout_config cfg;
	/* 8 fields: too many */
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0|0|9\n"), -1);
	ASSERT(strstr(err, "too many fields") != NULL);
	/* 5 fields (3 numbers): neither format */
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|1|2|3\n"), -1);
	/* empty name / empty url */
	ASSERT_EQ_I(parse(&cfg, "|rtsp://x|960|540|0|0\n"), -1);
	ASSERT_EQ_I(parse(&cfg, "cam1||960|540|0|0\n"), -1);
	/* whitespace around fields (README pads names for alignment) */
	ASSERT_EQ_I(parse(&cfg, "driveway  | rtsp://x/1 | 960 | 540 | 0 | 0 \n"), 0);
	ASSERT(strcmp(cfg.cam[0].name, "driveway") == 0);
	ASSERT(strcmp(cfg.cam[0].url, "rtsp://x/1") == 0);
	/* integer overflow in a numeric field */
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|99999999999999999999|540|0|0\n"), -1);
	/* negative tile position */
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|-1|0\n"), -1);
}

static void test_sixteen_cameras_accepted(void)
{
	struct layout_config cfg;
	char text[4096] = "GRID=4x4\n";
	for (int i = 0; i < LAYOUT_MAX_CAMERAS; i++) {
		char line[64];
		snprintf(line, sizeof line, "cam%d|rtsp://x|%d\n", i + 1, i + 1);
		strcat(text, line);
	}
	ASSERT_EQ_I(parse(&cfg, text), 0);
	ASSERT_EQ_I(cfg.count, LAYOUT_MAX_CAMERAS);
	ASSERT_EQ_I(layout_apply(&cfg, 1920, 1080, NULL, NULL, err, sizeof err), 0);
	ASSERT_EQ_I(cfg.cam[15].x, 1440);
	ASSERT_EQ_I(cfg.cam[15].y, 810);
	ASSERT_EQ_I(cfg.cam[15].width, 480);
	ASSERT_EQ_I(cfg.cam[15].height, 270);
}

static void test_parse_size_edges(void)
{
	int w = -1, h = -1;
	ASSERT_EQ_I(layout_parse_size(" 1920 x 1080 ", &w, &h), 0);
	ASSERT_EQ_I(w, 1920);
	ASSERT_EQ_I(h, 1080);
	ASSERT_EQ_I(layout_parse_size("1920x1080x", &w, &h), -1);
	ASSERT_EQ_I(layout_parse_size("x1080", &w, &h), -1);
	ASSERT_EQ_I(layout_parse_size("-1920x1080", &w, &h), -1);
	ASSERT_EQ_I(layout_parse_size("", &w, &h), -1);
	ASSERT_EQ_I(layout_parse_size("99999999999x1", &w, &h), -1);
}

/* -------------------------------------------------------- DRM device choice */

static void test_pick_drm_device(void)
{
	/* card0 = render-only 3D engine (no connectors), card1 = display
	 * controller with a connected HDMI */
	struct drm_candidate pi4[2] = {
		{ .usable = false },
		{ .usable = true, .has_connector = true, .has_connected = true },
	};
	ASSERT_EQ_I(layout_pick_drm_device(pi4, 2, false), 1);
	ASSERT_EQ_I(layout_pick_drm_device(pi4, 2, true), 1);

	/* two display devices: the first with a connected display wins */
	struct drm_candidate two[3] = {
		{ .usable = true, .has_connector = false, .has_connected = false },
		{ .usable = true, .has_connector = true,  .has_connected = true },
		{ .usable = true, .has_connector = false, .has_connected = true },
	};
	ASSERT_EQ_I(layout_pick_drm_device(two, 3, false), 1);
	/* CONNECTOR set: the first device that HAS that connector, connected or not */
	two[0].has_connector = true;
	ASSERT_EQ_I(layout_pick_drm_device(two, 3, true), 0);

	/* nothing connected, no CONNECTOR: first usable device, so that the
	 * display search can list its connectors in the error */
	struct drm_candidate idle[2] = {
		{ .usable = false },
		{ .usable = true },
	};
	ASSERT_EQ_I(layout_pick_drm_device(idle, 2, false), 1);
	/* CONNECTOR set but no device has it */
	ASSERT_EQ_I(layout_pick_drm_device(idle, 2, true), -1);
	/* nothing usable at all */
	ASSERT_EQ_I(layout_pick_drm_device(idle, 1, false), -1);
	ASSERT_EQ_I(layout_pick_drm_device(NULL, 0, false), -1);
}

static void test_drm_device_key(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT(strcmp(cfg.drm_device, "") == 0);   /* default: auto */
	ASSERT_EQ_I(parse(&cfg, "DRM_DEVICE=/dev/dri/card0\ncam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT(strcmp(cfg.drm_device, "/dev/dri/card0") == 0);
	ASSERT_EQ_I(parse(&cfg, "DRM_DEVICE=auto\ncam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT(strcmp(cfg.drm_device, "") == 0);
	ASSERT_EQ_I(warnings, 0);
}

/* ---------------------------------------------------- follow-up QA fixes */

static void test_mask_urls_in_text_second_url_in_same_run(void)
{
	char out[512];
	layout_mask_urls_in_text("rtsp://u:pw@h/x;rtsp://a:b@c/y", out, sizeof out);
	ASSERT(strcmp(out, "rtsp://u:***@h/x;rtsp://a:***@c/y") == 0);

	/* punctuation after userinfo-only URLs, and a quoted URL */
	layout_mask_urls_in_text("open 'rtsp://a:b@h:554/s1': 401.", out, sizeof out);
	ASSERT(strcmp(out, "open 'rtsp://a:***@h:554/s1': 401.") == 0);

	/* the trimmed punctuation stays in the output */
	layout_mask_urls_in_text("see rtsp://h/aB3dE5fG7hJ9kL1m, then", out, sizeof out);
	ASSERT(strcmp(out, "see rtsp://h/***, then") == 0);
}

static void test_sanitize_log_text(void)
{
	char s[] = "red \x1b[31mX\x1b[0m\tok\rfake\x7f";
	layout_sanitize_log_text(s);
	ASSERT(strcmp(s, "red ?[31mX?[0m\tok?fake?") == 0);
}

static void test_delay_range(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0|-10000\n"), 0);
	ASSERT_EQ_I(cfg.cam[0].delay_ms, -10000);
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0|10000\n"), 0);
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0|10001\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\ncam1|rtsp://x|1|-10001\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
}

static void test_ffmpeg_loglevel_key(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "cam1|rtsp://x|960|540|0|0\n"), 0);
	ASSERT_EQ_I(cfg.ffmpeg_loglevel, LAYOUT_FFMPEG_LOG_ERROR);
	const char *names[] = { "quiet", "error", "warning", "info" };
	for (int i = 0; i < 4; i++) {
		char text[128];
		snprintf(text, sizeof text, "FFMPEG_LOGLEVEL=%s\ncam1|rtsp://x|960|540|0|0\n",
			 names[i]);
		ASSERT_EQ_I(parse(&cfg, text), 0);
		ASSERT_EQ_I(cfg.ffmpeg_loglevel, i);
		ASSERT(strcmp(layout_ffmpeg_log_name(cfg.ffmpeg_loglevel), names[i]) == 0);
	}
	ASSERT_EQ_I(parse(&cfg, "FFMPEG_LOGLEVEL=debug\ncam1|rtsp://x|960|540|0|0\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);
}

static void test_overlong_key_and_name_are_errors(void)
{
	struct layout_config cfg;
	char text[LAYOUT_MAX_LINE];
	char name[100];

	memset(name, 'n', sizeof name);
	name[LAYOUT_NAME_MAX] = '\0';   /* one longer than allowed */
	snprintf(text, sizeof text, "%s|rtsp://x|960|540|0|0\n", name);
	ASSERT_EQ_I(parse(&cfg, text), -1);
	ASSERT(strstr(err, "name longer") != NULL);

	ASSERT_EQ_I(parse(&cfg, "AN_EXTREMELY_LONG_KEY_NAME_THAT_GOES_ON=1\n"
			  "cam1|rtsp://x|960|540|0|0\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);

	/* exactly at the limit: accepted */
	char path[LAYOUT_PATH_MAX];
	memset(path, 'a', sizeof path);
	memcpy(path, "/dev/", 5);
	path[LAYOUT_PATH_MAX - 1] = '\0';
	snprintf(text, sizeof text, "DRM_DEVICE=%s\ncam1|rtsp://x|960|540|0|0\n", path);
	ASSERT_EQ_I(parse(&cfg, text), 0);
	ASSERT_EQ_I(strlen(cfg.drm_device), LAYOUT_PATH_MAX - 1);
}

int main(void)
{
	test_line_empty();
	test_line_global();
	test_line_camera_six_fields();
	test_line_camera_url_with_equals_is_camera();
	test_line_camera_empty_fields_kept();

	test_manual_six_and_seven_fields();
	test_manual_too_few_fields_is_error();
	test_defaults_and_globals();
	test_unknown_key_is_warning_not_error();
	test_bad_global_values_are_errors();
	test_non_integer_field_is_error();
	test_no_cameras_is_error();
	test_too_many_cameras_is_error();

	test_grid_2x2_on_1080p();
	test_grid_3x3_on_1080p();
	test_grid_uneven_division_covers_screen();
	test_grid_720p();
	test_grid_shared_cell_forms_rotation_group();
	test_grid_cell_outside_grid_is_error();
	test_grid_key_after_cameras_still_counts();
	test_mixed_modes_are_errors();
	test_manual_tile_outside_screen_warns();
	test_parse_size();

	test_mask_url_userinfo();
	test_mask_url_unifi();
	test_mask_url_common_cameras_untouched_paths();
	test_mask_url_long_tokens_with_separators();
	test_mask_url_truncation();
	test_mask_urls_in_text();

	test_pick_drm_device();
	test_drm_device_key();

	/* QA additions */
	test_mask_url_edge_cases();
	test_mask_url_truncation_never_leaks();
	test_mask_urls_in_text_token_before_punctuation();
	test_crlf_whole_file();
	test_utf8_bom_is_ignored();
	test_overlong_url_and_paths_are_errors();
	test_line_length_boundary();
	test_field_count_limits();
	test_sixteen_cameras_accepted();
	test_parse_size_edges();

	test_mask_urls_in_text_second_url_in_same_run();
	test_sanitize_log_text();
	test_delay_range();
	test_ffmpeg_loglevel_key();
	test_overlong_key_and_name_are_errors();

	return test_summary("test_layout");
}
