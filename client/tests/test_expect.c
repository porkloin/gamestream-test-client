/* Tests for the --expect parser. main.c is included with its entry point renamed, so the
 * parser is compiled here and the tests run without a host. */
#define main client_main
#include "../src/main.c"
#undef main

#include <string.h>

static int failures;

#define CHECK(cond)                                                                                                    \
	do {                                                                                                           \
		if (!(cond)) {                                                                                         \
			fprintf(stderr, "fail %s:%d: %s\n", __FILE__, __LINE__, #cond);                               \
			failures++;                                                                                    \
		}                                                                                                          \
	} while (0)

static int parse(const char *expr, const char *name, int op, double value)
{
	struct expectation e;
	if (!parse_expectation(expr, &e))
		return 0;
	return strcmp(e.name, name) == 0 && e.op == op && e.value == value;
}

static void test_parse(void)
{
	CHECK(parse("corrupt==0", "corrupt", OP_EQ, 0));
	CHECK(parse("interval_ms_p95<=41.5", "interval_ms_p95", OP_LE, 41.5));
	CHECK(parse("frames>=120", "frames", OP_GE, 120));
	CHECK(parse("missing<1", "missing", OP_LT, 1));
	CHECK(parse("recovery_frames>0", "recovery_frames", OP_GT, 0));
	/* The one-character operators must not win over the two-character ones. */
	CHECK(parse("dropped_packets<=0", "dropped_packets", OP_LE, 0));
	CHECK(parse("dropped_packets<0", "dropped_packets", OP_LT, 0));
}

static void test_rejects(void)
{
	struct expectation e;
	const char *bad[] = {
		"frames", "frames=", "frames==", "frames==x", "frames==1x", "==1", "frames==1 ", "1<1<1", ""
	};
	for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
		CHECK(!parse_expectation(bad[i], &e));
}

static void test_holds(void)
{
	CHECK(holds(OP_LE, 0, 0));
	CHECK(!holds(OP_LE, 1, 0));
	CHECK(holds(OP_GE, 1, 0));
	CHECK(holds(OP_GT, 1, 0));
	CHECK(!holds(OP_GT, 0, 0));
	CHECK(holds(OP_EQ, 2, 2));
	CHECK(!holds(OP_EQ, 2, 3));
	CHECK(holds(OP_LT, 0, 1));
}

int main(void)
{
	test_parse();
	test_rejects();
	test_holds();
	if (failures > 0) {
		fprintf(stderr, "%d check(s) failed\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
