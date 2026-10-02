/*
 * budget.h — pure H.264 decoder budget model (no Linux/FFmpeg dependencies,
 * unit-tested by test_budget.c).
 *
 * The Pi 4's bcm2835-codec H.264 decoder is shared by every camera, shown or
 * not (rotation members decode all the time too). Its load is modelled as
 * macroblocks per second: ceil(W/16) * ceil(H/16) * nominal fps, summed over
 * ALL cameras in the config. 100 % is the H.264 level 4.2 limit MaxMBPS
 * 522 240 (1920x1080 at ~64 fps).
 *
 *   PASS  <= 90 %
 *   WARN  >  90 % and <= 100 %   (works, but little headroom)
 *   FAIL  > 100 %                (overload can wedge the decoder until reboot)
 *
 * Calibration: 4x1920x1080@15 = 93.8 % WARN, 6x1024x576@30 = 79.4 % PASS,
 * 6x1280x720@30 = 124 % FAIL, the 4x640x360@25 demo ~ 17.6 %.
 */
#ifndef BUDGET_H
#define BUDGET_H

#define BUDGET_MAX_MBPS     522240L   /* H.264 level 4.2 MaxMBPS = 100 % */
#define BUDGET_WARN_PERCENT 90

enum budget_verdict {
	BUDGET_PASS,
	BUDGET_WARN,
	BUDGET_FAIL,
};

/* The camera's nominal frame rate: fps rounded to the nearest integer, so
 * 29.97 -> 30, 14.985 -> 15, 59.94 -> 60. Returns 0 for fps <= 0 or
 * non-finite input (unknown). */
int budget_nominal_fps(double fps);

/* Macroblocks per second of one stream, or 0 if any input is unknown (<= 0). */
long budget_stream_mbps(int width, int height, double fps);

/* Share of the decoder for `mbps` macroblocks/s, in percent. */
double budget_percent(long mbps);

/* Verdict for a total load in macroblocks/s (integer comparison, no
 * floating point rounding at the thresholds). */
enum budget_verdict budget_verdict(long total_mbps);

/* "PASS", "WARN" or "FAIL". */
const char *budget_verdict_name(enum budget_verdict v);

#endif
