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

#endif
