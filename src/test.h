/*
 * test.h — minimal assert macros shared by the unit tests. No framework:
 * a failed check prints file, line and expression and is counted; the test
 * program's exit status reports whether anything failed.
 */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>

static int test_failures = 0;
static int test_checks = 0;

#define ASSERT(expr) do { \
	test_checks++; \
	if (!(expr)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
		test_failures++; \
	} \
} while (0)

#define ASSERT_EQ_I(a, b) do { \
	long long _a = (long long)(a), _b = (long long)(b); \
	test_checks++; \
	if (_a != _b) { \
		fprintf(stderr, "FAIL %s:%d: %s (%lld) != %s (%lld)\n", \
			__FILE__, __LINE__, #a, _a, #b, _b); \
		test_failures++; \
	} \
} while (0)

static inline int test_summary(const char *name)
{
	if (test_failures) {
		fprintf(stderr, "%s: %d of %d checks FAILED\n", name, test_failures, test_checks);
		return 1;
	}
	printf("%s: all %d checks passed\n", name, test_checks);
	return 0;
}

#endif
