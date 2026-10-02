/*
 * clilogic.c — pure helpers behind the user commands, see clilogic.h.
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "clilogic.h"
#include "layout.h"

/* ================================================================ H.264 SPS */

/* Bit reader over an RBSP (emulation prevention bytes already removed).
 * Reading past the end sets `overrun` and returns zeros, so the parser can
 * run to completion and check once. */
struct bits {
	const unsigned char *p;
	size_t len, pos;   /* pos in bits */
	int overrun;
};

static unsigned bit1(struct bits *b)
{
	if (b->pos >= b->len * 8) {
		b->overrun = 1;
		return 0;
	}
	unsigned v = (b->p[b->pos / 8] >> (7 - b->pos % 8)) & 1u;
	b->pos++;
	return v;
}

static uint32_t bitsn(struct bits *b, int n)
{
	uint32_t v = 0;
	while (n--)
		v = (v << 1) | bit1(b);
	return v;
}

/* Unsigned Exp-Golomb; values beyond 32 bits are treated as invalid. */
static uint32_t ue(struct bits *b)
{
	int zeros = 0;
	while (!bit1(b)) {
		if (b->overrun || ++zeros > 31) {
			b->overrun = 1;
			return 0;
		}
	}
	return ((1u << zeros) - 1) + bitsn(b, zeros);
}

/* Signed Exp-Golomb, clamped to +-65536: no syntax element the parser
 * reads is legitimately larger (delta_scale is -128..127, the POC offsets
 * are only skipped), and the clamp keeps every later sum far from int
 * overflow on hostile input. */
#define SE_LIMIT 65536

static int32_t se(struct bits *b)
{
	uint32_t k = ue(b);
	int64_t v = (k & 1) ? (int64_t)(((uint64_t)k + 1) / 2) : -(int64_t)(k / 2);
	if (v > SE_LIMIT)
		v = SE_LIMIT;
	if (v < -SE_LIMIT)
		v = -SE_LIMIT;
	return (int32_t)v;
}

static void skip_scaling_list(struct bits *b, int size)
{
	int last = 8, next = 8;
	for (int j = 0; j < size && !b->overrun; j++) {
		if (next != 0)   /* (last + delta + 256) % 256, for any clamped delta */
			next = ((last + se(b)) % 256 + 256) % 256;
		last = next == 0 ? last : next;
	}
}

int h264_parse_sps(const unsigned char *nal, size_t len, struct h264_sps_info *out)
{
	unsigned char rbsp[256];
	size_t n = 0;

	memset(out, 0, sizeof *out);
	if (!nal || len < 4 || (nal[0] & 0x1f) != 7)
		return -1;

	/* Strip emulation prevention (00 00 03 -> 00 00). An SPS is small;
	 * anything beyond 256 bytes is only VUI tail we may not need. */
	int zeros = 0;
	for (size_t i = 1; i < len && n < sizeof rbsp; i++) {
		if (zeros >= 2 && nal[i] == 3) {
			zeros = 0;
			continue;
		}
		zeros = nal[i] == 0 ? zeros + 1 : 0;
		rbsp[n++] = nal[i];
	}

	struct bits b = { rbsp, n, 0, 0 };
	int profile = (int)bitsn(&b, 8);
	int constraints = (int)bitsn(&b, 8);
	int level = (int)bitsn(&b, 8);
	uint32_t sps_id = ue(&b);
	if (sps_id > 31)
		return -1;

	int chroma = 1, bit_depth = 8, separate_planes = 0;
	switch (profile) {
	case 100: case 110: case 122: case 244: case 44: case 83: case 86:
	case 118: case 128: case 138: case 139: case 134: case 135:
		chroma = (int)ue(&b);
		if (chroma > 3)
			return -1;
		if (chroma == 3)
			separate_planes = (int)bit1(&b);
		bit_depth = (int)ue(&b) + 8;
		(void)ue(&b);              /* bit_depth_chroma_minus8 */
		(void)bit1(&b);            /* qpprime_y_zero_transform_bypass */
		if (bit1(&b)) {            /* seq_scaling_matrix_present */
			int lists = chroma != 3 ? 8 : 12;
			for (int i = 0; i < lists; i++)
				if (bit1(&b))
					skip_scaling_list(&b, i < 6 ? 16 : 64);
		}
		break;
	default:
		break;
	}

	(void)ue(&b);                      /* log2_max_frame_num_minus4 */
	uint32_t poc_type = ue(&b);
	if (poc_type == 0) {
		(void)ue(&b);              /* log2_max_pic_order_cnt_lsb_minus4 */
	} else if (poc_type == 1) {
		(void)bit1(&b);
		(void)se(&b);
		(void)se(&b);
		uint32_t cycle = ue(&b);
		if (cycle > 255)
			return -1;
		for (uint32_t i = 0; i < cycle && !b.overrun; i++)
			(void)se(&b);
	} else if (poc_type > 2) {
		return -1;
	}
	(void)ue(&b);                      /* max_num_ref_frames */
	(void)bit1(&b);                    /* gaps_in_frame_num_value_allowed */
	uint32_t w_mbs = ue(&b) + 1;
	uint32_t h_units = ue(&b) + 1;
	int frame_mbs_only = (int)bit1(&b);
	if (!frame_mbs_only)
		(void)bit1(&b);            /* mb_adaptive_frame_field */
	(void)bit1(&b);                    /* direct_8x8_inference */
	uint32_t crop_l = 0, crop_r = 0, crop_t = 0, crop_b = 0;
	if (bit1(&b)) {
		crop_l = ue(&b);
		crop_r = ue(&b);
		crop_t = ue(&b);
		crop_b = ue(&b);
	}
	if (b.overrun || w_mbs > 1024 || h_units > 1024)
		return -1;

	int sub_w = 1, sub_h = 1;
	if (chroma == 1) {
		sub_w = 2;
		sub_h = 2;
	} else if (chroma == 2) {
		sub_w = 2;
	}
	int crop_x = (chroma == 0 || separate_planes) ? 1 : sub_w;
	int crop_y = ((chroma == 0 || separate_planes) ? 1 : sub_h) * (2 - frame_mbs_only);
	/* In 64 bits: crop_l + crop_r can wrap in 32 (0xfffffffe + 2 = 0),
	 * which would read as "not cropped". The crop must leave a picture. */
	uint64_t coded_w = (uint64_t)w_mbs * 16;
	uint64_t coded_h = (uint64_t)(2 - frame_mbs_only) * h_units * 16;
	uint64_t cut_w = (uint64_t)crop_x * ((uint64_t)crop_l + crop_r);
	uint64_t cut_h = (uint64_t)crop_y * ((uint64_t)crop_t + crop_b);
	if (cut_w >= coded_w || cut_h >= coded_h)
		return -1;
	long width = (long)(coded_w - cut_w);
	long height = (long)(coded_h - cut_h);

	double fps = 0.0;
	if (bit1(&b)) {                    /* vui_parameters_present */
		if (bit1(&b)) {            /* aspect_ratio_info_present */
			if (bitsn(&b, 8) == 255)
				(void)bitsn(&b, 32);   /* sar_width, sar_height */
		}
		if (bit1(&b))              /* overscan_info_present */
			(void)bit1(&b);
		if (bit1(&b)) {            /* video_signal_type_present */
			(void)bitsn(&b, 4);
			if (bit1(&b))
				(void)bitsn(&b, 24);
		}
		if (bit1(&b)) {            /* chroma_loc_info_present */
			(void)ue(&b);
			(void)ue(&b);
		}
		if (bit1(&b)) {            /* timing_info_present */
			uint32_t units = bitsn(&b, 32);
			uint32_t scale = bitsn(&b, 32);
			if (!b.overrun && units > 0 && scale > 0)
				fps = (double)scale / (2.0 * units);
		}
		/* A truncated VUI only loses the frame rate, not the SPS. */
		if (b.overrun || fps > 240.0)
			fps = 0.0;
	}

	out->profile_idc = profile;
	out->constraint_flags = constraints;
	out->level_idc = level;
	out->width = (int)width;
	out->height = (int)height;
	out->chroma_format_idc = chroma;
	out->bit_depth = bit_depth;
	out->fps = fps;
	return 0;
}

int h264_parse_extradata(const unsigned char *d, size_t len, struct h264_sps_info *out)
{
	if (!d || len < 4)
		return -1;

	if (d[0] == 1) {                   /* avcC */
		if (len < 8)
			return -1;
		int nsps = d[5] & 0x1f;
		size_t pos = 6;
		for (int i = 0; i < nsps; i++) {
			if (pos + 2 > len)
				return -1;
			size_t n = ((size_t)d[pos] << 8) | d[pos + 1];
			pos += 2;
			if (pos + n > len)
				return -1;
			if (h264_parse_sps(d + pos, n, out) == 0)
				return 0;
			pos += n;
		}
		return -1;
	}

	/* Annex B: walk the start codes. */
	size_t i = 0;
	while (i + 3 <= len) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			size_t start = i + 3, end = start;
			while (end + 3 <= len && !(d[end] == 0 && d[end + 1] == 0 &&
						    (d[end + 2] == 1 || (d[end + 2] == 0 && end + 3 < len
									     && d[end + 3] == 1))))
				end++;
			if (end + 3 > len)
				end = len;
			if (start < end && (d[start] & 0x1f) == 7 &&
			    h264_parse_sps(d + start, end - start, out) == 0)
				return 0;
			i = end;
		} else {
			i++;
		}
	}
	return -1;
}

const char *h264_profile_name(int profile, int constraints)
{
	switch (profile) {
	case 66:  return (constraints & 0x40) ? "Constrained Baseline" : "Baseline";
	case 77:  return "Main";
	case 88:  return "Extended";
	case 100: return "High";
	case 110: return "High 10";
	case 122: return "High 4:2:2";
	case 244: return "High 4:4:4";
	case 44:  return "CAVLC 4:4:4";
	default:  return "unknown";
	}
}

/* ================================================================= verdicts */

#define CAMERAS_DOC "docs/cameras.md"

enum budget_verdict probe_stream_verdict(const struct probe_stream *s, char *hint, size_t hintlen)
{
	hint[0] = '\0';

	switch (s->codec) {
	case PROBE_CODEC_HEVC:
		snprintf(hint, hintlen,
			 "H.265 is not supported: switch this stream to H.264 / use the sub-stream ("
			 CAMERAS_DOC ")");
		return BUDGET_FAIL;
	case PROBE_CODEC_OTHER:
		snprintf(hint, hintlen,
			 "only H.264 is supported: switch this stream to H.264 / use the sub-stream ("
			 CAMERAS_DOC ")");
		return BUDGET_FAIL;
	case PROBE_CODEC_UNKNOWN:
		snprintf(hint, hintlen, "could not determine the codec; only H.264 works");
		return BUDGET_WARN;
	case PROBE_CODEC_H264:
		break;
	}

	if (s->width > PROBE_MAX_DIMENSION || s->height > PROBE_MAX_DIMENSION) {
		snprintf(hint, hintlen,
			 "%dx%d is larger than the decoder's 1920x1920 limit: use the sub-stream ("
			 CAMERAS_DOC ")", s->width, s->height);
		return BUDGET_FAIL;
	}
	switch (s->profile_idc) {
	case 110: case 122: case 244: case 44:
		snprintf(hint, hintlen,
			 "H.264 %s profile is not supported by the hardware decoder: set the camera "
			 "to Main or High profile", h264_profile_name(s->profile_idc, 0));
		return BUDGET_FAIL;
	default:
		break;
	}
	if (s->width <= 0 || s->height <= 0) {
		snprintf(hint, hintlen, "could not read the resolution; decoder budget not checked");
		return BUDGET_WARN;
	}
	if (budget_nominal_fps(s->fps) <= 0) {
		snprintf(hint, hintlen, "could not read the frame rate; decoder budget not checked");
		return BUDGET_WARN;
	}

	long mbps = budget_stream_mbps(s->width, s->height, s->fps);
	enum budget_verdict v = budget_verdict(mbps);
	if (v == BUDGET_FAIL)
		snprintf(hint, hintlen,
			 "this stream alone needs %.0f %% of the decoder: lower its resolution or "
			 "frame rate, or use the sub-stream", budget_percent(mbps));
	else if (v == BUDGET_WARN)
		snprintf(hint, hintlen,
			 "this stream alone needs %.0f %% of the decoder: little room for other "
			 "cameras", budget_percent(mbps));
	return v;
}

static bool is_unifi(const char *url)
{
	return url && (strstr(url, ":7441/") || strstr(url, ":7447/"));
}

const char *probe_error_hint(enum probe_error err, const char *masked_url)
{
	switch (err) {
	case PROBE_OK:
		return "";
	case PROBE_ERR_AUTH:
		return "check user name and password; special characters in the password must be "
		       "percent-encoded in the URL: @ -> %40, : -> %3A, / -> %2F, # -> %23, "
		       "? -> %3F, | -> %7C, % -> %25 (" CAMERAS_DOC ")";
	case PROBE_ERR_NOT_FOUND:
		if (is_unifi(masked_url))
			return "wrong path: copy the RTSP URL again in UniFi Protect (camera -> "
			       "Settings -> Advanced -> RTSP); the token changes when RTSP is "
			       "turned off and on";
		return "the server answered, but the path is wrong: check the stream path for "
		       "your camera brand (" CAMERAS_DOC ")";
	case PROBE_ERR_REFUSED:
		return "connection refused: RTSP probably disabled on the camera/NVR, or wrong port "
		       "(554 is the usual one; UniFi Protect uses 7441 for rtsps and 7447 for rtsp)";
	case PROBE_ERR_TIMEOUT:
		return "no answer within the timeout: check the IP address, that the Pi can reach "
		       "the camera's network/VLAN, and firewall rules";
	case PROBE_ERR_UNREACHABLE:
		return "host not reachable or name not resolved: check the IP address/host name";
	case PROBE_ERR_NO_VIDEO:
		return "the stream has no video track: pick another stream or channel";
	case PROBE_ERR_OTHER:
		return "could not read the stream description: check the URL (" CAMERAS_DOC ")";
	}
	return "check the URL";
}

const char *probe_error_label(enum probe_error err)
{
	switch (err) {
	case PROBE_OK:              return "ok";
	case PROBE_ERR_AUTH:        return "login failed (401)";
	case PROBE_ERR_NOT_FOUND:   return "not found (404)";
	case PROBE_ERR_REFUSED:     return "connection refused";
	case PROBE_ERR_TIMEOUT:     return "timeout";
	case PROBE_ERR_UNREACHABLE: return "host unreachable";
	case PROBE_ERR_NO_VIDEO:    return "no video stream";
	case PROBE_ERR_OTHER:       return "error";
	}
	return "error";
}

enum probe_error probe_error_from_status(int status)
{
	switch (status) {
	case 401: case 403: case 407:
		return PROBE_ERR_AUTH;
	case 404: case 454:
		return PROBE_ERR_NOT_FOUND;
	default:
		return PROBE_ERR_OTHER;
	}
}

/* ============================================================ doctor helpers */

enum board_kind board_classify(const char *model)
{
	static const char pi[] = "Raspberry Pi ";
	const char *p = model ? strstr(model, pi) : NULL;
	if (!p)
		return BOARD_UNKNOWN;
	p += sizeof pi - 1;

	if (strncmp(p, "Compute Module ", 15) == 0) {
		p += 15;
		if (*p == '4')
			return BOARD_PI4_FAMILY;
		if (*p == '5')
			return BOARD_PI5;
		return BOARD_OLDER_PI;
	}
	if (strncmp(p, "400", 3) == 0)
		return BOARD_PI4_FAMILY;
	if (strncmp(p, "500", 3) == 0)
		return BOARD_PI5;
	if (p[0] == '4' && (p[1] == ' ' || p[1] == '\0'))
		return BOARD_PI4;
	if (p[0] == '5' && (p[1] == ' ' || p[1] == '\0'))
		return BOARD_PI5;
	return BOARD_OLDER_PI;
}

/* config.txt, line by line, with the conditional filters evaluated for a
 * Raspberry Pi 4 Model B (see clilogic.h). */
enum tri { TRI_NO, TRI_MAYBE, TRI_YES };

enum gpu_key { KEY_NONE, KEY_GPU_MEM, KEY_GPU_MEM_1024, KEY_COUNT };

static const char *const gpu_key_name[KEY_COUNT] = { "", "gpu_mem", "gpu_mem_1024" };

struct ctxt_state {
	enum tri model;          /* the last model filter ([pi4] yes, [pi5] no) */
	bool none;               /* [none] since the last [all] */
	bool other;              /* a filter doctor cannot evaluate is active */
	char filter[64];         /* the first of those, for the message */
};

struct ctxt_line {
	const char *start, *end; /* line without '\n' (may end in '\r') */
	enum tri applies;
	enum gpu_key key;
	int value;
};

static void ctxt_reset(struct ctxt_state *st)
{
	st->model = TRI_YES;
	st->none = false;
	st->other = false;
	st->filter[0] = '\0';
}

/* [pi4], [pi400], [cm4], [pi02], [pi3+], ... */
static bool is_model_filter(const char *s, size_t n)
{
	if (n < 3 || !((s[0] == 'p' && s[1] == 'i') || (s[0] == 'c' && s[1] == 'm')))
		return false;
	for (size_t i = 2; i < n; i++)
		if (!isalnum((unsigned char)s[i]) && s[i] != '+')
			return false;
	return true;
}

/* s/n: the text between '[' and ']'. */
static void ctxt_section(struct ctxt_state *st, const char *s, size_t n)
{
	if (n == 3 && strncasecmp(s, "all", 3) == 0) {
		ctxt_reset(st);
	} else if (n == 4 && strncasecmp(s, "none", 4) == 0) {
		st->none = true;
	} else if (is_model_filter(s, n)) {
		st->model = n == 3 && strncmp(s, "pi4", 3) == 0 ? TRI_YES : TRI_NO;
	} else {
		/* [HDMI:0], [EDID=...], [gpio4=1], [0x1234abcd], [board-type=...],
		 * [tryboot], anything unknown: may or may not hold on this Pi. */
		st->other = true;
		if (!st->filter[0])
			snprintf(st->filter, sizeof st->filter, "[%.*s]", (int)(n < 60 ? n : 60), s);
	}
}

static const char *next_line(const char *p, struct ctxt_line *l, struct ctxt_state *st)
{
	if (!*p)
		return NULL;
	l->start = p;
	while (*p && *p != '\n')
		p++;
	l->end = p;
	if (*p == '\n')
		p++;

	const char *s = l->start, *e = l->end;
	while (s < e && isspace((unsigned char)*s))
		s++;
	while (e > s && isspace((unsigned char)e[-1]))
		e--;

	l->key = KEY_NONE;
	l->applies = st->none || st->model == TRI_NO ? TRI_NO : st->other ? TRI_MAYBE : TRI_YES;
	if (s < e && *s == '[') {
		const char *close = memchr(s, ']', (size_t)(e - s));
		if (close)
			ctxt_section(st, s + 1, (size_t)(close - s - 1));
		l->applies = TRI_NO;
		return p;
	}
	if (s < e && *s == '#')
		return p;

	const char *eq = memchr(s, '=', (size_t)(e - s));
	if (!eq)
		return p;
	const char *k_end = eq;
	while (k_end > s && isspace((unsigned char)k_end[-1]))
		k_end--;
	size_t kn = (size_t)(k_end - s);
	for (int k = KEY_GPU_MEM; k < KEY_COUNT; k++)
		if (kn == strlen(gpu_key_name[k]) && strncmp(s, gpu_key_name[k], kn) == 0) {
			const char *v = eq + 1;
			while (v < e && isspace((unsigned char)*v))
				v++;
			l->key = (enum gpu_key)k;
			l->value = atoi(v);
		}
	return p;
}

/* Per key: the last definite line, and whether a "maybe" line of the same
 * key came after it. */
struct ctxt_scan {
	bool set[KEY_COUNT];
	int  value[KEY_COUNT];
	const char *start[KEY_COUNT], *end[KEY_COUNT];
	bool maybe_after[KEY_COUNT];
	char maybe_filter[KEY_COUNT][64];
};

static void ctxt_scan(const char *text, struct ctxt_scan *sc)
{
	struct ctxt_state st;
	struct ctxt_line l;

	memset(sc, 0, sizeof *sc);
	ctxt_reset(&st);
	for (const char *p = text; (p = next_line(p, &l, &st)) != NULL;) {
		if (l.key == KEY_NONE)
			continue;
		if (l.applies == TRI_YES) {
			sc->set[l.key] = true;
			sc->value[l.key] = l.value;
			sc->start[l.key] = l.start;
			sc->end[l.key] = l.end;
			sc->maybe_after[l.key] = false;
		} else if (l.applies == TRI_MAYBE) {
			if (!sc->maybe_after[l.key])
				snprintf(sc->maybe_filter[l.key], sizeof sc->maybe_filter[0], "%s",
					 st.filter);
			sc->maybe_after[l.key] = true;
		}
	}
}

static enum gpu_key effective_key(const struct ctxt_scan *sc)
{
	return sc->set[KEY_GPU_MEM_1024] ? KEY_GPU_MEM_1024 :
	       sc->set[KEY_GPU_MEM] ? KEY_GPU_MEM : KEY_NONE;
}

void configtxt_gpu_mem_info(const char *text, struct configtxt_gpu *out)
{
	struct ctxt_scan sc;

	memset(out, 0, sizeof *out);
	ctxt_scan(text, &sc);
	enum gpu_key k = effective_key(&sc);
	out->value = k == KEY_NONE ? -1 : sc.value[k];
	out->key = gpu_key_name[k];
	/* A maybe-gpu_mem_1024 would override anything; a maybe-gpu_mem only
	 * matters while no definite gpu_mem_1024 overrides it. */
	if (sc.maybe_after[KEY_GPU_MEM_1024]) {
		out->uncertain = true;
		snprintf(out->filter, sizeof out->filter, "%s", sc.maybe_filter[KEY_GPU_MEM_1024]);
	} else if (!sc.set[KEY_GPU_MEM_1024] && sc.maybe_after[KEY_GPU_MEM]) {
		out->uncertain = true;
		snprintf(out->filter, sizeof out->filter, "%s", sc.maybe_filter[KEY_GPU_MEM]);
	}
}

int configtxt_gpu_mem(const char *text)
{
	struct configtxt_gpu g;
	configtxt_gpu_mem_info(text, &g);
	return g.value;
}

int configtxt_set_gpu_mem(const char *text, int mb, char *out, size_t outlen)
{
	struct ctxt_scan sc;
	ctxt_scan(text, &sc);
	enum gpu_key k = effective_key(&sc);

	int n;
	if (k != KEY_NONE) {
		const char *end = sc.end[k];
		if (end > sc.start[k] && end[-1] == '\r')
			end--;          /* keep the line's \r\n */
		n = snprintf(out, outlen, "%.*s" "%s=%d" "%s",
			     (int)(sc.start[k] - text), text, gpu_key_name[k], mb, end);
	} else {
		size_t len = strlen(text);
		const char *nl1 = strchr(text, '\n');
		const char *eol = nl1 && nl1 > text && nl1[-1] == '\r' ? "\r\n" : "\n";
		bool nl = len == 0 || text[len - 1] == '\n';
		n = snprintf(out, outlen, "%s%s[all]%sgpu_mem=%d%s", text, nl ? "" : eol, eol, mb,
			     eol);
	}
	return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

int doctor_gpu_mem_needed(long total_mbps, int n_cameras)
{
	(void)n_cameras;
	/* Above half the decoder budget the firmware-side codec buffers outgrow
	 * the 76 MB default ('Not enough GPU mem' in dmesg); 256 MB is the
	 * value used by the reference setups. */
	return budget_percent(total_mbps) > 50.0 ? 256 : 0;
}

static bool contains_ci(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (strncasecmp(hay, needle, n) == 0)
			return true;
	return false;
}

const char *dmesg_match(const char *line)
{
	if (contains_ci(line, "not enough gpu mem"))
		return "Not enough GPU mem (raise gpu_mem)";
	if (contains_ci(line, "vb2") && contains_ci(line, "driver bug"))
		return "vb2 driver bug (decoder buffer handling)";
	if (contains_ci(line, "mmal") && (contains_ci(line, "timeout") || contains_ci(line, "timed out")))
		return "MMAL timeout (decoder firmware did not answer)";
	return NULL;
}

unsigned perm_bits(mode_t mode, uid_t st_uid, gid_t st_gid,
		   uid_t uid, gid_t gid, const gid_t *groups, int ngroups)
{
	if (uid == 0)
		return 7;
	if (uid == st_uid)
		return (mode >> 6) & 7;
	bool in_group = gid == st_gid;
	for (int i = 0; i < ngroups && !in_group; i++)
		in_group = groups[i] == st_gid;
	return in_group ? (mode >> 3) & 7 : mode & 7;
}

bool perm_allows(mode_t mode, uid_t st_uid, gid_t st_gid,
		 uid_t uid, gid_t gid, const gid_t *groups, int ngroups, bool want_write)
{
	unsigned bits = perm_bits(mode, st_uid, st_gid, uid, gid, groups, ngroups);
	if (!(bits & 4))
		return false;
	return !want_write || (bits & 2);
}

/* ============================================================== add helpers */

void cfg_scan_text(const char *text, const char *name, struct cfg_scan *out)
{
	char line[LAYOUT_MAX_LINE];
	static struct config_fields f;   /* ~3.5 KB; not reentrant, CLI only */
	struct config_global g;

	memset(out, 0, sizeof *out);
	for (const char *p = text; *p;) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		if (n >= sizeof line)
			n = sizeof line - 1;   /* over-long: --check-config reports it */
		memcpy(line, p, n);
		line[n] = '\0';
		p = e ? e + 1 : p + strlen(p);

		switch (config_parse_line(line, &g, &f)) {
		case CONFIG_LINE_GLOBAL:
			if (strcmp(g.key, "GRID") == 0) {
				int c, r;
				if (layout_parse_size(g.value, &c, &r) == 0 && c >= 1 && r >= 1 &&
				    c <= LAYOUT_MAX_GRID && r <= LAYOUT_MAX_GRID) {
					out->grid_cols = c;
					out->grid_rows = r;
				}
			}
			break;
		case CONFIG_LINE_CAMERA:
			out->n_cameras++;
			if (name && strcmp(f.field[0], name) == 0)
				out->name_taken = true;
			if (f.count >= 6) {
				out->manual_mode = true;
			} else if (f.count >= 3) {
				char *end;
				long cell = strtol(f.field[2], &end, 10);
				if (*f.field[2] && !*end && cell >= 1 && cell <= 64)
					out->cell_used[cell] = true;
			}
			break;
		default:
			break;
		}
	}
}

int cfg_first_free_cell(const struct cfg_scan *s)
{
	if (s->manual_mode)
		return 0;
	int cells = s->grid_cols * s->grid_rows;
	for (int c = 1; c <= cells && c <= 64; c++)
		if (!s->cell_used[c])
			return c;
	return 0;
}

const char *cfg_check_name(const char *name)
{
	size_t n = strlen(name);
	if (n == 0)
		return "the name is empty";
	if (n >= LAYOUT_NAME_MAX)
		return "the name is too long (max 63 characters)";
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)name[i];
		if (!isalnum(c) && c != '-' && c != '_' && c != '.')
			return "use only letters, digits, '-', '_' and '.' in the name";
	}
	return NULL;
}

const char *cfg_check_url(const char *url)
{
	size_t n = strlen(url);
	if (n == 0)
		return "the URL is empty";
	if (n >= LAYOUT_URL_MAX)
		return "the URL is too long (max 511 characters)";
	if (strchr(url, '|'))
		return "the URL contains '|', which separates the fields of a config line: "
		       "write it as %7C (percent-encoded)";
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)url[i];
		if (c <= 0x20 || c == 0x7f)
			return "the URL contains a space or control character: percent-encode it "
			       "(space -> %20)";
	}
	return NULL;
}

int cfg_append_camera(const char *text, const char *name, const char *url, int cell,
		      char *out, size_t outlen)
{
	size_t len = strlen(text);
	bool nl = len == 0 || text[len - 1] == '\n';
	int n = snprintf(out, outlen, "%s%s%s|%s|%d\n", text, nl ? "" : "\n", name, url, cell);
	return (n < 0 || (size_t)n >= outlen) ? -1 : 0;
}

bool unit_restarted(const char *inv_before, const char *inv_after)
{
	if (!inv_before || !inv_after)
		return false;
	size_t nb = strcspn(inv_before, " \t\r\n"), na = strcspn(inv_after, " \t\r\n");
	if (na == 0)
		return false;
	return na != nb || memcmp(inv_before, inv_after, na) != 0;
}

const char *url_secret_reason(const char *url)
{
	const char *sch = strstr(url, "://");
	if (!sch)
		return NULL;                       /* a local file */
	const char *auth = sch + 3;
	size_t an = strcspn(auth, "/?#");
	if (memchr(auth, '@', an))
		return "a user name/password";
	if (strpbrk(auth + an, "?#"))
		return "a query string";
	char masked[LAYOUT_URL_MAX];
	layout_mask_url(url, masked, sizeof masked);
	if (strcmp(masked, url) != 0)
		return "a token-like path segment";
	return NULL;
}

/* =========================================================== report masking */

/* Key names that mark the value as a secret (case-insensitive). Long
 * words match anywhere in the key ("loginPassword", "x_signature"); the
 * short ones only as a whole component between '_', '-' and '.' ("sig",
 * "api-key", "Auth") or, for key/pwd/psw, at the end of one ("apikey",
 * "accesskey", "userpwd") — so "signal" or "author" stay readable. */
static bool secret_key(const char *k, size_t n)
{
	static const char *const anywhere[] = { "pass", "token", "secret", "cred", "apikey",
						"api_key", "signature", "loginpas" };
	static const char *const component[] = { "pwd", "psw", "key", "sig", "auth" };
	char low[64];
	if (n == 0 || n >= sizeof low)
		return false;
	for (size_t i = 0; i < n; i++)
		low[i] = (char)tolower((unsigned char)k[i]);
	low[n] = '\0';
	for (size_t i = 0; i < sizeof anywhere / sizeof anywhere[0]; i++)
		if (strstr(low, anywhere[i]))
			return true;
	for (char *c = low; *c;) {
		size_t cn = strcspn(c, "_-.");
		for (size_t i = 0; i < sizeof component / sizeof component[0]; i++) {
			const char *w = component[i];
			size_t wn = strlen(w);
			if (cn == wn && strncmp(c, w, wn) == 0)
				return true;
			if (cn > wn && w[0] != 's' && w[0] != 'a' && strncmp(c + cn - wn, w, wn) == 0)
				return true;     /* ...key, ...pwd, ...psw */
			if (cn > wn && w[0] == 'a' && strncmp(c, w, wn) == 0 &&
			    strncmp(c, "author", 6) != 0)
				return true;     /* authkey, authtoken, ... */
		}
		c += cn;
		if (*c)
			c++;
	}
	return false;
}

void report_mask_line(const char *in, char *out, size_t outlen)
{
	char tmp[4096];

	if (!outlen)
		return;
	layout_mask_urls_in_text(in, tmp, sizeof tmp);

	/* key=VALUE / key: VALUE pairs whose key names a secret, and
	 * "Authorization: ..." headers. */
	size_t o = 0;
	const char *p = tmp;
	while (*p && o + 1 < outlen) {
		if ((*p == '=' || *p == ':') && p > tmp) {
			const char *k = p;
			while (k > tmp && (isalnum((unsigned char)k[-1]) || k[-1] == '_' || k[-1] == '-'))
				k--;
			bool auth = (size_t)(p - k) == 13 && strncasecmp(k, "authorization", 13) == 0;
			if (auth || secret_key(k, (size_t)(p - k))) {
				const char *v = p + 1;
				while (*v == ' ')
					v++;
				/* Already masked by the URL pass, or "://": leave it. */
				if (strncmp(v, "***", 3) != 0 && strncmp(p, "://", 3) != 0 && *v) {
					size_t take = (size_t)(v - p);
					for (size_t i = 0; i < take && o + 1 < outlen; i++)
						out[o++] = p[i];
					const char *mask = "***";
					for (size_t i = 0; mask[i] && o + 1 < outlen; i++)
						out[o++] = mask[i];
					if (auth)
						v += strlen(v);
					else
						while (*v && !isspace((unsigned char)*v) && !strchr("&;,\"'", *v))
							v++;
					p = v;
					continue;
				}
			}
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
	layout_sanitize_log_text(out);
}

void report_init(struct report_buf *r)
{
	memset(r, 0, sizeof *r);
}

void report_free(struct report_buf *r)
{
	free(r->data);
	memset(r, 0, sizeof *r);
}

static void report_put(struct report_buf *r, const char *s, size_t n)
{
	if (r->oom)
		return;
	if (r->len + n + 1 > r->cap) {
		size_t cap = r->cap ? r->cap : 4096;
		while (r->len + n + 1 > cap)
			cap *= 2;
		char *d = realloc(r->data, cap);
		if (!d) {
			r->oom = true;
			return;
		}
		r->data = d;
		r->cap = cap;
	}
	memcpy(r->data + r->len, s, n);
	r->len += n;
	r->data[r->len] = '\0';
}

void report_section(struct report_buf *r, const char *title)
{
	report_put(r, "\n== ", 4);
	report_put(r, title, strlen(title));
	report_put(r, " ==\n", 4);
}

void report_append_masked(struct report_buf *r, const char *text)
{
	char line[4096], masked[4096];

	for (const char *p = text; *p;) {
		const char *e = strchr(p, '\n');
		size_t n = e ? (size_t)(e - p) : strlen(p);
		size_t keep = n;
		bool cut = false;
		if (n >= sizeof line - 16) {
			/* Never cut inside a URL: "rtsp://user:pass" without its "@host"
			 * would no longer look like userinfo and escape the mask. Cut at
			 * the last whitespace, or drop the whole run. */
			keep = sizeof line - 16;
			while (keep > 0 && !isspace((unsigned char)p[keep]))
				keep--;
			cut = true;
		}
		memcpy(line, p, keep);
		line[keep] = '\0';
		if (cut)
			strcat(line, " [cut]");
		p += n;
		if (*p == '\n')
			p++;
		if (keep && line[keep - 1] == '\r')
			line[keep - 1] = '\0';
		report_mask_line(line, masked, sizeof masked);
		report_put(r, masked, strlen(masked));
		report_put(r, "\n", 1);
	}
}
