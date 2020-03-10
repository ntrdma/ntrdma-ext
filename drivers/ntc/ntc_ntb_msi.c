
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/bitmap.h>
#include <linux/debugfs.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ntb.h>
#include <linux/pci.h>
#include <linux/timer.h>
#include <linux/cpumask.h>
#include <linux/slab.h>

#include "ntc.h"

#include <asm/e820/api.h>

#include "ntc_internal.h"
#define CREATE_TRACE_POINTS
#include "ntc-trace.h"

#define DRIVER_NAME			"ntc_ntb"
#define DRIVER_DESCRIPTION		"NTC Non Transparent Bridge"

#define DRIVER_VERSION			"0.3"

static unsigned long mw0_base_addr;
module_param(mw0_base_addr, ulong, 0444);
static unsigned long mw0_len;
module_param(mw0_len, ulong, 0444);
static unsigned long mw0_mm_len;
module_param(mw0_mm_len, ulong, 0444);
static unsigned long mw1_base_addr;
module_param(mw1_base_addr, ulong, 0444);
static unsigned long mw1_len;
module_param(mw1_len, ulong, 0444);
static unsigned long mw1_mm_len;
module_param(mw1_mm_len, ulong, 0444);
static unsigned num_dma_chan;
module_param(num_dma_chan, uint, 0444);

struct ntc_ntb_coherent_buffer {
	void *ptr;
	resource_size_t size;
	dma_addr_t dma_addr;
};

struct ntc_own_mw_data {
	unsigned long base_addr;
	unsigned long len;
	resource_size_t addr_align;
	resource_size_t size_align;
	resource_size_t size_max;
	unsigned long mm_len;
	unsigned long mm_prealloc;
	struct ntc_ntb_coherent_buffer coherent;
	bool reserved;
	bool reserved_used;
	bool coherent_used;
	bool flat_used;
	bool mm_inited;
} own_mw_data[2];

static struct kmem_cache *imm_slab;

static struct dentry *ntc_dbgfs;

/* Protocol version for backwards compatibility */
#define NTC_NTB_VERSION_NONE		0
#define NTC_NTB_VERSION_FIRST		2

#define NTC_NTB_VERSION_BAD_MAGIC	U32_C(~0)
#define NTC_NTB_VERSION_MAGIC_2		U32_C(0x2250cb1c)

#define NTC_NTB_SPAD_PING		0
#define NTC_NTB_SPAD_VERSION		1
#define NTC_NTB_SPAD_ADDR_LOWER		2
#define NTC_NTB_SPAD_ADDR_UPPER		3

#define NTC_NTB_LINK_QUIESCE		0
#define NTC_NTB_LINK_RESET		1
#define NTC_NTB_LINK_START		2
#define NTC_NTB_LINK_VER_SENT		3
#define NTC_NTB_LINK_VER_CHOSEN		4
#define NTC_NTB_LINK_DB_CONFIGURED	5
#define NTC_NTB_LINK_COMMITTED		6
#define NTC_NTB_LINK_HELLO		7

#define NTC_NTB_PING_PONG_SPAD		BIT(1)
#define NTC_NTB_PING_PONG_MEM		BIT(2)
#define NTC_NTB_PING_POLL_MEM		BIT(3)


#define NTC_NTB_PING_PONG_PERIOD	msecs_to_jiffies(100)
#define NTC_NTB_PING_POLL_PERIOD	msecs_to_jiffies(240)
#define NTC_NTB_PING_MISS_THRESHOLD	10

#define NTC_CTX_BUF_SIZE 1024

/* PCIe spec - TLP data must be 4-byte naturally
 * aligned and in increments of 4-byte Double Words (DW).
 */
#define PCIE_ADDR_ALIGN 4
#define INTEL_DOORBELL_REG_SIZE (4)
#define INTEL_DOORBELL_REG_OFFSET(__bit) (bit * INTEL_DOORBELL_REG_SIZE)

#define info(fmt, ...) do {						\
		pr_info(DRIVER_NAME ":%s: " fmt, __func__, ##__VA_ARGS__); \
	} while (0)

struct ntc_ntb_imm {
	char				data_buf[sizeof(u64)];
	size_t				data_len;
	struct device			*dma_dev;
	dma_addr_t			dma_addr;
	void				(*cb)(void *cb_ctx);
	void				*cb_ctx;
	u64				wrid;
	bool				data_trace;
};

#define MAX_SUPPORTED_VERSIONS 32
struct multi_version_support {
	u32 num;
	u32 versions[MAX_SUPPORTED_VERSIONS];
};

struct ntc_ntb_info {
	struct multi_version_support	versions;
	u32				magic;
	u32				ping;

	u32			done;

	/* ctx buf for use by the client */
	u8	ctx_buf[NTC_CTX_BUF_SIZE];
};

enum PINGPONG_CB_ID {
	PINGPONG_CB_ID_FROM_INSIDE_MODULE = 0,
	PINGPONG_CB_ID_FROM_TIMER_CB = 1
};


struct ntc_ntb_dev {
	struct ntc_dev			ntc;

	/* channel supporting hardware devices */
	struct ntb_dev			*ntb;

	/* direct interrupt message */
	struct ntc_remote_buf		peer_irq_base;
	u64				peer_irq_shift[NTB_MAX_IRQS];
	u32				peer_irq_data[NTB_MAX_IRQS];

	/* link state heartbeat */
	bool				ping_run;
	int				ping_miss;
	int				ping_flags;
	u16				ping_seq;
	u16				ping_msg;
	u32				poll_val;
	u16				poll_msg;
	cpumask_t			timer_cpu_mask;
	struct timer_list		ping_pong[NR_CPUS];
	struct timer_list		ping_poll;
	spinlock_t			ping_lock;
	unsigned long		last_ping_trigger_time;

	/* link state machine */
	int				link_state;
	struct work_struct		link_work;
	struct mutex			link_lock;

	struct dentry *dbgfs;
	wait_queue_head_t	reset_done;
	uint				reset_cnt;

	u32				self_info_done;
};

#define ntc_ntb_down_cast(__ntc) \
	container_of(__ntc, struct ntc_ntb_dev, ntc)

#define ntc_ntb_of_dev(__dev) \
	ntc_ntb_down_cast(ntc_of_dev(__dev))

#define ntc_ntb_of_link_work(__ws) \
	container_of(__ws, struct ntc_ntb_dev, link_work)

#define ntc_ntb_of_ptrhld(__ptrhld) \
	((void *)(unsigned long)(__ptrhld))

#define ntc_ntb_to_ptrhld(__ptr) \
	((unsigned long)(void *)(__ptr))

#define ntc_ntb_dma_dev(__dev) \
	(&(__dev)->ntb->pdev->dev)

#define ntc_ntb_dev_dbg(__dev, ...)		\
	ntc_dbg(&(__dev)->ntc, ##__VA_ARGS__)

#define ntc_ntb_dev_err(__dev, ...)		\
	ntc_err(&(__dev)->ntc, ##__VA_ARGS__)

#define ntc_ntb_dev_info(__dev, ...)		\
	ntc_info(&(__dev)->ntc, ##__VA_ARGS__)

#define ntc_ntb_dev_vdbg(__dev, ...)		\
	ntc_vdbg(&(__dev)->ntc, ##__VA_ARGS__)

static u32 supported_versions[] = {
		NTC_NTB_VERSION_FIRST
};

static bool ntc_check_reserved(unsigned long start, unsigned long end)
{
	bool success;
	int i;

	if (!start) {
		info("Reserved memory not specified.");
		return false;
	}

	if (end <= start) {
		info("Illegal reserved memory spec: start=%#lx end=%#lx",
			start, end);
		return false;
	}

	if (start & (PAGE_SIZE - 1)) {
		info("Unaligned reserved memory start %#lx", start);
		return false;
	}

	if (end & (PAGE_SIZE - 1)) {
		info("Unaligned reserved memory end %#lx", end);
		return false;
	}

	success = true;
	for (i = E820_TYPE_RAM; i <= E820_TYPE_RESERVED_KERN; i++) {
		if (e820__mapped_any(start, end, i)) {
			if (i != E820_TYPE_RESERVED) {
				info("Non-reserved type %d in %#lx:%#lx",
					i, start, end);
				success = false;
			} else
				info("Found reserved type %d in %#lx:%#lx",
					i, start, end);
		} else if (i == E820_TYPE_RESERVED) {
			info("No reserved memory in %#lx:%#lx",
				start, end);
			success = false;
		}
	}

	return success;
}

static inline void ntc_own_mw_data_check_reserved(struct ntc_own_mw_data *data)
{
	data->reserved = ntc_check_reserved(data->base_addr,
					data->base_addr + data->len);
}

static inline
const struct ntc_ntb_info *ntc_ntb_peer_info(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;

	return ntc->own_mws[NTC_INFO_MW_IDX].base_ptr;
}

static inline
struct ntc_ntb_info __iomem *ntc_ntb_self_info(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;

	return ntc->peer_mws[NTC_INFO_MW_IDX].base_ptr;
}

static inline void ntc_ntb_version(struct multi_version_support *versions)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(supported_versions); i++)
		versions->versions[i] = supported_versions[i];
	versions->num = ARRAY_SIZE(supported_versions);
	BUILD_BUG_ON(versions->num > MAX_SUPPORTED_VERSIONS);
}

static inline u32 ntc_ntb_version_matching(struct ntc_dev *ntc,
		struct multi_version_support *v1,
		struct multi_version_support *v2)
{
	int i, j;

	for (j = v1->num - 1; j >= 0; j--)
		for (i = v2->num - 1; i >= 0; i--)
			if (v1->versions[j] == v2->versions[i])
				return v1->versions[j];

	ntc_err(ntc, "Local supported versions (%d) are:", v1->num);
	for (j = 0; j < v1->num; j++)
		ntc_err(ntc, "0x%08x", v1->versions[j]);
	ntc_err(ntc, "Remote supported versions (%d) are:", v2->num);
	for (i = 0; i < v2->num; i++)
		ntc_err(ntc, "0x%08x", v2->versions[i]);
	return NTC_NTB_VERSION_NONE;
}

static inline u32 ntc_ntb_version_magic(int version)
{
	switch (version) {
	case 2:
		return NTC_NTB_VERSION_MAGIC_2;
	}
	return NTC_NTB_VERSION_BAD_MAGIC;
}

static inline u32 ntc_ntb_version_check_magic(int version, u32 magic)
{
	switch (version) {
	case 2:
		return magic == NTC_NTB_VERSION_MAGIC_2;
	}
	return false;
}

static inline int ntc_ntb_ping_flags(int msg)
{
	if (msg >= NTC_NTB_LINK_COMMITTED)
		return NTC_NTB_PING_PONG_MEM | NTC_NTB_PING_POLL_MEM;

	if (msg >= NTC_NTB_LINK_DB_CONFIGURED)
		return NTC_NTB_PING_PONG_MEM | NTC_NTB_PING_PONG_SPAD |
			NTC_NTB_PING_POLL_MEM;

	if (msg >= NTC_NTB_LINK_VER_CHOSEN)
		return NTC_NTB_PING_PONG_MEM | NTC_NTB_PING_PONG_SPAD;

	return NTC_NTB_PING_PONG_SPAD;
}

static inline u32 ntc_ntb_ping_val(u16 msg, u16 seq)
{
	return (((u32)msg) << 16) | ((u32)seq);
}

static inline u16 ntc_ntb_ping_msg(u32 val)
{
	return (u16)(val >> 16);
}

static void ntc_ntb_ping_pong(struct ntc_ntb_dev *dev,
				enum PINGPONG_CB_ID pingpong_caller_id)
{
	struct ntc_ntb_info __iomem *self_info;
	int ping_flags, poison_flags;
	u32 ping_val, tmp_jiffies;
	static u32 last_ping;
	int cpu, this_cpu;
	int is_correct_cpu = 0;
	unsigned long timer_next_trigger =
			dev->last_ping_trigger_time + NTC_NTB_PING_PONG_PERIOD;

	if (!dev->ping_run)
		return;
	tmp_jiffies = jiffies;
	if (unlikely(last_ping &&
			(tmp_jiffies > last_ping +
				5 * NTC_NTB_PING_PONG_PERIOD))) {
		dev_warn(&dev->ntc.dev, "****PINGPONG delayed by %u******\n",
			jiffies_to_msecs(tmp_jiffies - last_ping));
	}
	last_ping = tmp_jiffies;

	if (pingpong_caller_id == PINGPONG_CB_ID_FROM_TIMER_CB) {
		this_cpu = smp_processor_id();
		for_each_cpu(cpu, &dev->timer_cpu_mask) {
			if (cpu == this_cpu) {
				is_correct_cpu = 1;
				break;
			}
		}
		if (is_correct_cpu) {
			del_timer(&dev->ping_pong[cpu]);
			dev->ping_pong[cpu].expires = timer_next_trigger;
			dev->last_ping_trigger_time = timer_next_trigger;
			add_timer_on(&dev->ping_pong[cpu], cpu);
		}
	}

	ping_flags = ntc_ntb_ping_flags(dev->ping_msg);
	poison_flags = dev->ping_flags & ~ping_flags;
	(void)poison_flags; /* TODO: cleanup unused */

	ping_val = ntc_ntb_ping_val(dev->ping_msg, ++dev->ping_seq);

	ntc_ntb_dev_vdbg(dev, "ping val %x", ping_val);

	wmb(); /* fence anything prior to writing the message */

	self_info = ntc_ntb_self_info(dev);
	iowrite32(ping_val, &self_info->ping);

	dev->ping_flags = ping_flags;
}

static void ntc_ntb_ping_pong_cb(unsigned long ptrhld)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_ptrhld(ptrhld);
	unsigned long irqflags;

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	ntc_ntb_ping_pong(dev, PINGPONG_CB_ID_FROM_TIMER_CB);
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);
}

static bool ntc_ntb_ping_poll(struct ntc_ntb_dev *dev)
{
	const struct ntc_ntb_info *peer_info;
	int ping_flags;
	u32 poll_val;

	if (!dev->ping_run)
		return false;
	mod_timer(&dev->ping_poll, jiffies + NTC_NTB_PING_POLL_PERIOD);

	ping_flags = dev->ping_flags;
	(void)ping_flags; /* TODO: cleanup unused */

	peer_info = ntc_ntb_peer_info(dev);
	poll_val = peer_info->ping;

	ntc_ntb_dev_vdbg(dev, "poll val %x", poll_val);

	if (dev->poll_val != poll_val) {
		dev->poll_val = poll_val;
		return true;
	}
	return false;
}

static void ntc_ntb_ping_poll_cb(unsigned long ptrhld)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_ptrhld(ptrhld);
	unsigned long irqflags;
	int poll_msg;

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	if (ntc_ntb_ping_poll(dev)) {
		ntc_ntb_dev_vdbg(dev, "ping hit");
		dev->ping_miss = 0;
		poll_msg = ntc_ntb_ping_msg(dev->poll_val);
		if (dev->poll_msg != poll_msg) {
			ntc_ntb_dev_dbg(dev, "peer msg %d", poll_msg);
			dev->poll_msg = poll_msg;
			schedule_work(&dev->link_work);
		}
	} else if (dev->ping_miss < NTC_NTB_PING_MISS_THRESHOLD) {
		++dev->ping_miss;
		ntc_ntb_dev_dbg(dev, "ping miss %d", dev->ping_miss);
		if (dev->ping_miss == NTC_NTB_PING_MISS_THRESHOLD) {
			ntc_ntb_dev_err(dev,
					"peer lost - moving to quiesce state");
			dev->poll_msg = NTC_NTB_LINK_QUIESCE;
			schedule_work(&dev->link_work);
		}
	}
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);
}

static void ntc_ntb_ping_send(struct ntc_ntb_dev *dev, int msg)
{
	unsigned long irqflags;

	ntc_ntb_dev_dbg(dev, "ping send msg %x", msg);

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	dev->ping_msg = msg;
	ntc_ntb_ping_pong(dev, PINGPONG_CB_ID_FROM_INSIDE_MODULE);
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);
}

static int ntc_ntb_ping_start(struct ntc_ntb_dev *dev)
{
	unsigned long irqflags;
	unsigned long timer_next_trigger;
	int msg;
	int cpu;

	ntc_ntb_dev_dbg(dev, "ping start");

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	if (!dev->ping_run) {
		dev->ping_run = true;
		dev->ping_miss = NTC_NTB_PING_MISS_THRESHOLD;
		ntc_ntb_ping_pong(dev, PINGPONG_CB_ID_FROM_INSIDE_MODULE);
		ntc_ntb_ping_poll(dev);
		dev->poll_msg = NTC_NTB_LINK_QUIESCE;

		timer_next_trigger = jiffies;

		for_each_cpu(cpu, &dev->timer_cpu_mask) {
			timer_next_trigger += NTC_NTB_PING_PONG_PERIOD;
			del_timer(&dev->ping_pong[cpu]);
			dev->ping_pong[cpu].expires = timer_next_trigger;
			add_timer_on(&(dev->ping_pong[cpu]), cpu);
		}
		dev->last_ping_trigger_time = timer_next_trigger;
	}
	msg = dev->poll_msg;
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);

	rmb(); /* fence anything after reading the message */

	return msg;
}

static void ntc_ntb_ping_stop(struct ntc_ntb_dev *dev)
{
	unsigned long irqflags;
	int cpu;

	ntc_ntb_dev_dbg(dev, "ping stop");

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	dev->ping_run = false;
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);

	for_each_cpu(cpu, &dev->timer_cpu_mask) {
		del_timer_sync(&(dev->ping_pong[cpu]));
	}

	del_timer_sync(&dev->ping_poll);
}

static inline void ntc_ntb_link_set_state(struct ntc_ntb_dev *dev, int state)
{

	dev->link_state = state;
	ntc_ntb_ping_send(dev, state);
}

static inline int ntc_ntb_link_get_event(struct ntc_ntb_dev *dev)
{
	if (!ntb_link_is_up(dev->ntb, NULL, NULL)) {
		ntc_ntb_ping_stop(dev);
		return NTC_NTB_LINK_QUIESCE;
	}

	return ntc_ntb_ping_start(dev);
}

static inline void ntc_ntb_error(struct ntc_ntb_dev *dev)
{
	ntc_ntb_dev_err(dev, "link error");

	if (!(dev->link_state > NTC_NTB_LINK_RESET)) {
		info("NTC: link reset call rejected , current link state %d\n",
			dev->link_state);
		return;
	}
	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_QUIESCE);
	schedule_work(&dev->link_work);
}

static void ntc_ntb_link_update(struct ntc_ntb_dev *dev, bool link_up)
{
	struct ntc_dev *ntc = &dev->ntc;
	int err = 0;

	if (ntc->link_is_up == link_up)
		return;

	ntc->link_is_up = link_up;
	if (link_up) {
		err = ntc_ctx_enable(ntc);
		if (err) {
			ntc_ntb_dev_err(dev, "ntc_ctx_enable failed: rc=%d",
					err);
			ntc_ntb_error(dev);
		}
	} else
		ntc_ctx_disable(ntc);
}

static inline void ntc_ntb_quiesce(struct ntc_ntb_dev *dev)
{
	ntc_ntb_dev_info(dev, "link quiesce");
	ntc_ntb_link_update(dev, false);
	ntc_ctx_quiesce(&dev->ntc);

	/* TODO: cancel and wait for any outstanding dma requests */

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_RESET);
}

static inline void reset_peer_irq(struct ntc_ntb_dev *dev)
{
	ntc_remote_buf_unmap(&dev->peer_irq_base, &dev->ntc);
}

static inline void ntc_ntb_reset(struct ntc_ntb_dev *dev)
{
	ntc_ntb_dev_info(dev, "link reset");

	ntc_ctx_reset(&dev->ntc);

	reset_peer_irq(dev);
	dev->reset_cnt++;
	wake_up(&dev->reset_done);
	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_START);
}

static inline void ntc_ntb_send_version(struct ntc_ntb_dev *dev)
{
	struct ntc_ntb_info __iomem *self_info;
	struct multi_version_support versions;
	int i;


	ntc_ntb_dev_info(dev, "link send version");

	ntc_ntb_version(&versions);

	self_info = ntc_ntb_self_info(dev);
	for (i = 0; i < versions.num; i++)
		iowrite32(versions.versions[i],
			&self_info->versions.versions[i]);
	iowrite32(versions.num, &self_info->versions.num);

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_VER_SENT);
}

static inline void ntc_ntb_choose_version(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_ntb_info __iomem *self_info;
	const struct ntc_ntb_info *peer_info;
	struct multi_version_support versions;
	struct multi_version_support peer_versions;

	ntc_ntb_dev_info(dev, "link choose version\n");

	ntc_ntb_version(&versions);
	peer_info = ntc_ntb_peer_info(dev);
	peer_versions = peer_info->versions;
	if (peer_versions.num > MAX_SUPPORTED_VERSIONS) {
		ntc_ntb_dev_err(dev, "too many peer versions: %d > %d",
				peer_versions.num, MAX_SUPPORTED_VERSIONS);
		goto err;
	}

	if (versions.num > 0)
		ntc->latest_version = versions.versions[versions.num-1];

	ntc->version = ntc_ntb_version_matching(ntc, &versions, &peer_versions);
	if (ntc->version == NTC_NTB_VERSION_NONE) {
		ntc_ntb_dev_err(dev, "versions did not match");
		goto err;
	}

	ntc_ntb_dev_info(dev, "Agree on version %d", ntc->version);

	self_info = ntc_ntb_self_info(dev);
	iowrite32(ntc_ntb_version_magic(ntc->version), &self_info->magic);
	iowrite32(0, &self_info->done);
	dev->self_info_done = 0;

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_VER_CHOSEN);
	return;

err:
	ntc_ntb_error(dev);
}

static bool ntc_request_dma(struct ntc_dev *ntc);

static inline int ntc_ntb_db_config(struct ntc_ntb_dev *dev)
{
	int rc;
	int i;
	resource_size_t size;
	struct ntc_dev *ntc = &dev->ntc;
	phys_addr_t peer_irq_phys_addr_base;
	u64 peer_db_mask;
	int max_irqs;
	u64 db_bits;

	if (!ntc_request_dma(ntc)) {
		ntc_ntb_dev_err(dev, "no dma");
		return -ENODEV;
	}

	ntc->peer_irq_num = 0;

	max_irqs = ntb_db_vector_count(dev->ntb);

	if (max_irqs <= 0 || max_irqs> NTB_MAX_IRQS) {
		ntc_ntb_dev_err(dev, "max_irqs %d - not supported", max_irqs);
		rc = -EINVAL;
		goto err_ntb_db;
	}

	rc = ntb_peer_db_addr(dev->ntb,
			&peer_irq_phys_addr_base, &size);
	if ((rc < 0) || (size != sizeof(u32)) ||
		!IS_ALIGNED(peer_irq_phys_addr_base, PCIE_ADDR_ALIGN)) {
		ntc_ntb_dev_err(dev, "Peer DB addr invalid");
		goto err_ntb_db;
	}

	rc = ntc_remote_buf_map_phys(&dev->peer_irq_base,
				ntc,
				peer_irq_phys_addr_base,
				max_irqs * INTEL_DOORBELL_REG_SIZE);
	if (unlikely(rc < 0))
		goto err_res_map;

	db_bits = ntb_db_valid_mask(dev->ntb);
	for (i = 0; i < max_irqs && db_bits; i++) {
		/*FIXME This is not generic implementation,
		 * Sky-lake implementation, see intel_ntb3_peer_db_set() */
		int bit = __ffs(db_bits);

		dev->peer_irq_shift[i] = INTEL_DOORBELL_REG_OFFSET(bit);
		db_bits &= db_bits - 1;
		ntc->peer_irq_num++;
		dev->peer_irq_data[i] = 1;
	}

	peer_db_mask = ntb_db_valid_mask(dev->ntb);

	ntc_ntb_dev_dbg(dev, "Peer DB addr: %#llx count %d mask %#llx",
			peer_irq_phys_addr_base,
			ntc->peer_irq_num, peer_db_mask);

	ntb_db_clear(dev->ntb, peer_db_mask);
	ntb_db_clear_mask(dev->ntb, peer_db_mask);

	ntc_ntb_dev_info(dev, "link signaling method configured");
	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_DB_CONFIGURED);
	return 0;

err_res_map:
	ntc->peer_irq_num = 0;
err_ntb_db:
	return rc;
}

static inline void ntc_ntb_link_commit(struct ntc_ntb_dev *dev)
{
	ntc_ntb_dev_info(dev, "link commit - verifying both sides sync");

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_COMMITTED);
}

static inline bool ntc_ntb_done_hello(struct ntc_ntb_dev *dev)
{
	const struct ntc_ntb_info *peer_info;

	peer_info = ntc_ntb_peer_info(dev);

	/* no outstanding input or output buffers */
	return (peer_info->done == 1) && (dev->self_info_done == 1);
}

static inline int ntc_ntb_hello(struct ntc_ntb_dev *dev)
{
	struct ntc_ntb_info __iomem *self_info;
	int ret = 0;
	int phase = dev->link_state + 1 - NTC_NTB_LINK_HELLO;

	ntc_ntb_dev_info(dev, "link hello phase %d", phase);

	if (phase < 0)
		return -EINVAL;

	/* perform this phase of initialization */
	ret = ntc_ctx_hello(&dev->ntc, phase, 0, 0, 0, 0);
	if (ret < 0)
		return ret;

	if (ret == 1) {
		self_info = ntc_ntb_self_info(dev);
		iowrite32(1, &self_info->done);
		dev->self_info_done = 1;
	}

	ntc_ntb_dev_info(dev, "hello callback phase %d done %d", phase, ret);

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_HELLO + phase);

	return 0;
}

static void ntc_ntb_link_work(struct ntc_ntb_dev *dev)
{
	int link_event = ntc_ntb_link_get_event(dev);
	bool link_up = false;
	int err = 0;

	if (dev->link_state <= link_event)
		ntc_ntb_dev_info(dev, "link work state %d event %d",
				dev->link_state, link_event);
	else
		ntc_ntb_dev_dbg(dev, "link work state %d event %d",
				dev->link_state, link_event);

	ntc_request_dma(&dev->ntc);

	switch (dev->link_state) {
	case NTC_NTB_LINK_QUIESCE:
		ntc_ntb_quiesce(dev);
		/* fall through */
	case NTC_NTB_LINK_RESET:
		ntc_ntb_reset(dev);
		/* fall through */
	case NTC_NTB_LINK_START:
		switch (link_event) {
		default:
			goto out;
		case NTC_NTB_LINK_START:
		case NTC_NTB_LINK_VER_SENT:
			ntc_ntb_send_version(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_VER_SENT)
			goto out;
		/* fall through */

	case NTC_NTB_LINK_VER_SENT:
		switch (link_event) {
		default:
			ntc_ntb_dev_err(dev,
					"link work state LINK_VER_SENT(%d) "
					"event %d",
					dev->link_state, link_event);
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_START:
			goto out;
		case NTC_NTB_LINK_VER_SENT:
		case NTC_NTB_LINK_VER_CHOSEN:
			ntc_ntb_choose_version(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_VER_CHOSEN)
			goto out;
		/* fall through */

	case NTC_NTB_LINK_VER_CHOSEN:
		switch (link_event) {
		default:
			ntc_ntb_dev_err(dev,
					"link work state LINK_VER_CHOSEN(%d)"
					" event %d",
					dev->link_state, link_event);
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_VER_SENT:
			goto out;
		case NTC_NTB_LINK_VER_CHOSEN:
		case NTC_NTB_LINK_DB_CONFIGURED:
			if (ntc_ntb_db_config(dev)) {
				ntc_ntb_dev_err(dev,
						"ntc_ntb_db_config failed");
				ntc_ntb_error(dev);
			}
		}

		if (dev->link_state != NTC_NTB_LINK_DB_CONFIGURED)
			goto out;
		/* fall through */

	case NTC_NTB_LINK_DB_CONFIGURED:
		switch (link_event) {
		default:
			ntc_ntb_dev_err(dev,
					"link work state LINK_DB_CONFIGURED(%d)"
					" event %d",
					dev->link_state, link_event);
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_VER_CHOSEN:
			goto out;
		case NTC_NTB_LINK_DB_CONFIGURED:
		case NTC_NTB_LINK_COMMITTED:
			ntc_ntb_link_commit(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_COMMITTED)
			goto out;
		/* fall through */

	case NTC_NTB_LINK_COMMITTED:
		switch (link_event) {
		default:
			ntc_ntb_dev_err(dev,
					"link work state LINK_COMMITTED(%d)"
					" event %d",
					dev->link_state, link_event);
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_DB_CONFIGURED:
			goto out;
		case NTC_NTB_LINK_COMMITTED:
		case NTC_NTB_LINK_HELLO:
			err = ntc_ntb_hello(dev);
			if (err < 0) {
				ntc_ntb_dev_err(dev, "ntc_ntb_hello failed %d",
						err);
				ntc_ntb_error(dev);
			}
		}

		if (dev->link_state != NTC_NTB_LINK_HELLO)
			goto out;

	case NTC_NTB_LINK_HELLO:
	default:
		while (!ntc_ntb_done_hello(dev)) {
			WARN(dev->link_state < NTC_NTB_LINK_HELLO,
					"hello loop: state %d out of sync\n", dev->link_state);
			ntc_ntb_dev_dbg(dev, "not done hello");
			switch (link_event - dev->link_state) {
			default:
				ntc_ntb_dev_err(dev,
						"peer state is not in sync %d %d.",
						link_event, dev->link_state);
				ntc_ntb_error(dev);
				return;
			case -1:
				ntc_ntb_dev_dbg(dev, "peer is behind hello");
				goto out;
			case 0:
			case 1:
				ntc_ntb_dev_dbg(dev, "can advance hello");
				err = ntc_ntb_hello(dev);
				if (err < 0) {
					ntc_ntb_dev_err(dev,
							"ntc_ntb_hello failed "
							"err=%d", err);
					ntc_ntb_error(dev);
					return;
				}
			}
		}

		ntc_ntb_dev_dbg(dev, "done hello, event %d state %d",
				link_event, dev->link_state);

		switch (link_event - dev->link_state) {
		default:
			ntc_ntb_dev_err(dev,
					"peer state is not in sync %d %d. ",
					link_event, dev->link_state);
			ntc_ntb_error(dev);
			/* fall through */
		case -1:
			ntc_ntb_dev_dbg(dev, "peer is not done hello");
			goto out;
		case 0:
			ntc_ntb_dev_info(dev, "both peers are done hello");
			link_up = true;
		}
	}

out:
	ntc_ntb_link_update(dev, link_up);

}

static void ntc_ntb_link_work_cb(struct work_struct *ws)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_link_work(ws);

	mutex_lock(&dev->link_lock);
	ntc_ntb_link_work(dev);
	mutex_unlock(&dev->link_lock);
}

int _ntc_link_disable(struct ntc_dev *ntc, const char *caller)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	ntc_ntb_dev_err(dev, "link disable by upper layer (%s)", caller);

	ntc_ntb_ping_send(dev, NTC_NTB_LINK_QUIESCE);

	return ntb_link_disable(dev->ntb);
}
EXPORT_SYMBOL(_ntc_link_disable);

int _ntc_link_enable(struct ntc_dev *ntc, const char *caller)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	int rc;

	ntc_ntb_dev_info(dev, "link enabled by %s", caller);

	rc = ntb_link_enable(dev->ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);

	if (!rc) {
		ntc_ntb_ping_send(dev, dev->link_state);
		ntb_link_event(dev->ntb);
	}

	return rc;
}
EXPORT_SYMBOL(_ntc_link_enable);

#define RESET_TIMEOUT (1000) /*1 sec*/
int _ntc_link_reset(struct ntc_dev *ntc, bool wait, const char *caller)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	uint tmp_reset_cnt;
	int ret;

	ntc_ntb_dev_info(dev, "link reset requested by %s", caller);
	mutex_lock(&dev->link_lock);

	if (dev->link_state > NTC_NTB_LINK_START)
		ntc_ntb_error(dev);
	if (wait) {
		if (dev->link_state == NTC_NTB_LINK_START) {
			mutex_unlock(&dev->link_lock);
			ntc_ntb_dev_info(dev, "link reset already done");
			return 0;
		}

		tmp_reset_cnt = dev->reset_cnt;
	}
	mutex_unlock(&dev->link_lock);

	if (wait) {
		ret = wait_event_timeout(dev->reset_done,
				dev->reset_cnt - tmp_reset_cnt,
				RESET_TIMEOUT);

		if (unlikely(!ret)) {
			ntc_ntb_dev_err(dev,
					"link reset timeout after %d "
					"current state %d",
					RESET_TIMEOUT, dev->link_state);
			return -ETIME;
		}

		ntc_ntb_dev_info(dev, "link reset done, state %d",
				dev->link_state);
	}
	return 0;
}
EXPORT_SYMBOL(_ntc_link_reset);

static void ntc_req_imm_cb(void *ctx, const struct dmaengine_result *result)
{
	struct ntc_ntb_imm *imm = ctx;

	dev_vdbg(imm->dma_dev, "imm unmap phys %#llx  len %zu",
		imm->dma_addr,  imm->data_len);

	dma_unmap_single(imm->dma_dev,
			 imm->dma_addr,
			 imm->data_len,
			 DMA_TO_DEVICE);

	if (imm->cb)
		imm->cb(imm->cb_ctx);
	if (result && result->result) {
		pr_err(
			"%s: Completion of wrid %#llx addr %#llx len %zu, result %d, residue %d",
			__func__, imm->wrid, imm->dma_addr, imm->data_len,
			result->result, result->residue);
	}
	if (imm->data_trace) {
		WARN_ON(result == NULL);
		trace_dma_completion(imm->wrid, imm->dma_addr, imm->data_len,
				result ? result->result : -1,
				result ? result->residue : -1);
	}
	kmem_cache_free(imm_slab, imm);
}

int ntc_req_imm(struct ntc_dma_chan *chan,
		u64 dst, const void *ptr, size_t len, bool fence,
		void (*cb)(void *cb_ctx), void *cb_ctx, u64 wrid,
		bool need_trace_data)
{
	struct ntc_ntb_imm *imm;
	int rc;

	if (unlikely(!len || len > sizeof(imm->data_buf)))
		return -EINVAL;

	imm = kmem_cache_alloc_node(imm_slab, GFP_ATOMIC,
				dev_to_node(ntc_dma_chan_dev(chan)));
	if (unlikely(!imm)) {
		rc = -ENOMEM;
		goto err_imm;
	}

	memcpy(imm->data_buf, ptr, len);
	imm->data_len = len;

	imm->dma_dev = ntc_dma_chan_dev(chan);
	imm->dma_addr = dma_map_single(imm->dma_dev,
				       imm->data_buf,
				       imm->data_len,
				       DMA_TO_DEVICE);

	if (unlikely(dma_mapping_error(imm->dma_dev, imm->dma_addr))) {
		rc = -EIO;
		goto err_dma;
	}

	dev_vdbg(imm->dma_dev,
			"imm  map phys %#llx virt %p len %zu\n",
			imm->dma_addr, imm->data_buf,
			imm->data_len);

	imm->cb = cb;
	imm->cb_ctx = cb_ctx;
	imm->wrid = wrid;
	imm->data_trace = need_trace_data;

	rc = ntc_req_memcpy(chan, dst, imm->dma_addr, imm->data_len,
			fence, ntc_req_imm_cb, imm, wrid);
	if (unlikely(rc < 0))
		goto err_memcpy;

	return 0;

err_memcpy:
	dma_unmap_single(imm->dma_dev,
			 imm->dma_addr,
			 imm->data_len,
			 DMA_TO_DEVICE);
err_dma:
	kmem_cache_free(imm_slab, imm);
err_imm:
	return rc;
}
EXPORT_SYMBOL(ntc_req_imm);

int ntc_req_signal(struct ntc_dev *ntc, struct ntc_dma_chan *chan,
		void (*cb)(void *cb_ctx), void *cb_ctx, int vec)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	ntc_ntb_dev_vdbg(dev, "request signal to peer");

	if (WARN_ON(vec > ARRAY_SIZE(dev->peer_irq_shift)))
		return -EFAULT;

	if (WARN_ON(ntc->peer_irq_num < vec))
		return -EFAULT;

	return ntc_request_imm32(chan, &dev->peer_irq_base,
				dev->peer_irq_shift[vec],
				dev->peer_irq_data[vec],
				false, cb, cb_ctx);
}
EXPORT_SYMBOL(ntc_req_signal);

int ntc_clear_signal(struct ntc_dev *ntc, int vec)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	int db_bit;

	if (unlikely(vec >= BITS_PER_LONG_LONG))
		dev_WARN(&ntc->dev, "Invalid vec %d \n", vec);
	/* ntc->peer_irq_num could be null if it is not set yet */

	db_bit = BIT_ULL(vec % (ntc->peer_irq_num?:1));

	if (ntb_db_read(dev->ntb) & db_bit) {
		ntb_db_clear(dev->ntb, db_bit);
		ntb_db_read(dev->ntb);
		return 1;
	}

	return 0;
}
EXPORT_SYMBOL(ntc_clear_signal);

const void *ntc_local_hello_buf(struct ntc_dev *ntc, int *size)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	const struct ntc_ntb_info *peer_info;

	*size = NTC_CTX_BUF_SIZE;
	peer_info = ntc_ntb_peer_info(dev);
	return peer_info->ctx_buf;
}
EXPORT_SYMBOL(ntc_local_hello_buf);

void __iomem *ntc_peer_hello_buf(struct ntc_dev *ntc, int *size)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	struct ntc_ntb_info __iomem *self_info;

	self_info = ntc_ntb_self_info(dev);
	*size = NTC_CTX_BUF_SIZE;
	return self_info->ctx_buf;
}
EXPORT_SYMBOL(ntc_peer_hello_buf);

static void ntc_ntb_link_event(void *ctx)
{
	struct ntc_ntb_dev *dev = ctx;

	schedule_work(&dev->link_work);
}

static void ntc_ntb_db_event(void *ctx, int vec)
{
	struct ntc_ntb_dev *dev = ctx;

	ntc_ctx_signal(&dev->ntc, vec);
}

static struct ntb_ctx_ops ntc_ntb_ctx_ops = {
	.link_event			= ntc_ntb_link_event,
	.db_event			= ntc_ntb_db_event,
};

static void ntc_ntb_deinit_peer(struct ntc_ntb_dev *dev, int mw_idx)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_peer_mw *peer_mw = &ntc->peer_mws[mw_idx];

	if (peer_mw->base_ptr) {
		iounmap(peer_mw->base_ptr);
		peer_mw->base_ptr = NULL;
	}
}

static int ntc_ntb_init_peer(struct ntc_ntb_dev *dev, int mw_idx,
			size_t deref_size)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_peer_mw *peer_mw = &ntc->peer_mws[mw_idx];

	ntb_peer_mw_get_addr(dev->ntb, mw_idx,
			&peer_mw->base, &peer_mw->size);
	peer_mw->mw_idx = mw_idx;
	peer_mw->ntc = ntc;
	peer_mw->base_ptr = NULL;

	info("PEER MW: idx %d base %#llx size %#llx",
		mw_idx, peer_mw->base, peer_mw->size);

	if ((peer_mw->size < deref_size) || !peer_mw->base) {
		pr_debug("Not enough peer memory for %#lx bytes.", deref_size);
		return -ENOMEM;
	}

	if (!deref_size)
		return 0;

	peer_mw->base_ptr = ioremap(peer_mw->base, deref_size);
	if (!peer_mw->base_ptr) {
		info("Failed to remap peer memory.");
		return -EIO;
	}

	memset_io(peer_mw->base_ptr, 0, deref_size);
	dev->self_info_done = 0;

	return 0;
}

static int ntc_ntb_init_own_mw_flat(struct ntc_ntb_dev *dev, int mw_idx)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_own_mw_data *data = &own_mw_data[mw_idx];
	struct ntc_own_mw *own_mw = &ntc->own_mws[mw_idx];

	own_mw->base = 0;
	own_mw->base_ptr = NULL;
	own_mw->size = data->size_max;

	if (!own_mw->size) {
		info("size_max for MW %d is %#llx", mw_idx, data->size_max);
		return -EINVAL;
	}

	own_mw->is_flat = true;

	info("OWN MW: FLAT base %#llx size %#llx", own_mw->base, own_mw->size);

	return 0;
}

static int ntc_ntb_init_own_mw_coherent(struct ntc_ntb_dev *dev, int mw_idx)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_own_mw_data *data = &own_mw_data[mw_idx];
	struct ntc_own_mw *own_mw = &ntc->own_mws[mw_idx];
	struct ntc_ntb_coherent_buffer *buffer = &data->coherent;
	resource_size_t size;
	resource_size_t alloc_size;

	size = data->len;
	if (!size)
		size = data->size_max;
	if (data->size_align)
		size = ALIGN(size, data->size_align);
	if (!size)
		return -EINVAL;

	own_mw->size = size;

	alloc_size = size + data->addr_align;
	if (!alloc_size)
		return -EINVAL;
	if (alloc_size > KMALLOC_MAX_SIZE)
		return -EINVAL;

	buffer->size = alloc_size;
	buffer->ptr = dma_alloc_coherent(ntc_ntb_dma_dev(dev), alloc_size,
					&buffer->dma_addr, GFP_KERNEL);
	if (!buffer->ptr) {
		info("OWN INFO MW: cannot alloc. Actual size %#llx.",
			buffer->size);
		return -ENOMEM;
	}

	if (data->addr_align)
		own_mw->base = ALIGN(buffer->dma_addr, data->addr_align);
	else
		own_mw->base = buffer->dma_addr;

	own_mw->base_ptr = buffer->ptr + (own_mw->base - buffer->dma_addr);

	info("OWN MW: COHERENT base %#llx size %#llx ptr %p",
		own_mw->base, own_mw->size, own_mw->base_ptr);
	info("OWN MW: Actual DMA %#llx ptr %p",
		buffer->dma_addr, buffer->ptr);

	return 0;
}

static void ntc_ntb_deinit_own_coherent(struct ntc_ntb_dev *dev, int mw_idx)
{
	struct ntc_own_mw_data *data = &own_mw_data[mw_idx];
	struct ntc_ntb_coherent_buffer *buffer = &data->coherent;

	dma_free_coherent(ntc_ntb_dma_dev(dev),
			buffer->size, buffer->ptr, buffer->dma_addr);
}

static int ntc_ntb_init_own_mw_reserved(struct ntc_ntb_dev *dev, int mw_idx)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_own_mw_data *data = &own_mw_data[mw_idx];
	struct ntc_own_mw *own_mw = &ntc->own_mws[mw_idx];

	if (data->mm_len) {
		own_mw->base_ptr =
			memremap(data->base_addr, data->mm_len, MEMREMAP_WB);
		if (!own_mw->base_ptr) {
			info("OWN MW: cannot memremap. Start %#lx. Size %#lx.",
				data->base_addr, data->mm_len);
			return -EIO;
		}
	} else
		own_mw->base_ptr = NULL;

	own_mw->base = data->base_addr;
	own_mw->size = data->len;

	info("OWN MW: RESERVED base %#llx size %#llx base_ptr %p",
		own_mw->base, own_mw->size, own_mw->base_ptr);

	return 0;
}

static void ntc_ntb_deinit_own_mw_reserved(struct ntc_ntb_dev *dev,
					int mw_idx)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_own_mw *own_mw = &ntc->own_mws[mw_idx];

	if (own_mw->base_ptr)
		memunmap(own_mw->base_ptr);
}

static void ntc_ntb_deinit_own(struct ntc_ntb_dev *dev, int mw_idx)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_own_mw_data *data = &own_mw_data[mw_idx];
	struct ntc_own_mw *own_mw = &ntc->own_mws[mw_idx];

	ntb_mw_clear_trans(dev->ntb, NTB_DEF_PEER_IDX, mw_idx);

	if (data->mm_inited) {
		ntc_mm_deinit(&own_mw->mm);
		data->mm_inited = false;
	}

	if (data->reserved_used) {
		ntc_ntb_deinit_own_mw_reserved(dev, mw_idx);
		data->reserved_used = false;
	}

	if (data->coherent_used) {
		ntc_ntb_deinit_own_coherent(dev, mw_idx);
		data->coherent_used = false;
	}

	if (data->flat_used) {
		own_mw->is_flat = false;
		data->flat_used = false;
	}
}

static int ntc_ntb_init_own(struct ntc_ntb_dev *dev, int mw_idx)
{
	struct ntc_dev *ntc = &dev->ntc;
	struct ntc_own_mw_data *data = &own_mw_data[mw_idx];
	struct ntc_own_mw *own_mw = &ntc->own_mws[mw_idx];
	int rc;

	own_mw->mw_idx = mw_idx;
	own_mw->ntc = ntc;
	own_mw->ntb_dev = ntc->ntb_dev;
	own_mw->is_flat = false;

	data->reserved_used = false;
	data->coherent_used = false;
	data->flat_used = false;
	data->len = PAGE_ALIGN(data->len);
	data->mm_len = PAGE_ALIGN(data->mm_len);

	ntb_mw_clear_trans(dev->ntb, NTB_DEF_PEER_IDX, mw_idx);

	ntb_mw_get_align(dev->ntb, NTB_DEF_PEER_IDX, mw_idx,
			&data->addr_align, &data->size_align, &data->size_max);

	if (data->mm_len > data->len) {
		info("Requested MM of length %#lx in MW of length %#lx",
			data->mm_len, data->len);
		return -EINVAL;
	}

	if (data->mm_prealloc > data->mm_len) {
		info("Requested preallocation %#lx in MM of length %#lx",
			data->mm_prealloc, data->mm_len);
		return -EINVAL;
	}

	if (data->len > data->size_max) {
		info("Requested MW of length %#lx, but max_size is %#llx",
			data->len, data->size_max);
		return -EINVAL;
	}

	if (data->size_align &&
		(data->len & (data->size_align - 1))) {
		info("Requested MW of length %#lx, but alignment is %#llx",
			data->len, data->size_align);
		return -EINVAL;
	}

	if (data->addr_align &&
		(data->base_addr & (data->addr_align - 1))) {
		info("Requested MW @%#lx, but alignment is %#llx",
			data->base_addr, data->addr_align);
		return -EINVAL;
	}

	if (data->reserved) {
		rc = ntc_ntb_init_own_mw_reserved(dev, mw_idx);
		if (rc < 0)
			goto err;
		data->reserved_used = true;
		goto init_mm;
	}

	if (mw_idx == NTC_DRAM_MW_IDX) {
		rc = ntc_ntb_init_own_mw_flat(dev, mw_idx);
		if (rc < 0)
			goto err;
		data->flat_used = true;
		goto init_mm;
	}

	rc = ntc_ntb_init_own_mw_coherent(dev, mw_idx);
	if (rc < 0)
		goto err;
	data->coherent_used = true;

 init_mm:
	info("ntc_mm_init mw_idx %d mm_len=%#lx mm_prealloc=%#lx",
		mw_idx, data->mm_len, data->mm_prealloc);
	rc = ntc_mm_init(&own_mw->mm, own_mw->base_ptr + data->mm_prealloc,
			data->mm_len - data->mm_prealloc);
	if (rc < 0) {
		info("ntc_mm_init failed mm_len=%#lx mm_prealloc=%#lx",
			data->mm_len, data->mm_prealloc);
		goto err;
	}
	data->mm_inited = true;

	rc = ntb_mw_set_trans(dev->ntb, NTB_DEF_PEER_IDX, mw_idx,
			own_mw->base, own_mw->size);
	if (rc < 0) {
		info("ntb_mw_set_trans failed idx %d base %#llx size %#llx",
			mw_idx, own_mw->base, own_mw->size);
		goto err;
	}

	return rc;

 err:
	ntc_ntb_deinit_own(dev, mw_idx);
	return rc;
}

static int ntc_ntb_dev_init(struct ntc_ntb_dev *dev)
{
	int rc, mw_count;
	struct ntc_dev *ntc = &dev->ntc;
	int cpu;

	/* inherit ntb device name and configuration */
	dev_set_name(&ntc->dev, "%s", dev_name(&dev->ntb->dev));
	ntc->dev.parent = &dev->ntb->dev;

	ntc->ntb_dev = ntc_ntb_dma_dev(dev);

	/* make sure link is disabled and warnings are cleared */
	ntb_link_disable(dev->ntb);
	ntb_db_is_unsafe(dev->ntb);
	ntb_spad_is_unsafe(dev->ntb);

	/* we'll be using the last memory window if it exists */
	mw_count = ntb_mw_count(dev->ntb, NTB_DEF_PEER_IDX);
	if (mw_count <= 0) {
		ntc_err(ntc, "no mw for new device");
		return -EINVAL;
	}
	if (mw_count < 2) {
		ntc_err(ntc, "not enough memory windows for new device");
		return -EINVAL;
	}

	rc = ntc_ntb_init_own(dev, NTC_DRAM_MW_IDX);
	if (rc < 0)
		goto err_init_own_dram;

	rc = ntc_ntb_init_own(dev, NTC_INFO_MW_IDX);
	if (rc < 0)
		goto err_init_own_info;

	rc = ntc_ntb_init_peer(dev, NTC_DRAM_MW_IDX, 0);
	if (rc < 0)
		goto err_init_peer_dram;

	rc = ntc_ntb_init_peer(dev, NTC_INFO_MW_IDX,
			sizeof(struct ntc_ntb_info));
	if (rc < 0)
		goto err_init_peer_info;

	/* haven't negotiated the version */
	ntc->version = 0;
	ntc->latest_version = 0;

	/* haven't negotiated peer_irq_data */
	ntc->peer_irq_num = 0;
	ntc_remote_buf_clear(&dev->peer_irq_base);
	memset(dev->peer_irq_data, 0, sizeof(dev->peer_irq_data));

	/* init the link state heartbeat */
	dev->ping_run = false;
	dev->ping_miss = 0;
	dev->ping_flags = 0;
	dev->ping_seq = 0;
	dev->ping_msg = NTC_NTB_LINK_START;
	dev->poll_val = 0;
	dev->poll_msg = NTC_NTB_LINK_QUIESCE;
	dev->timer_cpu_mask = *cpu_online_mask;

	for_each_online_cpu(cpu) {
		setup_timer(&dev->ping_pong[cpu],
		ntc_ntb_ping_pong_cb,
		ntc_ntb_to_ptrhld(dev));
	}

	setup_timer(&dev->ping_poll,
		    ntc_ntb_ping_poll_cb,
		    ntc_ntb_to_ptrhld(dev));

	spin_lock_init(&dev->ping_lock);

	/* init the link state machine */
	ntc->link_is_up = false;
	dev->link_state = NTC_NTB_LINK_START;

	INIT_WORK(&dev->link_work,
		  ntc_ntb_link_work_cb);

	mutex_init(&dev->link_lock);
	init_waitqueue_head(&dev->reset_done);

	/* ready for context events */
	rc = ntb_set_ctx(dev->ntb, dev,
			 &ntc_ntb_ctx_ops);
	if (rc) {
		pr_debug("failed to set ctx for new device %s\n",
			 dev_name(&dev->ntb->dev));
		goto err_ctx;
	}

	return 0;

 err_ctx:
	ntc_ntb_deinit_peer(dev, NTC_INFO_MW_IDX);
 err_init_peer_info:
	ntc_ntb_deinit_peer(dev, NTC_DRAM_MW_IDX);
 err_init_peer_dram:
	ntc_ntb_deinit_own(dev, NTC_INFO_MW_IDX);
 err_init_own_info:
	ntc_ntb_deinit_own(dev, NTC_DRAM_MW_IDX);
 err_init_own_dram:
	return rc;
}

static void ntc_ntb_dev_deinit(struct ntc_ntb_dev *dev)
{
	ntb_clear_ctx(dev->ntb);

	ntb_link_disable(dev->ntb);

	ntc_ntb_ping_stop(dev);

	cancel_work_sync(&dev->link_work);

	ntc_ntb_deinit_peer(dev, NTC_INFO_MW_IDX);
	ntc_ntb_deinit_peer(dev, NTC_DRAM_MW_IDX);
	ntc_ntb_deinit_own(dev, NTC_INFO_MW_IDX);
	ntc_ntb_deinit_own(dev, NTC_DRAM_MW_IDX);
}

static bool ntc_ntb_filter_bus(struct dma_chan *chan,
			       void *filter_param)
{
	return true;
}

void ntc_init_dma(struct ntc_dev *ntc)
{
	int j;

	memset(&ntc->dma_chan, 0, sizeof(ntc->dma_chan));
	for (j = 0; j < ARRAY_SIZE(ntc->dma_chan); j++)
		ntc->dma_chan[j].ntc = ntc;
}

static bool ntc_request_dma(struct ntc_dev *ntc)
{
	struct dma_chan *dma;
	dma_cap_mask_t mask;
	int i, j;

	if (ntc->dma_chan[0].chan)
		return true;

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	for (i = 0, j = 0; i < num_dma_chan; i++) {
		dma = dma_request_channel(mask, ntc_ntb_filter_bus, NULL);
		ntc->dma_chan[j].chan = dma;
		if (dma)
			j++;
	}

	return j > 0;
}

static void ntc_release_dma(struct ntc_dev *ntc)
{
	struct dma_chan *dma;
	int i;

	for (i = 0; i < ARRAY_SIZE(ntc->dma_chan); i++) {
		dma = ntc->dma_chan[i].chan;
		ntc->dma_chan[i].chan = NULL;
		if (!dma)
			break;
		dma_release_channel(dma);
	}
}

static void ntc_ntb_release(struct device *device)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_dev(device);

	pr_debug("release %s\n", dev_name(&dev->ntc.dev));

	ntc_ntb_dev_deinit(dev);
	put_device(&dev->ntb->dev);

	ntc_release_dma(&dev->ntc);
	kfree(dev);
}

static int ntc_debugfs_read(struct seq_file *s, void *v)
{
	struct ntc_ntb_dev *dev = s->private;
	struct ntc_dev *ntc = &dev->ntc;
	const struct ntc_ntb_info *peer_info;
	int i;

	peer_info = ntc_ntb_peer_info(dev);

	seq_printf(s, "ntc->peer_mws[NTC_DRAM_MW_IDX].base %#llx\n",
		ntc->peer_mws[NTC_DRAM_MW_IDX].base);
	seq_printf(s, "ntc->own_mws[NTC_INFO_MW_IDX].size %#llx\n",
		   ntc->own_mws[NTC_INFO_MW_IDX].size);
	seq_printf(s, "ntc->own_mws[NTC_INFO_MW_IDX].base %#llx\n",
		   ntc->own_mws[NTC_INFO_MW_IDX].base);
	seq_puts(s, "info_peer_on_self:\n");
	seq_printf(s, "  magic %#x\n", peer_info->magic);
	seq_printf(s, "  ping %#x\n", peer_info->ping);
	seq_puts(s, "  ctx level negotiation:\n");
	seq_printf(s, "    done %#x\n", peer_info->done);
	seq_printf(s, "version %d\n", ntc->version);
	seq_printf(s, "peer_irq_num %d\n", ntc->peer_irq_num);
	seq_printf(s, "peer_irq_base.dma_addr %#llx\n",
		dev->peer_irq_base.dma_addr);
	for (i = 0; i < ntc->peer_irq_num; i++) {
		seq_printf(s, "peer_irq_shift %#llx\n",
			dev->peer_irq_shift[i]);
		seq_printf(s, "peer_irq_data %#x\n",
				dev->peer_irq_data[i]);
	}
	seq_printf(s, "ping_run %d\n",
		   dev->ping_run);
	seq_printf(s, "ping_miss %d\n",
		   dev->ping_miss);
	seq_printf(s, "ping_flags %#x\n",
		   dev->ping_flags);
	seq_printf(s, "ping_seq %#hx\n",
		   dev->ping_seq);
	seq_printf(s, "ping_msg %#hx\n",
		   dev->ping_msg);
	seq_printf(s, "poll_val %#x\n",
		   dev->poll_val);
	seq_printf(s, "poll_msg %#hx\n",
		   dev->poll_msg);
	seq_printf(s, "link_is_up %d\n",
		   ntc->link_is_up);
	seq_printf(s, "link_state %d\n",
		   dev->link_state);

	return 0;
}

static int ntc_debugfs_open(struct inode *inode, struct file *file)
{
	return single_open(file, ntc_debugfs_read, inode->i_private);
}

static const struct file_operations ntc_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = ntc_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static void ntc_setup_debugfs(struct ntc_ntb_dev *dev)
{
	if (!ntc_dbgfs) {
		dev->dbgfs = NULL;
		return;
	}

	dev->dbgfs = debugfs_create_dir(dev_name(&dev->ntb->dev),
					ntc_dbgfs);
	if (!dev->dbgfs)
		return;

	debugfs_create_file("info", S_IRUSR, dev->dbgfs,
			    dev, &ntc_debugfs_fops);
}

static int set_affinity(struct ntb_dev *ntb)
{
	cpumask_t cpu_mask;
	int max_irqs, msi_irqs_base, i = 0, rc = 0;
	struct pci_dev *pdev = ntb->pdev;
	unsigned int online_cpus = 0;

	max_irqs = ntb_db_vector_count(ntb);
	if (max_irqs <= 0 || max_irqs > NTB_MAX_IRQS) {
		pr_err("max_irqs %d is not supported\n", max_irqs);
		return -EFAULT;
	}

	msi_irqs_base = pci_irq_vector(pdev, 0);
	online_cpus = num_online_cpus();

	if (online_cpus <= 0) {
		pr_err("online_cpus is %d, modulo is undefined\n",
				online_cpus);
		return -EFAULT;
	}

	pr_info("msi_irq_base %u, max_irqs %d, online CPUs %d\n",
			msi_irqs_base, max_irqs, online_cpus);

	for (i = 0 ; i < max_irqs ; i++) {
		cpumask_clear(&cpu_mask);
		cpumask_set_cpu(i % online_cpus, &cpu_mask);
		pr_info("SET AFFINITY: irq %d cpu %d cpumask %lu\n",
			msi_irqs_base + i, i % online_cpus,
			*(unsigned long *)cpumask_bits(&cpu_mask));
		rc = irq_set_affinity_hint(msi_irqs_base + i, &cpu_mask);
		if (rc < 0)
			return rc;
	}
	return 0;
}

static int ntc_ntb_probe(struct ntb_client *self,
			 struct ntb_dev *ntb)
{
	struct ntc_ntb_dev *dev;
	enum ntc_dma_chan_type type;
	int rc;

	pr_debug("probe ntb %s\n", dev_name(&ntb->dev));

	dev = kzalloc_node(sizeof(*dev), GFP_KERNEL,
			   dev_to_node(&ntb->dev));
	if (!dev) {
		rc = -ENOMEM;
		goto err_dev;
	}

	ntc_init_dma(&dev->ntc);

	for (type = 0; type < NTC_NUM_DMA_CHAN_TYPES; type++)
		atomic_set(&dev->ntc.dma_chan_rr_index[type], 0);

	get_device(&ntb->dev);
	dev->ntb = ntb;

	rc = ntc_ntb_dev_init(dev);
	if (rc)
		goto err_init;

	dev->ntc.dev.release = ntc_ntb_release;

	ntc_setup_debugfs(dev);
	rc = set_affinity(dev->ntb);
	if (rc < 0)
		pr_debug("set_affinity failed rc %d\n", rc);

	ntc_ntb_dev_info(dev, "success");

	return ntc_register_device(&dev->ntc);

err_init:
	put_device(&ntb->dev);
	kfree(dev);
err_dev:
	pr_err("%s failure rc=%d", __func__, rc);
	return rc;
}

static void ntc_ntb_remove(struct ntb_client *self, struct ntb_dev *ntb)
{
	struct ntc_ntb_dev *dev = ntb->ctx;

	ntc_ntb_dev_info(dev, "called");

	debugfs_remove_recursive(dev->dbgfs);

	ntc_unregister_device(&dev->ntc);
}

struct ntb_client ntc_ntb_client = {
	.ops = {
		.probe			= ntc_ntb_probe,
		.remove			= ntc_ntb_remove,
	},
};

static void ntc_deinit(void)
{
	if (imm_slab) {
		kmem_cache_destroy(imm_slab);
		imm_slab = NULL;
	}
}

int __init ntc_init(void)
{
	int i;
	unsigned long mw0_mm_prealloc = sizeof(struct ntc_ntb_info);
	unsigned long mw0_min_mm_len = 0x100000;

	info("%s %s init", DRIVER_DESCRIPTION, DRIVER_VERSION);

	if (!(imm_slab = KMEM_CACHE(ntc_ntb_imm, 0))) {
		ntc_deinit();
		return -ENOMEM;
	}

	if (((int)num_dma_chan) <= 0)
		num_dma_chan = NTC_DEFAULT_DMA_CHANS;
	if (num_dma_chan > NTC_MAX_DMA_CHANS)
		num_dma_chan = NTC_MAX_DMA_CHANS;
	if (!mw0_mm_len)
		mw0_mm_len = mw0_len;
	if (!mw0_mm_len)
		mw0_mm_len = mw0_mm_prealloc;
	if (mw0_mm_len < mw0_min_mm_len)
		mw0_mm_len = mw0_min_mm_len;
	if (!mw0_len)
		mw0_len = mw0_mm_len;

	own_mw_data[NTC_INFO_MW_IDX].base_addr = mw0_base_addr;
	own_mw_data[NTC_INFO_MW_IDX].len = mw0_len;
	own_mw_data[NTC_INFO_MW_IDX].mm_len = mw0_mm_len;
	own_mw_data[NTC_INFO_MW_IDX].mm_prealloc =
		mw0_mm_len ? mw0_mm_prealloc : 0;
	own_mw_data[NTC_INFO_MW_IDX].reserved = false;

	own_mw_data[NTC_DRAM_MW_IDX].base_addr = mw1_base_addr;
	own_mw_data[NTC_DRAM_MW_IDX].len = mw1_len;
	own_mw_data[NTC_DRAM_MW_IDX].mm_len = mw1_mm_len;
	own_mw_data[NTC_DRAM_MW_IDX].mm_prealloc = 0;
	own_mw_data[NTC_DRAM_MW_IDX].reserved = false;

	for (i = 0; i < 2; i++)
		ntc_own_mw_data_check_reserved(&own_mw_data[i]);

	if (debugfs_initialized())
		ntc_dbgfs = debugfs_create_dir(KBUILD_MODNAME, NULL);
	ntb_register_client(&ntc_ntb_client);
	return 0;
}

void __exit ntc_exit(void)
{
	ntb_unregister_client(&ntc_ntb_client);
	if (ntc_dbgfs)
		debugfs_remove_recursive(ntc_dbgfs);
	ntc_deinit();

	info("%s %s exit", DRIVER_DESCRIPTION, DRIVER_VERSION);
}
