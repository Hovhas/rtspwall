/*
 * clilogic.h — pure helpers behind the user commands (probe, add, doctor,
 * demo). No Linux, DRM or FFmpeg dependencies: everything here works on
 * strings and numbers handed in by cli.c/probe.c/add.c/doctor.c, so it is
 * unit-tested by test_clilogic.c on any machine. The only dependency is
 * layout.c (config line parsing and URL masking), which is pure as well.
 */
#ifndef CLILOGIC_H
#define CLILOGIC_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "budget.h"

/* ------------------------------------------------------------- H.264 SPS */

struct h264_sps_info {
	int    profile_idc;      /* 66 baseline, 77 main, 100 high, ... */
	int    constraint_flags; /* constraint_set0..5 flags byte */
	int    level_idc;        /* 42 = level 4.2 */
	int    width, height;    /* displayed size (after cropping) */
	int    chroma_format_idc;
	int    bit_depth;        /* luma */
	double fps;              /* from VUI timing, 0 if not present */
};

/* Parses one SPS NAL unit (starting with the NAL header byte, type 7,
 * emulation prevention bytes still in place). Returns 0, or -1 if it is
 * not an SPS or is truncated/invalid. */
int h264_parse_sps(const unsigned char *nal, size_t len, struct h264_sps_info *out);

/* Finds and parses the first SPS in codec extradata, which is either an
 * avcC record (MP4/Matroska, first byte 1) or Annex B (RTSP
 * sprop-parameter-sets as decoded by libavformat: 00 00 01 / 00 00 00 01
 * start codes). Returns 0, or -1 if no valid SPS is found. */
int h264_parse_extradata(const unsigned char *data, size_t len, struct h264_sps_info *out);

/* "Baseline", "Constrained Baseline", "Main", "High", "High 10", ... */
const char *h264_profile_name(int profile_idc, int constraint_flags);

/* ------------------------------------------------------- probe verdicts */

enum probe_codec {
	PROBE_CODEC_UNKNOWN,
	PROBE_CODEC_H264,
	PROBE_CODEC_HEVC,
	PROBE_CODEC_OTHER,
};

/* Why a probe could not read the stream description. */
enum probe_error {
	PROBE_OK,
	PROBE_ERR_AUTH,        /* 401/403 */
	PROBE_ERR_NOT_FOUND,   /* 404 / 454 Session Not Found */
	PROBE_ERR_REFUSED,     /* TCP connection refused */
	PROBE_ERR_TIMEOUT,     /* no answer within the timeout */
	PROBE_ERR_UNREACHABLE, /* no route / host unreachable / DNS failure */
	PROBE_ERR_NO_VIDEO,    /* answered, but no video stream */
	PROBE_ERR_OTHER,
};

struct probe_stream {
	enum probe_codec codec;
	int    profile_idc, constraint_flags, level_idc;   /* H.264 only, 0 = unknown */
	int    width, height;
	double fps;
};

/* Largest stream the bcm2835-codec decoder accepts in either dimension. */
#define PROBE_MAX_DIMENSION 1920

/* Verdict for ONE stream on its own: FAIL for H.265/other codecs, sizes
 * above 1920x1920, H.264 profiles the decoder cannot do (High 10, 4:2:2,
 * 4:4:4) or a single stream above 100 % of the budget; WARN when the size
 * or frame rate is unknown or the stream alone uses > 90 %; else PASS.
 * A one-line, user-facing hint is written to `hint` (empty on PASS). */
enum budget_verdict probe_stream_verdict(const struct probe_stream *s,
					 char *hint, size_t hintlen);

/* The fix to print for a failed probe. `masked_url` is used to tailor the
 * hint (e.g. UniFi port 7441/7447). Never empty for err != PROBE_OK. */
const char *probe_error_hint(enum probe_error err, const char *masked_url);

/* Short label: "login failed (401)", "not found (404)", ... */
const char *probe_error_label(enum probe_error err);

/* Classifies an RTSP status code (as seen in an error) into a probe_error. */
enum probe_error probe_error_from_status(int status_code);

/* ---------------------------------------------------------- doctor helpers */

enum board_kind {
	BOARD_UNKNOWN,       /* not a Raspberry Pi (e.g. CI) */
	BOARD_PI4,           /* Raspberry Pi 4 Model B */
	BOARD_PI4_FAMILY,    /* Pi 400 / Compute Module 4: same SoC, untested */
	BOARD_PI5,           /* Pi 5 / Pi 500 / CM5: no H.264 hardware decoder */
	BOARD_OLDER_PI,      /* Pi 3 and older: different display stack */
};

/* Classifies the text of /proc/device-tree/model. */
enum board_kind board_classify(const char *model);

/* gpu_mem as the firmware on a Pi 4 would read config.txt.
 *
 * Keys: `gpu_mem_1024=` applies to boards with >= 1 GB RAM — every Pi 4 —
 * and overrides `gpu_mem=` wherever it appears; `gpu_mem_256=` and
 * `gpu_mem_512=` only apply to 256/512 MB boards and are ignored. Within a
 * key the last applicable line wins.
 *
 * Conditional filters stack until [all]: a model filter ([pi4], [pi5],
 * [cm4], ...) replaces the previous model filter, other filters ([HDMI:0],
 * [EDID=...], [gpio4=1], [0xSERIAL], [board-type=...], [tryboot], ...) are
 * added to it. A line applies when every active filter holds. [pi4] holds,
 * other model names and [none] (until [all]) do not, and every filter
 * doctor cannot evaluate counts as "maybe": lines under it are not used
 * for `value` but set `uncertain` when they could change the result, so
 * the caller can WARN instead of reporting a wrong PASS. */
struct configtxt_gpu {
	int  value;              /* effective MB from definite lines, -1 = unset
				    (firmware default, 76 MB on a Pi 4) */
	const char *key;         /* "gpu_mem", "gpu_mem_1024" or "" */
	bool uncertain;          /* a "maybe" line could override value */
	char filter[64];         /* the first such filter, e.g. "[HDMI:0]" */
};

void configtxt_gpu_mem_info(const char *text, struct configtxt_gpu *out);

/* Just the value of configtxt_gpu_mem_info. */
int configtxt_gpu_mem(const char *text);

/* Writes `text` to `out` with the effective gpu_mem set to `mb`: the
 * definite line in effect (gpu_mem_1024 if set, else gpu_mem; see
 * configtxt_gpu_mem_info) is rewritten in place, keeping its line ending
 * (\r\n stays \r\n); otherwise "[all]\ngpu_mem=MB\n" is appended (an [all]
 * header is needed because the file may end inside a conditional section;
 * \r\n if the file uses \r\n). Nothing else changes. Returns 0, or -1 if
 * `out` is too small. */
int configtxt_set_gpu_mem(const char *text, int mb, char *out, size_t outlen);

/* gpu_mem in MB the camera load needs, or 0 if the firmware default
 * (76 MB) is enough. `total_mbps` is the budget sum (budget.h) of the
 * streams that could be measured; `n_large` counts the H.264 streams of
 * 1080p or larger (gpu_mem_large_stream), frame rate known or not.
 *
 * 256 MB when the load is above half the decoder budget, or with 4 or more
 * large streams. Evidence (Pi 4, gpu_mem=76): 4 x 1920x1080 wedged the
 * codec until a reboot ("Not enough GPU mem"); 4 x 1024x576 at 25-30 fps
 * (~46 %) works.
 *
 * `why` (may be NULL) gets the rules that fired, GPU_NEED_* flags. The
 * large-stream rule matches what wedged the codec on hardware; the budget
 * rule is an unvalidated heuristic, so add only refuses on GPU_NEED_LARGE
 * (doctor WARNs on both). */
#define GPU_MEM_LARGE_COUNT 4
#define GPU_NEED_NONE   0u
#define GPU_NEED_BUDGET 1u      /* above half the decoder budget */
#define GPU_NEED_LARGE  2u      /* GPU_MEM_LARGE_COUNT+ streams of >= 1080p */
int doctor_gpu_mem_needed(long total_mbps, int n_large, unsigned *why);

/* True for a stream of at least 1920x1080 pixels (either orientation)
 * that the wall decodes: at most 1920 in both dimensions (larger streams
 * are refused, PACING_FAULT_TOO_LARGE, and hold no decoder memory). */
bool gpu_mem_large_stream(int width, int height);

enum gpu_check {
	GPU_CHECK_OK,          /* nothing needed, or the active value is enough */
	GPU_CHECK_UNKNOWN,     /* needed, but the active value is unknown */
	GPU_CHECK_LOW,         /* the active value is too low */
	GPU_CHECK_LOW_REBOOT,  /* too low, but config.txt already has enough:
				  active after a reboot */
};

/* Compares the need (doctor_gpu_mem_needed) with the active gpu_mem
 * (`live`, from vcgencmd; <= 0 = unknown) and config.txt's value
 * (`configtxt`, -1 = unset). An unknown active value is never LOW. */
enum gpu_check gpu_mem_check(int need, int live, int configtxt);

/* dmesg patterns worth reporting: returns a short label for the line, or
 * NULL. Patterns: "Not enough GPU mem", vb2 "driver bug", MMAL timeouts. */
const char *dmesg_match(const char *line);

/* Unix permission check: can a process with uid/gid/supplementary groups
 * open a file with st_mode/st_uid/st_gid for reading (and writing if
 * want_write)? uid 0 can always. ACLs are not considered. */
/* The rwx bits (4/2/1) that apply to that process: owner, group or other
 * class, as the kernel picks them (no ACLs). uid 0 gets 7. */
unsigned perm_bits(mode_t st_mode, uid_t st_uid, gid_t st_gid,
		   uid_t uid, gid_t gid, const gid_t *groups, int ngroups);

bool perm_allows(mode_t st_mode, uid_t st_uid, gid_t st_gid,
		 uid_t uid, gid_t gid, const gid_t *groups, int ngroups, bool want_write);

/* ------------------------------------------------------------ add helpers */

/* What add needs to know about an existing config file. */
struct cfg_scan {
	int  grid_cols, grid_rows;   /* 0/0 = no GRID line */
	bool manual_mode;            /* a camera line with 6-7 fields was seen */
	int  n_cameras;              /* active (uncommented) camera lines */
	bool cell_used[64 + 1];      /* 1-based; grid is at most 8x8 */
	bool name_taken;             /* `name` passed to cfg_scan() exists */
};

/* Scans a config file's text for GRID, used cells and whether `name` is
 * already in use (name may be NULL). Never fails: lines that do not parse
 * are skipped (--check-config reports those). */
void cfg_scan_text(const char *text, const char *name, struct cfg_scan *out);

/* First free cell of the grid (1-based), or 0 if the grid is full or no
 * GRID is set. */
int cfg_first_free_cell(const struct cfg_scan *s);

/* Validates a camera name for a config line: 1-63 characters from
 * [A-Za-z0-9._-]. Returns NULL if fine, else the reason. */
const char *cfg_check_name(const char *name);

/* Validates a URL for a config line: non-empty, shorter than the config's
 * URL limit, no '|' (field separator; the message suggests %7C), no
 * whitespace or control characters. Returns NULL if fine, else the reason. */
const char *cfg_check_url(const char *url);

/* Returns `text` with "name|url|cell\n" appended, making sure the
 * previous last line ends with a newline. Nothing else is touched.
 * Returns 0, or -1 if `out` is too small. */
int cfg_append_camera(const char *text, const char *name, const char *url, int cell,
		      char *out, size_t outlen);

/* Why a URL must not be given on the command line (argv ends up in shell
 * history, `ps` and sudo's log): "a user name/password" for userinfo,
 * "a query string" for ?/#, "a token-like path segment" for what
 * layout_mask_url would mask. NULL if the URL carries none of these. */
const char *url_secret_reason(const char *url);

/* `systemctl show -p InvocationID --value` before and after: true only if
 * the unit got a new, non-empty invocation (it was (re)started in
 * between). NULL (could not be read) or an unchanged/empty ID is false.
 * Trailing whitespace is ignored. */
bool unit_restarted(const char *inv_before, const char *inv_after);

/* --------------------------------------------------------- report masking */

/* A growable text buffer for doctor --report. Every line that goes in is
 * masked (all scheme:// URLs through layout_mask_urls_in_text, plus
 * password/token/secret=VALUE pairs) and control characters are replaced,
 * so nothing appended can leak a credential or inject escape sequences. */
struct report_buf {
	char  *data;
	size_t len, cap;
	bool   oom;     /* an allocation failed; the content is truncated */
};

void report_init(struct report_buf *r);
void report_free(struct report_buf *r);
/* Appends "== title ==\n" (unmasked; titles are constant strings). */
void report_section(struct report_buf *r, const char *title);
/* Appends `text` (any number of lines), masking each line. */
void report_append_masked(struct report_buf *r, const char *text);

/* Masks one line in place-to-out: URLs plus key=value secrets. */
void report_mask_line(const char *in, char *out, size_t outlen);

#endif
