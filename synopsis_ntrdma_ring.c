#include <stdio.h>

#define NTRDMA_OS_H
typedef unsigned bool;
typedef unsigned ntrdma_u32_t;

#include "ntrdma_ring.h"

#define CAP 13

ntrdma_u32_t prod;
ntrdma_u32_t cons;
ntrdma_u32_t cmpl;

bool foo_ring_produce(int howmany)
{
	ntrdma_u32_t start, pos, end, base;

	printf("produce howmany %d\n", howmany);
	printf("produce prod %d cmpl %d cap %d\n", prod, cmpl, CAP);

	while (howmany) {
		ntrdma_ring_produce(prod, cmpl, CAP, &start, &end, &base);
		printf("produce start %d end %d base %d\n", start, end, base);

		if (start == end) {
			printf("produce work queue overflow %d\n", howmany);
			break;
		}

		for (pos = start; howmany && pos < end; ++pos, --howmany)
			printf("produce pos %d\n", pos);

		prod = ntrdma_ring_update(pos, base, CAP);
		printf("produce update prod %d\n", prod);
	}

	printf("\n");
	return howmany;
}

bool foo_ring_consume(void)
{
	ntrdma_u32_t start, pos, end, base;

	printf("consume prod %d cons %d cap %d\n", prod, cons, CAP);

	ntrdma_ring_consume(prod, cons, CAP, &start, &end, &base);
	printf("consume start %d end %d base %d\n", start, end, base);

	if (start == end) {
		printf("consume work queue empty\n\n");
		return 0;
	}

	for (pos = start; pos < end; ++pos)
		printf("consume pos %d\n", pos);

	cons = ntrdma_ring_update(pos, base, CAP);
	printf("consume update cons %d\n\n", cons);

	return 1;
}

bool foo_ring_complete(void)
{
	ntrdma_u32_t start, pos, end, base;

	printf("complete cons %d cmpl %d cap %d\n", cons, cmpl, CAP);

	ntrdma_ring_consume(cons, cmpl, CAP, &start, &end, &base);
	printf("complete start %d end %d base %d\n", start, end, base);

	if (start == end) {
		printf("complete work queue empty\n\n");
		return 0;
	}

	for (pos = start; pos < end; ++pos)
		printf("complete pos %d\n", pos);

	cmpl = ntrdma_ring_update(pos, base, CAP);
	printf("complete update cmpl %d\n\n", cmpl);

	return 1;
}

void foo_reset(ntrdma_u32_t vprod, ntrdma_u32_t vcons, ntrdma_u32_t vcmpl)
{
	printf("\nreset prod %d cons %d cmpl %d\n\n", prod, cons, cmpl);
	prod = vprod;
	cons = vcons;
	cmpl = vcmpl;
}

int main(void)
{
	foo_reset(0, 0, 0);
	foo_ring_produce(7);
	foo_ring_consume();
	foo_ring_consume();
	foo_ring_complete();
	foo_ring_complete();

	foo_reset(0, 0, 0);
	foo_ring_produce(7);
	foo_ring_consume();
	foo_ring_complete();
	foo_ring_consume();
	foo_ring_complete();

	foo_reset(7, 7, 7);
	foo_ring_produce(7);
	foo_ring_complete();
	foo_ring_consume();
	foo_ring_complete();
	foo_ring_produce(7);
	foo_ring_consume();
	foo_ring_consume();
	foo_ring_complete();
	foo_ring_produce(7);
	foo_ring_complete();
	foo_ring_consume();
	foo_ring_complete();
	foo_ring_produce(7);
	foo_ring_consume();
	foo_ring_complete();
	foo_ring_produce(7);
	foo_ring_complete();
	foo_ring_consume();
	foo_ring_consume();
	foo_ring_complete();
	foo_ring_complete();

	return 0;
}

