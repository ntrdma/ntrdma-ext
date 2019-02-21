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

#define DRIVER_NAME			"ntc_ntb"
#define DRIVER_DESCRIPTION		"NTC Non Transparent Bridge"

#define DRIVER_LICENSE			"Dual BSD/GPL"
#define DRIVER_VERSION			"0.3"
#define DRIVER_RELDATE			"5 October 2015"
#define DRIVER_AUTHOR			"Allen Hubbe <Allen.Hubbe@emc.com>"

MODULE_LICENSE(DRIVER_LICENSE);
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);

static struct dentry *ntc_dbgfs;

static int ntc_ntb_info_size = 0x1000;
/* TODO: module param named info_size */

static bool use_msi;
module_param(use_msi, bool, 0444);
MODULE_PARM_DESC(use_msi, "Use MSI(X) as interrupts");

/* Protocol version for backwards compatibility */
#define NTC_NTB_VERSION_NONE		0
#define NTC_NTB_VERSION_MAX		2
#define NTC_NTB_VERSION_MIN		2

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

#define INTEL_ALIGN 16

#ifndef iowrite64
#ifdef writeq
#define iowrite64 writeq
#else
#define iowrite64 __lame_iowrite64
static inline void __lame_iowrite64(u64 val, void __iomem *ptr)
{
	iowrite32(val, ptr);
	iowrite32(val >> 32, ptr + sizeof(u32));
}
#endif
#endif

struct ntc_ntb_imm {
	char				data_buf[sizeof(u64)];
	size_t				data_len;
	struct device			*dma_dev;
	dma_addr_t			dma_addr;
	void				(*cb)(void *cb_ctx);
	void				*cb_ctx;
};

struct ntc_ntb_info {
	u32				version;
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
	struct dma_chan			*dma;

	/* local ntb window offset to peer dram */
	u64				peer_dram_base;
	size_t				peer_dram_size;

	/*Peer mw base for initialization*/
	u64                             peer_info_base;

	/* local buffer for remote driver to write info */
	void                            *info_peer_on_self_unaligned;
	struct ntc_ntb_info		*info_peer_on_self;
	size_t				info_peer_on_self_size;
	dma_addr_t			info_peer_on_self_dma;
	size_t                          info_peer_on_self_off;

	/* remote buffer for local driver to write info */
	struct ntc_ntb_info __iomem	*info_self_on_peer;

	/* negotiated protocol version for ntc */
	int				version;

	/* direct interrupt message */
	u64				peer_irq_addr[NTB_MAX_IRQS];
	u32				peer_irq_data[NTB_MAX_IRQS];
	int				peer_irq_num;

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
	bool				link_is_up;
	int				link_state;
	struct work_struct		link_work;
	struct mutex			link_lock;

	struct dentry *dbgfs;
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

static inline u32 ntc_ntb_version(int version, int version_min)
{
	BUILD_BUG_ON(NTC_NTB_VERSION_MAX & ((~0) << 16));
	BUILD_BUG_ON(NTC_NTB_VERSION_MIN & ((~0) << 16));

	return (version << 16) | version_min;
}

static inline int ntc_ntb_version_choose(u32 v1, u32 v2)
{
	int max1, min1, max2, min2, mask;

	mask = BIT(16) - 1;

	max1 = mask & (v1 >> 16);
	min1 = mask & v1;
	max2 = mask & (v2 >> 16);
	min2 = mask & v2;

	if (min1 <= max2 && max1 >= min2)
		return min(max1, max2);

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

static void ntc_ntb_ping_pong_fn(struct timer_list *t)
{
	struct ntc_ntb_dev *dev = from_timer(dev, t, ping_pong);
	unsigned long irqflags;

	spin_lock_irqsave(&dev->ping_lock, irqflags);
	ntc_ntb_ping_pong(dev);
	spin_unlock_irqrestore(&dev->ping_lock, irqflags);
}

static bool ntc_ntb_ping_poll(struct ntc_ntb_dev *dev)
{
	int ping_flags;
	u32 poll_val;

	if (!dev->ping_run)
		return false;
	mod_timer(&dev->ping_poll, jiffies + NTC_NTB_PING_POLL_PERIOD);

	ping_flags = dev->ping_flags;
	(void)ping_flags; /* TODO: cleanup unused */

	poll_val = dev->info_peer_on_self->ping;

	dev_vdbg(&dev->ntc.dev, "poll val %x\n", poll_val);

	if (dev->poll_val != poll_val) {
		dev->poll_val = poll_val;
		return true;
	}
	return false;
}

static void ntc_ntb_ping_poll_fn(struct timer_list *t)
{
	struct ntc_ntb_dev *dev = from_timer(dev, t, ping_poll);
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

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_QUIESCE);
	schedule_work(&dev->link_work);
}

static inline void ntc_ntb_quiesce(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link quiesce\n");

	ntc_ctx_quiesce(&dev->ntc);

	/* TODO: cancel and wait for any outstanding dma requests */

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_RESET);
}

static inline void ntc_ntb_reset(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link reset\n");

	ntc_ctx_reset(&dev->ntc);

	memset(dev->info_peer_on_self, 0, sizeof(*dev->info_peer_on_self));

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_START);
}

static inline void ntc_ntb_send_version(struct ntc_ntb_dev *dev)
{
	u32 version;

	dev_dbg(&dev->ntc.dev, "link send version\n");

	version = ntc_ntb_version(NTC_NTB_VERSION_MAX, NTC_NTB_VERSION_MIN);

	iowrite32(version, &dev->info_self_on_peer->version);

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_VER_SENT);
}

static inline void ntc_ntb_choose_version(struct ntc_ntb_dev *dev)
{
	u32 version, peer_version;
	u64 msi_addr[NTB_MAX_IRQS];
	u32 msi_data[NTB_MAX_IRQS];
	int msi_idx;
	u32 msi_irqs_num;

	dev_dbg(&dev->ntc.dev, "link choose version and send msi data\n");

	version = ntc_ntb_version(NTC_NTB_VERSION_MAX, NTC_NTB_VERSION_MIN);

	peer_version = dev->info_peer_on_self->version;

	dev_dbg(&dev->ntc.dev, "version %x peer_version %x\n",
		version, peer_version);

	dev->version = ntc_ntb_version_choose(version, peer_version);
	if (dev->version == NTC_NTB_VERSION_NONE)
		goto err;

	dev_dbg(&dev->ntc.dev, "ok version\n");

	iowrite32(ntc_ntb_version_magic(dev->version),
		  &dev->info_self_on_peer->magic);

	iowrite32(0, &dev->info_self_on_peer->done);

	msi_irqs_num = ntc_ntb_read_msi_config(dev, msi_addr, msi_data);
	if (msi_irqs_num > NTB_MAX_IRQS) {
		dev_err(&dev->ntc.dev, "msi_irqs_num %u is above max\n",
				msi_irqs_num);
		goto err;
	}

	dev->info_self_on_peer->msi_irqs_num = msi_irqs_num;

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

static inline void ntc_ntb_db_config(struct ntc_ntb_dev *dev)
{
	int rc;
	int msi_idx;
	resource_size_t size;

	u32 msi_irqs_num = dev->info_peer_on_self->msi_irqs_num;

	if (use_msi && msi_irqs_num > 0 && msi_irqs_num < NTB_MAX_IRQS) {
		for (msi_idx = 0; msi_idx < msi_irqs_num; msi_idx++) {
			dev->peer_irq_data[msi_idx] = dev->info_peer_on_self->msi_data[msi_idx];
			dev->peer_irq_addr[msi_idx] = dev->peer_dram_base +
				(((u64)dev->info_peer_on_self->msi_addr_lower[msi_idx]) |
				 (((u64)dev->info_peer_on_self->msi_addr_upper[msi_idx]) << 32));
		}
	} else {
		u64 peer_irq_addr_base;
		u64 peer_db_mask;
		int db_idx;
		int max_irqs;
		u64 db_bits;

		dev->peer_irq_num = 0;

		max_irqs = ntb_db_vector_count(dev->ntb);

		if (max_irqs <= 0 || max_irqs> NTB_MAX_IRQS) {
			dev_err(&dev->ntc.dev, "max_irqs %d - not supported\n",
					dev->info_peer_on_self->msi_irqs_num);
			return;
		}

		rc = ntb_peer_db_addr(dev->ntb,
				(phys_addr_t *)&peer_irq_addr_base,
				&size);
		if ((rc < 0) || (size != sizeof(u32)) ||
				!IS_ALIGNED(peer_irq_addr_base, INTEL_ALIGN)) {
			dev_err(&dev->ntc.dev, "Peer DB addr invalid\n");
			return;
		}

		db_bits = ntb_db_valid_mask(dev->ntb);
		for (db_idx = 0; db_idx < max_irqs && db_bits; db_idx++) {
			/*FIXME This is not generic implementation,
			 * Sky-lake implementation, see intel_ntb3_peer_db_set() */
			int bit = __ffs(db_bits);

			dev->peer_irq_addr[db_idx] = peer_irq_addr_base + (bit * 4);
			db_bits &= db_bits - 1;
			dev->peer_irq_num++;
			dev->peer_irq_data[db_idx] = 1;
		}

		peer_db_mask = ntb_db_valid_mask(dev->ntb);

		dev_dbg(&dev->ntc.dev, "Peer DB addr: %#llx count %d mask %#llx\n",
				peer_irq_addr_base, dev->peer_irq_num, peer_db_mask);

		ntb_db_clear(dev->ntb, peer_db_mask);
		ntb_db_clear_mask(dev->ntb, peer_db_mask);
	}

	dev_dbg(&dev->ntc.dev, "link signaling method configured\n");
	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_DB_CONFIGURED);
}

static inline void ntc_ntb_link_commit(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link commit - verifying both sides sync\n");

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_COMMITTED);
}

static inline bool ntc_ntb_done_hello(struct ntc_ntb_dev *dev)
{
	/* no outstanding input or output buffers */
	return (dev->info_peer_on_self->done == 1) && (dev->info_self_on_peer->done == 1);
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

		if (dev->link_state != NTC_NTB_LINK_RESET)
			goto out;

	case NTC_NTB_LINK_RESET:
		switch (link_event) {
		default:
			goto out;
		case NTC_NTB_LINK_RESET:
		case NTC_NTB_LINK_START:
			ntc_ntb_reset(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_START)
			goto out;

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
			ntc_ntb_db_config(dev);
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
			dev_dbg(&dev->ntc.dev, "not done hello\n");
			switch (link_event - dev->link_state) {
			default:
				dev_err(&dev->ntc.dev, "peer state is not in sync\n");
				ntc_ntb_error(dev);
			case -1:
				dev_dbg(&dev->ntc.dev, "peer is behind hello\n");
				goto out;
			case 0:
			case 1:
				dev_dbg(&dev->ntc.dev, "can advance hello\n");
				err = ntc_ntb_hello(dev);
				if (err < 0) {
					ntc_ntb_error(dev);
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
	if (dev->link_is_up != link_up) {
		dev->link_is_up = link_up;
		if (link_up)
			ntc_ctx_enable(&dev->ntc);
		else
			ntc_ctx_disable(&dev->ntc);
	}
}

static void ntc_ntb_link_work_cb(struct work_struct *ws)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_link_work(ws);

	mutex_lock(&dev->link_lock);
	ntc_ntb_link_work(dev);
	mutex_unlock(&dev->link_lock);
}

static struct device *ntc_ntb_map_dev(struct ntc_dev *ntc)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	return ntc_ntb_dma_dev(dev);
}

static int ntc_ntb_link_disable(struct ntc_dev *ntc)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	dev_dbg(&dev->ntc.dev, "link disable\n");

	ntc_ntb_ping_send(dev, NTC_NTB_LINK_QUIESCE);

	return ntb_link_disable(dev->ntb);
}

static int ntc_ntb_link_enable(struct ntc_dev *ntc)
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

static int ntc_ntb_link_reset(struct ntc_dev *ntc)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	dev_dbg(&dev->ntc.dev, "link reset requested by upper layer\n");

	mutex_lock(&dev->link_lock);
	ntc_ntb_error(dev);
	mutex_unlock(&dev->link_lock);

	return 0;
}

static u64 ntc_ntb_peer_addr(struct ntc_dev *ntc, u64 addr)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	if (addr > dev->peer_dram_size || !IS_ALIGNED(addr, INTEL_ALIGN)) {
		dev_err(&dev->ntc.dev, "dram_base 0x%llx + off 0x%llx\n",
				dev->peer_dram_base, addr);
		return 0;
	}

	return dev->peer_dram_base + addr;
}

static void *ntc_ntb_req_create(struct ntc_dev *ntc)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	dev_vdbg(&ntc->dev, "create request\n");

	return dev->dma;
}

static void ntc_ntb_req_cancel(struct ntc_dev *ntc, void *req)
{
	dev_vdbg(&ntc->dev, "cancel request\n");

	/* nothing to do */
}

static int ntc_ntb_req_submit(struct ntc_dev *ntc, void *req)
{
	struct dma_chan *chan = req;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;

	dev_vdbg(&ntc->dev, "submit request\n");

	tx = chan->device->device_prep_dma_interrupt(chan, 0);
	if (!tx)
		return -ENOMEM;

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie))
		return -EIO;

	dma_async_issue_pending(chan);

	return 0;
}

static int ntc_ntb_req_prep_flags(struct ntc_dev *ntc, bool fence)
{
	int flags = NTC_NTB_DMA_PREP_FLAGS;

	if (fence)
		flags |= DMA_PREP_FENCE;

	return flags;
}

static int ntc_ntb_req_memcpy(struct ntc_dev *ntc, void *req,
			      u64 dst, u64 src, u64 len, bool fence,
			      void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct dma_chan *chan = req;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	int flags;

	dev_vdbg(&ntc->dev, "request memcpy dst %llx src %llx len %llx\n",
		 dst, src, len);

	if (!len)
		return -EINVAL;

	flags = ntc_ntb_req_prep_flags(ntc, fence);

	tx = chan->device->device_prep_dma_memcpy(chan, dst, src,
						  len, flags);
	if (!tx)
		return -ENOMEM;

	tx->callback = cb;
	tx->callback_param = cb_ctx;

	cookie = dmaengine_submit(tx);
	if (dma_submit_error(cookie))
		return -EIO;

	return 0;
}

static void ntc_ntb_req_imm_cb(void *ctx)
{
	struct ntc_ntb_imm *imm = ctx;

	dma_unmap_single(imm->dma_dev,
			 imm->dma_addr,
			 imm->data_len,
			 DMA_TO_DEVICE);

	if (imm->cb)
		imm->cb(imm->cb_ctx);

	kfree(imm);
}

static int ntc_ntb_req_imm(struct ntc_dev *ntc, void *req,
			   u64 dst, void *ptr, size_t len, bool fence,
			   void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct ntc_ntb_imm *imm;
	int rc;

	struct dma_chan *chan = req;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	int flags;

	if (chan->device->device_prep_dma_imm_data && len == 8) {
		flags = ntc_ntb_req_prep_flags(ntc, fence);

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
			   dev_to_node(&ntc->dev));
	if (!imm) {
		rc = -ENOMEM;
		goto err_imm;
	}

	memcpy(imm->data_buf, ptr, len);
	imm->data_len = len;

	imm->dma_dev = ntc_ntb_dma_dev(ntc_ntb_down_cast(ntc));
	imm->dma_addr = dma_map_single(imm->dma_dev,
				       imm->data_buf,
				       imm->data_len,
				       DMA_TO_DEVICE);

	if (!imm->dma_addr) {
		rc = -EIO;
		goto err_dma;
	}

	imm->cb = cb;
	imm->cb_ctx = cb_ctx;

	rc = ntc_ntb_req_memcpy(ntc, req,
				dst, imm->dma_addr,
				imm->data_len, fence,
				ntc_ntb_req_imm_cb, imm);
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

static int ntc_ntb_req_imm32(struct ntc_dev *ntc, void *req,
			     u64 dst, u32 val, bool fence,
			     void (*cb)(void *cb_ctx), void *cb_ctx)
{
	dev_vdbg(&ntc->dev, "request imm32 dst %llx val %x\n",
		 dst, val);

	return ntc_ntb_req_imm(ntc, req, dst,
			       &val, sizeof(val),
			       fence, cb, cb_ctx);
}

static int ntc_ntb_req_imm64(struct ntc_dev *ntc, void *req,
			     u64 dst, u64 val, bool fence,
			     void (*cb)(void *cb_ctx), void *cb_ctx)
{
	dev_vdbg(&ntc->dev, "request imm64 dst %llx val %llx\n",
		 dst, val);

	return ntc_ntb_req_imm(ntc, req, dst,
			       &val, sizeof(val),
			       fence, cb, cb_ctx);
}

static int ntc_ntb_req_signal(struct ntc_dev *ntc, void *req,
			      void (*cb)(void *cb_ctx), void *cb_ctx, int vec)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	dev_vdbg(&ntc->dev, "request signal to peer\n");

	BUG_ON(vec > ARRAY_SIZE(dev->peer_irq_addr));

	BUG_ON(dev->peer_irq_addr[vec] == (0ULL));

	BUG_ON(dev->peer_irq_num < vec);

	return ntc_ntb_req_imm32(ntc, req,
				 dev->peer_irq_addr[vec],
				 dev->peer_irq_data[vec],
				 false, cb, cb_ctx);
}

static int ntc_ntb_clear_signal(struct ntc_dev *ntc, int vec)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	int db_bit;

	if (use_msi)
		return 0;

	if (unlikely(vec >= BITS_PER_LONG_LONG))
		dev_WARN(&ntc->dev, "Invalid vec %d \n", vec);
	/* dev->peer_irq_num could be null if it is not set yet */

	db_bit = BIT_ULL(vec % (dev->peer_irq_num?:1));

	if (ntb_db_read(dev->ntb) & db_bit) {
		ntb_db_clear(dev->ntb, db_bit);
		ntb_db_read(dev->ntb);
		return 1;
	}

	return 0;
}

int ntc_ntb_max_peer_irqs(struct ntc_dev *ntc)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	return dev->peer_irq_num;
}

void *ntc_ntb_local_hello_buf(struct ntc_dev *ntc, int *size)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	*size = NTC_CTX_BUF_SIZE;
	return dev->info_peer_on_self->ctx_buf;
}

void *ntc_ntb_peer_hello_buf(struct ntc_dev *ntc, int *size)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	*size = NTC_CTX_BUF_SIZE;
	return dev->info_self_on_peer->ctx_buf;
}


static struct ntc_dev_ops ntc_ntb_dev_ops = {
	.map_dev			= ntc_ntb_map_dev,
	.link_disable		= ntc_ntb_link_disable,
	.link_enable		= ntc_ntb_link_enable,
	.link_reset			= ntc_ntb_link_reset,
	.peer_addr			= ntc_ntb_peer_addr,
	.req_create			= ntc_ntb_req_create,
	.req_cancel			= ntc_ntb_req_cancel,
	.req_submit			= ntc_ntb_req_submit,
	.req_memcpy			= ntc_ntb_req_memcpy,
	.req_imm32			= ntc_ntb_req_imm32,
	.req_imm64			= ntc_ntb_req_imm64,
	.req_signal			= ntc_ntb_req_signal,
	.clear_signal		= ntc_ntb_clear_signal,
	.max_peer_irqs		= ntc_ntb_max_peer_irqs,
	.local_hello_buf	= ntc_ntb_local_hello_buf,
	.peer_hello_buf		= ntc_ntb_peer_hello_buf,
};

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

static int ntc_ntb_dev_init(struct ntc_ntb_dev *dev)
{
	phys_addr_t mw_base;
	resource_size_t mw_size;
	int rc, i, mw_idx;

	/* inherit ntb device name and configuration */
	dev_set_name(&dev->ntc.dev, "%s", dev_name(&dev->ntb->dev));
	dev->ntc.dev.parent = &dev->ntb->dev;

	/* specify the callback behavior of this device */
	dev->ntc.dev_ops = &ntc_ntb_dev_ops;
	dev->ntc.map_ops = &ntc_phys_map_ops;

	/* make sure link is disabled and warnings are cleared */
	ntb_link_disable(dev->ntb);
	ntb_db_is_unsafe(dev->ntb);
	ntb_spad_is_unsafe(dev->ntb);

	/* we'll be using the last memory window if it exists */
	mw_idx = ntb_mw_count(dev->ntb, 0);
	if (mw_idx <= 0) {
		pr_debug("no mw for new device %s\n",
			 dev_name(&dev->ntb->dev));
		rc = -EINVAL;
		goto err_mw;
	}
	--mw_idx;

	/* clear any garbage translations */
	for (i = 0; i < mw_idx; ++i)
		ntb_mw_clear_trans(dev->ntb, 0, i);

	/* this is the window we'll translate to local dram */
	ntb_peer_mw_get_addr(dev->ntb, mw_idx, &mw_base, &mw_size);

	/*
	 * FIXME: ensure window is large enough.
	 * Fail under 8G here, but mw should be >= dram.
	 */
	if (mw_size < 0x200000000ul) {
		pr_debug("invalid mw size for new device %s\n",
			 dev_name(&dev->ntb->dev));
		rc = -EINVAL;
		goto err_mw;
	}

	/* FIXME: zero is not a portable address for local dram */
	rc = ntb_mw_set_trans(dev->ntb, 0, mw_idx, 0, mw_size);
	if (rc) {
		pr_debug("failed to translate mw for new device %s\n",
			 dev_name(&dev->ntb->dev));
		goto err_mw;
	}

	dev->peer_dram_base = mw_base;
	dev->peer_dram_size = mw_size;

	/* a local buffer for peer driver to write */
	ntb_peer_mw_get_addr(dev->ntb, mw_idx - 1, &mw_base, &mw_size);

	if (mw_size < ntc_ntb_info_size) {
		pr_debug("invalid alignement of peer info for new device %s\n",
			 dev_name(&dev->ntb->dev));
		rc = -ENOMEM;
		goto err_info;
	}

	dev->peer_info_base = mw_base;
	dev->info_peer_on_self_size = mw_size;

	dev->info_peer_on_self_unaligned =
		dma_alloc_coherent(ntc_ntb_dma_dev(dev),
				   dev->info_peer_on_self_size * 2,
				   &dev->info_peer_on_self_dma,
				   GFP_KERNEL);
	if (!dev->info_peer_on_self_unaligned) {
		pr_debug("failed to allocate peer info for new device %s\n",
			 dev_name(&dev->ntb->dev));
		rc = -ENOMEM;
		goto err_info;
	}

	/* get the offset to the aligned memory */
	dev->info_peer_on_self_off =
		PTR_ALIGN(dev->info_peer_on_self_unaligned,
			  dev->info_peer_on_self_size) -
		dev->info_peer_on_self_unaligned;

	/* get the type-cast pointer to the aligned memory */
	dev->info_peer_on_self =
		dev->info_peer_on_self_unaligned +
		dev->info_peer_on_self_off;

	dev->info_self_on_peer = ioremap(dev->peer_info_base,
					 sizeof(*dev->info_self_on_peer));
	if (!dev->info_self_on_peer) {
		pr_debug("failed to remap info on peer for new device %s\n",
			 dev_name(&dev->ntb->dev));
		rc = -EIO;
		goto err_map;
	}

	/* set the ntb translation to the aligned dma memory */
	rc = ntb_mw_set_trans(dev->ntb, 0, mw_idx - 1,
			      dev->info_peer_on_self_dma
			      + dev->info_peer_on_self_off,
			      dev->info_peer_on_self_size);
	if (rc) {
		pr_debug("failed to translate info mw for new device %s rc %d\n",
			 dev_name(&dev->ntb->dev), rc);
		goto err_ctx;
	}

	/* haven't negotiated the version */
	dev->version = 0;

	/* haven't negotiated the msi pair */
	dev->peer_irq_num = 0;
	memset(dev->peer_irq_addr, 0, sizeof(dev->peer_irq_addr));
	memset(dev->peer_irq_data, 0, sizeof(dev->peer_irq_data));

	/* init the link state heartbeat */
	dev->ping_run = false;
	dev->ping_miss = 0;
	dev->ping_flags = 0;
	dev->ping_seq = 0;
	dev->ping_msg = NTC_NTB_LINK_START;
	dev->poll_val = 0;
	dev->poll_msg = NTC_NTB_LINK_QUIESCE;

	timer_setup(&dev->ping_pong,
		    ntc_ntb_ping_pong_fn,
		    0);

	timer_setup(&dev->ping_poll,
		    ntc_ntb_ping_poll_fn,
		    0);

	spin_lock_init(&dev->ping_lock);

	/* init the link state machine */
	dev->link_is_up = false;
	dev->link_state = NTC_NTB_LINK_START;

	INIT_WORK(&dev->link_work,
		  ntc_ntb_link_work_cb);

	mutex_init(&dev->link_lock);

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
	iounmap(dev->info_self_on_peer);
err_map:
	dma_free_coherent(ntc_ntb_dma_dev(dev),
			  dev->info_peer_on_self_size * 2,
			  dev->info_peer_on_self_unaligned,
			  dev->info_peer_on_self_dma);
err_info:
	ntb_mw_clear_trans(dev->ntb, 0, mw_idx);
err_mw:
	return rc;
}

static void ntc_ntb_dev_deinit(struct ntc_ntb_dev *dev)
{
	int i, mw_idx;

	ntb_clear_ctx(dev->ntb);

	ntb_link_disable(dev->ntb);

	mw_idx = ntb_mw_count(dev->ntb, 0);
	for (i = 0; i < mw_idx; ++i)
		ntb_mw_clear_trans(dev->ntb, 0, i);

	ntc_ntb_ping_stop(dev);

	cancel_work_sync(&dev->link_work);

	iounmap(dev->info_self_on_peer);

	dma_free_coherent(ntc_ntb_dma_dev(dev),
			  dev->info_peer_on_self_size * 2,
			  dev->info_peer_on_self_unaligned,
			  dev->info_peer_on_self_dma);
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
	dma_release_channel(dev->dma);
	kfree(dev);
}

static int ntc_debugfs_read(struct seq_file *s, void *v)
{
	struct ntc_ntb_dev *dev = s->private;
	int i;

	seq_printf(s, "peer_dram_base %#llx\n",
		   dev->peer_dram_base);
	seq_printf(s, "info_peer_on_self_size %#zx\n",
		   dev->info_peer_on_self_size);
	seq_printf(s, "info_peer_on_self_dma %#llx\n",
		   dev->info_peer_on_self_dma);
	seq_puts(s, "info_peer_on_self:\n");
	seq_printf(s, "  magic %#x\n",
		   dev->info_peer_on_self->magic);
	seq_printf(s, "  ping %#x\n",
		   dev->info_peer_on_self->ping);
	seq_printf(s, "  msi_irqs_num %d\n",
			dev->info_peer_on_self->msi_irqs_num);
	for (i = 0; i <  dev->info_peer_on_self->msi_irqs_num; i ++) {
		seq_printf(s, "  msi_data %#x\n",
			   dev->info_peer_on_self->msi_data[i]);
		seq_printf(s, "  msi_addr_lower %#x\n",
			   dev->info_peer_on_self->msi_addr_lower[i]);
		seq_printf(s, "  msi_addr_upper %#x\n",
		   dev->info_peer_on_self->msi_addr_upper[i]);
	}
	seq_puts(s, "  ctx level negotiation:\n");
	seq_printf(s, "    done %#x\n",
		   dev->info_peer_on_self->done);
	seq_printf(s, "version %d\n",
		   dev->version);
	seq_printf(s, "peer_irq_num %d\n",
			   dev->peer_irq_num);
	for (i = 0; i <  dev->peer_irq_num; i++) {
		seq_printf(s, "peer_irq_addr %#llx\n",
				dev->peer_irq_addr[i]);
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
		   dev->link_is_up);
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

	dev->dma = dma;

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

static int __init ntc_init(void)
{
	pr_info("%s: %s %s\n", DRIVER_NAME,
		DRIVER_DESCRIPTION, DRIVER_VERSION);
	if (debugfs_initialized())
		ntc_dbgfs = debugfs_create_dir(KBUILD_MODNAME, NULL);
	ntb_register_client(&ntc_ntb_client);
	return 0;
}
module_init(ntc_init);

static void __exit ntc_exit(void)
{
	ntb_unregister_client(&ntc_ntb_client);
	debugfs_remove_recursive(ntc_dbgfs);
}
module_exit(ntc_exit);
