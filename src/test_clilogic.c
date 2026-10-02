/*
 * test_clilogic.c — unit tests for the pure helpers behind probe, add and
 * doctor (clilogic.c). The SPS fixtures are real parameter sets produced by
 * ffmpeg/libx264 6.1 for the sizes named.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "clilogic.h"
#include "test.h"

/* ------------------------------------------------------------------ SPS */

static const unsigned char sps_360p_baseline_25[] = {
	0x67, 0x42, 0xc0, 0x1e, 0xd9, 0x00, 0xa0, 0x2f, 0xf9, 0x70, 0x11, 0x00, 0x00, 0x03,
	0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x32, 0x0f, 0x16, 0x2e, 0x48 };
static const unsigned char sps_1080p_high_15[] = {
	0x67, 0x64, 0x00, 0x28, 0xac, 0xb2, 0x00, 0xf0, 0x04, 0x4f, 0xcb, 0x80, 0x88, 0x00,
	0x00, 0x03, 0x00, 0x08, 0x00, 0x00, 0x03, 0x00, 0xf0, 0x78, 0xc1, 0x92, 0x40 };
static const unsigned char sps_1440p_main_30[] = {
	0x67, 0x4d, 0x40, 0x32, 0xd9, 0x00, 0x28, 0x00, 0xb5, 0xb0, 0x11, 0x00, 0x00, 0x03,
	0x00, 0x01, 0x00, 0x00, 0x03, 0x00, 0x3c, 0x0f, 0x18, 0x32, 0x48 };
static const unsigned char sps_720p_main_2997[] = {
	0x67, 0x4d, 0x40, 0x1f, 0xd9, 0x00, 0x50, 0x05, 0xbb, 0x01, 0x10, 0x00, 0x00, 0x3e,
	0x90, 0x00, 0x0e, 0xa6, 0x00, 0xf1, 0x83, 0x24, 0x80 };

static void test_sps_parse(void)
{
	struct h264_sps_info s;

	ASSERT_EQ_I(h264_parse_sps(sps_360p_baseline_25, sizeof sps_360p_baseline_25, &s), 0);
	ASSERT_EQ_I(s.profile_idc, 66);
	ASSERT_EQ_I(s.level_idc, 30);
	ASSERT_EQ_I(s.width, 640);
	ASSERT_EQ_I(s.height, 360);
	ASSERT(fabs(s.fps - 25.0) < 0.001);
	ASSERT(strcmp(h264_profile_name(s.profile_idc, s.constraint_flags),
		      "Constrained Baseline") == 0);

	ASSERT_EQ_I(h264_parse_sps(sps_1080p_high_15, sizeof sps_1080p_high_15, &s), 0);
	ASSERT_EQ_I(s.profile_idc, 100);
	ASSERT_EQ_I(s.level_idc, 40);
	ASSERT_EQ_I(s.width, 1920);
	ASSERT_EQ_I(s.height, 1080);     /* 1088 coded, cropped by 8 */
	ASSERT_EQ_I(s.chroma_format_idc, 1);
	ASSERT_EQ_I(s.bit_depth, 8);
	ASSERT(fabs(s.fps - 15.0) < 0.001);
	ASSERT(strcmp(h264_profile_name(100, 0), "High") == 0);

	ASSERT_EQ_I(h264_parse_sps(sps_1440p_main_30, sizeof sps_1440p_main_30, &s), 0);
	ASSERT_EQ_I(s.profile_idc, 77);
	ASSERT_EQ_I(s.width, 2560);
	ASSERT_EQ_I(s.height, 1440);
	ASSERT(fabs(s.fps - 30.0) < 0.001);

	ASSERT_EQ_I(h264_parse_sps(sps_720p_main_2997, sizeof sps_720p_main_2997, &s), 0);
	ASSERT_EQ_I(s.width, 1280);
	ASSERT_EQ_I(s.height, 720);
	ASSERT(fabs(s.fps - 29.97) < 0.01);
}

static void test_sps_invalid(void)
{
	struct h264_sps_info s;
	unsigned char pps[] = { 0x68, 0xce, 0x38, 0x80 };

	ASSERT_EQ_I(h264_parse_sps(pps, sizeof pps, &s), -1);
	ASSERT_EQ_I(h264_parse_sps(sps_1080p_high_15, 0, &s), -1);
	/* Truncated at every length: must fail cleanly (no overread under ASan). */
	for (size_t n = 1; n < 9; n++)
		ASSERT_EQ_I(h264_parse_sps(sps_1080p_high_15, n, &s), -1);
	for (size_t n = 9; n < sizeof sps_1080p_high_15; n++)
		(void)h264_parse_sps(sps_1080p_high_15, n, &s);
}

static void test_sps_extradata(void)
{
	struct h264_sps_info s;
	unsigned char buf[128];
	size_t n = 0;

	/* Annex B with a 4-byte start code, SPS then PPS. */
	memcpy(buf, "\x00\x00\x00\x01", 4);
	n = 4;
	memcpy(buf + n, sps_360p_baseline_25, sizeof sps_360p_baseline_25);
	n += sizeof sps_360p_baseline_25;
	memcpy(buf + n, "\x00\x00\x01\x68\xce\x38\x80", 7);
	n += 7;
	ASSERT_EQ_I(h264_parse_extradata(buf, n, &s), 0);
	ASSERT_EQ_I(s.width, 640);

	/* Annex B with the PPS first and a 3-byte start code for the SPS. */
	n = 0;
	memcpy(buf, "\x00\x00\x00\x01\x68\xce\x38\x80\x00\x00\x01", 11);
	n = 11;
	memcpy(buf + n, sps_1080p_high_15, sizeof sps_1080p_high_15);
	n += sizeof sps_1080p_high_15;
	ASSERT_EQ_I(h264_parse_extradata(buf, n, &s), 0);
	ASSERT_EQ_I(s.height, 1080);

	/* avcC: version 1, profile, compat, level, 0xff, 0xe1 (1 SPS), len, SPS, ... */
	n = 0;
	buf[n++] = 1; buf[n++] = 0x4d; buf[n++] = 0x40; buf[n++] = 0x32;
	buf[n++] = 0xff; buf[n++] = 0xe1;
	buf[n++] = 0; buf[n++] = sizeof sps_1440p_main_30;
	memcpy(buf + n, sps_1440p_main_30, sizeof sps_1440p_main_30);
	n += sizeof sps_1440p_main_30;
	buf[n++] = 1; buf[n++] = 0; buf[n++] = 4;
	memcpy(buf + n, "\x68\xce\x38\x80", 4);
	n += 4;
	ASSERT_EQ_I(h264_parse_extradata(buf, n, &s), 0);
	ASSERT_EQ_I(s.width, 2560);

	/* avcC whose SPS length runs past the end. */
	buf[7] = 200;
	ASSERT_EQ_I(h264_parse_extradata(buf, n, &s), -1);

	ASSERT_EQ_I(h264_parse_extradata(NULL, 0, &s), -1);
	ASSERT_EQ_I(h264_parse_extradata((const unsigned char *)"\x00\x00\x01", 3, &s), -1);
}

/* -------------------------------------------------------------- verdicts */

static void test_stream_verdict(void)
{
	char hint[256];
	struct probe_stream s = { PROBE_CODEC_H264, 66, 0xc0, 30, 640, 360, 25.0 };

	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_PASS);
	ASSERT_EQ_I(hint[0], '\0');

	s = (struct probe_stream){ PROBE_CODEC_HEVC, 0, 0, 0, 1920, 1080, 25.0 };
	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_FAIL);
	ASSERT(strstr(hint, "H.264") && strstr(hint, "sub-stream") && strstr(hint, "docs/cameras.md"));

	s = (struct probe_stream){ PROBE_CODEC_H264, 77, 0, 50, 2560, 1440, 30.0 };
	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_FAIL);
	ASSERT(strstr(hint, "1920x1920") && strstr(hint, "sub-stream"));

	/* Portrait 1080x1920 is within the decoder limit. */
	s = (struct probe_stream){ PROBE_CODEC_H264, 100, 0, 40, 1080, 1920, 15.0 };
	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_PASS);

	s = (struct probe_stream){ PROBE_CODEC_H264, 110, 0, 40, 1280, 720, 25.0 };   /* High 10 */
	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_FAIL);
	ASSERT(strstr(hint, "High 10") != NULL);

	s = (struct probe_stream){ PROBE_CODEC_OTHER, 0, 0, 0, 640, 480, 10.0 };      /* MJPEG */
	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_FAIL);

	s = (struct probe_stream){ PROBE_CODEC_H264, 100, 0, 42, 1920, 1080, 0 };     /* fps unknown */
	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_WARN);
	ASSERT(strstr(hint, "frame rate") != NULL);

	s = (struct probe_stream){ PROBE_CODEC_H264, 100, 0, 42, 1920, 1080, 60.0 };  /* 94 % alone */
	ASSERT_EQ_I(probe_stream_verdict(&s, hint, sizeof hint), BUDGET_WARN);
}

static void test_error_hints(void)
{
	ASSERT(strstr(probe_error_hint(PROBE_ERR_AUTH, "rtsp://u:***@h/x"), "percent") != NULL);
	ASSERT(strstr(probe_error_hint(PROBE_ERR_AUTH, "rtsp://u:***@h/x"), "%40") != NULL);
	ASSERT(strstr(probe_error_hint(PROBE_ERR_REFUSED, "rtsp://h/x"),
		      "RTSP probably disabled on the camera/NVR") != NULL);
	ASSERT(strstr(probe_error_hint(PROBE_ERR_NOT_FOUND, "rtsp://h/x"), "path") != NULL);
	for (int e = PROBE_ERR_AUTH; e <= PROBE_ERR_OTHER; e++) {
		ASSERT(probe_error_hint((enum probe_error)e, "rtsp://h/")[0] != '\0');
		ASSERT(probe_error_label((enum probe_error)e)[0] != '\0');
	}
	ASSERT_EQ_I(probe_error_from_status(401), PROBE_ERR_AUTH);
	ASSERT_EQ_I(probe_error_from_status(403), PROBE_ERR_AUTH);
	ASSERT_EQ_I(probe_error_from_status(404), PROBE_ERR_NOT_FOUND);
	ASSERT_EQ_I(probe_error_from_status(454), PROBE_ERR_NOT_FOUND);
	ASSERT_EQ_I(probe_error_from_status(500), PROBE_ERR_OTHER);
}

/* ---------------------------------------------------------------- doctor */

static void test_board(void)
{
	ASSERT_EQ_I(board_classify("Raspberry Pi 4 Model B Rev 1.4"), BOARD_PI4);
	ASSERT_EQ_I(board_classify("Raspberry Pi 400 Rev 1.0"), BOARD_PI4_FAMILY);
	ASSERT_EQ_I(board_classify("Raspberry Pi Compute Module 4 Rev 1.0"), BOARD_PI4_FAMILY);
	ASSERT_EQ_I(board_classify("Raspberry Pi 5 Model B Rev 1.0"), BOARD_PI5);
	ASSERT_EQ_I(board_classify("Raspberry Pi 500 Rev 1.0"), BOARD_PI5);
	ASSERT_EQ_I(board_classify("Raspberry Pi Compute Module 5 Rev 1.0"), BOARD_PI5);
	ASSERT_EQ_I(board_classify("Raspberry Pi 3 Model B Plus Rev 1.3"), BOARD_OLDER_PI);
	ASSERT_EQ_I(board_classify("Raspberry Pi Zero 2 W Rev 1.0"), BOARD_OLDER_PI);
	ASSERT_EQ_I(board_classify(""), BOARD_UNKNOWN);
	ASSERT_EQ_I(board_classify("QEMU Virtual Machine"), BOARD_UNKNOWN);
}

static void test_configtxt(void)
{
	ASSERT_EQ_I(configtxt_gpu_mem(""), -1);
	ASSERT_EQ_I(configtxt_gpu_mem("dtoverlay=vc4-kms-v3d\n#gpu_mem=128\n"), -1);
	ASSERT_EQ_I(configtxt_gpu_mem("gpu_mem=128\n"), 128);
	ASSERT_EQ_I(configtxt_gpu_mem("gpu_mem=64\n[pi4]\ngpu_mem=256\n"), 256);
	ASSERT_EQ_I(configtxt_gpu_mem("[pi5]\ngpu_mem=256\n[all]\n"), -1);
	ASSERT_EQ_I(configtxt_gpu_mem("[cm4]\ngpu_mem=32\n[all]\ngpu_mem=96\n"), 96);
	ASSERT_EQ_I(configtxt_gpu_mem("  gpu_mem = 200 \r\n"), 200);
	ASSERT_EQ_I(configtxt_gpu_mem("gpu_mem_1024=300\n"), -1);   /* different key */

	char out[512];
	ASSERT_EQ_I(configtxt_set_gpu_mem("dtparam=audio=on\n[pi5]\nfoo=1\n", 256, out, sizeof out), 0);
	ASSERT(strcmp(out, "dtparam=audio=on\n[pi5]\nfoo=1\n[all]\ngpu_mem=256\n") == 0);
	ASSERT_EQ_I(configtxt_gpu_mem(out), 256);

	ASSERT_EQ_I(configtxt_set_gpu_mem("a=1\ngpu_mem=64\nb=2", 256, out, sizeof out), 0);
	ASSERT(strcmp(out, "a=1\ngpu_mem=256\nb=2") == 0);

	/* Not ending in a newline, no gpu_mem yet. */
	ASSERT_EQ_I(configtxt_set_gpu_mem("a=1", 256, out, sizeof out), 0);
	ASSERT(strcmp(out, "a=1\n[all]\ngpu_mem=256\n") == 0);

	/* A [pi5] gpu_mem line is left alone. */
	ASSERT_EQ_I(configtxt_set_gpu_mem("[pi5]\ngpu_mem=64\n", 256, out, sizeof out), 0);
	ASSERT(strcmp(out, "[pi5]\ngpu_mem=64\n[all]\ngpu_mem=256\n") == 0);

	ASSERT_EQ_I(configtxt_set_gpu_mem("a=1\n", 256, out, 8), -1);
}

static void test_gpu_need(void)
{
	long m;
	m = 4 * budget_stream_mbps(1920, 1080, 15);
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 4), 256);
	m = 4 * budget_stream_mbps(640, 360, 25);
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 4), 0);
	m = 5 * budget_stream_mbps(640, 360, 25);   /* demo */
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 5), 0);
	m = 6 * budget_stream_mbps(1024, 576, 30);
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 6), 256);
}

static void test_dmesg(void)
{
	ASSERT(dmesg_match("[  12.3] bcm2835-codec: Not enough GPU mem for buffers") != NULL);
	ASSERT(dmesg_match("[  12.3] videobuf2_common: vb2: driver bug: ...") != NULL);
	ASSERT(dmesg_match("[  12.3] bcm2835_mmal_vchiq: mmal: timeout waiting for reply") != NULL);
	ASSERT(dmesg_match("[  12.3] vb2: queue setup ok") == NULL);
	ASSERT(dmesg_match("[  12.3] usb 1-1: new device") == NULL);
}

static void test_perm(void)
{
	gid_t video = 44, render = 105;
	gid_t groups[] = { video, render };

	/* crw-rw---- root:video, user in video */
	ASSERT(perm_allows(S_IFCHR | 0660, 0, video, 999, 999, groups, 2, true));
	/* user not in the group */
	ASSERT(!perm_allows(S_IFCHR | 0660, 0, video, 999, 999, groups + 1, 1, true));
	/* primary gid matches */
	ASSERT(perm_allows(S_IFCHR | 0660, 0, video, 999, video, NULL, 0, true));
	/* config 0640 root:rtspwall — read yes, write no */
	ASSERT(perm_allows(S_IFREG | 0640, 0, 998, 997, 998, NULL, 0, false));
	ASSERT(!perm_allows(S_IFREG | 0640, 0, 998, 997, 998, NULL, 0, true));
	/* 0600 root:root — no */
	ASSERT(!perm_allows(S_IFREG | 0600, 0, 0, 997, 998, NULL, 0, false));
	/* other bits */
	ASSERT(perm_allows(S_IFREG | 0644, 0, 0, 997, 998, NULL, 0, false));
	/* owner bits take precedence even if "other" would allow */
	ASSERT(!perm_allows(S_IFREG | 0066, 997, 0, 997, 998, NULL, 0, false));
	/* directory search bit for a group member */
	ASSERT_EQ_I(perm_bits(S_IFDIR | 0750, 0, 998, 997, 998, NULL, 0), 5);
	ASSERT_EQ_I(perm_bits(S_IFDIR | 0700, 0, 998, 997, 998, NULL, 0), 0);
	ASSERT_EQ_I(perm_bits(S_IFDIR | 0755, 0, 0, 997, 998, NULL, 0), 5);
	ASSERT_EQ_I(perm_bits(S_IFDIR | 0000, 0, 0, 0, 0, NULL, 0), 7);
	/* root */
	ASSERT(perm_allows(S_IFREG | 0000, 5, 5, 0, 0, NULL, 0, true));
}

/* ------------------------------------------------------------------- add */

static const char *base_conf =
	"# rtspwall config\n"
	"GRID=2x2\n"
	"#front|rtsp://viewer:CHANGE_ME@192.168.1.10/stream1|1\n"
	"front|rtsp://u:p@10.0.0.1/s1|1\n"
	"back|rtsp://u:p@10.0.0.2/s1|3|40\n";

static void test_cfg_scan(void)
{
	struct cfg_scan s;

	cfg_scan_text(base_conf, "front", &s);
	ASSERT_EQ_I(s.grid_cols, 2);
	ASSERT_EQ_I(s.grid_rows, 2);
	ASSERT_EQ_I(s.n_cameras, 2);
	ASSERT(!s.manual_mode);
	ASSERT(s.cell_used[1] && !s.cell_used[2] && s.cell_used[3] && !s.cell_used[4]);
	ASSERT(s.name_taken);
	ASSERT_EQ_I(cfg_first_free_cell(&s), 2);

	cfg_scan_text(base_conf, "garage", &s);
	ASSERT(!s.name_taken);

	/* A commented-out camera does not take its name or cell. */
	cfg_scan_text("GRID=1x2\n#a|rtsp://h/x|1\n", "a", &s);
	ASSERT(!s.name_taken);
	ASSERT_EQ_I(cfg_first_free_cell(&s), 1);

	cfg_scan_text("GRID=1x2\na|rtsp://h/x|1\nb|rtsp://h/y|2\n", NULL, &s);
	ASSERT_EQ_I(cfg_first_free_cell(&s), 0);

	cfg_scan_text("a|rtsp://h/x|960|540|0|0\n", NULL, &s);
	ASSERT(s.manual_mode);
	ASSERT_EQ_I(s.grid_cols, 0);
	ASSERT_EQ_I(cfg_first_free_cell(&s), 0);

	/* Empty file / no GRID. */
	cfg_scan_text("", NULL, &s);
	ASSERT_EQ_I(s.grid_cols, 0);
	ASSERT_EQ_I(s.n_cameras, 0);

	/* Out-of-range cells are ignored, not a crash. */
	cfg_scan_text("GRID=8x8\na|rtsp://h/x|99\nb|rtsp://h/x|-1\nc|rtsp://h/x|64\n", NULL, &s);
	ASSERT(s.cell_used[64]);
	ASSERT_EQ_I(cfg_first_free_cell(&s), 1);
}

static void test_cfg_checks(void)
{
	ASSERT(cfg_check_name("front-door") == NULL);
	ASSERT(cfg_check_name("cam_1.a") == NULL);
	ASSERT(cfg_check_name("") != NULL);
	ASSERT(cfg_check_name("a|b") != NULL);
	ASSERT(cfg_check_name("a b") != NULL);
	ASSERT(cfg_check_name("#x") != NULL);
	char longname[80];
	memset(longname, 'a', sizeof longname - 1);
	longname[sizeof longname - 1] = '\0';
	ASSERT(cfg_check_name(longname) != NULL);

	ASSERT(cfg_check_url("rtsp://u:p@h:554/s") == NULL);
	ASSERT(cfg_check_url("") != NULL);
	ASSERT(strstr(cfg_check_url("rtsp://u:a|b@h/s"), "%7C") != NULL);
	ASSERT(cfg_check_url("rtsp://h/s x") != NULL);
	ASSERT(cfg_check_url("rtsp://h/s\n") != NULL);
	char longurl[600];
	memset(longurl, 'a', sizeof longurl - 1);
	longurl[sizeof longurl - 1] = '\0';
	ASSERT(cfg_check_url(longurl) != NULL);
}

static void test_cfg_append(void)
{
	char out[1024];

	ASSERT_EQ_I(cfg_append_camera(base_conf, "garage", "rtsp://u:p@10.0.0.3/s1", 2,
				      out, sizeof out), 0);
	ASSERT(strncmp(out, base_conf, strlen(base_conf)) == 0);
	ASSERT(strcmp(out + strlen(base_conf), "garage|rtsp://u:p@10.0.0.3/s1|2\n") == 0);

	/* The commented line stays commented. */
	ASSERT(strstr(out, "#front|rtsp://viewer:CHANGE_ME") != NULL);

	ASSERT_EQ_I(cfg_append_camera("GRID=2x2", "a", "rtsp://h/x", 1, out, sizeof out), 0);
	ASSERT(strcmp(out, "GRID=2x2\na|rtsp://h/x|1\n") == 0);

	ASSERT_EQ_I(cfg_append_camera("", "a", "rtsp://h/x", 1, out, sizeof out), 0);
	ASSERT(strcmp(out, "a|rtsp://h/x|1\n") == 0);

	ASSERT_EQ_I(cfg_append_camera(base_conf, "a", "rtsp://h/x", 1, out, 10), -1);

	/* The result scans as expected. */
	struct cfg_scan s;
	cfg_append_camera(base_conf, "garage", "rtsp://h/x", 2, out, sizeof out);
	cfg_scan_text(out, "garage", &s);
	ASSERT(s.name_taken);
	ASSERT_EQ_I(cfg_first_free_cell(&s), 4);
}

/* ---------------------------------------------------------------- report */

static void test_report_masking(void)
{
	/* Fixture: config excerpt, journal lines and a sudo log line, each
	 * carrying a secret that must not survive into the report. */
	const char *fixture =
		"front|rtsp://admin:hunter2pass@192.168.1.10:554/stream1|1\n"
		"unifi|rtsps://192.168.1.1:7441/aB3dE5fG7hJ9kL1m?enableSrtp|2\n"
		"[12:00:01] ffmpeg: [rtsp @ 0x55] method DESCRIBE failed: 401 Unauthorized "
		"(rtsp://viewer:s3cr%40t@10.0.0.7/Streaming/Channels/102)\n"
		"Oct 02 12:00:00 pi sudo[812]:     pi : TTY=pts/0 ; PWD=/home/pi ; USER=root ; "
		"COMMAND=/usr/bin/rtspwall probe rtsp://root:Tr0ub4dor@cam.local/h264Preview_01_sub\n"
		"camera 2: plain rtsp://10.0.0.9:7447/Zx9Yw8Vu7Ts6Rq5P\n"
		"cgi: password=hunter3 token=QwErTy123 user=bob\n"
		"evil \x1b[2J escape\n";
	struct report_buf r;

	report_init(&r);
	report_section(&r, "journal");
	report_append_masked(&r, fixture);
	ASSERT(!r.oom);
	ASSERT(r.data != NULL);

	const char *secrets[] = { "hunter2pass", "aB3dE5fG7hJ9kL1m", "enableSrtp", "s3cr",
				  "Tr0ub4dor", "Zx9Yw8Vu7Ts6Rq5P", "hunter3", "QwErTy123", "\x1b" };
	for (size_t i = 0; i < sizeof secrets / sizeof secrets[0]; i++) {
		if (strstr(r.data, secrets[i])) {
			fprintf(stderr, "secret %zu leaked:\n%s\n", i, r.data);
			ASSERT(0);
		}
	}
	/* Still useful: host names, users, paths and the structure remain. */
	ASSERT(strstr(r.data, "== journal ==") != NULL);
	ASSERT(strstr(r.data, "admin:***@192.168.1.10:554/stream1") != NULL);
	ASSERT(strstr(r.data, "401 Unauthorized") != NULL);
	ASSERT(strstr(r.data, "cam.local/h264Preview_01_sub") != NULL);
	ASSERT(strstr(r.data, "user=bob") != NULL);
	ASSERT(strstr(r.data, "COMMAND=/usr/bin/rtspwall probe") != NULL);
	report_free(&r);

	/* An over-long line whose URL straddles the internal line limit: the
	 * cut must not leave "user:pass" without its "@host". */
	{
		char *big = malloc(6000);
		ASSERT(big != NULL);
		if (big) {
			for (int off = 4060; off <= 4090; off += 3) {
				memset(big, 'x', (size_t)off);
				big[off - 1] = ' ';
				strcpy(big + off, "rtsp://admin:LongLinePw@10.1.1.1/stream1 tail\n");
				report_init(&r);
				report_append_masked(&r, big);
				ASSERT(r.data && strstr(r.data, "LongLinePw") == NULL);
				report_free(&r);
			}
			free(big);
		}
	}

	/* Large input grows the buffer. */
	report_init(&r);
	for (int i = 0; i < 2000; i++)
		report_append_masked(&r, "line rtsp://a:b@h/x\n");
	ASSERT(!r.oom);
	ASSERT(strstr(r.data, "a:b@") == NULL);
	ASSERT(strlen(r.data) == r.len);
	report_free(&r);
}

int main(void)
{
	test_sps_parse();
	test_sps_invalid();
	test_sps_extradata();
	test_stream_verdict();
	test_error_hints();
	test_board();
	test_configtxt();
	test_gpu_need();
	test_dmesg();
	test_perm();
	test_cfg_scan();
	test_cfg_checks();
	test_cfg_append();
	test_report_masking();
	return test_summary("test_clilogic");
}
