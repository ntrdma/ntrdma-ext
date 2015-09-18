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

#ifndef NTRDMA_OS_LINUX_H
#define NTRDMA_OS_LINUX_H

#ifdef NTRDMA_OS_DEFINED
#error "there can be only one"
#else
#define NTRDMA_OS_DEFINED
#endif

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/device.h>
#include <linux/printk.h>

/* Debugging */

#ifdef CONFIG_NTRDMA_DEBUG
#define NTRDMA_HARD_ASSERT(expr) BUG_ON(!(expr))
#define NTRDMA_SOFT_ASSERT(expr) WARN_ON(!(expr))
#define NTRDMA_RETURN_ASSERT(expr, ret) \
	do { if (WARN_ON(!(expr))) { return ret; } } while (0)
#else
#define NTRDMA_HARD_ASSERT(expr) do {} while (0)
#define NTRDMA_SOFT_ASSERT(expr) do {} while (0)
#define NTRDMA_RETURN_ASSERT(expr, ret) do {} while(0)
#endif

#define ntrdma_pr(format, ...) \
	pr_debug(format, ## __VA_ARGS__)

#define ntrdma_dbg(dev, format, ...) \
	dev_dbg(ntrdma_port_device(dev), format, ## __VA_ARGS__)

#define ntrdma_vdbg(dev, format, ...) \
	dev_vdbg(ntrdma_port_device(dev), format, ## __VA_ARGS__)

/* Simple types */

#define ntrdma_bool_t bool

#define ntrdma_u8_t u8
#define ntrdma_u16_t u16
#define ntrdma_u32_t u32
#define ntrdma_u64_t u64
#define ntrdma_s8_t s8
#define ntrdma_s16_t s16
#define ntrdma_s32_t s32
#define ntrdma_s64_t s64

#define ntrdma_dma_addr_t dma_addr_t
#define ntrdma_phys_addr_t phys_addr_t
#define ntrdma_size_t size_t

#define ntrdma_min_t(type, a, b) min_t(type, a, b)

#define NTRDMA_PAGE_SIZE PAGE_SIZE

#define NTRDMA_BITS_PER_BYTE BITS_PER_BYTE
#define NTRDMA_BITS_PER_LONG BITS_PER_LONG

#define NTRDMA_BITS_TO_BYTES(b) NTRDMA_CEIL_DIV(b, NTRDMA_BITS_PER_BYTE)
#define NTRDMA_BITS_TO_LONGS(b) NTRDMA_CEIL_DIV(b, NTRDMA_BITS_PER_LONG)

#define NTRDMA_FLOOR_DIV(n,d) ((n) / (d))
#define NTRDMA_CEIL_DIV(n,d) DIV_ROUND_UP(n, d)

/* bitscan forward: find the least significant one bit pos in nonzero expr */
#define ntrdma_bsf_long(expr) __builtin_ctzl(expr)
/* bitscan reverse: find the most significant one bit pos in nonzero expr */
#define ntrdma_bsr_long(expr) (__builtin_clzl(expr) ^ (NTRDMA_BITS_PER_LONG - 1))

#define ntrdma_ctzl(expr) __ffs(expr)

/* Utilities */

#define NTRDMA_OFFSET_OF(type, mbr) offsetof(type,mbr)
#define NTRDMA_CONTAINER_OF(ptr, type, mbr) container_of(ptr, type, mbr)

#define ntrdma_unlikely(expr) unlikely(expr)

#define ntrdma_memcpy(dst, src, len) memcpy(dst, src, len)
#define ntrdma_memset(dst, chr, len) memset(dst, chr, len)
#define ntrdma_strlcpy(dst, src, len) strlcpy(dst, src, len)

/* Miscellaneous */

#define NTRDMA_THIS_MODULE THIS_MODULE

static inline void *_ntrdma_ulong_to_pvoid(unsigned long x)
{
	return (void*)x;
}

static inline unsigned long _ntrdma_pvoid_to_ulong(void *x)
{
	return (unsigned long)x;
}

/* Intrusive 2-Link List */

#define NTRDMA_DECL_LIST_HEAD(name) \
	struct list_head name

#define NTRDMA_DECL_LIST_ENTRY(name) \
	struct list_head name

#define ntrdma_list_for_each_entry(pos, head, type, member) \
	list_for_each_entry(pos, head, member)

#define ntrdma_list_for_each_entry_safe(pos, next, head, type, member) \
	list_for_each_entry_safe(pos, next, head, member)

#define ntrdma_list_for_each_entry_reverse(pos, head, type, member) \
	list_for_each_entry_reverse(pos, head, member)

#define ntrdma_list_for_each_entry_safe_reverse(pos, next, head, type, member) \
	list_for_each_entry_safe_reverse(pos, next, head, member)

#define ntrdma_list_init(head) \
	INIT_LIST_HEAD(head)

#define ntrdma_list_empty(head) \
	list_empty(head)

#define ntrdma_list_add_head(head, entry) \
	list_add_head(entry, head)

#define ntrdma_list_add_tail(head, entry) \
	list_add_tail(entry, head)

#define ntrdma_list_append(head_dst, head_src) \
	list_splice_tail(head_src, head_dst)

#define ntrdma_list_del(entry) \
	list_del(entry)

#define ntrdma_list_first_entry(head, type, member) \
	(list_empty(head) ? NULL : list_first_entry(head, type, member))

#define ntrdma_list_last_entry(head, type, member) \
	(list_empty(head) ? NULL : list_entry((head)->prev, type, member))

#define ntrdma_list_remove_first_entry(head, type, member) \
	(list_empty(head) ? NULL :				\
	 ({							\
	  struct list_head *entry = (head)->next;		\
	  list_del(entry);					\
	  list_entry(entry, type, member);			\
	  }))

#define ntrdma_list_move_first_entry(to_head, from_head, type, member) \
	(list_empty(from_head) ? NULL :				\
	 ({							\
	  struct list_head *entry = (from_head)->next;		\
	  list_move_tail(entry, to_head);			\
	  list_entry(entry, type, member);			\
	  }))

#define ntrdma_list_rotate_tail(head, entry) \
	list_move(head, entry)

/* Spinlock (Software interrupt disabling, no hardware interrupt disabling) */

#define NTRDMA_DECL_SPL(name) \
	spinlock_t name

#define ntrdma_spl_create(spl_obj, spl_name) \
	spin_lock_init(spl_obj)

#define ntrdma_spl_destroy(spl_obj) \
	do {} while(0)

#define ntrdma_spl_lock(spl_obj) \
	spin_lock_bh(spl_obj)

#define ntrdma_spl_trylock(spl_obj) \
	spin_trylock_bh(spl_obj)

#define ntrdma_spl_unlock(spl_obj) \
	spin_unlock_bh(spl_obj)

/* Mutex */

#define NTRDMA_DECL_MUT(name) \
	struct mutex name

#define ntrdma_mut_create(mut_obj, mut_name) \
	mutex_init(mut_obj)

#define ntrdma_mut_destroy(mut_obj) \
	mutex_destroy(mut_obj)

#define ntrdma_mut_lock(mut_obj) \
	mutex_lock(mut_obj)

#define ntrdma_mut_trylock(mut_obj) \
	mutex_trylock(mut_obj)

#define ntrdma_mut_unlock(mut_obj) \
	mutex_unlock(mut_obj)

/* Manual Reset Event */

#define NTRDMA_DECL_MRE(name) \
	struct completion name

#define ntrdma_mre_create(mre_obj, mre_name) \
	init_completion(mre_obj)

#define ntrdma_mre_destroy(mre_obj) \
	do {} while(0)

#define ntrdma_mre_reset(mre_obj) \
	reinit_completion(mre_obj)

#define ntrdma_mre_wait(mre_obj) \
	wait_for_completion(mre_obj)

#define ntrdma_mre_broadcast(mre_obj) \
	complete_all(mre_obj)

/* Condition Variable Half (without its own lock) */

#define NTRDMA_DECL_CVH(name) \
	wait_queue_head_t name

#define ntrdma_cvh_create(wtq_obj, wtq_name) \
	init_waitqueue_head(wtq_obj)

#define ntrdma_cvh_destroy(wtq_obj) \
	do {} while(0)

#define ntrdma_cvh_wait(wtq_obj, cond) \
	wait_event(*(wtq_obj), cond)

#define ntrdma_cvh_mut_wait(wtq_obj, mut_obj, cond) \
	wait_event_cmd(*(wtq_obj), cond,		\
		       ntrdma_mut_unlock(mut_obj),	\
		       ntrdma_mut_lock(mut_obj))

#define ntrdma_cvh_spl_wait(wtq_obj, spl_obj, cond) \
	wait_event_cmd(*(wtq_obj), cond,		\
		       ntrdma_spl_unlock(spl_obj),	\
		       ntrdma_spl_lock(spl_obj))

#define ntrdma_cvh_signal(wtq_obj) \
	wake_up(wtq_obj)

#define ntrdma_cvh_broadcast(wtq_obj) \
	wake_up_all(wtq_obj)

/* Deferred Procedure Calls */

#define NTRDMA_DECL_DPC(name) \
	struct tasklet_struct name

#define NTRDMA_DECL_DPC_CB(name, dpc_ptrhld) \
	void name(unsigned long dpc_ptrhld)

#define NTRDMA_CAST_DPC_CTX(dpc_ptrhld) \
	_ntrdma_ulong_to_pvoid(dpc_ptrhld)

#define ntrdma_dpc_create(dpc_obj, dpc_name, dpc_cb, dpc_ctx) \
	tasklet_init(dpc_obj, dpc_cb, _ntrdma_pvoid_to_ulong(dpc_ctx));

#define ntrdma_dpc_destroy(dpc_obj) \
	tasklet_kill(dpc_obj)

#define ntrdma_dpc_enable(dpc_obj) \
	tasklet_enable(dpc_obj)

#define ntrdma_dpc_disable(dpc_obj) \
	tasklet_disable(dpc_obj)

#define ntrdma_dpc_fire(dpc_obj) \
	tasklet_schedule(dpc_obj)

/* Deferred Work Items */

struct ntrdma_dwi { struct work_struct work; void *ctx; };

#define NTRDMA_DECL_DWI(name) \
	struct ntrdma_dwi name

#define NTRDMA_DECL_DWI_CB(name, dwi_ptrhld) \
	void name(struct work_struct *dwi_ptrhld)

#define NTRDMA_CAST_DWI_CTX(dwi_ptrhld) \
	(container_of(dwi_ptrhld, struct ntrdma_dwi, work)->ctx)

#define ntrdma_dwi_create(dwi_obj, dwi_name, dwi_cb, dwi_ctx) \
	do { \
		INIT_WORK(&(dwi_obj)->work, dwi_cb); \
		(dwi_obj)->ctx = (void *)dwi_ctx; \
	} while (0)

#define ntrdma_dwi_destroy(dwi_obj) \
	do {} while (0)

#define ntrdma_dwi_fire(dwi_obj) \
	schedule_work(&(dwi_obj)->work)

/* Timers */

#define NTRDMA_DECL_TIM(name) \
	struct timer_list name

#define NTRDMA_DECL_TIM_CB(name, tim_ptrhld) \
	void name(unsigned long tim_ptrhld)

#define NTRDMA_CAST_TIM_CTX(dpc_ptrhld) \
	_ntrdma_ulong_to_pvoid(dpc_ptrhld)

#define ntrdma_tim_create(tim_obj, tim_name, tim_cb, tim_ctx) \
	setup_timer(tim_obj, tim_cb, _ntrdma_pvoid_to_ulong(tim_ctx))

#define ntrdma_tim_destroy(tim_obj) \
	do {} while (0)

#define ntrdma_tim_cancel_async(tim_obj) \
	del_timer(tim_obj)

#define ntrdma_tim_cancel_sync(tim_obj) \
	del_timer_sync(tim_obj)

#define ntrdma_tim_fire(tim_obj, msecs) \
	mod_timer(tim_obj, jiffies + msecs_to_jiffies(msecs))

#endif
