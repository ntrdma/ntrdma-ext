/*
 * Copyright (c) 2014, 2015 EMC Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifndef NTC_TRACE_H
#define NTC_TRACE_H

#include <linux/ftrace.h>
#include <linux/timex.h>
#include <linux/types.h>
#include <linux/smp.h>

#define TRACE_EN

#ifdef TRACE_EN
#define TRACE(fmt, ...) do {						\
		char _______FMT[] = __stringify(fmt);			\
		int _______SIZE = sizeof(_______FMT);			\
		if ((_______SIZE >= 4) &&				\
			(_______FMT[_______SIZE - 4] == '\\') &&	\
			(_______FMT[_______SIZE - 3] == 'n'))		\
			trace_printk(fmt, ##__VA_ARGS__);		\
		else							\
			trace_printk(fmt "\n", ##__VA_ARGS__);		\
	} while (0)
#else
#define TRACE(...) do {} while (0)
#endif

#ifdef TRACE_DATA_ENABLE
#define TRACE_DATA(...) TRACE(__VA_ARGS__)
#else
#define TRACE_DATA(...) do {} while (0)
#endif

struct ntc_perf_tracker {
	cycles_t total;
	cycles_t total_out;
	cycles_t last;
	u64 num_calls;
};

struct ntc_perf_tracker_current {
	struct ntc_perf_tracker *tracker;
	cycles_t start;
	const char *prefix;
	u64 window;
};

static inline cycles_t ntc_get_cycles(void)
{
	asm volatile ("":::"memory");
	return get_cycles();
}

static inline void ntc_perf_finish_measure(struct ntc_perf_tracker_current *c)
{
	struct ntc_perf_tracker *t = c->tracker;

	if (likely(t->last || t->num_calls))
		t->total_out += c->start - t->last;

	t->num_calls++;
	t->last = ntc_get_cycles();
	t->total += t->last - c->start;

	if (t->num_calls != c->window)
		return;

	pr_info("PERF: %s [%d]: %lld calls. %lld%% of time. %lld cyc average.",
		c->prefix, smp_processor_id(), t->num_calls,
		t->total * 100 / (t->total + t->total_out),
		t->total / t->num_calls);

	t->num_calls = 0;
	t->total_out = 0;
	t->total = 0;
}

#define NTC_PERF_TRACK

#ifdef NTC_PERF_TRACK

#define DEFINE_NTC_PERF_TRACKER(name, p, w)				\
	static DEFINE_PER_CPU(struct ntc_perf_tracker, name##_per_cpu);	\
	struct ntc_perf_tracker_current name =				\
	{ .start = ntc_get_cycles(), .prefix = p, .window = w }

#define NTC_PERF_MEASURE(name) do {					\
		name.tracker = get_cpu_ptr(&name##_per_cpu);		\
		ntc_perf_finish_measure(&name);				\
		put_cpu_ptr(&name##_per_cpu);				\
	} while (0)

#else
#define DEFINE_NTC_PERF_TRACKER(name, p, w) int name  __attribute__ ((unused))
#define NTC_PERF_MEASURE(name) do {} while (0)
#endif


#define DEFINE_NTC_FUNC_PERF_TRACKER(name, w)		\
	DEFINE_NTC_PERF_TRACKER(name, __func__, w)

#endif
