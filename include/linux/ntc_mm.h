/*
 * Copyright (c) 2019 Dell Technologies.  All rights reserved.
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

#ifndef _NTC_MM_H_
#define _NTC_MM_H_

#include <linux/idr.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/kernel.h>
#include "ntc_trace.h"

#define FREE_MAGIC (0xdeadc0de)
#define FIXED_MAGIC (0xbed00bad)
struct ntc_mm_free_entry {
	struct ntc_mm_free_entry *next;
	u32 magic;
};

struct ntc_fixed_mm {
	struct ntc_mm_free_entry *free;
	spinlock_t lock;
	u32 magic;
};

struct ntc_mm {
	void *memory;
	void *end;
	void *brk;
	struct idr fixed;
	spinlock_t lock;
	struct mutex idr_lock;
};

static inline int _ntc_mm_chunk_size(int size)
{
	int clz;

	BUILD_BUG_ON((sizeof(struct ntc_mm_free_entry) - 1) &
		sizeof(struct ntc_mm_free_entry));

	if (unlikely(!size))
		return 0;

	size = ALIGN(size, sizeof(struct ntc_mm_free_entry));
	clz = __builtin_clz(size); /* The number of leading zeros in size. */

	if (unlikely(clz < 2))
		return 0;

	if (size << (clz + 1) != 0)
		return 1 << (8 * sizeof(int) - clz);
	else
		return 1 << (8 * sizeof(int) - clz - 1);
}

static inline void *_ntc_mm_round_up_ptr(void *ptr)
{
	return (void *)ALIGN((long)ptr, sizeof(struct ntc_mm_free_entry));
}

static inline void ntc_mm_fixed_init(struct ntc_fixed_mm *fixed)
{
	fixed->free = NULL;
	fixed->magic = FIXED_MAGIC;
	spin_lock_init(&fixed->lock);
}

static inline void *ntc_mm_fixed_alloc(struct ntc_fixed_mm *fixed)
{
	struct ntc_mm_free_entry *free;
	unsigned long irqflags;

	spin_lock_irqsave(&fixed->lock, irqflags);
	free = fixed->free;
	if (free) {
		BUG_ON(free->magic != FREE_MAGIC);
		fixed->free = free->next;
	} else
		free = ERR_PTR(-ENOMEM);
	spin_unlock_irqrestore(&fixed->lock, irqflags);

	return free;
}

static inline void ntc_mm_fixed_free(struct ntc_fixed_mm *fixed, void *ptr)
{
	struct ntc_mm_free_entry *free = ptr;
	unsigned long irqflags;

	spin_lock_irqsave(&fixed->lock, irqflags);
	free->next = fixed->free;
	free->magic = FREE_MAGIC;
	fixed->free = free;
	spin_unlock_irqrestore(&fixed->lock, irqflags);
}

static inline int ntc_mm_init(struct ntc_mm *mm, void *memory, size_t size)
{
	void *m;
	void *end;

	spin_lock_init(&mm->lock);

	m = _ntc_mm_round_up_ptr(memory);
	if (size)
		size -= (m - memory);
	memory = m;

	end = memory + size;
	if (end < memory)
		return -EINVAL;

	mm->memory = memory;
	mm->brk = memory;
	mm->end = end;

	mutex_init(&mm->idr_lock);
	idr_init(&mm->fixed);

	return 0;
}

static inline void ntc_mm_deinit(struct ntc_mm *mm)
{
	struct ntc_fixed_mm *fixed;
	int size;

	idr_for_each_entry(&mm->fixed, fixed, size)
		kfree(fixed);

	idr_destroy(&mm->fixed);
	mutex_destroy(&mm->idr_lock);
}

static inline void *ntc_mm_sbrk(struct ntc_mm *mm, int inc)
{
	void *result;
	void *brk;
	unsigned long irqflags;

	if (unlikely(inc < 0))
		return ERR_PTR(-EINVAL);

	spin_lock_irqsave(&mm->lock, irqflags);

	result = mm->brk;
	brk = result + inc;

	if (unlikely(brk < result)) {
		result = ERR_PTR(-ENOMEM);
		goto out;
	}

	if (unlikely(brk > mm->end)) {
		result = ERR_PTR(-ENOMEM);
		goto out;
	}

	mm->brk = brk;

 out:
	spin_unlock_irqrestore(&mm->lock, irqflags);

	return result;
}

static inline struct ntc_fixed_mm *_ntc_mm_find_fixed(struct ntc_mm *mm,
						int size)
{
	struct ntc_fixed_mm *rc;
	rcu_read_lock();
	rc = idr_find(&mm->fixed, size);
	rcu_read_unlock();
	return rc;
}

static inline struct ntc_fixed_mm *
_ntc_mm_get_fixed(struct ntc_mm *mm, int size, gfp_t gfp)
{
	struct ntc_fixed_mm *fixed;
	int rc;

	fixed = _ntc_mm_find_fixed(mm, size);
	if (likely(fixed)) {
		BUG_ON(fixed->magic != FIXED_MAGIC);
		return fixed;
	}

	if (size <= 0)
		return ERR_PTR(-EINVAL);

	fixed = kmalloc(sizeof(*fixed), gfp);
	if (!fixed)
		return ERR_PTR(-ENOMEM);
	gfp &= ~__GFP_ZERO;
	ntc_mm_fixed_init(fixed);

	mutex_lock(&mm->idr_lock);
	rc = idr_alloc(&mm->fixed, fixed, size, size + 1, gfp);
	mutex_unlock(&mm->idr_lock);
	if (rc < 0) {
		kfree(fixed);

		if (rc == -ENOSPC) {
			fixed = _ntc_mm_find_fixed(mm, size);
			if (fixed) {
				BUG_ON(fixed->magic != FIXED_MAGIC);
				return fixed;
			} else
				return ERR_PTR(-EFAULT);
		}

		return ERR_PTR(rc);
	}

	return fixed;
}

static inline int ntc_mm_preinit(struct ntc_mm *mm, int size, int num_els)
{
	struct ntc_fixed_mm *fixed;
	void *ptr;
	int i;

	size = _ntc_mm_chunk_size(size);

	fixed = _ntc_mm_get_fixed(mm, size, GFP_KERNEL);
	if (IS_ERR(fixed))
		return PTR_ERR(fixed);

	for (i = 0; i < num_els; i++) {
		ptr = ntc_mm_sbrk(mm, size);
		if (IS_ERR(ptr)) {
			if (i > 0)
				return i;
			else
				return PTR_ERR(ptr);
		}
		ntc_mm_fixed_free(fixed, ptr);
	}

	TRACE("mm %p: added %d buffers of size %d. brk is %ld",
		mm, num_els, size, mm->brk - mm->memory);
	pr_debug("mm %p: added %d buffers of size %d. brk is %ld",
		mm, num_els, size, mm->brk - mm->memory);

	return num_els;
}

static inline void *ntc_mm_alloc(const char *caller, int line,
				struct ntc_mm *mm, int size, gfp_t gfp)
{
	struct ntc_fixed_mm *fixed;
	void *ptr;

	size = _ntc_mm_chunk_size(size);

	fixed = _ntc_mm_get_fixed(mm, size, gfp);
	if (unlikely(IS_ERR(fixed))) {
		TRACE("mm %p from %s:%d: failed to add %d bytes. No fixed mm",
			mm, caller, line, size);
		pr_debug("mm %p from %s:%d: failed to add %d bytes. No fixed mm",
			mm, caller, line, size);
		return fixed;
	}

	ptr = ntc_mm_fixed_alloc(fixed);
	if (likely(!IS_ERR(ptr))) {
		if (gfp & __GFP_ZERO)
			memset(ptr, 0, size);
		return ptr;
	}

	ptr = ntc_mm_sbrk(mm, size);
	if (unlikely(IS_ERR(ptr))) {
		TRACE("mm %p from %s:%d: failed to add %d bytes. brk is %ld",
			mm, caller, line, size, mm->brk - mm->memory);
		pr_debug("mm %p from %s:%d: failed to add %d bytes. brk is %ld",
			mm, caller, line, size, mm->brk - mm->memory);
		return ptr;
	}

	if (gfp & __GFP_ZERO)
		memset(ptr, 0, size);

	TRACE("mm %p from %s:%d: added buffer of size %d. brk is %ld",
		mm, caller, line, size, mm->brk - mm->memory);
	pr_debug("mm %p from %s:%d: added buffer of size %d. brk is %ld",
		mm, caller, line, size, mm->brk - mm->memory);

	return ptr;
}

static inline void ntc_mm_free(struct ntc_mm *mm, void *ptr, int size)
{
	struct ntc_fixed_mm *fixed;

	size = _ntc_mm_chunk_size(size);

	fixed = _ntc_mm_find_fixed(mm, size);
	if (likely(fixed)) {
		BUG_ON(fixed->magic != FIXED_MAGIC);
		ntc_mm_fixed_free(fixed, ptr);
	} else
		WARN_ON(!fixed);
}

#endif
