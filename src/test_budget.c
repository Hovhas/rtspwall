/*
 * test_budget.c — unit tests for the decoder budget model (budget.c),
 * including the golden configurations from the MMP-6 calibration.
 */
#include <math.h>

#include "budget.h"
#include "test.h"

static long sum(int n, int w, int h, double fps)
{
	return n * budget_stream_mbps(w, h, fps);
}

static int pct10(long mbps)   /* percent with one decimal, as an integer x10 */
{
	return (int)lround(budget_percent(mbps) * 10.0);
}

static void test_nominal_fps(void)
{
	ASSERT_EQ_I(budget_nominal_fps(30.0), 30);
	ASSERT_EQ_I(budget_nominal_fps(29.97), 30);
	ASSERT_EQ_I(budget_nominal_fps(14.985), 15);
	ASSERT_EQ_I(budget_nominal_fps(59.94), 60);
	ASSERT_EQ_I(budget_nominal_fps(25.0), 25);
	ASSERT_EQ_I(budget_nominal_fps(0.0), 0);
	ASSERT_EQ_I(budget_nominal_fps(-5.0), 0);
	ASSERT_EQ_I(budget_nominal_fps(NAN), 0);
	ASSERT_EQ_I(budget_nominal_fps(INFINITY), 0);
}

static void test_stream_mbps(void)
{
	/* 1080 is not a multiple of 16: 68 macroblock rows (1088 coded). */
	ASSERT_EQ_I(budget_stream_mbps(1920, 1080, 15), 120L * 68 * 15);
	ASSERT_EQ_I(budget_stream_mbps(640, 360, 25), 40L * 23 * 25);
	ASSERT_EQ_I(budget_stream_mbps(1920, 1080, 29.97), 120L * 68 * 30);
	ASSERT_EQ_I(budget_stream_mbps(0, 1080, 30), 0);
	ASSERT_EQ_I(budget_stream_mbps(1920, 0, 30), 0);
	ASSERT_EQ_I(budget_stream_mbps(1920, 1080, 0), 0);
}

static void test_golden_configs(void)
{
	long m;

	m = sum(4, 1920, 1080, 15);                /* README / production */
	ASSERT_EQ_I(pct10(m), 938);
	ASSERT_EQ_I(budget_verdict(m), BUDGET_WARN);

	m = sum(6, 1024, 576, 30);                 /* reference setup */
	ASSERT_EQ_I(pct10(m), 794);
	ASSERT_EQ_I(budget_verdict(m), BUDGET_PASS);

	m = sum(6, 1280, 720, 30);
	ASSERT_EQ_I((int)lround(budget_percent(m)), 124);
	ASSERT_EQ_I(budget_verdict(m), BUDGET_FAIL);

	m = sum(4, 640, 360, 25);                  /* demo, 4 tiles */
	ASSERT_EQ_I((int)lround(budget_percent(m)), 18);
	ASSERT(budget_percent(m) > 17.0 && budget_percent(m) < 18.0);
	ASSERT_EQ_I(budget_verdict(m), BUDGET_PASS);

	m = sum(5, 640, 360, 25);                  /* demo, all 5 clips */
	ASSERT(budget_percent(m) <= 30.0);
	ASSERT_EQ_I(budget_verdict(m), BUDGET_PASS);

	m = sum(6, 1120, 630, 30);                 /* emteria measurement */
	ASSERT_EQ_I(pct10(m), 965);
	ASSERT_EQ_I(budget_verdict(m), BUDGET_WARN);
}

static void test_thresholds(void)
{
	/* Exactly 90 % passes, one macroblock more warns; exactly 100 % warns,
	 * one more fails. */
	long ninety = BUDGET_MAX_MBPS * 90 / 100;
	ASSERT_EQ_I(BUDGET_MAX_MBPS * 90 % 100, 0);
	ASSERT_EQ_I(budget_verdict(ninety), BUDGET_PASS);
	ASSERT_EQ_I(budget_verdict(ninety + 1), BUDGET_WARN);
	ASSERT_EQ_I(budget_verdict(BUDGET_MAX_MBPS), BUDGET_WARN);
	ASSERT_EQ_I(budget_verdict(BUDGET_MAX_MBPS + 1), BUDGET_FAIL);
	ASSERT_EQ_I(budget_verdict(0), BUDGET_PASS);
}

static void test_names(void)
{
	ASSERT(budget_verdict_name(BUDGET_PASS)[0] == 'P');
	ASSERT(budget_verdict_name(BUDGET_WARN)[0] == 'W');
	ASSERT(budget_verdict_name(BUDGET_FAIL)[0] == 'F');
}

int main(void)
{
	test_nominal_fps();
	test_stream_mbps();
	test_golden_configs();
	test_thresholds();
	test_names();
	return test_summary("test_budget");
}
