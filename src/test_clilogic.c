/*
 * test_clilogic.c — unit tests for the pure helpers behind probe, add and
 * doctor (clilogic.c). The SPS fixtures are real parameter sets produced by
 * ffmpeg/libx264 6.1 for the sizes named.
 */
#include <math.h>
#include <stdint.h>
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
	/* gpu_mem_1024 overrides gpu_mem on every Pi 4 (all have >= 1 GB). */
	ASSERT_EQ_I(configtxt_gpu_mem("gpu_mem_1024=300\n"), 300);

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
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 4, NULL), 256);
	m = 4 * budget_stream_mbps(640, 360, 25);
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 0, NULL), 0);
	m = 5 * budget_stream_mbps(640, 360, 25);   /* demo */
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 0, NULL), 0);
	m = 6 * budget_stream_mbps(1024, 576, 30);
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 0, NULL), 256);
}

/* Seen on a Pi 4 at the default gpu_mem=76: 4 x 1920x1080 (fps not known
 * to the budget) wedged the codec until a reboot ("Not enough GPU mem"),
 * while 4 x 1024x576 at 25-30 fps (~46 % budget) works. Four or more
 * streams of 1080p or larger need 256 MB whatever the frame rate. */
static void test_gpu_need_large_streams(void)
{
	/* fps unknown: the budget is 0, the stream count still decides */
	ASSERT_EQ_I(doctor_gpu_mem_needed(0, 4, NULL), 256);
	ASSERT_EQ_I(doctor_gpu_mem_needed(0, 5, NULL), 256);
	ASSERT_EQ_I(doctor_gpu_mem_needed(0, 3, NULL), 0);
	/* the working 4 x 1024x576 setup: 2 at 25 fps, 2 at 30 fps */
	long m = 2 * budget_stream_mbps(1024, 576, 25) + 2 * budget_stream_mbps(1024, 576, 30);
	ASSERT(budget_percent(m) < 50.0);
	ASSERT_EQ_I(doctor_gpu_mem_needed(m, 0, NULL), 0);
	/* 3 x 1080p at 10 fps is below half the budget and below 4 streams */
	ASSERT_EQ_I(doctor_gpu_mem_needed(3 * budget_stream_mbps(1920, 1080, 10), 3, NULL), 0);

	ASSERT(gpu_mem_large_stream(1920, 1080));
	ASSERT(gpu_mem_large_stream(1080, 1920));     /* portrait */
	ASSERT(gpu_mem_large_stream(2560, 1440));
	ASSERT(gpu_mem_large_stream(3840, 2160));
	ASSERT(!gpu_mem_large_stream(1280, 720));
	ASSERT(!gpu_mem_large_stream(1920, 1072));
	ASSERT(!gpu_mem_large_stream(0, 0));
	ASSERT(!gpu_mem_large_stream(-1920, -1080));
}

/* Which rule fired: add refuses only on the large-stream rule (seen
 * wedging the codec), the budget rule is an unvalidated heuristic. */
static void test_gpu_need_reasons(void)
{
	unsigned why = 99;
	ASSERT_EQ_I(doctor_gpu_mem_needed(0, 0, &why), 0);
	ASSERT_EQ_I(why, GPU_NEED_NONE);
	/* 4 x 1080p, fps unknown: large only */
	why = 99;
	ASSERT_EQ_I(doctor_gpu_mem_needed(0, 4, &why), 256);
	ASSERT_EQ_I(why, GPU_NEED_LARGE);
	/* 4 x 1024x576 at 30 fps = 52.9 %: budget only */
	why = 99;
	ASSERT_EQ_I(doctor_gpu_mem_needed(4 * budget_stream_mbps(1024, 576, 30), 0, &why), 256);
	ASSERT_EQ_I(why, GPU_NEED_BUDGET);
	/* 6 x 1024x576 at 30 fps (~79 %): budget only */
	why = 99;
	ASSERT_EQ_I(doctor_gpu_mem_needed(6 * budget_stream_mbps(1024, 576, 30), 0, &why), 256);
	ASSERT_EQ_I(why, GPU_NEED_BUDGET);
	/* 4 x 1080p at 15 fps (93.8 %): both */
	why = 99;
	ASSERT_EQ_I(doctor_gpu_mem_needed(4 * budget_stream_mbps(1920, 1080, 15), 4, &why), 256);
	ASSERT_EQ_I(why, GPU_NEED_BUDGET | GPU_NEED_LARGE);
	/* exactly 50 % is not above half */
	why = 99;
	ASSERT_EQ_I(doctor_gpu_mem_needed(BUDGET_MAX_MBPS / 2, 3, &why), 0);
	ASSERT_EQ_I(why, GPU_NEED_NONE);
}

static void test_gpu_mem_check(void)
{
	/* nothing needed: OK whatever is known */
	ASSERT_EQ_I(gpu_mem_check(0, -1, -1), GPU_CHECK_OK);
	ASSERT_EQ_I(gpu_mem_check(0, 76, -1), GPU_CHECK_OK);
	/* vcgencmd missing: unknown, never a refusal */
	ASSERT_EQ_I(gpu_mem_check(256, -1, -1), GPU_CHECK_UNKNOWN);
	ASSERT_EQ_I(gpu_mem_check(256, -1, 256), GPU_CHECK_UNKNOWN);
	ASSERT_EQ_I(gpu_mem_check(256, 0, 76), GPU_CHECK_UNKNOWN);
	/* enough */
	ASSERT_EQ_I(gpu_mem_check(256, 256, 256), GPU_CHECK_OK);
	ASSERT_EQ_I(gpu_mem_check(256, 512, -1), GPU_CHECK_OK);
	/* too little */
	ASSERT_EQ_I(gpu_mem_check(256, 76, -1), GPU_CHECK_LOW);
	ASSERT_EQ_I(gpu_mem_check(256, 76, 128), GPU_CHECK_LOW);
	/* config.txt already has enough, not active until a reboot */
	ASSERT_EQ_I(gpu_mem_check(256, 76, 256), GPU_CHECK_LOW_REBOOT);
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
		"front|rtsp://admin:examplepw@192.168.1.10:554/stream1|1\n"
		"unifi|rtsps://192.168.1.1:7441/EXAMPLEtoken1234?enableSrtp|2\n"
		"[12:00:01] ffmpeg: [rtsp @ 0x55] method DESCRIBE failed: 401 Unauthorized "
		"(rtsp://viewer:ex%40ample@10.0.0.7/Streaming/Channels/102)\n"
		"Oct 02 12:00:00 pi sudo[812]:     pi : TTY=pts/0 ; PWD=/home/pi ; USER=root ; "
		"COMMAND=/usr/bin/rtspwall probe rtsp://root:notreal@cam.local/h264Preview_01_sub\n"
		"camera 2: plain rtsp://10.0.0.9:7447/NOTrealToken5678\n"
		"cgi: password=fakepw token=faketoken user=bob\n"
		"evil \x1b[2J escape\n";
	struct report_buf r;

	report_init(&r);
	report_section(&r, "journal");
	report_append_masked(&r, fixture);
	ASSERT(!r.oom);
	ASSERT(r.data != NULL);

	const char *secrets[] = { "examplepw", "EXAMPLEtoken1234", "enableSrtp", "ex%40",
				  "notreal", "NOTrealToken5678", "fakepw", "faketoken", "\x1b" };
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
				strcpy(big + off, "rtsp://admin:fakelongpw@10.1.1.1/stream1 tail\n");
				report_init(&r);
				report_append_masked(&r, big);
				ASSERT(r.data && strstr(r.data, "fakelongpw") == NULL);
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

/* ------------------------------------------------ QA review of Phase A
 * QA_KNOWN_BUGS: see test_layout.c. */

/* Passes today (run under SANITIZE=1): the SPS/extradata parsers stay in
 * bounds on random and truncated input. */
static void qa_test_sps_parser_random_input(void)
{
	unsigned char buf[300];
	struct h264_sps_info info;
	unsigned seed = 12345;

	for (int iter = 0; iter < 200000; iter++) {
		size_t len = (size_t)(seed % sizeof buf);
		for (size_t i = 0; i < len; i++) {
			seed = seed * 1103515245u + 12345u;
			buf[i] = (unsigned char)(seed >> 16);
		}
		seed = seed * 1103515245u + 12345u;
		if (len > 0)
			buf[0] = (iter & 1) ? 0x67 : 0x01;   /* SPS NAL / avcC */
		if (h264_parse_sps(buf, len, &info) == 0) {
			ASSERT(info.width > 0 && info.width <= 1024 * 16);
			ASSERT(info.height > 0 && info.height <= 2 * 1024 * 16);
		}
		(void)h264_parse_extradata(buf, len, &info);
	}
	/* every prefix of a real SPS */
	for (size_t n = 0; n <= sizeof sps_1080p_high_15; n++)
		(void)h264_parse_sps(sps_1080p_high_15, n, &info);
}

static void qa_test_cfg_check_url_rejects_control_chars(void)
{
	ASSERT(cfg_check_url("rtsp://h/a\tb") != NULL);
	ASSERT(cfg_check_url("rtsp://h/a\nb") != NULL);
	ASSERT(cfg_check_url("rtsp://h/a\x1b[31m") != NULL);
	ASSERT(cfg_check_url("rtsp://u:p%7Cq@h/a") == NULL);
}

/* Was a known bug (fixed): on a Pi 4 gpu_mem_1024= overrides gpu_mem=
 * (likewise gpu_mem_256/512 on 256/512 MB boards). doctor read only gpu_mem
 * and --fix wrote gpu_mem=256, which the firmware then ignored. */
static void qa_test_configtxt_gpu_mem_1024_override(void)
{
	char out[256];
	ASSERT_EQ_I(configtxt_set_gpu_mem("gpu_mem_1024=64\n", 256, out, sizeof out), 0);
	ASSERT(strstr(out, "gpu_mem_1024=64") == NULL);
}

/* ------------------------------------------- review fixes (qa + security) */

/* Bit writer for constructed SPS NAL units (the fixtures from the qa and
 * security reviews: out-of-range Exp-Golomb values). */
struct bw {
	unsigned char b[256];
	size_t bits;
};

static void bw_put(struct bw *w, uint64_t v, int n)
{
	while (n-- > 0) {
		if (w->bits / 8 >= sizeof w->b)
			return;
		if ((v >> n) & 1)
			w->b[w->bits / 8] |= (unsigned char)(0x80u >> (w->bits % 8));
		w->bits++;
	}
}

static void bw_ue(struct bw *w, uint32_t v)
{
	uint64_t x = (uint64_t)v + 1;
	int len = 0;
	while ((x >> len) > 1)
		len++;
	bw_put(w, 0, len);
	bw_put(w, x, len + 1);
}

static void bw_se(struct bw *w, int64_t v)
{
	bw_ue(w, v > 0 ? (uint32_t)(2 * v - 1) : (uint32_t)(-2 * v));
}

/* RBSP trailing bits, then NAL header + emulation prevention. */
static size_t bw_nal(struct bw *w, unsigned char *out, size_t cap)
{
	bw_put(w, 1, 1);
	while (w->bits % 8)
		bw_put(w, 0, 1);
	size_t n = 0, zeros = 0;
	out[n++] = 0x67;
	for (size_t i = 0; i < w->bits / 8 && n + 2 < cap; i++) {
		if (zeros >= 2 && w->b[i] <= 3) {
			out[n++] = 3;
			zeros = 0;
		}
		out[n++] = w->b[i];
		zeros = w->b[i] == 0 ? zeros + 1 : 0;
	}
	return n;
}

/* 640x368 coded (40x23 MBs), displayed size set by the
 * crop offsets; profile 100 with one scaling-list delta if scaling != 0. */
static size_t make_sps(unsigned char *out, size_t cap, int profile, const uint32_t crop[4],
		       int64_t scaling)
{
	struct bw w;
	memset(&w, 0, sizeof w);
	bw_put(&w, (uint64_t)profile, 8);
	bw_put(&w, 0, 8);                /* constraint flags */
	bw_put(&w, 30, 8);               /* level 3.0 */
	bw_ue(&w, 0);                    /* sps_id */
	if (profile == 100) {
		bw_ue(&w, 1);            /* chroma 4:2:0 */
		bw_ue(&w, 0);            /* bit depth luma */
		bw_ue(&w, 0);            /* bit depth chroma */
		bw_put(&w, 0, 1);        /* transform bypass */
		bw_put(&w, scaling ? 1 : 0, 1);
		if (scaling) {
			bw_put(&w, 1, 1);        /* list 0 present */
			bw_se(&w, scaling);
			for (int j = 1; j < 16; j++)
				bw_se(&w, 0);
			for (int i = 1; i < 8; i++)
				bw_put(&w, 0, 1);
		}
	}
	bw_ue(&w, 0);                    /* log2_max_frame_num_minus4 */
	bw_ue(&w, 0);                    /* poc type 0 */
	bw_ue(&w, 0);                    /* log2_max_poc_lsb_minus4 */
	bw_ue(&w, 1);                    /* max_num_ref_frames */
	bw_put(&w, 0, 1);                /* gaps */
	bw_ue(&w, 39);                   /* 40 MBs = 640 */
	bw_ue(&w, 22);                   /* 23 MBs = 368 */
	bw_put(&w, 1, 1);                /* frame_mbs_only */
	bw_put(&w, 1, 1);                /* direct_8x8 */
	bw_put(&w, 1, 1);                /* cropping */
	for (int i = 0; i < 4; i++)
		bw_ue(&w, crop[i]);
	bw_put(&w, 0, 1);                /* no VUI */
	return bw_nal(&w, out, cap);
}

static void test_sps_constructed_overflow(void)
{
	unsigned char nal[300];
	struct h264_sps_info s;
	const uint32_t ok_crop[4] = { 0, 0, 0, 4 };
	size_t n;

	n = make_sps(nal, sizeof nal, 66, ok_crop, 0);
	ASSERT_EQ_I(h264_parse_sps(nal, n, &s), 0);
	ASSERT_EQ_I(s.width, 640);
	ASSERT_EQ_I(s.height, 360);

	/* crop_left + crop_right wraps to 0 in uint32 arithmetic: must be
	 * rejected, not read as an uncropped 640 pixels. */
	const uint32_t wrap_lr[4] = { 0xFFFFFFFEu, 2, 0, 0 };
	n = make_sps(nal, sizeof nal, 66, wrap_lr, 0);
	ASSERT_EQ_I(h264_parse_sps(nal, n, &s), -1);
	const uint32_t wrap_tb[4] = { 0, 0, 0xFFFFFFFEu, 2 };
	n = make_sps(nal, sizeof nal, 66, wrap_tb, 0);
	ASSERT_EQ_I(h264_parse_sps(nal, n, &s), -1);
	const uint32_t huge[4] = { 0x7FFFFFFFu, 0, 0, 0 };
	n = make_sps(nal, sizeof nal, 66, huge, 0);
	ASSERT_EQ_I(h264_parse_sps(nal, n, &s), -1);

	/* delta_scale far outside -128..127 (8 + 2147483647 was signed
	 * overflow in skip_scaling_list; run with SANITIZE=1). */
	n = make_sps(nal, sizeof nal, 100, ok_crop, 2147483647);
	ASSERT_EQ_I(h264_parse_sps(nal, n, &s), 0);
	ASSERT_EQ_I(s.width, 640);
	n = make_sps(nal, sizeof nal, 100, ok_crop, -2147483647);
	ASSERT_EQ_I(h264_parse_sps(nal, n, &s), 0);
	ASSERT_EQ_I(s.height, 360);
	n = make_sps(nal, sizeof nal, 100, ok_crop, 5);
	ASSERT_EQ_I(h264_parse_sps(nal, n, &s), 0);
	ASSERT_EQ_I(s.width, 640);
}

static void test_configtxt_variants(void)
{
	struct configtxt_gpu g;

	/* gpu_mem_1024 applies to boards with >= 1 GB (every Pi 4) and
	 * overrides gpu_mem wherever it is in the file. */
	configtxt_gpu_mem_info("gpu_mem=128\ngpu_mem_1024=300\n", &g);
	ASSERT_EQ_I(g.value, 300);
	ASSERT(strcmp(g.key, "gpu_mem_1024") == 0);
	ASSERT(!g.uncertain);
	configtxt_gpu_mem_info("gpu_mem_1024=300\ngpu_mem=128\n", &g);
	ASSERT_EQ_I(g.value, 300);
	/* gpu_mem_256/512 only apply to 256/512 MB boards: never a Pi 4. */
	configtxt_gpu_mem_info("gpu_mem_256=32\ngpu_mem_512=64\ngpu_mem=128\n", &g);
	ASSERT_EQ_I(g.value, 128);
	ASSERT(strcmp(g.key, "gpu_mem") == 0);
	configtxt_gpu_mem_info("[pi5]\ngpu_mem_1024=16\n[all]\ngpu_mem=128\n", &g);
	ASSERT_EQ_I(g.value, 128);
	ASSERT(!g.uncertain);
	configtxt_gpu_mem_info("", &g);
	ASSERT_EQ_I(g.value, -1);
	ASSERT(!g.uncertain);

	/* Stacked filters: [pi4] then [HDMI:0] — both must hold, and doctor
	 * cannot tell whether the HDMI filter matches: "maybe", not PASS. */
	configtxt_gpu_mem_info("[pi4]\n[HDMI:0]\ngpu_mem=256\n", &g);
	ASSERT_EQ_I(g.value, -1);
	ASSERT(g.uncertain);
	ASSERT(strstr(g.filter, "HDMI:0") != NULL);
	configtxt_gpu_mem_info("gpu_mem=256\n[board-type=0x11]\ngpu_mem=64\n", &g);
	ASSERT_EQ_I(g.value, 256);
	ASSERT(g.uncertain);
	ASSERT(strstr(g.filter, "board-type") != NULL);
	configtxt_gpu_mem_info("[gpio4=1]\ngpu_mem=64\n", &g);
	ASSERT(g.uncertain);
	configtxt_gpu_mem_info("[0x12345678]\ngpu_mem_1024=64\n[all]\ngpu_mem=256\n", &g);
	ASSERT_EQ_I(g.value, 256);
	ASSERT(g.uncertain);          /* a maybe-gpu_mem_1024 would override */
	/* A later definite line of the same key settles it. */
	configtxt_gpu_mem_info("[HDMI:0]\ngpu_mem=64\n[all]\ngpu_mem=256\n", &g);
	ASSERT_EQ_I(g.value, 256);
	ASSERT(!g.uncertain);
	/* A model filter that excludes the Pi 4 excludes it whatever else is
	 * stacked on it. */
	configtxt_gpu_mem_info("[pi4]\ngpu_mem=128\n[pi5]\n[HDMI:0]\ngpu_mem=64\n", &g);
	ASSERT_EQ_I(g.value, 128);
	ASSERT(!g.uncertain);
	/* A maybe-gpu_mem under a definite gpu_mem_1024 does not matter. */
	configtxt_gpu_mem_info("[pi4]\ngpu_mem_1024=64\n[HDMI:1]\ngpu_mem=256\n", &g);
	ASSERT_EQ_I(g.value, 64);
	ASSERT(!g.uncertain);
	configtxt_gpu_mem_info("[none]\ngpu_mem=16\n[all]\ngpu_mem=96\n", &g);
	ASSERT_EQ_I(g.value, 96);
	ASSERT(!g.uncertain);

	/* --fix rewrites the line that is in effect, keeping CRLF. */
	char out[512];
	ASSERT_EQ_I(configtxt_set_gpu_mem("gpu_mem=64\ngpu_mem_1024=64\n", 256, out, sizeof out), 0);
	ASSERT(strcmp(out, "gpu_mem=64\ngpu_mem_1024=256\n") == 0);
	ASSERT_EQ_I(configtxt_gpu_mem(out), 256);
	ASSERT_EQ_I(configtxt_set_gpu_mem("gpu_mem=64\r\nb=2\r\n", 256, out, sizeof out), 0);
	ASSERT(strcmp(out, "gpu_mem=256\r\nb=2\r\n") == 0);
	ASSERT_EQ_I(configtxt_set_gpu_mem("a=1\r\n", 256, out, sizeof out), 0);
	ASSERT(strcmp(out, "a=1\r\n[all]\r\ngpu_mem=256\r\n") == 0);
}

static void test_url_secret(void)
{
	ASSERT(url_secret_reason("rtsp://10.0.0.5:554/Streaming/Channels/101") == NULL);
	ASSERT(url_secret_reason("rtsp://cam.local/h264Preview_01_main") == NULL);
	ASSERT(url_secret_reason("/home/pi/clip.mp4") == NULL);
	ASSERT(url_secret_reason("rtsp://admin:pw@10.0.0.5/x") != NULL);
	ASSERT(url_secret_reason("rtsp://admin@10.0.0.5/x") != NULL);
	ASSERT(url_secret_reason("rtsp://10.0.0.5/cam/realmonitor?channel=1") != NULL);
	ASSERT(url_secret_reason("rtsp://10.0.0.5/x#frag") != NULL);
	ASSERT(url_secret_reason("rtsps://192.168.1.1:7441/EXAMPLEtoken1234") != NULL);
	ASSERT(url_secret_reason("rtsp://10.0.0.5/0123456789abcdef0123456789abcdef") != NULL);
}

static void test_report_mask_more_keys(void)
{
	static const char *const masked[] = {
		"pwd=Hx1", "PSW=Hx1", "loginPassword=Hx1", "Auth=Hx1", "api-key=Hx1",
		"sig=Hx1", "SECRET=Hx1", "cred=Hx1", "x_signature=Hx1", "authKey=Hx1",
		"loginpas=Hx1", "accesskey=Hx1",
	};
	char out[256];
	for (size_t i = 0; i < sizeof masked / sizeof masked[0]; i++) {
		report_mask_line(masked[i], out, sizeof out);
		if (strstr(out, "Hx1")) {
			fprintf(stderr, "not masked: %s -> %s\n", masked[i], out);
			ASSERT(0);
		}
	}
	report_mask_line("user=bob signal=11 path=/x", out, sizeof out);
	ASSERT(strcmp(out, "user=bob signal=11 path=/x") == 0);
}

/* B2: add only credits the watcher with a restart when the unit's
 * InvocationID really changed. */
static void test_unit_restarted(void)
{
	ASSERT(unit_restarted("aaaa", "bbbb"));
	ASSERT(unit_restarted("", "bbbb"));             /* first start */
	ASSERT(unit_restarted("aaaa\n", "bbbb\n"));
	ASSERT(!unit_restarted("aaaa", "aaaa"));        /* watcher refused or was blocked */
	ASSERT(!unit_restarted("aaaa\n", "aaaa"));      /* trailing newline ignored */
	ASSERT(!unit_restarted("", ""));
	ASSERT(!unit_restarted("aaaa", ""));            /* stopped: not a restart */
	ASSERT(!unit_restarted(NULL, "bbbb"));          /* unknown before: do not guess */
	ASSERT(!unit_restarted("aaaa", NULL));
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
	test_gpu_need_large_streams();
	test_gpu_need_reasons();
	test_gpu_mem_check();
	test_dmesg();
	test_perm();
	test_cfg_scan();
	test_cfg_checks();
	test_cfg_append();
	test_report_masking();

	/* QA review of Phase A */
	qa_test_sps_parser_random_input();
	qa_test_cfg_check_url_rejects_control_chars();
	qa_test_configtxt_gpu_mem_1024_override();

	/* review fixes */
	test_sps_constructed_overflow();
	test_configtxt_variants();
	test_url_secret();
	test_report_mask_more_keys();
	test_unit_restarted();
	return test_summary("test_clilogic");
}
