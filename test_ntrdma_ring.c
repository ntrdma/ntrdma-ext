#include <stdio.h>

#define NTRDMA_OS_H
typedef unsigned bool;
typedef unsigned ntrdma_u32_t;

#include "ntrdma_ring.h"

bool test_ring_produce(ntrdma_u32_t prod, ntrdma_u32_t cons, ntrdma_u32_t cap,
		       ntrdma_u32_t idx, ntrdma_u32_t end, ntrdma_u32_t base)
{
	ntrdma_u32_t rc_idx, rc_end, rc_base;
	bool test;

	ntrdma_ring_produce(prod, cons, cap, &rc_idx, &rc_end, &rc_base);

	test = rc_idx == idx && rc_end == end && rc_base == base;

	printf("ntrdma_ring_produce(%u,%u,%u) = %u,%u,%u [%u,%u,%u] .. %s\n",
	       prod, cons, cap, idx, end, base,
	       rc_idx, rc_end, rc_base,
	       test ? "ok" : "fail");

	return !test;
}

bool test_ring_consume(ntrdma_u32_t prod, ntrdma_u32_t cons, ntrdma_u32_t cap,
		       ntrdma_u32_t idx, ntrdma_u32_t end, ntrdma_u32_t base)
{
	ntrdma_u32_t rc_idx, rc_end, rc_base;
	bool test;

	ntrdma_ring_consume(prod, cons, cap, &rc_idx, &rc_end, &rc_base);

	test = rc_idx == idx && rc_end == end && rc_base == base;

	printf("ntrdma_ring_consume(%u,%u,%u) = %u,%u,%u [%u,%u,%u] .. %s\n",
	       prod, cons, cap, idx, end, base,
	       rc_idx, rc_end, rc_base,
	       test ? "ok" : "fail");

	return !test;
}

bool test_ring_update(ntrdma_u32_t idx, ntrdma_u32_t base, ntrdma_u32_t cap, ntrdma_u32_t valid)
{
	ntrdma_u32_t rc;
	bool test;

	rc = ntrdma_ring_update(idx, base, cap);

	test = rc == valid;

	printf("ntrdma_ring_update(%u,%u,%u) = %u [%u] .. %s\n",
	       idx, base, cap, valid, rc, test ? "ok" : "fail");

	return !test;
}

bool test_ring_count(ntrdma_u32_t prod, ntrdma_u32_t cons, ntrdma_u32_t cap, ntrdma_u32_t valid)
{
	ntrdma_u32_t rc;
	bool test;

	rc = ntrdma_ring_count(prod, cons, cap);

	test = rc == valid;

	printf("ntrdma_ring_count(%u,%u,%u) = %u [%u] .. %s\n",
	       prod, cons, cap, valid, rc, test ? "ok" : "fail");

	return !test;
}

bool test_ring_space(ntrdma_u32_t prod, ntrdma_u32_t cons, ntrdma_u32_t cap, ntrdma_u32_t valid)
{
	ntrdma_u32_t rc;
	bool test;

	rc = ntrdma_ring_space(prod, cons, cap);

	test = rc == valid;

	printf("ntrdma_ring_space(%u,%u,%u) = %u [%u] .. %s\n",
	       prod, cons, cap, valid, rc, test ? "ok" : "fail");

	return !test;
}

bool test_ring_count_ctg(ntrdma_u32_t prod, ntrdma_u32_t cons, ntrdma_u32_t cap, ntrdma_u32_t valid)
{
	ntrdma_u32_t rc;
	bool test;

	rc = ntrdma_ring_count_ctg(prod, cons, cap);

	test = rc == valid;

	printf("ntrdma_ring_count_ctg(%u,%u,%u) = %u [%u] .. %s\n",
	       prod, cons, cap, valid, rc, test ? "ok" : "fail");

	return !test;
}

bool test_ring_space_ctg(ntrdma_u32_t prod, ntrdma_u32_t cons, ntrdma_u32_t cap, ntrdma_u32_t valid)
{
	ntrdma_u32_t rc;
	bool test;

	rc = ntrdma_ring_space_ctg(prod, cons, cap);

	test = rc == valid;

	printf("ntrdma_ring_space_ctg(%u,%u,%u) = %u [%u] .. %s\n",
	       prod, cons, cap, valid, rc, test ? "ok" : "fail");

	return !test;
}

bool test_ring_valid(ntrdma_u32_t prod, ntrdma_u32_t cons, ntrdma_u32_t cap, bool valid)
{
	bool test, rc;

	rc = ntrdma_ring_valid(prod, cons, cap);

	test = rc == valid;

	printf("ntrdma_ring_valid(%u,%u,%u) = %d [%d] .. %s\n",
	       prod, cons, cap, valid, rc, test ? "ok" : "fail");

	return !test;
}

#define TEST(fn, ...) do { if (fn(__VA_ARGS__)) ++fail; ++count; } while(0)
#define false 0
#define true !false

int main(void)
{
	int fail = 0, count = 0;

	TEST(test_ring_valid, 0, 0, 0,  false);

	TEST(test_ring_valid, 0, 0, 1,  true);

	TEST(test_ring_valid, 0, 0, 5,  true);
	TEST(test_ring_valid, 5, 0, 5,  true);
	TEST(test_ring_valid, 5, 5, 5,  true);
	TEST(test_ring_valid, 6, 0, 5,  false);
	TEST(test_ring_valid, 6, 1, 5,  true);
	TEST(test_ring_valid, 6, 4, 5,  true);
	TEST(test_ring_valid, 6, 5, 5,  true);
	TEST(test_ring_valid, 9, 5, 5,  true);
	TEST(test_ring_valid, 0, 5, 5,  true);
	TEST(test_ring_valid, 0, 6, 5,  true);
	TEST(test_ring_valid, 0, 9, 5,  true);
	TEST(test_ring_valid, 1, 5, 5,  false);
	TEST(test_ring_valid, 1, 6, 5,  true);
	TEST(test_ring_valid, 4, 9, 5,  true);
	TEST(test_ring_valid, 4, 8, 5,  false);
	TEST(test_ring_valid, 5, 9, 5,  false);
	TEST(test_ring_valid, 4, 0, 5,  true);
	TEST(test_ring_valid, 10, 10, 5,  false);

	TEST(test_ring_count, 0, 0, 5,  0);
	TEST(test_ring_count, 5, 0, 5,  5);
	TEST(test_ring_count, 5, 5, 5,  0);
	TEST(test_ring_count, 6, 1, 5,  5);
	TEST(test_ring_count, 6, 4, 5,  2);
	TEST(test_ring_count, 6, 5, 5,  1);
	TEST(test_ring_count, 9, 5, 5,  4);
	TEST(test_ring_count, 0, 5, 5,  5);
	TEST(test_ring_count, 0, 6, 5,  4);
	TEST(test_ring_count, 0, 9, 5,  1);
	TEST(test_ring_count, 1, 6, 5,  5);
	TEST(test_ring_count, 4, 9, 5,  5);
	TEST(test_ring_count, 4, 0, 5,  4);

	/* ring count should work for overflow, too,
	 * because it is used to determine ring_valid() */
	TEST(test_ring_count, 6, 0, 5,  6);
	TEST(test_ring_count, 9, 0, 5,  9);
	TEST(test_ring_count, 1, 5, 5,  6);
	TEST(test_ring_count, 4, 5, 5,  9);

	TEST(test_ring_space, 0, 0, 5,  5);
	TEST(test_ring_space, 5, 0, 5,  0);
	TEST(test_ring_space, 5, 5, 5,  5);
	TEST(test_ring_space, 6, 1, 5,  0);
	TEST(test_ring_space, 6, 4, 5,  3);
	TEST(test_ring_space, 6, 5, 5,  4);
	TEST(test_ring_space, 9, 5, 5,  1);
	TEST(test_ring_space, 0, 5, 5,  0);
	TEST(test_ring_space, 0, 6, 5,  1);
	TEST(test_ring_space, 0, 9, 5,  4);
	TEST(test_ring_space, 1, 6, 5,  0);
	TEST(test_ring_space, 4, 9, 5,  0);
	TEST(test_ring_space, 4, 0, 5,  1);

	TEST(test_ring_count_ctg, 0, 0, 5,  0);
	TEST(test_ring_count_ctg, 5, 0, 5,  5);
	TEST(test_ring_count_ctg, 5, 5, 5,  0);
	TEST(test_ring_count_ctg, 6, 1, 5,  4);
	TEST(test_ring_count_ctg, 6, 4, 5,  1);
	TEST(test_ring_count_ctg, 6, 5, 5,  1);
	TEST(test_ring_count_ctg, 9, 5, 5,  4);
	TEST(test_ring_count_ctg, 0, 5, 5,  5);
	TEST(test_ring_count_ctg, 0, 6, 5,  4);
	TEST(test_ring_count_ctg, 0, 9, 5,  1);
	TEST(test_ring_count_ctg, 1, 6, 5,  4);
	TEST(test_ring_count_ctg, 4, 9, 5,  1);
	TEST(test_ring_count_ctg, 4, 0, 5,  4);

	TEST(test_ring_space_ctg, 0, 0, 5,  5);
	TEST(test_ring_space_ctg, 5, 0, 5,  0);
	TEST(test_ring_space_ctg, 5, 5, 5,  5);
	TEST(test_ring_space_ctg, 6, 1, 5,  0);
	TEST(test_ring_space_ctg, 6, 4, 5,  3);
	TEST(test_ring_space_ctg, 6, 5, 5,  4);
	TEST(test_ring_space_ctg, 9, 5, 5,  1);
	TEST(test_ring_space_ctg, 0, 5, 5,  0);
	TEST(test_ring_space_ctg, 0, 6, 5,  1);
	TEST(test_ring_space_ctg, 0, 9, 5,  4);
	TEST(test_ring_space_ctg, 1, 6, 5,  0);
	TEST(test_ring_space_ctg, 4, 9, 5,  0);
	TEST(test_ring_space_ctg, 4, 0, 5,  1);

	TEST(test_ring_update, 0, 0, 5,  0);
	TEST(test_ring_update, 0, 5, 5,  5);
	TEST(test_ring_update, 1, 0, 5,  1);
	TEST(test_ring_update, 1, 5, 5,  6);
	TEST(test_ring_update, 4, 0, 5,  4);
	TEST(test_ring_update, 4, 5, 5,  9);
	TEST(test_ring_update, 5, 0, 5,  5);
	TEST(test_ring_update, 5, 5, 5,  0);

	TEST(test_ring_consume, 0, 0, 5,  0, 0, 0);
	TEST(test_ring_consume, 5, 0, 5,  0, 5, 0);
	TEST(test_ring_consume, 5, 5, 5,  0, 0, 5);
	TEST(test_ring_consume, 6, 1, 5,  1, 5, 0);
	TEST(test_ring_consume, 6, 4, 5,  4, 5, 0);
	TEST(test_ring_consume, 6, 5, 5,  0, 1, 5);
	TEST(test_ring_consume, 9, 5, 5,  0, 4, 5);
	TEST(test_ring_consume, 0, 5, 5,  0, 5, 5);
	TEST(test_ring_consume, 0, 6, 5,  1, 5, 5);
	TEST(test_ring_consume, 0, 9, 5,  4, 5, 5);
	TEST(test_ring_consume, 1, 6, 5,  1, 5, 5);
	TEST(test_ring_consume, 4, 9, 5,  4, 5, 5);
	TEST(test_ring_consume, 4, 0, 5,  0, 4, 0);

	TEST(test_ring_produce, 0, 0, 5,  0, 5, 0);
	TEST(test_ring_produce, 5, 0, 5,  0, 0, 5);
	TEST(test_ring_produce, 5, 5, 5,  0, 5, 5);
	TEST(test_ring_produce, 6, 1, 5,  1, 1, 5);
	TEST(test_ring_produce, 6, 4, 5,  1, 4, 5);
	TEST(test_ring_produce, 6, 5, 5,  1, 5, 5);
	TEST(test_ring_produce, 9, 5, 5,  4, 5, 5);
	TEST(test_ring_produce, 0, 5, 5,  0, 0, 0);
	TEST(test_ring_produce, 0, 6, 5,  0, 1, 0);
	TEST(test_ring_produce, 0, 9, 5,  0, 4, 0);
	TEST(test_ring_produce, 1, 6, 5,  1, 1, 0);
	TEST(test_ring_produce, 4, 9, 5,  4, 4, 0);
	TEST(test_ring_produce, 4, 0, 5,  4, 5, 0);

	printf("\ncount: %d\npass: %d\nfail: %d\n",
	       count, count - fail, fail);

	return !!fail;
}
