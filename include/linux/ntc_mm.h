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
#include <linux/ntc_trace.h>

struct ntc_mm_free_entry {
	struct ntc_mm_free_entry *next;
};

struct ntc_fixed_mm {
	struct ntc_mm_free_entry *free;
	spinlock_t lock;
};

struct ntc_mm {
	void *memory;
	void *end;
	void *brk;
	struct idr fixed;
	spinlock_t lock;
};

static inline int _ntc_mm_round_size(int size)
{
	BUILD_BUG_ON((sizeof(struct ntc_mm_free_entry) - 1) &
		sizeof(struct ntc_mm_free_entry));
	return ALIGN(size, sizeof(struct ntc_mm_free_entry));
}

static inline void *_ntc_mm_round_up_ptr(void *ptr)
{
	return (void *)ALIGN((long)ptr, sizeof(struct ntc_mm_free_entry));
}

static inline void ntc_mm_fixed_init(struct ntc_fixed_mm *fixed)
{
	fixed->free = NULL;
	spin_lock_init(&fixed->lock);
}

static inline void *ntc_mm_fixed_alloc(struct ntc_fixed_mm *fixed)
{
	struct ntc_mm_free_entry *free;
	unsigned long irqflags;

	spin_lock_irqsave(&fixed->lock, irqflags);
	free = fixed->free;
	if (free)
		fixed->free = free->next;
	else
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
	fixed->free = free;
	spin_unlock_irqrestore(&fixed->lock, irqflags);
}

static inline int ntc_mm_init(struct ntc_mm *mm, void *memory, size_t size)
{
	void *m;
	void *end;

	spin_lock_init(&mm->lock);

	m = _ntc_mm_round_up_ptr(memory);
	size -= (m - memory);
	memory = m;

	end = memory + size;
	if (end < memory)
		return -EINVAL;

	mm->memory = memory;
	mm->brk = memory;
	mm->end = end;

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
	return idr_find(&mm->fixed, size);
}

static inline struct ntc_fixed_mm *
_ntc_mm_get_fixed(struct ntc_mm *mm, int size, gfp_t gfp)
{
	struct ntc_fixed_mm *fixed;
	int rc;

	fixed = _ntc_mm_find_fixed(mm, size);
	if (likely(fixed))
		return fixed;

	if (size <= 0)
		return ERR_PTR(-EINVAL);

	fixed = kmalloc(sizeof(*fixed), gfp);
	if (!fixed)
		return ERR_PTR(-ENOMEM);
	ntc_mm_fixed_init(fixed);

	rc = idr_alloc(&mm->fixed, fixed, size, size + 1, gfp);
	if (rc < 0) {
		kfree(fixed);

		if (rc == -ENOSPC) {
			fixed = _ntc_mm_find_fixed(mm, size);
			if (fixed)
				return fixed;
			else
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

	size = _ntc_mm_round_size(size);

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
	pr_info("mm %p: added %d buffers of size %d. brk is %ld",
		mm, num_els, size, mm->brk - mm->memory);

	return num_els;
}

static inline void *ntc_mm_alloc(struct ntc_mm *mm, int size, gfp_t gfp)
{
	struct ntc_fixed_mm *fixed;
	void *ptr;

	size = _ntc_mm_round_size(size);

	fixed = _ntc_mm_get_fixed(mm, size, gfp);
	if (unlikely(IS_ERR(fixed)))
		return fixed;

	ptr = ntc_mm_fixed_alloc(fixed);
	if (likely(!IS_ERR(ptr))) {
		if (gfp & __GFP_ZERO)
			memset(ptr, 0, size);
		return ptr;
	}

	ptr = ntc_mm_sbrk(mm, size);
	if (unlikely(IS_ERR(ptr)))
		return ptr;

	if (gfp & __GFP_ZERO)
		memset(ptr, 0, size);

	TRACE("mm %p: added buffer of size %d. brk is %ld",
		mm, size, mm->brk - mm->memory);
	pr_info("mm %p: added buffer of size %d. brk is %ld",
		mm, size, mm->brk - mm->memory);

	return ptr;
}

static inline void ntc_mm_free(struct ntc_mm *mm, void *ptr, int size)
{
	struct ntc_fixed_mm *fixed;

	size = _ntc_mm_round_size(size);

	fixed = _ntc_mm_find_fixed(mm, size);
	if (likely(fixed))
		ntc_mm_fixed_free(fixed, ptr);
	else
		WARN_ON(!fixed);
}

#endif
