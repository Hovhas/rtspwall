/*
 * budget.c — H.264 decoder budget model, see budget.h.
 */
#include <math.h>

#include "budget.h"

int budget_nominal_fps(double fps)
{
	if (!isfinite(fps) || fps <= 0.0 || fps > 1000.0)
		return 0;
	return (int)lround(fps);
}

long budget_stream_mbps(int width, int height, double fps)
{
	int nominal = budget_nominal_fps(fps);
	if (width <= 0 || height <= 0 || nominal <= 0)
		return 0;
	long mb_w = (width + 15) / 16;
	long mb_h = (height + 15) / 16;
	return mb_w * mb_h * nominal;
}

double budget_percent(long mbps)
{
	return (double)mbps * 100.0 / (double)BUDGET_MAX_MBPS;
}

enum budget_verdict budget_verdict(long total_mbps)
{
	if (total_mbps > BUDGET_MAX_MBPS)
		return BUDGET_FAIL;
	if (total_mbps * 100 > BUDGET_MAX_MBPS * BUDGET_WARN_PERCENT)
		return BUDGET_WARN;
	return BUDGET_PASS;
}

const char *budget_verdict_name(enum budget_verdict v)
{
	switch (v) {
	case BUDGET_PASS: return "PASS";
	case BUDGET_WARN: return "WARN";
	case BUDGET_FAIL: return "FAIL";
	}
	return "?";
}
