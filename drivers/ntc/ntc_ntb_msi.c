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

#include <linux/debugfs.h>
#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ntb.h>
#include <linux/pci.h>
#include <linux/timer.h>
#include <linux/msi.h>

#include <linux/ntc.h>
#include <linux/ntc_trace.h>

#include <asm/e820/api.h>

#include "ntc_internal.h"

#define DRIVER_NAME			"ntc_ntb"
#define DRIVER_DESCRIPTION		"NTC Non Transparent Bridge"

#define DRIVER_VERSION			"0.3"

static bool mw0_reserved;
static unsigned long mw0_base_addr;
module_param(mw0_base_addr, ulong, 0444);
static unsigned long mw0_len;
module_param(mw0_len, ulong, 0444);
static bool mw1_reserved;
static unsigned long mw1_base_addr;
module_param(mw1_base_addr, ulong, 0444);
static unsigned long mw1_len;
module_param(mw1_len, ulong, 0444);

static struct dentry *ntc_dbgfs;

static int ntc_ntb_info_size = 0x1000;
/* TODO: module param named info_size */

static bool use_msi;
module_param(use_msi, bool, 0444);
MODULE_PARM_DESC(use_msi, "Use MSI(X) as interrupts");

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

#define NTC_NTB_DMA_PREP_FLAGS		0

#define NTC_NTB_PING_PONG_SPAD		BIT(1)
#define NTC_NTB_PING_PONG_MEM		BIT(2)
#define NTC_NTB_PING_POLL_MEM		BIT(3)

#define NTC_NTB_PING_PONG_PERIOD	msecs_to_jiffies(100)
#define NTC_NTB_PING_POLL_PERIOD	msecs_to_jiffies(257)
#define NTC_NTB_PING_MISS_THRESHOLD	10

#define NTC_CTX_BUF_SIZE 1024

/* PCIe spec - TLP data must be 4-byte naturally
 * aligned and in increments of 4-byte Double Words (DW).
 */
#define PCIE_ADDR_ALIGN 4
#define INTEL_DOORBELL_REG_SIZE (4)
#define INTEL_DOORBELL_REG_OFFSET(__bit) (bit * INTEL_DOORBELL_REG_SIZE)

#define info(...) do { pr_info(DRIVER_NAME ": " __VA_ARGS__); } while (0)

struct ntc_ntb_imm {
	char				data_buf[sizeof(u64)];
	size_t				data_len;
	struct device			*dma_dev;
	dma_addr_t			dma_addr;
	void				(*cb)(void *cb_ctx);
	void				*cb_ctx;
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

	u32				msi_data[NTB_MAX_IRQS];
	u32				msi_addr_lower[NTB_MAX_IRQS];
	u32				msi_addr_upper[NTB_MAX_IRQS];
	u32				msi_irqs_num;

	u32			done;

	/* ctx buf for use by the client */
	u8	ctx_buf[NTC_CTX_BUF_SIZE];
};

struct ntc_ntb_dev {
	struct ntc_dev			ntc;

	/* channel supporting hardware devices */
	struct ntb_dev			*ntb;

	/* local ntb window offset to peer dram */
	dma_addr_t		peer_dram_base_dma;

	bool				info_mem_reserved;
	/* The following field used only when !info_mem_reserved */
	struct {
		void		*ptr;
		resource_size_t	size;
		dma_addr_t	dma_addr;
	} info_mem_buffer;

	/* remote buffer for local driver to write info */
	struct ntc_ntb_info __iomem	*info_self_on_peer;

	/* direct interrupt message */
	struct ntc_remote_buf		peer_irq_base;
	u64				peer_irq_shift[NTB_MAX_IRQS];
	u32				peer_irq_data[NTB_MAX_IRQS];
	bool				use_peer_irq_base;

	/* link state heartbeat */
	bool				ping_run;
	int				ping_miss;
	int				ping_flags;
	u16				ping_seq;
	u16				ping_msg;
	u32				poll_val;
	u16				poll_msg;
	struct timer_list		ping_pong;
	struct timer_list		ping_poll;
	spinlock_t			ping_lock;

	/* link state machine */
	int				link_state;
	struct work_struct		link_work;
	struct mutex			link_lock;

	struct dentry *dbgfs;
	wait_queue_head_t	reset_done;
	uint				reset_cnt;
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

static inline
const struct ntc_ntb_info *ntc_ntb_peer_info(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;

	return ntc->own_info_mw.base_ptr;
}

static int ntc_ntb_read_msi_config(struct ntc_ntb_dev *dev,
				    u64 *addr, u32 *val)
{
	u32 addr_lo, addr_hi = 0, data;
	struct pci_dev *pdev = dev->ntb->pdev;
	void __iomem *base;

	if (!use_msi)
		return 0;

	/* FIXME: add support for 64-bit MSI header, and MSI-X.  Until it is
	 * fixed here, we need to disable MSI-X in the NTB HW driver.
	 */

	if (pdev->msi_enabled) {
		pci_read_config_dword(pdev,
				      PCI_MSI_ADDRESS_LO + pdev->msi_cap,
				      &addr_lo);

		/*
		 * FIXME: msi header layout differs for 32/64
		 * bit addr width.
		 */
		pci_read_config_dword(pdev,
				      pdev->msi_cap + PCI_MSI_DATA_32,
				      &data);
		addr[0] = (u64)addr_lo | (u64)addr_hi << 32;
		val[0] = data;

		dev_dbg(&dev->ntc.dev, "msi addr %#llx data %#x\n", *addr, *val);

		return 1;
	} else if (pdev->msix_enabled) {
		struct msi_desc *entry;
		int msi_idx = 0;

		list_for_each_entry(entry, &pdev->dev.msi_list, list) {
			base = entry->mask_base +
				entry->msi_attrib.entry_nr * PCI_MSIX_ENTRY_SIZE;

			addr_hi = readl(base + PCI_MSIX_ENTRY_UPPER_ADDR);
			addr_lo = readl(base + PCI_MSIX_ENTRY_LOWER_ADDR);
			data = readl(base + PCI_MSIX_ENTRY_DATA);
			
			addr[msi_idx] = (u64)addr_lo | (u64)addr_hi << 32;
			val[msi_idx] = data;

			dev_dbg(&dev->ntc.dev, "msix addr hi %x addr low %x data %x\n", addr_hi ,addr_lo, data);

			msi_idx++;
		}

		return msi_idx;
	}

	return 0;


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

	dev_err(&ntc->dev, "Local supported versions (%d) are:\n", v1->num);
	for (j = 0; j < v1->num; j++)
		dev_err(&ntc->dev, "0x%08x\n", v1->versions[j]);
	dev_err(&ntc->dev, "Remote supported versions (%d) are:\n", v2->num);
	for (i = 0; i < v2->num; i++)
		dev_err(&ntc->dev, "0x%08x\n", v2->versions[i]);
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

static void ntc_ntb_ping_pong(struct ntc_ntb_dev *dev)
{
	int ping_flags, poison_flags;
	u32 ping_val;

	if (!dev->ping_run)
		return;
	mod_timer(&dev->ping_pong, jiffies + NTC_NTB_PING_PONG_PERIOD);

	ping_flags = ntc_ntb_ping_flags(dev->ping_msg);
	poison_flags = dev->ping_flags & ~ping_flags;
	(void)poison_flags; /* TODO: cleanup unused */

	ping_val = ntc_ntb_ping_val(dev->ping_msg, ++dev->ping_seq);

	dev_vdbg(&dev->ntc.dev, "ping val %x\n", ping_val);

	wmb(); /* fence anything prior to writing the message */

	iowrite32(ping_val, &dev->info_self_on_peer->ping);

	dev->ping_flags = ping_flags;
}

static void ntc_ntb_ping_pong_cb(unsigned long ptrhld)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_ptrhld(ptrhld);
	unsigned long irqflags;

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	ntc_ntb_ping_pong(dev);
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

	dev_vdbg(&dev->ntc.dev, "poll val %x\n", poll_val);

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
		dev_vdbg(&dev->ntc.dev, "ping hit\n");
		dev->ping_miss = 0;
		poll_msg = ntc_ntb_ping_msg(dev->poll_val);
		if (dev->poll_msg != poll_msg) {
			dev_dbg(&dev->ntc.dev, "peer msg %d\n", poll_msg);
			dev->poll_msg = poll_msg;
			schedule_work(&dev->link_work);
		}
	} else if (dev->ping_miss < NTC_NTB_PING_MISS_THRESHOLD) {
		++dev->ping_miss;
		dev_dbg(&dev->ntc.dev, "ping miss %d\n", dev->ping_miss);
		if (dev->ping_miss == NTC_NTB_PING_MISS_THRESHOLD) {
			dev_err(&dev->ntc.dev, "peer lost - moving to quiesce state\n");
			dev->poll_msg = NTC_NTB_LINK_QUIESCE;
			schedule_work(&dev->link_work);
		}
	}
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);
}

static void ntc_ntb_ping_send(struct ntc_ntb_dev *dev, int msg)
{
	unsigned long irqflags;

	dev_dbg(&dev->ntc.dev, "ping send msg %x\n", msg);

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	dev->ping_msg = msg;
	ntc_ntb_ping_pong(dev);
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);
}

static int ntc_ntb_ping_start(struct ntc_ntb_dev *dev)
{
	unsigned long irqflags;
	int msg;

	dev_dbg(&dev->ntc.dev, "ping start\n");

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	if (!dev->ping_run) {
		dev->ping_run = true;
		dev->ping_miss = NTC_NTB_PING_MISS_THRESHOLD;
		ntc_ntb_ping_pong(dev);
		ntc_ntb_ping_poll(dev);
		dev->poll_msg = NTC_NTB_LINK_QUIESCE;
	}
	msg = dev->poll_msg;
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);

	rmb(); /* fence anything after reading the message */

	return msg;
}

static void ntc_ntb_ping_stop(struct ntc_ntb_dev *dev)
{
	unsigned long irqflags;

	dev_dbg(&dev->ntc.dev, "ping stop\n");

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	dev->ping_run = false;
	del_timer_sync(&dev->ping_pong);
	del_timer_sync(&dev->ping_poll);
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);
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
	dev_err(&dev->ntc.dev, "link error\n");

	if (!(dev->link_state > NTC_NTB_LINK_RESET)) {
		pr_info(
				"NTC: link reset call rejected , current link state %d\n",
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
		if (err)
			ntc_ntb_error(dev);
	} else
		ntc_ctx_disable(ntc);
}

static inline void ntc_ntb_quiesce(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link quiesce\n");
	ntc_ntb_link_update(dev, false);
	ntc_ctx_quiesce(&dev->ntc);

	/* TODO: cancel and wait for any outstanding dma requests */

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_RESET);
}

static inline void reset_peer_irq(struct ntc_ntb_dev *dev)
{
	if (use_msi)
		return;

	if (dev->use_peer_irq_base)
		ntc_remote_buf_unmap(&dev->peer_irq_base, &dev->ntc);
}

static inline void ntc_ntb_reset(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link reset\n");

	ntc_ctx_reset(&dev->ntc);

	reset_peer_irq(dev);
	dev->reset_cnt++;
	wake_up(&dev->reset_done);
	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_START);
}

static inline void ntc_ntb_send_version(struct ntc_ntb_dev *dev)
{
	struct multi_version_support versions;
	int i;


	dev_dbg(&dev->ntc.dev, "link send version\n");

	ntc_ntb_version(&versions);

	for (i = 0; i < versions.num; i++)
		iowrite32(versions.versions[i],
			&dev->info_self_on_peer->versions.versions[i]);
	iowrite32(versions.num, &dev->info_self_on_peer->versions.num);

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_VER_SENT);
}

static inline void ntc_ntb_choose_version(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;
	const struct ntc_ntb_info *peer_info;
	struct multi_version_support versions;
	struct multi_version_support peer_versions;
	u64 msi_addr[NTB_MAX_IRQS];
	u32 msi_data[NTB_MAX_IRQS];
	int msi_idx;
	u32 msi_irqs_num;

	dev_dbg(&ntc->dev, "link choose version and send msi data\n");

	ntc_ntb_version(&versions);
	peer_info = ntc_ntb_peer_info(dev);
	peer_versions = peer_info->versions;
	if (peer_versions.num > MAX_SUPPORTED_VERSIONS)
		goto err;

	if (versions.num > 0)
		ntc->latest_version = versions.versions[versions.num-1];

	ntc->version = ntc_ntb_version_matching(ntc, &versions, &peer_versions);
	if (ntc->version == NTC_NTB_VERSION_NONE)
		goto err;

	dev_dbg(&ntc->dev, "Agree on version %d\n", ntc->version);

	iowrite32(ntc_ntb_version_magic(ntc->version),
		  &dev->info_self_on_peer->magic);

	iowrite32(0, &dev->info_self_on_peer->done);

	msi_irqs_num = ntc_ntb_read_msi_config(dev, msi_addr, msi_data);
	if (msi_irqs_num > NTB_MAX_IRQS) {
		dev_err(&ntc->dev, "msi_irqs_num %u is above max\n",
			msi_irqs_num);
		goto err;
	}

	iowrite32(msi_irqs_num, &dev->info_self_on_peer->msi_irqs_num);

	for (msi_idx = 0; msi_idx < msi_irqs_num; msi_idx++) {
		iowrite32(msi_data[msi_idx], &dev->info_self_on_peer->msi_data[msi_idx]);
		iowrite32(lower_32_bits(msi_addr[msi_idx]),
				&dev->info_self_on_peer->msi_addr_lower[msi_idx]);
		iowrite32(upper_32_bits(msi_addr[msi_idx]),
				&dev->info_self_on_peer->msi_addr_upper[msi_idx]);
	}

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_VER_CHOSEN);
	return;

err:
	ntc_ntb_error(dev);
}

static inline int ntc_ntb_db_config(struct ntc_ntb_dev *dev)
{
	int rc;
	int i;
	resource_size_t size;
	struct ntc_dev *ntc = &dev->ntc;
	const struct ntc_ntb_info *peer_info;
	u32 msi_irqs_num;

	peer_info = ntc_ntb_peer_info(dev);
	msi_irqs_num = peer_info->msi_irqs_num;

	if (use_msi && msi_irqs_num > 0 && msi_irqs_num < NTB_MAX_IRQS) {
		dev->use_peer_irq_base = false;
		for (i = 0; i < msi_irqs_num; i++) {
			dev->peer_irq_data[i] =
					peer_info->msi_data[i];
			dev->peer_irq_shift[i] =
				(((u64)peer_info->msi_addr_lower[i]) |
				 (((u64)peer_info->msi_addr_upper[i]) << 32));
		}
	} else {
		phys_addr_t peer_irq_phys_addr_base;
		u64 peer_db_mask;
		int max_irqs;
		u64 db_bits;

		dev->use_peer_irq_base = true;
		ntc->peer_irq_num = 0;

		max_irqs = ntb_db_vector_count(dev->ntb);

		if (max_irqs <= 0 || max_irqs> NTB_MAX_IRQS) {
			dev_err(&ntc->dev, "max_irqs %d - not supported\n",
					peer_info->msi_irqs_num);
			rc = -EINVAL;
			goto err_ntb_db;
		}

		rc = ntb_peer_db_addr(dev->ntb,
				&peer_irq_phys_addr_base, &size);
		if ((rc < 0) || (size != sizeof(u32)) ||
			!IS_ALIGNED(peer_irq_phys_addr_base, PCIE_ADDR_ALIGN)) {
			dev_err(&ntc->dev, "Peer DB addr invalid\n");
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

		dev_dbg(&ntc->dev,
				"Peer DB addr: %#llx count %d mask %#llx\n",
				peer_irq_phys_addr_base,
				ntc->peer_irq_num, peer_db_mask);

		ntb_db_clear(dev->ntb, peer_db_mask);
		ntb_db_clear_mask(dev->ntb, peer_db_mask);
	}

	dev_dbg(&ntc->dev, "link signaling method configured\n");
	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_DB_CONFIGURED);
	return 0;

err_res_map:
	ntc->peer_irq_num = 0;
err_ntb_db:
	return rc;
}

static inline void ntc_ntb_link_commit(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link commit - verifying both sides sync\n");

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_COMMITTED);
}

static inline bool ntc_ntb_done_hello(struct ntc_ntb_dev *dev)
{
	const struct ntc_ntb_info *peer_info;

	peer_info = ntc_ntb_peer_info(dev);

	/* no outstanding input or output buffers */
	return (peer_info->done == 1) && (dev->info_self_on_peer->done == 1);
}

static inline int ntc_ntb_hello(struct ntc_ntb_dev *dev)
{
	int ret = 0;
	int phase = dev->link_state + 1 - NTC_NTB_LINK_HELLO;

	dev_dbg(&dev->ntc.dev, "link hello phase %d\n", phase);

	if (phase < 0)
		return -EINVAL;

	/* perform this phase of initialization */
	ret = ntc_ctx_hello(&dev->ntc, phase, 0, 0, 0, 0);
	if (ret < 0)
		return ret;

	if (ret == 1)
		iowrite32(1, &dev->info_self_on_peer->done);

	dev_dbg(&dev->ntc.dev, "hello callback phase %d done %d\n", phase, ret);

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_HELLO + phase);

	return 0;
}

static void ntc_ntb_link_work(struct ntc_ntb_dev *dev)
{
	int link_event = ntc_ntb_link_get_event(dev);
	bool link_up = false;
	int err = 0;

	dev_dbg(&dev->ntc.dev, "link work state %d event %d\n",
		dev->link_state, link_event);

	switch (dev->link_state) {
	case NTC_NTB_LINK_QUIESCE:
		ntc_ntb_quiesce(dev);
		/* no break */
	case NTC_NTB_LINK_RESET:
		ntc_ntb_reset(dev);
		/* no break */
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

	case NTC_NTB_LINK_VER_SENT:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_START:
			goto out;
		case NTC_NTB_LINK_VER_SENT:
		case NTC_NTB_LINK_VER_CHOSEN:
			ntc_ntb_choose_version(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_VER_CHOSEN)
			goto out;

	case NTC_NTB_LINK_VER_CHOSEN:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_VER_SENT:
			goto out;
		case NTC_NTB_LINK_VER_CHOSEN:
		case NTC_NTB_LINK_DB_CONFIGURED:
			if (ntc_ntb_db_config(dev))
				ntc_ntb_error(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_DB_CONFIGURED)
			goto out;

	case NTC_NTB_LINK_DB_CONFIGURED:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_VER_CHOSEN:
			goto out;
		case NTC_NTB_LINK_DB_CONFIGURED:
		case NTC_NTB_LINK_COMMITTED:
			ntc_ntb_link_commit(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_COMMITTED)
			goto out;

	case NTC_NTB_LINK_COMMITTED:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_DB_CONFIGURED:
			goto out;
		case NTC_NTB_LINK_COMMITTED:
		case NTC_NTB_LINK_HELLO:
			err = ntc_ntb_hello(dev);
			if (err < 0) 
				ntc_ntb_error(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_HELLO)
			goto out;

	case NTC_NTB_LINK_HELLO:
	default:
		while (!ntc_ntb_done_hello(dev)) {
			WARN(dev->link_state < NTC_NTB_LINK_HELLO,
					"hello loop: state %d out of sync\n", dev->link_state);
			dev_dbg(&dev->ntc.dev, "not done hello\n");
			switch (link_event - dev->link_state) {
			default:
				dev_err(&dev->ntc.dev, "peer state is not in sync %d %d\n",
						link_event, dev->link_state);
				ntc_ntb_error(dev);
				return;
			case -1:
				dev_dbg(&dev->ntc.dev, "peer is behind hello\n");
				goto out;
			case 0:
			case 1:
				dev_dbg(&dev->ntc.dev, "can advance hello\n");
				err = ntc_ntb_hello(dev);
				if (err < 0) {
					ntc_ntb_error(dev);
					return;
				}
			}
		}

		dev_dbg(&dev->ntc.dev, "done hello, event %d state %d \n", link_event, dev->link_state);

		switch (link_event - dev->link_state) {
		default:
			ntc_ntb_error(dev);
		case -1:
			dev_dbg(&dev->ntc.dev, "peer is not done hello\n");
			goto out;
		case 0:
			dev_dbg(&dev->ntc.dev, "both peers are done hello\n");
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

	dev_dbg(&dev->ntc.dev, "link disable by upper layer (%s)", caller);

	TRACE("NTC link disable by upper layer (%s)\n", caller);

	ntc_ntb_ping_send(dev, NTC_NTB_LINK_QUIESCE);

	return ntb_link_disable(dev->ntb);
}
EXPORT_SYMBOL(_ntc_link_disable);

int ntc_link_enable(struct ntc_dev *ntc)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	int rc;

	dev_dbg(&dev->ntc.dev, "link enable\n");

	rc = ntb_link_enable(dev->ntb, NTB_SPEED_AUTO, NTB_WIDTH_AUTO);

	if (!rc) {
		ntc_ntb_ping_send(dev, dev->link_state);
		ntb_link_event(dev->ntb);
	}

	return rc;
}
EXPORT_SYMBOL(ntc_link_enable);

#define RESET_TIMEOUT (1000) /*1 sec*/
int _ntc_link_reset(struct ntc_dev *ntc, bool wait, const char *caller)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	uint tmp_reset_cnt;
	int ret;

	dev_dbg(&dev->ntc.dev, "link reset requested by upper layer\n");
	mutex_lock(&dev->link_lock);

	if (dev->link_state > NTC_NTB_LINK_START)
		ntc_ntb_error(dev);
	if (wait) {
		if (dev->link_state == NTC_NTB_LINK_START) {
			mutex_unlock(&dev->link_lock);
			dev_dbg(&dev->ntc.dev,
				"link reset requested by upper layer but already reseted\n");
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
			dev_err(&dev->ntc.dev,
					"link reset timeout after %d current state %d",
					RESET_TIMEOUT, dev->link_state);
			return -ETIME;
		}

		dev_dbg(&dev->ntc.dev,
				"link reset done, state %d\n",
				dev->link_state);
	}
	return 0;
}
EXPORT_SYMBOL(_ntc_link_reset);

int ntc_req_submit(struct ntc_dev *ntc, struct dma_chan *chan)
{
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	struct dma_tx_state txstate;
	static int i;

	dev_vdbg(&ntc->dev, "submit request\n");

	tx = chan->device->device_prep_dma_interrupt(chan, 0);
	if (!tx)
		return -ENOMEM;

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie))
		return -EIO;

	/* locked area per DMA engine*/
	i++;
	dma_async_issue_pending(chan);

	/*
	 *  We do not really care about the status but still calling
	 * tx status func to clear the ioat ring.
	 */

	if (!(i & 0xff))
		chan->device->device_tx_status(chan, cookie, &txstate);

	return 0;
}
EXPORT_SYMBOL(ntc_req_submit);

static int ntc_ntb_req_prep_flags(bool fence)
{
	int flags = NTC_NTB_DMA_PREP_FLAGS;

	if (fence)
		flags |= DMA_PREP_FENCE;

	return flags;
}

int ntc_req_memcpy(struct dma_chan *chan,
		dma_addr_t dst, dma_addr_t src, u64 len,
		bool fence, void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct dma_async_tx_descriptor *tx;
	static dma_cookie_t last_cookie;
	dma_cookie_t my_cookie;
	int flags;

	dev_vdbg(chan->device->dev,
		"request memcpy dst %llx src %llx len %llx\n",
		 dst, src, len);

	if (!len)
		return -EINVAL;

	flags = ntc_ntb_req_prep_flags(fence);

	tx = chan->device->device_prep_dma_memcpy(chan, dst, src,
			len, flags);  /*spin_lock_bh per chan*/
	if (!tx) {

		pr_warn("DMA ring is full for len %llu waiting ...\n", len);
		dma_sync_wait(chan, last_cookie); /* Busy waiting */
		pr_warn("DMA ring full for len %llu retring...\n", len);

		tx = chan->device->device_prep_dma_memcpy(chan, dst, src,
				len, flags);/*spin_lock_bh per chan*/
		if (!tx) {
			pr_err("DMA ring still full for len %llu after retring\n",
					len);
			return -ENOMEM;
		}
	}

	tx->callback = cb;
	tx->callback_param = cb_ctx;

	my_cookie = dmaengine_submit(tx); /*spin_unlock_bh per chan*/
	if (dma_submit_error(my_cookie))
		return -EIO;

	last_cookie = my_cookie;

	return 0;
}
EXPORT_SYMBOL(ntc_req_memcpy);

static void ntc_req_imm_cb(void *ctx)
{
	struct ntc_ntb_imm *imm = ctx;

	dev_vdbg(imm->dma_dev,
			"imm unmap phys %llx  len %zu\n",
			(u64)imm->dma_addr,  imm->data_len);

	dma_unmap_single(imm->dma_dev,
			 imm->dma_addr,
			 imm->data_len,
			 DMA_TO_DEVICE);

	if (imm->cb)
		imm->cb(imm->cb_ctx);

	kfree(imm);
}

int ntc_req_imm(struct dma_chan *chan,
		u64 dst, void *ptr, size_t len, bool fence,
		void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct ntc_ntb_imm *imm;
	int rc;

	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	int flags;

	if (chan->device->device_prep_dma_imm_data && len == 8) {
		flags = ntc_ntb_req_prep_flags(fence);

		tx = chan->device->device_prep_dma_imm_data(chan, dst,
							    *(u64 *)ptr,
							    flags);
		if (!tx)
			return -ENOMEM;

		tx->callback = cb;
		tx->callback_param = cb_ctx;

		cookie = dmaengine_submit(tx);
		if (dma_submit_error(cookie))
			return -EIO;

		return 0;
	}

	imm = kmalloc_node(sizeof(*imm), GFP_ATOMIC,
			dev_to_node(chan->device->dev));
	if (!imm) {
		rc = -ENOMEM;
		goto err_imm;
	}

	memcpy(imm->data_buf, ptr, len);
	imm->data_len = len;

	imm->dma_dev = chan->device->dev;
	imm->dma_addr = dma_map_single(imm->dma_dev,
				       imm->data_buf,
				       imm->data_len,
				       DMA_TO_DEVICE);

	if (dma_mapping_error(imm->dma_dev, imm->dma_addr)) {
		rc = -EIO;
		goto err_dma;
	}

	dev_vdbg(imm->dma_dev,
			"imm  map phys %llx virt %p len %zu\n",
			(u64)imm->dma_addr, imm->data_buf,
			imm->data_len);

	imm->cb = cb;
	imm->cb_ctx = cb_ctx;

	rc = ntc_req_memcpy(chan, dst, imm->dma_addr, imm->data_len,
			fence, ntc_req_imm_cb, imm);
	if (rc)
		goto err_memcpy;

	return 0;

err_memcpy:
	dma_unmap_single(imm->dma_dev,
			 imm->dma_addr,
			 imm->data_len,
			 DMA_TO_DEVICE);
err_dma:
	kfree(imm);
err_imm:
	return rc;
}
EXPORT_SYMBOL(ntc_req_imm);

int ntc_req_signal(struct ntc_dev *ntc, struct dma_chan *chan,
		void (*cb)(void *cb_ctx), void *cb_ctx, int vec)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	dev_vdbg(&ntc->dev, "request signal to peer\n");

	if (WARN_ON(vec > ARRAY_SIZE(dev->peer_irq_shift)))
		return -EFAULT;

	if (WARN_ON(ntc->peer_irq_num < vec))
		return -EFAULT;

	if (dev->use_peer_irq_base)
		return ntc_request_imm32(chan, &dev->peer_irq_base,
					dev->peer_irq_shift[vec],
					dev->peer_irq_data[vec],
					false, cb, cb_ctx);
	else
		return ntc_req_imm32(chan,
				ntc->peer_dram_mw.base +
				dev->peer_irq_shift[vec],
				dev->peer_irq_data[vec], false, cb, cb_ctx);
}
EXPORT_SYMBOL(ntc_req_signal);

int ntc_clear_signal(struct ntc_dev *ntc, int vec)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	int db_bit;

	if (use_msi)
		return 0;

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

void *ntc_peer_hello_buf(struct ntc_dev *ntc, int *size)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	*size = NTC_CTX_BUF_SIZE;
	return dev->info_self_on_peer->ctx_buf;
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

static int ntc_ntb_init_mw0_coherent(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;
	resource_size_t alignment = ntc->own_info_mw.size;
	resource_size_t size = ntc->own_info_mw.size;
	resource_size_t alloc_size = size * 2;

	if (!alloc_size)
		return -EINVAL;

	if (alloc_size > KMALLOC_MAX_SIZE)
		return -EINVAL;

	dev->info_mem_buffer.size = alloc_size;
	dev->info_mem_buffer.ptr = dma_alloc_coherent(ntc_ntb_dma_dev(dev),
						alloc_size,
						&dev->info_mem_buffer.dma_addr,
						GFP_KERNEL);
	if (!dev->info_mem_buffer.ptr) {
		pr_info("OWN INFO MW: cannot alloc. Actual size %#llx.",
			dev->info_mem_buffer.size);
		return -ENOMEM;
	}

	ntc->own_info_mw.base = ALIGN(dev->info_mem_buffer.dma_addr, alignment);
	ntc->own_info_mw.base_ptr = dev->info_mem_buffer.ptr +
		(ntc->own_info_mw.base - dev->info_mem_buffer.dma_addr);
	pr_info("OWN INFO MW: base %#llx size %llx ptr %p",
		ntc->own_info_mw.base, size, ntc->own_info_mw.base_ptr);
	pr_info("OWN INFO MW: Actual DMA %llx ptr %p",
		dev->info_mem_buffer.dma_addr, dev->info_mem_buffer.ptr);

	dev->info_mem_reserved = false;
	return 0;
}

static void ntc_ntb_deinit_mw0_coherent(struct ntc_ntb_dev *dev)
{
	dma_free_coherent(ntc_ntb_dma_dev(dev),
			dev->info_mem_buffer.size,
			dev->info_mem_buffer.ptr,
			dev->info_mem_buffer.dma_addr);
}

static int ntc_ntb_init_mw0_reserved(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;
	resource_size_t alignment = ntc->own_info_mw.size;
	resource_size_t size = ntc->own_info_mw.size;
	unsigned long start = mw0_base_addr;
	unsigned long end = start + mw0_len;
	unsigned long aligned_start;

	if (!mw0_reserved)
		return -EINVAL;

	if (!alignment || (alignment & (alignment - 1)))
		return -EINVAL;

	aligned_start = ALIGN(start, alignment);
	if (aligned_start >= end)
		return -EINVAL;

	if (end - aligned_start < size)
		return -EINVAL;

	ntc->own_info_mw.base_ptr = memremap(aligned_start, size, MEMREMAP_WB);
	if (!ntc->own_info_mw.base_ptr) {
		pr_info("OWN INFO MW: cannot ioremap. Start %#lx. Size %#llx.",
			aligned_start, size);
		return -EIO;
	}
	ntc->own_info_mw.base = aligned_start;

	pr_info("OWN INFO MW: base %#llx size %llx ptr %p IN RESERVED MW",
		ntc->own_info_mw.base, size, ntc->own_info_mw.base_ptr);

	dev->info_mem_reserved = true;

	return 0;
}

static void ntc_ntb_deinit_mw0_reserved(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;

	memunmap(ntc->own_info_mw.base_ptr);
}

static int ntc_ntb_init_mw0(struct ntc_ntb_dev *dev)
{
	int rc_reserved;
	int rc_coherent;

	rc_reserved = ntc_ntb_init_mw0_reserved(dev);
	if (rc_reserved >= 0)
		return rc_reserved;

	rc_coherent = ntc_ntb_init_mw0_coherent(dev);
	if (rc_coherent >= 0)
		return rc_coherent;

	if (rc_reserved != -EINVAL)
		return rc_reserved;

	return rc_coherent;
}

static void ntc_ntb_deinit_mw0(struct ntc_ntb_dev *dev)
{
	if (dev->info_mem_reserved)
		ntc_ntb_deinit_mw0_reserved(dev);
	else
		ntc_ntb_deinit_mw0_coherent(dev);
}

static int ntc_ntb_dev_init(struct ntc_ntb_dev *dev)
{
	int rc, i, mw_idx, mw_count;
	resource_size_t own_dram_addr_align, own_dram_size_align,
		own_dram_size_max;
	resource_size_t own_info_addr_align, own_info_size_align,
		own_info_size_max;
	struct ntc_dev *ntc = &dev->ntc;

	/* inherit ntb device name and configuration */
	dev_set_name(&ntc->dev, "%s", dev_name(&dev->ntb->dev));
	ntc->dev.parent = &dev->ntb->dev;

	ntc->ntb_dev = ntc_ntb_dma_dev(dev);
	ntc->dma_engine_dev = ntc->dma_chan->device->dev;

	/* make sure link is disabled and warnings are cleared */
	ntb_link_disable(dev->ntb);
	ntb_db_is_unsafe(dev->ntb);
	ntb_spad_is_unsafe(dev->ntb);

	/* we'll be using the last memory window if it exists */
	mw_count = ntb_mw_count(dev->ntb, NTB_DEF_PEER_IDX);
	if (mw_count <= 0) {
		pr_err("no mw for new device %s\n", dev_name(&dev->ntb->dev));
		rc = -EINVAL;
		goto err_mw;
	}
	if (mw_count < 2) {
		pr_err("not enough memory windows for new device %s\n",
			dev_name(&dev->ntb->dev));
		rc = -EINVAL;
		goto err_mw;
	}
	mw_idx = mw_count - 1;

	/* clear any garbage translations */
	for (i = 0; i < mw_count; ++i)
		ntb_mw_clear_trans(dev->ntb, NTB_DEF_PEER_IDX, i);

	/* this is the window we'll translate to local dram */
	ntb_peer_mw_get_addr(dev->ntb, mw_idx,
			&ntc->peer_dram_mw.base, &ntc->peer_dram_mw.size);
	pr_info("PEER DRAM MW: idx %d base %#llx size %#llx",
		mw_idx, ntc->peer_dram_mw.base,
		ntc->peer_dram_mw.size);
	ntc->peer_dram_mw.desc = NTC_CHAN_MW1;
	ntc->peer_dram_mw.ntc = ntc;

	/*
	 * FIXME: ensure window is large enough.
	 * Fail under 8G here, but mw should be >= dram.
	 */
	if (ntc->peer_dram_mw.size < 0x200000000ul) {
		pr_debug("invalid mw size for new device %s\n",
			 dev_name(&dev->ntb->dev));
		rc = -EINVAL;
		goto err_mw_size;
	}

	ntb_mw_get_align(dev->ntb, NTB_DEF_PEER_IDX, mw_idx,
			&own_dram_addr_align, &own_dram_size_align,
			&own_dram_size_max);

	/* FIXME: zero is not a portable address for local dram */
	ntc->own_dram_mw.base = 0;
	ntc->own_dram_mw.base_ptr = NULL;
	ntc->own_dram_mw.size = own_dram_size_max;
	ntc->own_dram_mw.desc = NTC_CHAN_MW1;
	ntc->own_dram_mw.ntc = ntc;
	ntc->own_dram_mw.ntb_dev = ntc->ntb_dev;
	ntc_mm_init(&ntc->own_dram_mw.mm, NULL, 0);

	rc = ntb_mw_set_trans(dev->ntb, NTB_DEF_PEER_IDX, mw_idx,
			ntc->own_dram_mw.base, ntc->own_dram_mw.size);
	if (rc) {
		pr_debug("failed to translate mw for new device %s\n",
			 dev_name(&dev->ntb->dev));
		goto err_mw;
	}

	/* a local buffer for peer driver to write */
	ntb_peer_mw_get_addr(dev->ntb, mw_idx - 1,
			&ntc->peer_info_mw.base, &ntc->peer_info_mw.size);
	pr_info("PEER INFO MW: idx %d base %#llx size %#llx",
		mw_idx - 1, ntc->peer_info_mw.base,
		ntc->peer_info_mw.size);

	if (ntc->peer_info_mw.size < ntc_ntb_info_size) {
		pr_debug("invalid alignement of peer info for new device %s\n",
			 dev_name(&dev->ntb->dev));
		rc = -ENOMEM;
		goto err_info;
	}

	ntc->peer_info_mw.desc = NTC_CHAN_MW0;
	ntc->peer_info_mw.ntc = ntc;

	ntb_mw_get_align(dev->ntb, NTB_DEF_PEER_IDX, mw_idx - 1,
			&own_info_addr_align, &own_info_size_align,
			&own_info_size_max);

	ntc->own_info_mw.size = own_info_size_max;
	ntc->own_info_mw.desc = NTC_CHAN_MW0;
	ntc->own_info_mw.ntc = ntc;
	ntc->own_info_mw.ntb_dev = ntc->ntb_dev;

	rc = ntc_ntb_init_mw0(dev);
	if (rc < 0)
		goto err_info;

	dev->info_self_on_peer = ioremap(ntc->peer_info_mw.base,
					sizeof(*dev->info_self_on_peer));
	if (!dev->info_self_on_peer) {
		pr_info("failed to remap info on peer for new device %s\n",
			dev_name(&dev->ntb->dev));
		rc = -EIO;
		goto err_map;
	}

	/* set the ntb translation to the aligned dma memory */
	rc = ntb_mw_set_trans(dev->ntb, NTB_DEF_PEER_IDX, mw_idx - 1,
			ntc->own_info_mw.base, ntc->own_info_mw.size);
	if (rc) {
		pr_debug("failed to translate info mw for new device %s rc %d\n",
			 dev_name(&dev->ntb->dev), rc);
		goto err_trans;
	}

	ntc_mm_init(&ntc->own_info_mw.mm,
		ntc->own_info_mw.base_ptr + sizeof(struct ntc_ntb_info),
		ntc->own_info_mw.size - sizeof(struct ntc_ntb_info));

	/* haven't negotiated the version */
	ntc->version = 0;
	ntc->latest_version = 0;

	/* haven't negotiated the msi pair */
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

	setup_timer(&dev->ping_pong,
		    ntc_ntb_ping_pong_cb,
		    ntc_ntb_to_ptrhld(dev));

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
	ntc_mm_deinit(&ntc->own_info_mw.mm);
 err_trans:
	iounmap(dev->info_self_on_peer);
 err_map:
	ntc_ntb_deinit_mw0(dev);
 err_info:
	for (i = 0; i < mw_count; ++i)
		ntb_mw_clear_trans(dev->ntb, NTB_DEF_PEER_IDX, i);
 err_mw:
	ntc_mm_deinit(&ntc->own_dram_mw.mm);
 err_mw_size:
	return rc;
}

static void ntc_ntb_dev_deinit(struct ntc_ntb_dev *dev)
{
	struct ntc_dev *ntc = &dev->ntc;
	int i, mw_idx;

	ntb_clear_ctx(dev->ntb);

	ntb_link_disable(dev->ntb);

	ntc_mm_deinit(&ntc->own_info_mw.mm);
	ntc_mm_deinit(&ntc->own_dram_mw.mm);

	mw_idx = ntb_mw_count(dev->ntb, NTB_DEF_PEER_IDX);
	for (i = 0; i < mw_idx; ++i)
		ntb_mw_clear_trans(dev->ntb, NTB_DEF_PEER_IDX, i);

	ntc_ntb_ping_stop(dev);

	cancel_work_sync(&dev->link_work);

	iounmap(dev->info_self_on_peer);

	ntc_ntb_deinit_mw0(dev);
}

static bool ntc_ntb_filter_bus(struct dma_chan *chan,
			       void *filter_param)
{
	int node = *(int *)filter_param;

	return node == dev_to_node(&chan->dev->device);
}

static void ntc_ntb_release(struct device *device)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_dev(device);

	pr_debug("release %s\n", dev_name(&dev->ntc.dev));

	ntc_ntb_dev_deinit(dev);
	put_device(&dev->ntb->dev);
	dma_release_channel(dev->ntc.dma_chan);
	kfree(dev);
}

static int ntc_debugfs_read(struct seq_file *s, void *v)
{
	struct ntc_ntb_dev *dev = s->private;
	struct ntc_dev *ntc = &dev->ntc;
	const struct ntc_ntb_info *peer_info;
	int i;

	peer_info = ntc_ntb_peer_info(dev);

	seq_printf(s, "peer_dram_mw.base %#llx\n",
		ntc->peer_dram_mw.base);
	seq_printf(s, "ntc->own_info_mw.size %#llx\n",
		   ntc->own_info_mw.size);
	seq_printf(s, "ntc->own_info_mw.base %#llx\n",
		   ntc->own_info_mw.base);
	seq_puts(s, "info_peer_on_self:\n");
	seq_printf(s, "  magic %#x\n", peer_info->magic);
	seq_printf(s, "  ping %#x\n", peer_info->ping);
	seq_printf(s, "  msi_irqs_num %d\n", peer_info->msi_irqs_num);
	for (i = 0; i < peer_info->msi_irqs_num; i++) {
		seq_printf(s, "  msi_data %#x\n", peer_info->msi_data[i]);
		seq_printf(s, "  msi_addr_lower %#x\n",
			peer_info->msi_addr_lower[i]);
		seq_printf(s, "  msi_addr_upper %#x\n",
			peer_info->msi_addr_upper[i]);
	}
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

static int ntc_ntb_probe(struct ntb_client *self,
			 struct ntb_dev *ntb)
{
	struct ntc_ntb_dev *dev;
	struct dma_chan *dma;
	dma_cap_mask_t mask;
	int node, rc;

	pr_debug("probe ntb %s\n", dev_name(&ntb->dev));

	dev = kzalloc_node(sizeof(*dev), GFP_KERNEL,
			   dev_to_node(&ntb->dev));
	if (!dev) {
		rc = -ENOMEM;
		goto err_dev;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	node = dev_to_node(&ntb->dev);
	dma = dma_request_channel(mask, ntc_ntb_filter_bus, &node);
	if (!dma) {
		pr_debug("no dma for new device %s\n",
			 dev_name(&ntb->dev));
		rc = -ENODEV;
		goto err_dma;
	}

	dev->ntc.dma_chan = dma;

	get_device(&ntb->dev);
	dev->ntb = ntb;

	rc = ntc_ntb_dev_init(dev);
	if (rc)
		goto err_init;

	dev->ntc.dev.release = ntc_ntb_release;

	ntc_setup_debugfs(dev);

	return ntc_register_device(&dev->ntc);

err_init:
	put_device(&ntb->dev);
	dma_release_channel(dma);
err_dma:
	kfree(dev);
err_dev:
	return rc;
}

static void ntc_ntb_remove(struct ntb_client *self, struct ntb_dev *ntb)
{
	struct ntc_ntb_dev *dev = ntb->ctx;

	dev_vdbg(&dev->ntc.dev, "remove\n");

	debugfs_remove_recursive(dev->dbgfs);

	ntc_unregister_device(&dev->ntc);
}

struct ntb_client ntc_ntb_client = {
	.ops = {
		.probe			= ntc_ntb_probe,
		.remove			= ntc_ntb_remove,
	},
};

int __init ntc_init(void)
{
	info("%s %s init", DRIVER_DESCRIPTION, DRIVER_VERSION);

	mw0_reserved =
		ntc_check_reserved(mw0_base_addr, mw0_base_addr + mw0_len);
	mw1_reserved =
		ntc_check_reserved(mw1_base_addr, mw1_base_addr + mw1_len);

	if (debugfs_initialized())
		ntc_dbgfs = debugfs_create_dir(KBUILD_MODNAME, NULL);
	ntb_register_client(&ntc_ntb_client);
	return 0;
}

void __exit ntc_exit(void)
{
	ntb_unregister_client(&ntc_ntb_client);
	debugfs_remove_recursive(ntc_dbgfs);

	info("%s %s exit", DRIVER_DESCRIPTION, DRIVER_VERSION);
}
