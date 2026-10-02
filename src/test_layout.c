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
	expect_mask("rtsp://admin:fakepw@192.168.1.10:554/stream1",
		    "rtsp://admin:***@192.168.1.10:554/stream1");
	/* password containing '@' — the LAST '@' in the authority counts */
	expect_mask("rtsp://user:ex@mple@host/path", "rtsp://user:***@host/path");
	/* '@' in the path is not userinfo */
	expect_mask("rtsp://host/a:b@c", "rtsp://host/a:b@c");
	/* no credentials / user only: unchanged */
	expect_mask("rtsp://192.168.1.10:554/stream1", "rtsp://192.168.1.10:554/stream1");
	/* userinfo without a colon may itself be the secret (a bare token
	 * or password): masked as a whole */
	expect_mask("rtsp://user@host/x", "rtsp://***@host/x");
}

static void test_mask_url_unifi(void)
{
	/* UniFi Protect: the 16-character alphanumeric path segment IS the
	 * credential. Also without any digit (a random token may lack one). */
	expect_mask("rtsp://192.168.1.1:7447/EXAMPLEtoken1234",
		    "rtsp://192.168.1.1:7447/***");
	expect_mask("rtsp://192.168.1.1:7447/AbCdEfGhIjKlMnOp",
		    "rtsp://192.168.1.1:7447/***");
	expect_mask("rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp",
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
	layout_mask_url("rtsp://admin:fakepw@host/x", out, sizeof out);
	ASSERT_EQ_I(strlen(out), 9);
}

static void test_mask_urls_in_text(void)
{
	char out[512];
	layout_mask_urls_in_text(
		"Connection to rtsp://u:pw@10.0.0.1:7447/EXAMPLEtoken1234 failed: 401\n",
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
	expect_mask("rtsp://[::1]:7447/EXAMPLEtoken1234", "rtsp://[::1]:7447/***");
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
	expect_mask("rtsp://host/EXAMPLEtoken1234/", "rtsp://host/***/");
	/* empty input */
	expect_mask("", "");
}

static void test_mask_url_truncation_never_leaks(void)
{
	/* Whatever the output size, the result is a prefix of the full masked
	 * URL and never contains any part of the secret. */
	const char *url = "rtsp://admin:fakepw@192.168.1.10:7447/EXAMPLEtoken1234?token=xyz";
	char full[512];
	layout_mask_url(url, full, sizeof full);
	for (size_t n = 1; n <= strlen(full) + 1; n++) {
		char out[512];
		memset(out, 'Z', sizeof out);
		layout_mask_url(url, out, n);
		ASSERT(strlen(out) == n - 1 || strlen(out) == strlen(full));
		ASSERT(strncmp(out, full, strlen(out)) == 0);
		ASSERT(strstr(out, "fak") == NULL);
		ASSERT(strstr(out, "EXA") == NULL);
		ASSERT(strstr(out, "xyz") == NULL);
		ASSERT(out[n] == 'Z' || n == sizeof out);   /* nothing written past outlen */
	}
	/* same for the free-text variant */
	for (size_t n = 1; n < 80; n++) {
		char out[128];
		memset(out, 'Z', sizeof out);
		layout_mask_urls_in_text("err: rtsp://u:fakepw@h/x", out, n);
		ASSERT(strlen(out) < n);
		ASSERT(strstr(out, "fak") == NULL);
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

	layout_mask_urls_in_text("rtsp://h:7447/EXAMPLEtoken1234: Connection refused",
				 out, sizeof out);
	ASSERT(strstr(out, "EXAMPLEtoken1234") == NULL);

	layout_mask_urls_in_text("url=rtsp://h:7447/EXAMPLEtoken1234, retrying", out, sizeof out);
	ASSERT(strstr(out, "EXAMPLEtoken1234") == NULL);

	layout_mask_urls_in_text("failed to open rtsp://h:7447/EXAMPLEtoken1234.", out, sizeof out);
	ASSERT(strstr(out, "EXAMPLEtoken1234") == NULL);
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
	layout_mask_urls_in_text("see rtsp://h/EXAMPLEtoken1234, then", out, sizeof out);
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

/* ------------------------------------------------ MMP-2: safe defaults */

static int parse_strict(struct layout_config *cfg, const char *text)
{
	warnings = 0;
	last_warning[0] = '\0';
	err[0] = '\0';
	return layout_parse_flags(cfg, text, LAYOUT_PARSE_STRICT, count_warning, NULL,
				  err, sizeof err);
}

static void test_no_cameras_configured_message(void)
{
	struct layout_config cfg;
	/* the shipped example: everything commented out, GRID active */
	ASSERT_EQ_I(parse(&cfg,
		"GRID=2x2\n"
		"#front-door|rtsp://viewer:CHANGE_ME@192.168.1.10:554/stream1|1\n"), -1);
	ASSERT(strstr(err, "no cameras configured") != NULL);
	ASSERT(strncmp(err, "line ", 5) != 0);   /* file-level, no line number */
}

static void test_placeholder_change_me_is_error(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg,
		"GRID=2x2\n"
		"# comment with CHANGE_ME is fine\n"
		"front|rtsp://viewer:CHANGE_ME@192.168.1.10/stream1|1\n"), -1);
	ASSERT(strstr(err, "line 3:") != NULL);
	ASSERT(strstr(err, "CHANGE_ME") != NULL);
	ASSERT(strstr(err, "placeholder") != NULL);
	/* the password must not be echoed beyond the placeholder itself */
	ASSERT(strstr(err, "192.168.1.10") == NULL);

	/* case-insensitive */
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\nfront|rtsp://u:change_me@h/s|1\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
	/* strict mode too */
	ASSERT_EQ_I(parse_strict(&cfg, "GRID=2x2\nfront|rtsp://u:CHANGE_ME@h/s|1\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
}

static void test_placeholder_angle_brackets_is_error(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg,
		"GRID=2x2\n"
		"front|rtsp://<user>:<password>@<camera-ip>/stream1|1\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
	/* a URL field is never echoed, not even the placeholder text */
	ASSERT(strstr(err, "<user>") == NULL);
	ASSERT(strstr(err, "placeholder in the URL of camera front") != NULL);

	ASSERT_EQ_I(parse(&cfg, "<name>|rtsp://u:p@h/s|1\nGRID=2x2\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);

	/* in a global value too */
	ASSERT_EQ_I(parse(&cfg, "CONNECTOR=<connector>\nGRID=1x1\nc|rtsp://h/s|1\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);

	/* a lone '<' or '>' is not a placeholder (not valid in a URL anyway,
	 * but we only reject the <...> pattern) */
	ASSERT(!layout_has_placeholder("rtsp://h/a>b<c"));
	ASSERT(layout_has_placeholder("rtsp://h/<x>"));
	ASSERT(!layout_has_placeholder("rtsp://h/<>"));
	ASSERT(layout_has_placeholder("CHANGE_ME"));
	ASSERT(!layout_has_placeholder("rtsp://viewer:fakepw@h/stream1"));
}

static void test_unknown_key_strict_is_error_with_suggestion(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse_strict(&cfg, "conector=HDMI-A-1\nGRID=1x1\nc|rtsp://h/s|1\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);
	ASSERT(strstr(err, "conector") != NULL);
	ASSERT(strstr(err, "did you mean CONNECTOR?") != NULL);

	/* far from every key: no suggestion, still an error */
	ASSERT_EQ_I(parse_strict(&cfg, "GRID=1x1\nSOMETHING=1\nc|rtsp://h/s|1\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
	ASSERT(strstr(err, "did you mean") == NULL);

	/* daemon (non-strict): a warning with the same suggestion, no error */
	ASSERT_EQ_I(parse(&cfg, "BUFER_MS=100\nGRID=1x1\nc|rtsp://h/s|1\n"), 0);
	ASSERT_EQ_I(warnings, 1);
	ASSERT(strstr(last_warning, "did you mean BUFFER_MS?") != NULL);
	ASSERT_EQ_I(cfg.buffer_ms, LAYOUT_DEFAULT_BUFFER_MS);
}

static void test_suggest_key(void)
{
	ASSERT(layout_suggest_key("conector") && !strcmp(layout_suggest_key("conector"), "CONNECTOR"));
	ASSERT(layout_suggest_key("connector") && !strcmp(layout_suggest_key("connector"), "CONNECTOR"));
	ASSERT(layout_suggest_key("GRIDS") && !strcmp(layout_suggest_key("GRIDS"), "GRID"));
	ASSERT(layout_suggest_key("MODES") && !strcmp(layout_suggest_key("MODES"), "MODE"));
	ASSERT(layout_suggest_key("DECODR") && !strcmp(layout_suggest_key("DECODR"), "DECODER"));
	ASSERT(layout_suggest_key("ROTATE_SECOND") && !strcmp(layout_suggest_key("ROTATE_SECOND"), "ROTATE_SECONDS"));
	ASSERT(layout_suggest_key("UNIFI_REWITE") && !strcmp(layout_suggest_key("UNIFI_REWITE"), "UNIFI_REWRITE"));
	ASSERT(layout_suggest_key("SOMETHING") == NULL);
	ASSERT(layout_suggest_key("X") == NULL);
	ASSERT(layout_suggest_key("") == NULL);

	ASSERT_EQ_I(layout_levenshtein("kitten", "sitting"), 3);
	ASSERT_EQ_I(layout_levenshtein("", "abc"), 3);
	ASSERT_EQ_I(layout_levenshtein("abc", "ABC"), 0);   /* case-insensitive */
}

static void test_known_keys_are_accepted_strictly(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse_strict(&cfg,
		"BUFFER_MS=100\nROTATE_SECONDS=10\nGRID=1x1\nDECODER=/dev/video10\n"
		"DRM_DEVICE=auto\nCONNECTOR=HDMI-A-1\nFFMPEG_LOGLEVEL=error\n"
		"MODE=auto\nUNIFI_REWRITE=auto\nc|rtsp://h/s|1\n"), 0);
	ASSERT_EQ_I(warnings, 0);
}

static void test_pipe_in_password_hint(void)
{
	struct layout_config cfg;
	/* grid line, password "pa|ss" */
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\ncam|rtsp://user:pa|ss@10.0.0.1/s|1\n"), -1);
	ASSERT(strstr(err, "line 2:") != NULL);
	ASSERT(strstr(err, "%7C") != NULL);

	/* manual line, so many pipes the field count overflows */
	ASSERT_EQ_I(parse(&cfg, "cam|rtsp://u:a|b|c|d@h/s|960|540|0|0\n"), -1);
	ASSERT(strstr(err, "%7C") != NULL);

	/* no '@' after the break: plain non-integer error without the hint */
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\ncam|rtsp://h/s|one\n"), -1);
	ASSERT(strstr(err, "%7C") == NULL);

	/* %7C itself is accepted and kept verbatim */
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\ncam|rtsp://user:pa%7Css@10.0.0.1/s|1\n"), 0);
	ASSERT(strcmp(cfg.cam[0].url, "rtsp://user:pa%7Css@10.0.0.1/s") == 0);
}

/* ---------------------------------------------------------- MMP-5: MODE */

static void test_parse_mode_key(void)
{
	int w, h, mhz;
	ASSERT_EQ_I(layout_parse_mode("auto", &w, &h, &mhz), 0);
	ASSERT_EQ_I(w, 0); ASSERT_EQ_I(h, 0); ASSERT_EQ_I(mhz, 0);
	ASSERT_EQ_I(layout_parse_mode("1920x1080", &w, &h, &mhz), 0);
	ASSERT_EQ_I(w, 1920); ASSERT_EQ_I(h, 1080); ASSERT_EQ_I(mhz, 0);
	ASSERT_EQ_I(layout_parse_mode("1280x720@50", &w, &h, &mhz), 0);
	ASSERT_EQ_I(w, 1280); ASSERT_EQ_I(h, 720); ASSERT_EQ_I(mhz, 50000);
	ASSERT_EQ_I(layout_parse_mode("1920x1080@59.94", &w, &h, &mhz), 0);
	ASSERT_EQ_I(mhz, 59940);
	ASSERT_EQ_I(layout_parse_mode("1920x1080@", &w, &h, &mhz), -1);
	ASSERT_EQ_I(layout_parse_mode("1920x1080@0", &w, &h, &mhz), -1);
	ASSERT_EQ_I(layout_parse_mode("1920x1080@abc", &w, &h, &mhz), -1);
	ASSERT_EQ_I(layout_parse_mode("1920", &w, &h, &mhz), -1);
	ASSERT_EQ_I(layout_parse_mode("", &w, &h, &mhz), -1);
	ASSERT_EQ_I(layout_parse_mode("99999x1", &w, &h, &mhz), -1);

	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\nc|rtsp://h/s|1\n"), 0);
	ASSERT_EQ_I(cfg.mode_w, 0);   /* default auto */
	ASSERT_EQ_I(parse(&cfg, "MODE=1280x720@50\nGRID=1x1\nc|rtsp://h/s|1\n"), 0);
	ASSERT_EQ_I(cfg.mode_w, 1280);
	ASSERT_EQ_I(cfg.mode_h, 720);
	ASSERT_EQ_I(cfg.mode_mhz, 50000);
	ASSERT_EQ_I(parse(&cfg, "MODE=big\nGRID=1x1\nc|rtsp://h/s|1\n"), -1);
	ASSERT(strstr(err, "line 1:") != NULL);
	ASSERT(strstr(err, "MODE") != NULL);
}

#define M(w, h, hz, pref) { (w), (h), (int)((hz) * 1000), false, (pref) }
#define MI(w, h, hz) { (w), (h), (int)((hz) * 1000), true, false }

static void test_select_mode_4k30_tv(void)
{
	/* a typical 4K TV on a Pi 4 without hdmi_enable_4kp60 */
	const struct layout_mode m[] = {
		M(3840, 2160, 30, true), M(4096, 2160, 24, false), M(3840, 2160, 25, false),
		MI(1920, 1080, 60), M(1920, 1080, 120, false), M(1920, 1080, 50, false),
		M(1920, 1080, 60, false), M(1280, 720, 60, false), M(720, 576, 50, false),
	};
	enum layout_mode_reason why;
	int i = layout_select_mode(m, 9, 0, 0, 0, &why);
	ASSERT_EQ_I(i, 6);   /* 1920x1080@60: not interlaced, not 120 */
	ASSERT_EQ_I(why, LAYOUT_MODE_AUTO_LOW_REFRESH);
}

static void test_select_mode_4k60(void)
{
	const struct layout_mode m[] = {
		M(3840, 2160, 60, true), M(3840, 2160, 30, false), M(2560, 1440, 60, false),
		M(1920, 1080, 60, false), M(1920, 1080, 59.94, false), M(1280, 720, 60, false),
	};
	enum layout_mode_reason why;
	ASSERT_EQ_I(layout_select_mode(m, 6, 0, 0, 0, &why), 3);
	ASSERT_EQ_I(why, LAYOUT_MODE_AUTO_4K);
}

static void test_select_mode_keeps_ultrawide_and_1440p(void)
{
	const struct layout_mode uw[] = {
		M(2560, 1080, 60, true), M(1920, 1080, 60, false), M(1280, 720, 60, false),
	};
	const struct layout_mode qhd[] = {
		M(2560, 1440, 60, true), M(1920, 1080, 60, false),
	};
	enum layout_mode_reason why;
	ASSERT_EQ_I(layout_select_mode(uw, 3, 0, 0, 0, &why), 0);
	ASSERT_EQ_I(why, LAYOUT_MODE_PREFERRED);
	ASSERT_EQ_I(layout_select_mode(qhd, 2, 0, 0, 0, &why), 0);
	ASSERT_EQ_I(why, LAYOUT_MODE_PREFERRED);
}

static void test_select_mode_keeps_1080p50_and_720p(void)
{
	const struct layout_mode p50[] = {
		M(1920, 1080, 50, true), M(1920, 1080, 60, false),
	};
	const struct layout_mode p720[] = {
		M(1280, 720, 60, true), M(1024, 768, 60, false),
	};
	enum layout_mode_reason why;
	ASSERT_EQ_I(layout_select_mode(p50, 2, 0, 0, 0, &why), 0);
	ASSERT_EQ_I(layout_select_mode(p720, 2, 0, 0, 0, &why), 0);
	ASSERT_EQ_I(why, LAYOUT_MODE_PREFERRED);
}

static void test_select_mode_preferred_flag_and_interlaced(void)
{
	/* preferred is not first in the list */
	const struct layout_mode a[] = {
		M(1280, 720, 60, false), M(1920, 1080, 60, true),
	};
	/* preferred interlaced: skip to the first progressive mode */
	const struct layout_mode b[] = {
		MI(1920, 1080, 60), M(1920, 1080, 30, false), M(1280, 720, 60, false),
	};
	/* no preferred flag at all: first mode counts as preferred */
	const struct layout_mode c[] = {
		M(1680, 1050, 60, false), M(1280, 1024, 60, false),
	};
	const struct layout_mode only_i[] = { MI(1920, 1080, 60) };
	enum layout_mode_reason why;
	ASSERT_EQ_I(layout_select_mode(a, 2, 0, 0, 0, &why), 1);
	/* b: base 1080p30 (< 50 Hz) -> highest refresh with the same aspect */
	ASSERT_EQ_I(layout_select_mode(b, 3, 0, 0, 0, &why), 2);
	ASSERT_EQ_I(why, LAYOUT_MODE_AUTO_LOW_REFRESH);
	ASSERT_EQ_I(layout_select_mode(c, 2, 0, 0, 0, &why), 0);
	ASSERT_EQ_I(layout_select_mode(only_i, 1, 0, 0, 0, &why), -1);
	ASSERT_EQ_I(layout_select_mode(c, 0, 0, 0, 0, &why), -1);
}

static void test_select_mode_4k_without_same_aspect_keeps_preferred(void)
{
	const struct layout_mode m[] = {
		M(4096, 2160, 60, true), M(1920, 1080, 60, false),
	};
	enum layout_mode_reason why;
	ASSERT_EQ_I(layout_select_mode(m, 2, 0, 0, 0, &why), 0);
	ASSERT_EQ_I(why, LAYOUT_MODE_PREFERRED);
}

static void test_select_mode_explicit(void)
{
	const struct layout_mode m[] = {
		M(3840, 2160, 30, true), M(1920, 1080, 60, false), M(1920, 1080, 59.94, false),
		M(1920, 1080, 50, false), M(1280, 720, 60, false), M(1280, 720, 50, false),
		MI(1920, 1080, 50),
	};
	enum layout_mode_reason why;
	ASSERT_EQ_I(layout_select_mode(m, 7, 1280, 720, 50000, &why), 5);
	ASSERT_EQ_I(why, LAYOUT_MODE_EXPLICIT);
	/* refresh tolerance: 59.94 asked, both 60 and 59.94 within 1 Hz,
	 * the closer one wins */
	ASSERT_EQ_I(layout_select_mode(m, 7, 1920, 1080, 59940, &why), 2);
	ASSERT_EQ_I(layout_select_mode(m, 7, 1920, 1080, 60000, &why), 1);
	/* no refresh given: highest refresh of that size, up to 60 Hz */
	ASSERT_EQ_I(layout_select_mode(m, 7, 1920, 1080, 0, &why), 1);
	/* the explicit choice is honoured even for 4K30 */
	ASSERT_EQ_I(layout_select_mode(m, 7, 3840, 2160, 0, &why), 0);
	/* not available */
	ASSERT_EQ_I(layout_select_mode(m, 7, 1280, 720, 30000, &why), -1);
	ASSERT_EQ_I(layout_select_mode(m, 7, 1366, 768, 0, &why), -1);
	/* interlaced never matches */
	const struct layout_mode i50[] = { M(1280, 720, 60, true), MI(1920, 1080, 50) };
	ASSERT_EQ_I(layout_select_mode(i50, 2, 1920, 1080, 50000, &why), -1);
}

/* ------------------------------------------------------- MMP-5: UniFi */

static void unifi(const char *in, int expect_r, const char *expect)
{
	char out[LAYOUT_URL_MAX];
	int r = layout_unifi_rewrite(in, out, sizeof out);
	ASSERT_EQ_I(r, expect_r);
	if (expect_r == 1 && strcmp(out, expect)) {
		fprintf(stderr, "FAIL unifi: \"%s\" -> \"%s\", expected \"%s\"\n", in, out, expect);
		test_failures++;
	}
}

static void test_unifi_rewrite(void)
{
	unifi("rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp", 1,
	      "rtsp://192.168.1.1:7447/EXAMPLEtoken1234");
	unifi("rtsps://nvr.local:7441/EXAMPLEtoken1234", 1,
	      "rtsp://nvr.local:7447/EXAMPLEtoken1234");
	unifi("RTSPS://NVR:7441/Tok?enableSrtp", 1, "rtsp://NVR:7447/Tok");
	unifi("rtsps://h:7441/T?enableSrtp&foo=1", 1, "rtsp://h:7447/T?foo=1");
	unifi("rtsps://h:7441/T?foo=1&enableSrtp=true", 1, "rtsp://h:7447/T?foo=1");
	unifi("rtsps://h:7441/T?a=1&enablesrtp&b=2", 1, "rtsp://h:7447/T?a=1&b=2");
	unifi("rtsps://u:p@h:7441/T?enableSrtp", 1, "rtsp://u:p@h:7447/T");
	unifi("rtsps://[fd00::1]:7441/T?enableSrtp", 1, "rtsp://[fd00::1]:7447/T");
	unifi("  rtsps://h:7441/T?enableSrtp", 0, NULL);   /* fields arrive trimmed */

	/* not UniFi Protect rtsps: untouched */
	unifi("rtsp://192.168.1.1:7447/EXAMPLEtoken1234", 0, NULL);
	unifi("rtsps://cam:322/stream1", 0, NULL);
	unifi("rtsps://cam/stream1", 0, NULL);
	unifi("rtsps://cam:74410/stream1", 0, NULL);
	unifi("rtsp://cam:7441/stream1?enableSrtp", 0, NULL);
	unifi("/usr/share/rtspwall/demo/cam1.mp4", 0, NULL);

	/* output buffer too small: -1, NUL-terminated */
	char small[8];
	ASSERT_EQ_I(layout_unifi_rewrite("rtsps://h:7441/T", small, sizeof small), -1);
	ASSERT(strlen(small) < sizeof small);
}

static void test_unifi_rewrite_in_config(void)
{
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg,
		"GRID=1x2\n"
		"door|rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp|1\n"
		"yard|rtsp://cam/stream1|2\n"), 0);
	/* default (tls): rtsps and port 7441 kept, only ?enableSrtp dropped */
	ASSERT_EQ_I(cfg.unifi_mode, LAYOUT_UNIFI_MODE_TLS);
	ASSERT(strcmp(cfg.cam[0].url, "rtsps://192.168.1.1:7441/EXAMPLEtoken1234") == 0);
	ASSERT_EQ_I(cfg.cam[0].unifi, LAYOUT_UNIFI_KEPT_TLS);
	ASSERT(!cfg.cam[0].unifi_rewritten);
	ASSERT_EQ_I(cfg.cam[1].unifi, LAYOUT_UNIFI_NOT_UNIFI);
	ASSERT(!cfg.cam[1].unifi_rewritten);

	/* plain: the old rewrite to rtsp:7447 */
	ASSERT_EQ_I(parse(&cfg,
		"GRID=1x2\nUNIFI_REWRITE=plain\n"
		"door|rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp|1\n"
		"yard|rtsp://cam/stream1|2\n"), 0);
	ASSERT_EQ_I(cfg.unifi_mode, LAYOUT_UNIFI_MODE_PLAIN);
	ASSERT(strcmp(cfg.cam[0].url, "rtsp://192.168.1.1:7447/EXAMPLEtoken1234") == 0);
	ASSERT_EQ_I(cfg.cam[0].unifi, LAYOUT_UNIFI_PLAIN);
	ASSERT(cfg.cam[0].unifi_rewritten);
	ASSERT(!cfg.cam[1].unifi_rewritten);

	/* auto: alias for tls (Phase A configs keep working, now encrypted) */
	ASSERT_EQ_I(parse(&cfg,
		"GRID=1x1\nUNIFI_REWRITE=auto\n"
		"door|rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp|1\n"), 0);
	ASSERT_EQ_I(cfg.unifi_mode, LAYOUT_UNIFI_MODE_TLS);
	ASSERT(strcmp(cfg.cam[0].url, "rtsps://192.168.1.1:7441/EXAMPLEtoken1234") == 0);

	/* UNIFI_REWRITE=off keeps the URL unchanged, even when set after
	 * the camera line */
	ASSERT_EQ_I(parse(&cfg,
		"GRID=1x1\n"
		"door|rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp|1\n"
		"UNIFI_REWRITE=off\n"), 0);
	ASSERT_EQ_I(cfg.unifi_mode, LAYOUT_UNIFI_MODE_OFF);
	ASSERT(strcmp(cfg.cam[0].url, "rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp") == 0);
	ASSERT(!cfg.cam[0].unifi_rewritten);
	ASSERT_EQ_I(cfg.cam[0].unifi, LAYOUT_UNIFI_OFF);

	ASSERT_EQ_I(parse(&cfg, "UNIFI_REWRITE=maybe\nGRID=1x1\nc|rtsp://h/s|1\n"), -1);
	ASSERT(strstr(err, "UNIFI_REWRITE") != NULL);
}

/* ------------------------------------------------ MMP-7: URL kinds */

static void test_url_is_live(void)
{
	/* every network URL is live: the file pacing branch is never taken */
	ASSERT(layout_url_is_live("rtsp://cam/stream1"));
	ASSERT(layout_url_is_live("rtsp://u:p@192.168.1.10:554/Streaming/Channels/102"));
	ASSERT(layout_url_is_live("RTSP://CAM/stream1"));
	ASSERT(layout_url_is_live("rtsps://nvr:7441/tok"));
	ASSERT(layout_url_is_live("http://cam/video.flv"));
	ASSERT(layout_url_is_live("srt://cam:9000"));
	ASSERT(layout_url_is_live("udp://239.0.0.1:1234"));
	/* local files */
	ASSERT(!layout_url_is_live("/usr/share/rtspwall/demo/cam1.mp4"));
	ASSERT(!layout_url_is_live("demo/cam1.mp4"));
	ASSERT(!layout_url_is_live("file:/usr/share/rtspwall/demo/cam1.mp4"));
	ASSERT(!layout_url_is_live("file:///usr/share/rtspwall/demo/cam1.mp4"));
	ASSERT(!layout_url_is_live("FILE:///x.mp4"));
	ASSERT(!layout_url_is_live("clip:with:colons.mp4"));
	ASSERT(!layout_url_is_live(""));

	ASSERT(layout_url_is_rtsp("rtsp://cam/s"));
	ASSERT(layout_url_is_rtsp("RTSPS://cam/s"));
	ASSERT(!layout_url_is_rtsp("http://cam/s"));
	ASSERT(!layout_url_is_rtsp("/x/rtsp://y"));
	ASSERT(!layout_url_is_rtsp("file:///x.mp4"));
}

/* ------------------------------------------------- MMP-3: sd_notify */

static void test_notify_sockaddr(void)
{
	char path[108];
	size_t len = 0;
	ASSERT_EQ_I(layout_notify_sockaddr("/run/systemd/notify", path, sizeof path, &len), 0);
	ASSERT(strcmp(path, "/run/systemd/notify") == 0);
	ASSERT_EQ_I(len, strlen("/run/systemd/notify"));

	/* abstract namespace: leading '@' becomes a NUL, length counts it */
	ASSERT_EQ_I(layout_notify_sockaddr("@/org/freedesktop/systemd1/notify/123", path,
					   sizeof path, &len), 0);
	ASSERT_EQ_I(path[0], '\0');
	ASSERT(memcmp(path + 1, "/org/freedesktop/systemd1/notify/123", len - 1) == 0);
	ASSERT_EQ_I(len, strlen("@/org/freedesktop/systemd1/notify/123"));

	ASSERT_EQ_I(layout_notify_sockaddr(NULL, path, sizeof path, &len), -1);
	ASSERT_EQ_I(layout_notify_sockaddr("", path, sizeof path, &len), -1);
	ASSERT_EQ_I(layout_notify_sockaddr("relative/path", path, sizeof path, &len), -1);
	ASSERT_EQ_I(layout_notify_sockaddr("@", path, sizeof path, &len), -1);
	char longp[200];
	memset(longp, 'a', sizeof longp - 1);
	longp[0] = '/';
	longp[sizeof longp - 1] = '\0';
	ASSERT_EQ_I(layout_notify_sockaddr(longp, path, sizeof path, &len), -1);
}

/* ---------------------------------------------- MMP-3: DRM master holder */

static void test_display_server_names(void)
{
	ASSERT(layout_is_display_server("labwc"));
	ASSERT(layout_is_display_server("Xorg"));
	ASSERT(layout_is_display_server("Xwayland"));
	ASSERT(layout_is_display_server("wayfire"));
	ASSERT(layout_is_display_server("lightdm"));
	ASSERT(layout_is_display_server("gdm3"));
	ASSERT(layout_is_display_server("sddm"));
	ASSERT(layout_is_display_server("weston"));
	ASSERT(layout_is_display_server("kodi.bin"));
	ASSERT(layout_is_display_server("kodi"));
	ASSERT(layout_is_display_server("labwc\n"));   /* /proc/PID/comm ends in '\n' */
	ASSERT(!layout_is_display_server("rtspwall"));
	ASSERT(!layout_is_display_server("bash"));
	ASSERT(!layout_is_display_server("Xorgish"));
	ASSERT(!layout_is_display_server(""));
}

/* ------------------------------------------------ QA review of Phase A
 *
 * Tests under QA_KNOWN_BUGS document bugs found in the Phase A review and
 * FAIL on 00a38be. They are left out of the default `make test` so the
 * suite stays green for parallel work; run them with
 *   make test OPTFLAGS="-O2 -g -DQA_KNOWN_BUGS"
 */

static bool contains(const char *hay, const char *needle)
{
	return strstr(hay, needle) != NULL;
}

/* Regression guards (pass today). */
static void qa_test_unifi_rewrite_edges(void)
{
	/* userinfo that itself looks like ":7441" must not confuse the port */
	unifi("rtsps://u:7441@h:7441/T?enableSrtp", 1, "rtsp://u:7441@h:7447/T");
	/* '@' inside the password (unencoded): the last '@' ends userinfo */
	unifi("rtsps://u:ex@mple@h:7441/T?enableSrtp", 1, "rtsp://u:ex@mple@h:7447/T");
	/* fragment survives, enableSrtp alone leaves no dangling '?' */
	unifi("rtsps://h:7441/T?enableSrtp#x", 1, "rtsp://h:7447/T#x");
	/* IPv6 with zone id */
	unifi("rtsps://[fe80::1%25eth0]:7441/T?enableSrtp", 1, "rtsp://[fe80::1%25eth0]:7447/T");
	/* IPv6 without port: untouched */
	unifi("rtsps://[fe80::1]/T", 0, NULL);
}

static void qa_test_mask_url_regular_passwords(void)
{
	char out[LAYOUT_URL_MAX];
	layout_mask_url("rtsp://admin:ex@mple@1.2.3.4/s", out, sizeof out);
	ASSERT(!contains(out, "ex@mple"));
	layout_mask_url("rtsp://admin:ex%2Fample@1.2.3.4/s", out, sizeof out);
	ASSERT(!contains(out, "ex%2Fample"));
}

/* Fixed (was a QA_KNOWN_BUGS test): layout.c mask_url_into ended the authority at the
 * first '/', '?' or '#'. A password typed unencoded with one of those (the
 * very mistake the %-encoding hint is about) is then logged in clear by
 * camera_thread.c ("cannot open %s") and shown by probe/--report. */
static void qa_test_mask_url_password_with_slash_query_hash(void)
{
	char out[LAYOUT_URL_MAX];

	layout_mask_url("rtsp://admin:ex/ample@1.2.3.4/stream", out, sizeof out);
	ASSERT(!contains(out, "admin:ex"));
	ASSERT(!contains(out, "ample@1.2.3.4"));

	layout_mask_url("rtsp://admin:ex?ample@1.2.3.4/s", out, sizeof out);
	ASSERT(!contains(out, "admin:ex"));

	layout_mask_url("rtsp://admin:ex#ample@1.2.3.4/s", out, sizeof out);
	ASSERT(!contains(out, "admin:ex"));
}

/* Fixed (was a QA_KNOWN_BUGS test): layout.c url_char excludes ( ) ' " and space, so
 * layout_mask_urls_in_text stops the URL inside the password and masks
 * nothing. Used for FFmpeg log lines, doctor --report and the add/doctor
 * --check-config output. ( ) ' are RFC 3986 sub-delims and common in
 * passwords. */
static void qa_test_mask_text_password_with_sub_delims(void)
{
	char out[512];

	layout_mask_urls_in_text("cannot open rtsp://admin:p(1)@h/a: x", out, sizeof out);
	ASSERT(!contains(out, "p(1)"));
	layout_mask_urls_in_text("cannot open rtsp://admin:it's@h/a: x", out, sizeof out);
	ASSERT(!contains(out, "it's"));
}

/* Fixed (was a QA_KNOWN_BUGS test): layout.c find_placeholder: a password containing
 * "<...>" is reported as a placeholder AND echoed in clear in the error
 * (journal + sd_notify STATUS via config.c:105). */
static void qa_test_angle_brackets_in_password_not_echoed(void)
{
	static struct layout_config cfg;
	char err[512];
	int r = layout_parse(&cfg, "GRID=1x1\ncam|rtsp://admin:<notreal>@h/s|1\n",
			     NULL, NULL, err, sizeof err);
	ASSERT_EQ_I(r, -1);
	ASSERT(!contains(err, "notreal"));
}

/* ------------------------------------------- Phase A security review fixes */

/* M1: userinfo, path and ;param credentials. */
static void test_mask_url_review_m1(void)
{
	char out[LAYOUT_URL_MAX];

	/* unencoded '/', '?', '#' in the password: masked completely */
	expect_mask("rtsp://admin:ex/ample@1.2.3.4/stream", "rtsp://admin:***@1.2.3.4/stream");
	expect_mask("rtsp://admin:ex?ample@1.2.3.4/s", "rtsp://admin:***@1.2.3.4/s");
	expect_mask("rtsp://admin:ex#ample@1.2.3.4/s", "rtsp://admin:***@1.2.3.4/s");
	expect_mask("rtsp://u:a/b@c@h/x", "rtsp://u:***@h/x");
	expect_mask("rtsp://admin:ex/ample@1.2.3.4", "rtsp://admin:***@1.2.3.4");
	/* an '@' in the path of a URL without a port/userinfo is still a path */
	expect_mask("rtsp://host/a:b@c", "rtsp://host/a:b@c");
	/* a numeric-looking password before an unencoded '/' */
	expect_mask("rtsp://admin:12/34@h/x", "rtsp://admin:***@h/x");

	/* userinfo without a colon */
	expect_mask("rtsp://faketokenonly@host/live", "rtsp://***@host/live");

	/* an authority cut short (no '@' reached, e.g. a truncated key in a
	 * config warning): a non-numeric "port" is a password */
	expect_mask("rtsp://admin:fakepwonly", "rtsp://admin:***");
	expect_mask("rtsp://host:554/s", "rtsp://host:554/s");
	expect_mask("rtsp://[fe80::1]:554/s", "rtsp://[fe80::1]:554/s");

	/* XMEye: the whole segment carries '&' and '=' */
	layout_mask_url("rtsp://h:554/user=admin&password=examplepw&channel=1&stream=0.sdp",
			out, sizeof out);
	ASSERT(!contains(out, "examplepw"));
	expect_mask("rtsp://h:554/user=admin&password=examplepw&channel=1&stream=0.sdp",
		    "rtsp://h:554/***");
	/* Foscam: ;param with a credential-like key */
	layout_mask_url("rtsp://h:88/videoMain;user=admin;pwd=notreal", out, sizeof out);
	ASSERT(!contains(out, "notreal"));
	expect_mask("rtsp://h:88/videoMain;user=admin;pwd=notreal",
		    "rtsp://h:88/videoMain;user=admin;pwd=***");
	/* single key=value segments; key match is case-insensitive */
	expect_mask("rtsp://h/live/Password=fakepw/x", "rtsp://h/live/Password=***/x");
	expect_mask("rtsp://h/live/token=abc", "rtsp://h/live/token=***");
	expect_mask("rtsp://h/s;AuthKey=zz;ch=1", "rtsp://h/s;AuthKey=***;ch=1");
	expect_mask("rtsp://h/s;psw=1;sig=2;secret=3;cred=4;pas=5",
		    "rtsp://h/s;psw=***;sig=***;secret=***;cred=***;pas=***");
	/* harmless key=value and ;params are kept */
	expect_mask("rtsp://h/live/ch=1;stream=0", "rtsp://h/live/ch=1;stream=0");
	expect_mask("rtsp://h/mpeg4/media.amp", "rtsp://h/mpeg4/media.amp");
}

/* M1: free text with ( ) ' " or spaces inside a password. */
static void test_mask_text_review_m1(void)
{
	char out[512];

	layout_mask_urls_in_text("open rtsp://admin:ex\"ample@host/x failed", out, sizeof out);
	ASSERT(!contains(out, "ex\"ample"));
	ASSERT(!contains(out, "ample@"));
	ASSERT(contains(out, "rtsp://admin:***@host/x failed"));

	layout_mask_urls_in_text("cannot open rtsp://admin:p(1)@h/a: x", out, sizeof out);
	ASSERT(strcmp(out, "cannot open rtsp://admin:***@h/a: x") == 0);

	layout_mask_urls_in_text("cannot open rtsp://admin:it's@h/a: x", out, sizeof out);
	ASSERT(strcmp(out, "cannot open rtsp://admin:***@h/a: x") == 0);

	layout_mask_urls_in_text("err rtsp://admin:my fake pw@h/a, retrying", out, sizeof out);
	ASSERT(!contains(out, "fake"));
	ASSERT(strcmp(out, "err rtsp://admin:***@h/a, retrying") == 0);

	/* a password-less URL followed by unrelated text is left alone */
	layout_mask_urls_in_text("open rtsp://host/x (user bob@home)", out, sizeof out);
	ASSERT(strcmp(out, "open rtsp://host/x (user bob@home)") == 0);
	/* a config warning about a key that was a URL */
	layout_mask_urls_in_text("line 3: unknown key rtsp://ad:notreal@1.2.3.4/x, ignored",
				 out, sizeof out);
	ASSERT(!contains(out, "notreal"));
}

/* L2 / placeholder echo: <...> only counts as a whole field or segment,
 * and the URL is never echoed. */
static void test_placeholder_whole_segment_only(void)
{
	struct layout_config cfg;

	ASSERT(layout_has_placeholder("<password>"));
	ASSERT(layout_has_placeholder("rtsp://admin:<password>@<ip>/s"));
	ASSERT(layout_has_placeholder("rtsp://admin:pw@[<ip>]:554/s"));
	ASSERT(layout_has_placeholder("rtsp://admin:pw@h/live?user=<user>"));
	ASSERT(!layout_has_placeholder("rtsp://admin:ab<c>d@h/s"));
	ASSERT(!layout_has_placeholder("rtsp://admin:<abc@h/s"));
	ASSERT(!layout_has_placeholder("x<y>"));

	/* a real password with <...> inside is accepted ... */
	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\ncam|rtsp://admin:a<notreal>b@h/s|1\n"), 0);
	/* ... (with a warning about unencoded characters, without the URL) */
	ASSERT(warnings >= 1);
	ASSERT(!contains(last_warning, "notreal"));
	ASSERT(contains(last_warning, "camera cam"));

	/* a whole-segment placeholder is an error that never echoes the URL */
	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\ncam|rtsp://admin:<notreal>@h/s|1\n"), -1);
	ASSERT(!contains(err, "notreal"));
	ASSERT(contains(err, "line 2: placeholder in the URL of camera cam"));
	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\ncam|rtsp://admin:CHANGE_ME@10.1.2.3/s|1\n"), -1);
	ASSERT(!contains(err, "10.1.2.3"));
	ASSERT(contains(err, "placeholder in the URL of camera cam"));

	/* other fields still name the placeholder */
	ASSERT_EQ_I(parse(&cfg, "DECODER=<decoder>\nGRID=1x1\nc|rtsp://h/s|1\n"), -1);
	ASSERT(contains(err, "<decoder>"));
}

/* Unencoded " ' < > { } \ ^ ` in a URL: a warning, not an error. */
static void test_url_unsafe_chars_warn(void)
{
	struct layout_config cfg;

	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\ncam|rtsp://admin:it's{x}@h/s|1\n"), 0);
	ASSERT_EQ_I(warnings, 1);
	ASSERT(contains(last_warning, "line 2:"));
	ASSERT(contains(last_warning, "URL of camera cam"));
	ASSERT(contains(last_warning, "'"));
	ASSERT(contains(last_warning, "{"));
	ASSERT(!contains(last_warning, "admin"));

	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\ncam|rtsp://admin:p%22w@h/s|1\n"), 0);
	ASSERT_EQ_I(warnings, 0);
	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\ncam|rtsp://a:b\\c^d`e\"f@h/s|1\n"), 0);
	ASSERT_EQ_I(warnings, 1);
}

/* M2: the three UniFi modes through the API. */
static void unifi_mode(const char *in, enum layout_unifi_mode mode,
		       enum layout_unifi_result want_res, const char *want)
{
	char out[LAYOUT_URL_MAX];
	enum layout_unifi_result res = (enum layout_unifi_result)99;
	int r = layout_unifi_apply(in, mode, out, sizeof out, &res);
	test_checks++;
	if (r < 0 || res != want_res || strcmp(out, want ? want : in)) {
		fprintf(stderr, "FAIL unifi_apply(\"%s\", %d) = %d/%d \"%s\", want %d \"%s\"\n",
			in, (int)mode, r, (int)res, out, (int)want_res, want ? want : in);
		test_failures++;
	}
}

static void test_unifi_modes(void)
{
	const char *u = "rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp";

	unifi_mode(u, LAYOUT_UNIFI_MODE_TLS, LAYOUT_UNIFI_KEPT_TLS,
		   "rtsps://192.168.1.1:7441/EXAMPLEtoken1234");
	unifi_mode(u, LAYOUT_UNIFI_MODE_PLAIN, LAYOUT_UNIFI_PLAIN,
		   "rtsp://192.168.1.1:7447/EXAMPLEtoken1234");
	unifi_mode(u, LAYOUT_UNIFI_MODE_OFF, LAYOUT_UNIFI_OFF, NULL);

	/* tls: other query parameters and fragment survive, userinfo too */
	unifi_mode("rtsps://u:p@h:7441/T?a=1&enableSrtp&b=2#f", LAYOUT_UNIFI_MODE_TLS,
		   LAYOUT_UNIFI_KEPT_TLS, "rtsps://u:p@h:7441/T?a=1&b=2#f");
	unifi_mode("RTSPS://NVR:7441/Tok", LAYOUT_UNIFI_MODE_TLS, LAYOUT_UNIFI_KEPT_TLS,
		   "RTSPS://NVR:7441/Tok");

	/* not UniFi: untouched in every mode */
	for (int m = LAYOUT_UNIFI_MODE_TLS; m <= LAYOUT_UNIFI_MODE_OFF; m++) {
		unifi_mode("rtsp://cam/stream1", (enum layout_unifi_mode)m,
			   LAYOUT_UNIFI_NOT_UNIFI, NULL);
		unifi_mode("rtsps://cam:322/s", (enum layout_unifi_mode)m,
			   LAYOUT_UNIFI_NOT_UNIFI, NULL);
	}

	/* names, and the parser keys */
	ASSERT(strcmp(layout_unifi_mode_name(LAYOUT_UNIFI_MODE_TLS), "tls") == 0);
	ASSERT(strcmp(layout_unifi_mode_name(LAYOUT_UNIFI_MODE_PLAIN), "plain") == 0);
	ASSERT(strcmp(layout_unifi_mode_name(LAYOUT_UNIFI_MODE_OFF), "off") == 0);
	struct layout_config cfg;
	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\nc|rtsp://h/s|1\n"), 0);
	ASSERT_EQ_I(cfg.unifi_mode, LAYOUT_UNIFI_MODE_TLS);
	ASSERT_EQ_I(parse(&cfg, "UNIFI_REWRITE=tls\nGRID=1x1\nc|rtsp://h/s|1\n"), 0);
	ASSERT_EQ_I(cfg.unifi_mode, LAYOUT_UNIFI_MODE_TLS);
	ASSERT_EQ_I(parse(&cfg, "UNIFI_REWRITE=plain\nGRID=1x1\nc|rtsp://h/s|1\n"), 0);
	ASSERT_EQ_I(cfg.unifi_mode, LAYOUT_UNIFI_MODE_PLAIN);
	ASSERT_EQ_I(parse(&cfg, "UNIFI_REWRITE=yes\nGRID=1x1\nc|rtsp://h/s|1\n"), -1);
	ASSERT(contains(err, "UNIFI_REWRITE must be tls, plain or off"));

	/* too small an output buffer */
	char small[8];
	enum layout_unifi_result res;
	ASSERT_EQ_I(layout_unifi_apply(u, LAYOUT_UNIFI_MODE_TLS, small, sizeof small, &res), -1);
	ASSERT(strlen(small) < sizeof small);
}

/* sd_notify: an abstract name filling sun_path exactly (107 bytes + the
 * leading NUL = 108) is valid — abstract names are not NUL-terminated. */
static void test_notify_sockaddr_abstract_max(void)
{
	char path[108];
	char env[110];
	size_t len = 0;

	env[0] = '@';
	memset(env + 1, 'n', 107);
	env[108] = '\0';
	ASSERT_EQ_I(layout_notify_sockaddr(env, path, sizeof path, &len), 0);
	ASSERT_EQ_I(len, 108);
	ASSERT_EQ_I(path[0], '\0');
	ASSERT_EQ_I(path[107], 'n');

	env[108] = 'n';
	env[109] = '\0';
	ASSERT_EQ_I(layout_notify_sockaddr(env, path, sizeof path, &len), -1);

	/* a path needs its NUL: 107 characters fit, 108 do not */
	char p2[110];
	p2[0] = '/';
	memset(p2 + 1, 'p', 106);
	p2[107] = '\0';
	ASSERT_EQ_I(layout_notify_sockaddr(p2, path, sizeof path, &len), 0);
	p2[107] = 'p';
	p2[108] = '\0';
	ASSERT_EQ_I(layout_notify_sockaddr(p2, path, sizeof path, &len), -1);
}

/* Startup wait: does a manual layout fit a screen (layout_tiles_fit)? */
static void test_tiles_fit(void)
{
	struct layout_config cfg;
	int need_w = 0, need_h = 0;
	ASSERT_EQ_I(parse(&cfg, "a|rtsp://x|960|540|960|540\nb|rtsp://y|960|540|0|0\n"), 0);
	ASSERT(layout_tiles_fit(&cfg, 1920, 1080, &need_w, &need_h));
	ASSERT_EQ_I(need_w, 1920);
	ASSERT_EQ_I(need_h, 1080);
	ASSERT(!layout_tiles_fit(&cfg, 1024, 768, &need_w, &need_h));
	ASSERT(!layout_tiles_fit(&cfg, 1920, 1079, NULL, NULL));
	/* grid mode always fits */
	ASSERT_EQ_I(parse(&cfg, "GRID=2x2\nc|rtsp://h/s|4\n"), 0);
	ASSERT(layout_tiles_fit(&cfg, 640, 480, NULL, NULL));
}

/* ------------------------------------------- QA final review (c4fb7e5)
 *
 * Readability guards: the stricter masking must keep common camera paths
 * readable (only userinfo and the query are hidden). Pass on c4fb7e5. */
static void qa_final_mask_keeps_camera_paths_readable(void)
{
	expect_mask("rtsp://admin:pw@192.168.1.64:554/Streaming/Channels/101",
		    "rtsp://admin:***@192.168.1.64:554/Streaming/Channels/101");
	expect_mask("rtsp://admin:pw@192.168.1.64/ISAPI/Streaming/channels/102",
		    "rtsp://admin:***@192.168.1.64/ISAPI/Streaming/channels/102");
	expect_mask("rtsp://admin:pw@192.168.1.20:554/h264Preview_01_sub",
		    "rtsp://admin:***@192.168.1.20:554/h264Preview_01_sub");
	expect_mask("rtsp://127.0.0.1:8554/front_door", "rtsp://127.0.0.1:8554/front_door");
	expect_mask("rtsp://127.0.0.1:8554/backyard_camera_main_stream_hd",
		    "rtsp://127.0.0.1:8554/backyard_camera_main_stream_hd");
	expect_mask("rtsp://admin:pw@10.0.0.7:554/cam/realmonitor?channel=1&subtype=0",
		    "rtsp://admin:***@10.0.0.7:554/cam/realmonitor?***");
	expect_mask("rtsp://10.0.0.7:554/live/ch00_0", "rtsp://10.0.0.7:554/live/ch00_0");
	expect_mask("rtsp://10.0.0.7:554/MediaInput/h264/stream_1",
		    "rtsp://10.0.0.7:554/MediaInput/h264/stream_1");
	expect_mask("rtsp://10.0.0.7/keyframe_stream", "rtsp://10.0.0.7/keyframe_stream");
	expect_mask("rtsp://10.0.0.7/passage_cam", "rtsp://10.0.0.7/passage_cam");
	expect_mask("rtsp://[fe80::1]:554/stream1", "rtsp://[fe80::1]:554/stream1");
	expect_mask("rtsp://user:ex@mple@[fe80::1]:554/stream1", "rtsp://user:***@[fe80::1]:554/stream1");

	/* An FFmpeg log prefix "[rtsp @ 0x...]" before a URL does not trigger
	 * the "extend to the next '@'" rule. */
	char out[512];
	layout_mask_urls_in_text("[rtsp @ 0x5583c] rtsp://10.0.0.5:554/stream1: 404 Not Found",
				 out, sizeof out);
	ASSERT(contains(out, "rtsp://10.0.0.5:554/stream1: 404 Not Found"));
}

/* Fixed (was a QA_KNOWN_BUGS test): layout.c layout_mask_urls_in_text: a
 * URL without a path (host:port only)
 * followed later on the same line by any '@' is "extended" to that '@',
 * swallowing the diagnostic text in between (false positive, readability). */
static void qa_final_text_pathless_url_then_at_keeps_text(void)
{
	char out[512];
	layout_mask_urls_in_text("cannot open rtsp://cam.local:554: Connection refused "
				 "(see admin@example.com)", out, sizeof out);
	ASSERT(contains(out, "Connection refused"));
}

/* Fixed (was a QA_KNOWN_BUGS test): layout.c layout_mask_urls_in_text: a
 * password with BOTH an unencoded '/'
 * and a space is cut at the space; the URL part has a '/' so the
 * "extend to '@'" rule does not apply and the rest leaks. */
static void qa_final_text_password_slash_and_space(void)
{
	char out[512];
	layout_mask_urls_in_text("front: cannot open rtsp://admin:ex/am ple@h/s (401)", out,
				 sizeof out);
	ASSERT(!contains(out, "am ple"));
	ASSERT(strcmp(out, "front: cannot open rtsp://admin:***@h/s (401)") == 0);
}

/* Guards for the two fixes above: a port is not a password, an IPv6 host
 * has no userinfo, and a password that merely starts with digits is still
 * masked. */
static void test_mask_text_port_vs_password(void)
{
	char out[512];
	layout_mask_urls_in_text("cannot open rtsp://cam.local:554: Connection refused "
				 "(see admin@example.com)", out, sizeof out);
	ASSERT(strcmp(out, "cannot open rtsp://cam.local:554: Connection refused "
			   "(see admin@example.com)") == 0);
	layout_mask_urls_in_text("open rtsp://h:554/s failed (bob@home)", out, sizeof out);
	ASSERT(strcmp(out, "open rtsp://h:554/s failed (bob@home)") == 0);
	layout_mask_urls_in_text("open rtsp://[fe80::1]:554/s failed (bob@home)", out, sizeof out);
	ASSERT(strcmp(out, "open rtsp://[fe80::1]:554/s failed (bob@home)") == 0);
	layout_mask_urls_in_text("err rtsp://admin:1234 5@h/a, retrying", out, sizeof out);
	ASSERT(strcmp(out, "err rtsp://admin:***@h/a, retrying") == 0);
	layout_mask_urls_in_text("[rtsp @ 0x55] rtsp://admin:a b/c d@h/x: 401", out, sizeof out);
	ASSERT(strcmp(out, "[rtsp @ 0x55] rtsp://admin:***@h/x: 401") == 0);
}

/* A4: only rtsp://, rtsps:// (any case), file: and local paths are
 * played; anything else is a config error with the line number. */
static void test_url_scheme_whitelist(void)
{
	static struct layout_config cfg;
	static const char *const ok[] = {
		"rtsp://h/s", "RTSP://h/s", "rtsps://h:7441/T", "RtSpS://h/s",
		"/home/pi/clip.mp4", "clips/cam1.mp4", "./cam:1.mp4", "file:///home/pi/a.mp4",
		"FILE:/home/pi/a.mp4", "file:a.mp4",
	};
	static const char *const bad[] = {
		"http://h/s.mjpg", "https://h/s", "rtmp://h/live", "udp://239.0.0.1:1234",
		"tcp://h:554", "srt://h:9000", "HTTP://h/s", "rtp://h:5004", "pipe:0",
		"concat:a.mp4|b.mp4", "subfile:,start,0,end,0,:/etc/shadow", "rtsp:/h/s",
	};
	char text[256];
	for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
		snprintf(text, sizeof text, "GRID=1x1\ncam|%s|1\n", ok[i]);
		if (parse(&cfg, text) != 0) {
			fprintf(stderr, "rejected %s: %s\n", ok[i], err);
			ASSERT(0);
		}
	}
	for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
		if (strchr(bad[i], '|'))
			continue;             /* '|' would split the line */
		snprintf(text, sizeof text, "GRID=1x1\n\ncam|%s|1\n", bad[i]);
		ASSERT_EQ_I(parse(&cfg, text), -1);
		if (!contains(err, "line 3: ") ||
		    !contains(err, "only rtsp://, rtsps:// URLs or local video files are supported")) {
			fprintf(stderr, "accepted or wrong error for %s: \"%s\"\n", bad[i], err);
			ASSERT(0);
		}
	}
	/* the URL is not echoed (it may hold a password) */
	ASSERT_EQ_I(parse(&cfg, "GRID=1x1\ncam|http://admin:examplepw@h/s|1\n"), -1);
	ASSERT(!contains(err, "examplepw"));
	ASSERT(contains(err, "camera cam"));
}


/* qa A10: a TV in standby (or without a readable EDID) offers only the
 * driver's reserve modes. Treated like a MODE fallback, so the 30 s probe
 * switches to a real mode once the EDID shows up. */
static void test_only_reserve_modes(void)
{
	struct layout_mode noedid[] = {          /* drm_add_modes_noedid + preferred 1024x768 */
		{ 1024, 768, 60004, false, true },
		{ 800, 600, 60317, false, false },
		{ 640, 480, 59940, false, false },
	};
	ASSERT(layout_only_reserve_modes(noedid, 3));

	struct layout_mode tv[] = {
		{ 1920, 1080, 60000, false, true },
		{ 1280, 720, 60000, false, false },
		{ 1024, 768, 60004, false, false },
	};
	ASSERT(!layout_only_reserve_modes(tv, 3));

	/* large modes but none flagged preferred: no EDID behind them */
	struct layout_mode nopref[] = {
		{ 1920, 1080, 60000, false, false },
		{ 1280, 720, 60000, false, false },
	};
	ASSERT(layout_only_reserve_modes(nopref, 2));

	/* exactly 1280x720 preferred is a real (720p) display */
	struct layout_mode p720[] = { { 1280, 720, 60000, false, true } };
	ASSERT(!layout_only_reserve_modes(p720, 1));

	/* interlaced 1080i does not count as a real large mode */
	struct layout_mode inter[] = {
		{ 1920, 1080, 60000, true, true },
		{ 1024, 768, 60004, false, false },
	};
	ASSERT(layout_only_reserve_modes(inter, 2));

	ASSERT(layout_only_reserve_modes(NULL, 0));
}

/* qa item 3: probe and the daemon pick the same stream - the first H.264
 * video stream, else the first video stream (for the "not H.264" verdict). */
static void test_pick_video_stream(void)
{
	struct layout_stream a[] = {
		{ .video = false }, { .video = true, .h264 = false }, { .video = true, .h264 = true },
		{ .video = true, .h264 = true },
	};
	bool h264 = false;
	ASSERT_EQ_I(layout_pick_video_stream(a, 4, &h264), 2);   /* first H.264, not the last */
	ASSERT(h264);

	struct layout_stream b[] = { { .video = false }, { .video = true }, { .video = true } };
	ASSERT_EQ_I(layout_pick_video_stream(b, 3, &h264), 1);   /* H.265 only: first video */
	ASSERT(!h264);

	struct layout_stream c[] = { { .video = false }, { .video = false, .h264 = true } };
	ASSERT_EQ_I(layout_pick_video_stream(c, 2, &h264), -1);
	ASSERT(!h264);
	ASSERT_EQ_I(layout_pick_video_stream(NULL, 0, NULL), -1);
	ASSERT_EQ_I(layout_pick_video_stream(a, 4, NULL), 2);
}


/* qa B1: only real external monitor connectors are targets of the
 * automatic move; panels, TV outputs and virtual connectors that always
 * report "connected" never are. */
static void test_connector_auto_target(void)
{
	ASSERT(layout_connector_auto_target("HDMI-A"));
	ASSERT(layout_connector_auto_target("HDMI-B"));
	ASSERT(layout_connector_auto_target("DVI-I"));
	ASSERT(layout_connector_auto_target("DVI-D"));
	ASSERT(layout_connector_auto_target("DVI-A"));
	ASSERT(layout_connector_auto_target("DP"));
	ASSERT(!layout_connector_auto_target("DSI"));
	ASSERT(!layout_connector_auto_target("Composite"));
	ASSERT(!layout_connector_auto_target("TV"));
	ASSERT(!layout_connector_auto_target("SVIDEO"));
	ASSERT(!layout_connector_auto_target("Component"));
	ASSERT(!layout_connector_auto_target("Virtual"));
	ASSERT(!layout_connector_auto_target("Writeback"));
	ASSERT(!layout_connector_auto_target("eDP"));
	ASSERT(!layout_connector_auto_target("VGA"));
	ASSERT(!layout_connector_auto_target("Unknown"));
	ASSERT(!layout_connector_auto_target("HDMI"));
	ASSERT(!layout_connector_auto_target(""));
	ASSERT(!layout_connector_auto_target(NULL));
}


/* Startup without CONNECTOR: the same type filter as the cable follow -
 * a TV in standby must not make the wall start on DSI/Composite. */
static void test_pick_start_connector(void)
{
	/* TV in standby (HDMI disconnected), DSI + Composite report connected: wait */
	struct layout_conn standby[] = {
		{ .auto_target = true,  .usable = false },   /* HDMI-A-1 */
		{ .auto_target = true,  .usable = false },   /* HDMI-A-2 */
		{ .auto_target = false, .usable = true },    /* DSI-1 */
		{ .auto_target = false, .usable = true },    /* Composite-1 */
	};
	ASSERT_EQ_I(layout_pick_start_connector(standby, 4), -1);

	/* HDMI connected: HDMI, even when DSI comes first */
	struct layout_conn hdmi[] = {
		{ .auto_target = false, .usable = true },    /* DSI-1 */
		{ .auto_target = true,  .usable = false },   /* HDMI-A-1 */
		{ .auto_target = true,  .usable = true },    /* HDMI-A-2 */
	};
	ASSERT_EQ_I(layout_pick_start_connector(hdmi, 3), 2);

	/* only a DSI panel on the card: DSI */
	struct layout_conn dsi[] = { { .auto_target = false, .usable = true } };
	ASSERT_EQ_I(layout_pick_start_connector(dsi, 1), 0);

	/* no auto-target type, first usable of any type */
	struct layout_conn other[] = {
		{ .auto_target = false, .usable = false },
		{ .auto_target = false, .usable = true },
	};
	ASSERT_EQ_I(layout_pick_start_connector(other, 2), 1);

	/* nothing usable at all */
	struct layout_conn none[] = { { .auto_target = false, .usable = false } };
	ASSERT_EQ_I(layout_pick_start_connector(none, 1), -1);
	ASSERT_EQ_I(layout_pick_start_connector(NULL, 0), -1);
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

	/* MMP-2 / MMP-3 / MMP-5 / MMP-7 */
	test_no_cameras_configured_message();
	test_placeholder_change_me_is_error();
	test_placeholder_angle_brackets_is_error();
	test_unknown_key_strict_is_error_with_suggestion();
	test_suggest_key();
	test_known_keys_are_accepted_strictly();
	test_pipe_in_password_hint();
	test_parse_mode_key();
	test_select_mode_4k30_tv();
	test_select_mode_4k60();
	test_select_mode_keeps_ultrawide_and_1440p();
	test_select_mode_keeps_1080p50_and_720p();
	test_select_mode_preferred_flag_and_interlaced();
	test_select_mode_4k_without_same_aspect_keeps_preferred();
	test_select_mode_explicit();
	test_unifi_rewrite();
	test_unifi_rewrite_in_config();
	test_url_is_live();
	test_notify_sockaddr();
	test_display_server_names();

	/* QA review of Phase A */
	qa_test_unifi_rewrite_edges();
	qa_test_mask_url_regular_passwords();
	qa_test_mask_url_password_with_slash_query_hash();
	qa_test_mask_text_password_with_sub_delims();
	qa_test_angle_brackets_in_password_not_echoed();

	/* Phase A security review fixes */
	test_mask_url_review_m1();
	test_mask_text_review_m1();
	test_placeholder_whole_segment_only();
	test_url_unsafe_chars_warn();
	test_unifi_modes();
	test_notify_sockaddr_abstract_max();
	test_tiles_fit();

	test_url_scheme_whitelist();

	/* QA final review (c4fb7e5) */
	qa_final_mask_keeps_camera_paths_readable();
	qa_final_text_pathless_url_then_at_keeps_text();
	qa_final_text_password_slash_and_space();
	test_mask_text_port_vs_password();

	/* qa rc1 "can wait" items */
	test_only_reserve_modes();
	test_pick_video_stream();
	test_connector_auto_target();
	test_pick_start_connector();

	return test_summary("test_layout");
}
