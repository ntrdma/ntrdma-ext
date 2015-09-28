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

#include <linux/dmaengine.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/ntb.h>
#include <linux/pci.h>
#include <linux/timer.h>

#include <linux/ntc.h>

#define DRIVER_NAME			"ntc_ntb"
#define DRIVER_DESCRIPTION		"NTC Non Transparent Bridge"

#define DRIVER_LICENSE			"Dual BSD/GPL"
#define DRIVER_VERSION			"0.2"
#define DRIVER_RELDATE			"5 October 2015"
#define DRIVER_AUTHOR			"Allen Hubbe <Allen.Hubbe@emc.com>"

MODULE_LICENSE(DRIVER_LICENSE);
MODULE_VERSION(DRIVER_VERSION);
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);

static int ntc_ntb_info_size = 0x1000;
/* TODO: module param named info_size */

static int ntc_ntb_req_int = 0x1000;
/* TODO: module param named req_int */

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
#define NTC_NTB_LINK_RESET_REQ		1
#define NTC_NTB_LINK_RESET_ACK		2
#define NTC_NTB_LINK_INIT_SPAD		3
#define NTC_NTB_LINK_INIT_INFO		4
#define NTC_NTB_LINK_PING_READY		5
#define NTC_NTB_LINK_PING_COMMIT	6
#define NTC_NTB_LINK_HELLO		7

#define NTC_NTB_DMA_PREP_FLAGS		0

#define NTC_NTB_PING_PONG_SPAD		BIT(1)
#define NTC_NTB_PING_PONG_MEM		BIT(2)
#define NTC_NTB_PING_POLL_MEM		BIT(3)

#define NTC_NTB_PING_PONG_PERIOD	msecs_to_jiffies(100)
#define NTC_NTB_PING_POLL_PERIOD	msecs_to_jiffies(257)
#define NTC_NTB_PING_MISS_THRESHOLD	10

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

struct ntc_ntb_hello_info {
	u32				size;
	u32				addr_lower;
	u32				addr_upper;
};

struct ntc_ntb_info {
	u32				magic;
	u32				ping;

	struct ntc_ntb_hello_info	hello_odd;
	struct ntc_ntb_hello_info	hello_even;

	u32				hello_even_size;
	u32				hello_even_addr_lower;
	u32				hello_even_addr_upper;

	u32				msi_data;
	u32				msi_addr_lower;
	u32				msi_addr_upper;
};

struct ntc_ntb_hello_map {
	void				*buf;
	size_t				size;
	dma_addr_t			dma;
};

struct ntc_ntb_dev {
	struct ntc_dev			ntc;

	/* channel supporting hardware devices */
	struct ntb_dev			*ntb;
	struct dma_chan			*dma;

	/* local ntb window offset to peer dram */
	u64				peer_dram_base;

	/* local buffer for remote driver to write info */
	struct ntc_ntb_info		*info_peer_on_self;
	size_t				info_peer_on_self_size;
	dma_addr_t			info_peer_on_self_dma;

	/* remote buffer for local driver to write info */
	struct ntc_ntb_info __iomem	*info_self_on_peer;

	/* negotiated protocol version for ntc */
	int				version;

	/* direct interrupt message */
	u64				peer_msi_addr;
	u32				peer_msi_data;

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

	struct ntc_ntb_hello_map	hello_odd;
	struct ntc_ntb_hello_map	hello_even;

	/* request interrupt interval */
	int				req_int;
	spinlock_t			req_lock;
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

static void ntc_ntb_read_msi_config(struct ntc_ntb_dev *dev,
				    u64 *addr, u32 *val)
{
	u32 addr_lo, addr_hi, data;

	/* FIXME: add support for 64-bit MSI header, and MSI-X.  Until it is
	 * fixed here, we need to disable MSI-X in the NTB HW driver.
	 */

	pci_read_config_dword(dev->ntb->pdev,
			      PCI_MSI_ADDRESS_LO + dev->ntb->pdev->msi_cap,
			      &addr_lo);

	/* FIXME: msi header layout differs for 32/64 bit addr width. */
	addr_hi = 0;

	pci_read_config_dword(dev->ntb->pdev,
			      dev->ntb->pdev->msi_cap + PCI_MSI_DATA_32,
			      &data);

	*addr = (u64)addr_lo | (u64)addr_hi << 32;
	*val = data;

	dev_dbg(&dev->ntc.dev, "msi addr %#llx data %#x\n", *addr, *val);
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
	if (msg >= NTC_NTB_LINK_PING_COMMIT)
		return NTC_NTB_PING_PONG_MEM | NTC_NTB_PING_POLL_MEM;

	if (msg >= NTC_NTB_LINK_PING_READY)
		return NTC_NTB_PING_PONG_MEM | NTC_NTB_PING_PONG_SPAD |
			NTC_NTB_PING_POLL_MEM;

	if (msg >= NTC_NTB_LINK_INIT_INFO)
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

	ping_val = ntc_ntb_ping_val(dev->ping_msg, ++dev->ping_seq);

	dev_vdbg(&dev->ntc.dev, "ping val %x\n", ping_val);

	wmb();

	if (ping_flags & NTC_NTB_PING_PONG_MEM)
		iowrite32(ping_val, &dev->info_self_on_peer->ping);

	if (ping_flags & NTC_NTB_PING_PONG_SPAD)
		ntb_peer_spad_write(dev->ntb, NTC_NTB_SPAD_PING, ping_val);

	if (poison_flags & NTC_NTB_PING_PONG_MEM)
		iowrite32(0, &dev->info_self_on_peer->ping);

	if (poison_flags & NTC_NTB_PING_PONG_SPAD)
		ntb_peer_spad_write(dev->ntb, NTC_NTB_SPAD_PING, 0);

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
	int ping_flags;
	u32 poll_val;

	if (!dev->ping_run)
		return false;
	mod_timer(&dev->ping_poll, jiffies + NTC_NTB_PING_POLL_PERIOD);

	ping_flags = dev->ping_flags;

	if (ping_flags & NTC_NTB_PING_POLL_MEM)
		poll_val = dev->info_peer_on_self->ping;
	else
		poll_val = ntb_spad_read(dev->ntb, NTC_NTB_SPAD_PING);

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
			dev_dbg(&dev->ntc.dev, "peer lost\n");
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

	rmb();

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
	dev_dbg(&dev->ntc.dev, "link error\n");

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_QUIESCE);
	schedule_work(&dev->link_work);
}

static inline void ntc_ntb_quiesce(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link quiesce\n");

	ntc_ctx_quiesce(&dev->ntc);

	/* TODO: cancel and wait for any outstanding dma requests */

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_RESET_REQ);
}

static inline void ntc_ntb_reset(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link reset\n");

	ntc_ctx_reset(&dev->ntc);

	if (dev->hello_odd.dma) {
		dma_unmap_single(ntc_ntb_dma_dev(dev),
				 dev->hello_odd.dma,
				 dev->hello_odd.size,
				 DMA_FROM_DEVICE);
	}
	kfree(dev->hello_odd.buf);
	dev->hello_odd.buf = NULL;
	dev->hello_odd.size = 0;
	dev->hello_odd.dma = 0;

	if (dev->hello_even.dma) {
		dma_unmap_single(ntc_ntb_dma_dev(dev),
				 dev->hello_even.dma,
				 dev->hello_even.size,
				 DMA_FROM_DEVICE);
	}
	kfree(dev->hello_even.buf);
	dev->hello_even.buf = NULL;
	dev->hello_even.size = 0;
	dev->hello_even.dma = 0;

	memset(dev->info_peer_on_self, 0, sizeof(*dev->info_peer_on_self));

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_RESET_ACK);
}

static inline void ntc_ntb_init_spad(struct ntc_ntb_dev *dev)
{
	u32 version;

	dev_dbg(&dev->ntc.dev, "link init spad\n");

	version = ntc_ntb_version(NTC_NTB_VERSION_MAX, NTC_NTB_VERSION_MIN);
	ntb_peer_spad_write(dev->ntb, NTC_NTB_SPAD_VERSION, version);

	ntb_peer_spad_write(dev->ntb, NTC_NTB_SPAD_ADDR_LOWER,
			    lower_32_bits(dev->info_peer_on_self_dma));
	ntb_peer_spad_write(dev->ntb, NTC_NTB_SPAD_ADDR_UPPER,
			    upper_32_bits(dev->info_peer_on_self_dma));

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_INIT_SPAD);
}

static inline void ntc_ntb_init_info(struct ntc_ntb_dev *dev)
{
	u32 version, peer_version, msi_data;
	u64 peer_addr, msi_addr;

	dev_dbg(&dev->ntc.dev, "link init info\n");

	version = ntc_ntb_version(NTC_NTB_VERSION_MAX, NTC_NTB_VERSION_MIN);
	peer_version = ntb_spad_read(dev->ntb, NTC_NTB_SPAD_VERSION);

	dev_dbg(&dev->ntc.dev, "version %x peer_version %x\n", version, peer_version);

	dev->version = ntc_ntb_version_choose(version, peer_version);
	if (dev->version == NTC_NTB_VERSION_NONE)
		goto err;

	dev_dbg(&dev->ntc.dev, "ok version\n");

	peer_addr = ((u64)ntb_spad_read(dev->ntb, NTC_NTB_SPAD_ADDR_LOWER)) |
		(((u64)ntb_spad_read(dev->ntb, NTC_NTB_SPAD_ADDR_UPPER)) << 32);
	dev->info_self_on_peer = ioremap(dev->peer_dram_base + peer_addr,
					 sizeof(*dev->info_self_on_peer));
	if (!dev->info_self_on_peer)
		goto err;

	dev_dbg(&dev->ntc.dev, "ok self on peer\n");

	iowrite32(ntc_ntb_version_magic(dev->version),
		  &dev->info_self_on_peer->magic);

	iowrite32(0, &dev->info_self_on_peer->hello_odd.size);
	iowrite32(0, &dev->info_self_on_peer->hello_odd.addr_lower);
	iowrite32(0, &dev->info_self_on_peer->hello_odd.addr_upper);

	iowrite32(0, &dev->info_self_on_peer->hello_even.size);
	iowrite32(0, &dev->info_self_on_peer->hello_even.addr_lower);
	iowrite32(0, &dev->info_self_on_peer->hello_even.addr_upper);

	ntc_ntb_read_msi_config(dev, &msi_addr, &msi_data);
	iowrite32(msi_data, &dev->info_self_on_peer->msi_data);
	iowrite32(lower_32_bits(msi_addr),
		  &dev->info_self_on_peer->msi_addr_lower);
	iowrite32(upper_32_bits(msi_addr),
		  &dev->info_self_on_peer->msi_addr_upper);

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_INIT_INFO);
	return;

err:
	ntc_ntb_error(dev);
}

static inline void ntc_ntb_ping_ready(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link ping ready\n");

	dev->peer_msi_data = dev->info_peer_on_self->msi_data;

	dev->peer_msi_addr = dev->peer_dram_base +
		(((u64)dev->info_peer_on_self->msi_addr_lower) |
		 (((u64)dev->info_peer_on_self->msi_addr_upper) << 32));

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_PING_READY);
}

static inline void ntc_ntb_ping_commit(struct ntc_ntb_dev *dev)
{
	dev_dbg(&dev->ntc.dev, "link ping commit\n");

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_PING_COMMIT);
}

static int ntc_ntb_prep_hello(struct ntc_ntb_dev *dev, size_t buf_size,
			      struct ntc_ntb_hello_map *map,
			      struct ntc_ntb_hello_info __iomem *info)
{
	int rc;

	dev_dbg(&dev->ntc.dev, "link prep hello\n");

	if (upper_32_bits(buf_size))
		return -EINVAL;

	map->size = buf_size;

	map->buf = kmalloc_node(map->size, GFP_ATOMIC,
				dev_to_node(&dev->ntc.dev));
	if (!map->buf) {
		rc = -ENOMEM;
		goto err_buf;
	}

	map->dma = dma_map_single(ntc_ntb_dma_dev(dev),
				  map->buf, map->size,
				  DMA_FROM_DEVICE);
	if (!map->dma) {
		rc = -EIO;
		goto err_dma;
	}

	iowrite32(lower_32_bits(map->size), &info->size);
	iowrite32(lower_32_bits(map->dma), &info->addr_lower);
	iowrite32(upper_32_bits(map->dma), &info->addr_upper);

	return 0;

err_dma:
	kfree(map->buf);
	map->buf = NULL;
err_buf:
	map->size = 0;
	return rc;
}

static inline bool ntc_ntb_done_hello(struct ntc_ntb_dev *dev)
{
	/* no outstanding input or output buffers */
	return !dev->hello_odd.size && !dev->hello_even.size &&
		!dev->info_peer_on_self->hello_odd.size &&
		!dev->info_peer_on_self->hello_even.size;
}

static inline void ntc_ntb_hello(struct ntc_ntb_dev *dev)
{
	struct ntc_ntb_hello_map *in;
	struct ntc_ntb_hello_info __iomem *info_in;
	struct ntc_ntb_hello_info *info_out;
	size_t out_size;
	void *out_buf;
	u64 out_addr;
	void __iomem *out_mmio;
	ssize_t next_size;
	int rc, phase;

	phase = dev->link_state + 1 - NTC_NTB_LINK_HELLO;

	dev_dbg(&dev->ntc.dev, "link hello phase %d\n", phase);

	if (phase < 0)
		goto err_phase;

	if (phase & 1) {
		/* odd phases have even input buffers, odd output buffers */
		in = &dev->hello_even;
		info_in = &dev->info_self_on_peer->hello_even;
		info_out = &dev->info_peer_on_self->hello_odd;
	} else {
		/* even phases have odd input buffers, even output buffers */
		in = &dev->hello_odd;
		info_in = &dev->info_self_on_peer->hello_odd;
		info_out = &dev->info_peer_on_self->hello_even;
	}

	/* unmap the input buffer for this phase */
	iowrite32(0, &info_in->size);
	iowrite32(0, &info_in->addr_lower);
	iowrite32(0, &info_in->addr_upper);

	if (in->dma) {
		dma_unmap_single(ntc_ntb_dma_dev(dev),
				 in->size,
				 in->dma,
				 DMA_FROM_DEVICE);
		in->dma = 0;
	}

	/* allocate a temporary output buffer for this phase */
	out_size = info_out->size;
	if (out_size) {
		out_buf = kmalloc_node(out_size, GFP_KERNEL,
				       dev_to_node(&dev->ntc.dev));
		if (!out_buf)
			goto err_out_buf;
	} else {
		out_buf = NULL;
	}

	/* perform this phase of initialization */
	next_size = ntc_ctx_hello(&dev->ntc, phase,
				  in->buf, in->size,
				  out_buf, out_size);

	/* check if this phase was successful */
	if (next_size < 0)
		goto err_hello;

	dev_dbg(&dev->ntc.dev, "successfull hello callback\n");

	/* copy the temporary output buffer to the peer */
	if (out_size) {
		dev_dbg(&dev->ntc.dev, "going to copy output\n");

		out_addr = ((u64)info_out->addr_lower) |
			(((u64)info_out->addr_upper) << 32);

		out_mmio = ioremap(dev->peer_dram_base + out_addr, out_size);
		if (!out_mmio)
			goto err_hello;
		memcpy_toio(out_mmio, out_buf, out_size);
		iounmap(out_mmio);

		dev_dbg(&dev->ntc.dev, "successfully copied output\n");
	}

	/* free the input and output buffers of this phase */
	kfree(out_buf);
	kfree(in->buf);
	in->buf = NULL;
	in->size = 0;

	/* prepare for the phase after next */
	if (next_size) {
		dev_dbg(&dev->ntc.dev, "prepare for next phase\n");
		rc = ntc_ntb_prep_hello(dev, next_size, in, info_in);
		if (rc)
			goto err_phase;
	}

	ntc_ntb_link_set_state(dev, NTC_NTB_LINK_HELLO + phase);

	return;

err_hello:
	kfree(out_buf);
err_out_buf:
	kfree(in->buf);
	in->buf = NULL;
	in->size = 0;
err_phase:
	ntc_ntb_error(dev);
}

static void ntc_ntb_link_work(struct ntc_ntb_dev *dev)
{
	int link_event = ntc_ntb_link_get_event(dev);
	bool link_up = false;

	dev_dbg(&dev->ntc.dev, "link work state %d event %d\n",
		dev->link_state, link_event);

	switch (dev->link_state) {
	case NTC_NTB_LINK_QUIESCE:
		ntc_ntb_quiesce(dev);

		if (dev->link_state != NTC_NTB_LINK_RESET_REQ)
			goto out;

	case NTC_NTB_LINK_RESET_REQ:
		switch (link_event) {
		default:
			goto out;
		case NTC_NTB_LINK_RESET_REQ:
		case NTC_NTB_LINK_RESET_ACK:
			ntc_ntb_reset(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_RESET_ACK)
			goto out;

	case NTC_NTB_LINK_RESET_ACK:
		switch (link_event) {
		default:
			goto out;
		case NTC_NTB_LINK_RESET_ACK:
		case NTC_NTB_LINK_INIT_SPAD:
			ntc_ntb_init_spad(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_INIT_SPAD)
			goto out;

	case NTC_NTB_LINK_INIT_SPAD:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_RESET_ACK:
			goto out;
		case NTC_NTB_LINK_INIT_SPAD:
		case NTC_NTB_LINK_INIT_INFO:
			ntc_ntb_init_info(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_INIT_INFO)
			goto out;

	case NTC_NTB_LINK_INIT_INFO:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_INIT_SPAD:
			goto out;
		case NTC_NTB_LINK_INIT_INFO:
		case NTC_NTB_LINK_PING_READY:
			ntc_ntb_ping_ready(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_PING_READY)
			goto out;

	case NTC_NTB_LINK_PING_READY:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_INIT_INFO:
			goto out;
		case NTC_NTB_LINK_PING_READY:
		case NTC_NTB_LINK_PING_COMMIT:
			ntc_ntb_ping_commit(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_PING_COMMIT)
			goto out;

	case NTC_NTB_LINK_PING_COMMIT:
		switch (link_event) {
		default:
			ntc_ntb_error(dev);
		case NTC_NTB_LINK_PING_READY:
			goto out;
		case NTC_NTB_LINK_PING_COMMIT:
		case NTC_NTB_LINK_HELLO:
			ntc_ntb_hello(dev);
		}

		if (dev->link_state != NTC_NTB_LINK_HELLO)
			goto out;

	case NTC_NTB_LINK_HELLO:
	default:
		while (!ntc_ntb_done_hello(dev)) {
			dev_dbg(&dev->ntc.dev, "not done hello\n");
			switch (link_event - dev->link_state) {
			default:
				ntc_ntb_error(dev);
			case -1:
				dev_dbg(&dev->ntc.dev, "peer is behind hello\n");
				goto out;
			case 0:
			case 1:
				dev_dbg(&dev->ntc.dev, "can advance hello\n");
				ntc_ntb_hello(dev);
			}
		}

		dev_dbg(&dev->ntc.dev, "done hello\n");

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

	dev_vdbg(&ntc->dev, "submit request\n");

	dma_async_issue_pending(chan);

	return 0;
}

static int ntc_ntb_req_prep_flags(struct ntc_dev *ntc, bool fence)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);
	unsigned long irqflags;
	int flags = NTC_NTB_DMA_PREP_FLAGS;

	if (fence)
		flags |= DMA_PREP_FENCE;

	spin_lock_irqsave(&dev->req_lock, irqflags);
	if (dev->req_int) {
		--dev->req_int;
	} else {
		dev->req_int = ntc_ntb_req_int;
		flags |= DMA_PREP_INTERRUPT;
	}
	spin_unlock_irqrestore(&dev->req_lock, irqflags);

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

#ifdef CONFIG_NTC_NTB_DMA_REQ_IMM
	struct dma_chan *chan = req;
	struct dma_async_tx_descriptor *tx;
	dma_cookie_t cookie;
	int flags;

	if (chan->device->device_prep_dma_imm) {
		flags = ntc_ntb_req_prep_flags(ntc, fence);

		tx = chan->device->device_prep_dma_imm(chan, dst, ptr,
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
#endif

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
		       void (*cb)(void *cb_ctx), void *cb_ctx)
{
	struct ntc_ntb_dev *dev = ntc_ntb_down_cast(ntc);

	dev_vdbg(&ntc->dev, "request signal to peer\n");

	return ntc_ntb_req_imm32(ntc, req,
				 dev->peer_msi_addr,
				 dev->peer_msi_data,
				 false, cb, cb_ctx);
}

static struct ntc_dev_ops ntc_ntb_dev_ops = {
	.map_dev			= ntc_ntb_map_dev,
	.link_disable			= ntc_ntb_link_disable,
	.link_enable			= ntc_ntb_link_enable,
	.link_reset			= ntc_ntb_link_reset,
	.peer_addr			= ntc_ntb_peer_addr,
	.req_create			= ntc_ntb_req_create,
	.req_cancel			= ntc_ntb_req_cancel,
	.req_submit			= ntc_ntb_req_submit,
	.req_memcpy			= ntc_ntb_req_memcpy,
	.req_imm32			= ntc_ntb_req_imm32,
	.req_imm64			= ntc_ntb_req_imm64,
	.req_signal			= ntc_ntb_req_signal,
};

static void ntc_ntb_link_event(void *ctx)
{
	struct ntc_ntb_dev *dev = ctx;

	schedule_work(&dev->link_work);
}

static void ntc_ntb_db_event(void *ctx, int vec)
{
	struct ntc_ntb_dev *dev = ctx;

	ntc_ctx_signal(&dev->ntc);
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
	mw_idx = ntb_mw_count(dev->ntb);
	if (mw_idx <= 0) {
		rc = -EINVAL;
		goto err_mw;
	}
	--mw_idx;

	/* clear any garbage translations */
	for (i = 0; i < mw_idx; ++i)
		ntb_mw_clear_trans(dev->ntb, i);

	/* this is the window we'll translate to local dram */
	ntb_mw_get_range(dev->ntb, mw_idx, &mw_base, &mw_size, NULL, NULL);

	/* FIXME: ensure window is large enough */
	if (mw_size < 1) {
		rc = -EINVAL;
		goto err_mw;
	}

	/* FIXME: zero is not a portable address for local dram */
	rc = ntb_mw_set_trans(dev->ntb, mw_idx, 0, mw_size);
	if (rc)
		goto err_mw;

	dev->peer_dram_base = mw_base;

	/* a local buffer for peer driver to write */
	dev->info_peer_on_self_size = ntc_ntb_info_size;
	dev->info_peer_on_self =
		dma_alloc_coherent(ntc_ntb_dma_dev(dev),
				   dev->info_peer_on_self_size,
				   &dev->info_peer_on_self_dma,
				   GFP_KERNEL);
	if (!dev->info_peer_on_self) {
		rc = -ENOMEM;
		goto err_info;
	}

	/* haven't negotiated the remote buffer */
	dev->info_self_on_peer = NULL;

	/* haven't negotiated the version */
	dev->version = 0;

	/* haven't negotiated the msi pair */
	dev->peer_msi_addr = 0;
	dev->peer_msi_data = 0;

	/* init the link state heartbeat */
	dev->ping_run = false;
	dev->ping_miss = 0;
	dev->ping_flags = 0;
	dev->ping_seq = 0;
	dev->ping_msg = NTC_NTB_LINK_RESET_ACK;
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
	dev->link_is_up = false;
	dev->link_state = NTC_NTB_LINK_RESET_ACK;

	INIT_WORK(&dev->link_work,
		  ntc_ntb_link_work_cb);

	mutex_init(&dev->link_lock);

	/* ready for context events */
	rc = ntb_set_ctx(dev->ntb, dev,
			 &ntc_ntb_ctx_ops);
	if (rc)
		goto err_ctx;

	return 0;

err_ctx:
	dma_free_coherent(ntc_ntb_dma_dev(dev),
			  dev->info_peer_on_self_size,
			  dev->info_peer_on_self,
			  dev->info_peer_on_self_dma);
err_info:
	ntb_mw_clear_trans(dev->ntb, mw_idx);
err_mw:
	return rc;
}

static void ntc_ntb_dev_deinit(struct ntc_ntb_dev *dev)
{
	int i, mw_idx;

	ntb_clear_ctx(dev->ntb);

	ntb_link_disable(dev->ntb);

	mw_idx = ntb_mw_count(dev->ntb);
	for (i = 0; i < mw_idx; ++i)
		ntb_mw_clear_trans(dev->ntb, i);

	ntc_ntb_ping_stop(dev);

	cancel_work_sync(&dev->link_work);

	dma_free_coherent(ntc_ntb_dma_dev(dev),
			  dev->info_peer_on_self_size,
			  dev->info_peer_on_self,
			  dev->info_peer_on_self_dma);
}

static inline struct pci_bus *ntc_ntb_ascend_bus(struct pci_bus *bus)
{
	while(bus->parent)
		bus = bus->parent;

	return bus;
}

static bool ntc_ntb_filter_bus(struct dma_chan *chan,
			       void *filter_param)
{
	struct pci_dev *pdev;

	pdev = to_pci_dev(chan->dev->device.parent);

	return ntc_ntb_ascend_bus(pdev->bus) == filter_param;
}

static void ntc_ntb_release(struct device *device)
{
	struct ntc_ntb_dev *dev = ntc_ntb_of_dev(device);

	pr_devel("release %s\n", dev_name(&dev->ntc.dev));

	ntc_ntb_dev_deinit(dev);
	put_device(&dev->ntb->dev);
	dma_release_channel(dev->dma);
	kfree(dev);
}

static int ntc_ntb_probe(struct ntb_client *self,
			 struct ntb_dev *ntb)
{
	struct ntc_ntb_dev *dev;
	struct dma_chan *dma;
	dma_cap_mask_t mask;
	int rc;

	pr_devel("probe ntb %s\n", dev_name(&ntb->dev));

	dev = kzalloc_node(sizeof(*dev), GFP_KERNEL,
			   dev_to_node(&ntb->dev));
	if (!dev) {
		rc = -ENOMEM;
		goto err_dev;
	}

	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	dma = dma_request_channel(mask, ntc_ntb_filter_bus,
				       ntc_ntb_ascend_bus(ntb->pdev->bus));
	if (!dma) {
		rc = -ENODEV;
		goto err_dma;
	}

	pr_devel("probe dma %s\n", dev_name(&dma->dev->device));

	dev->dma = dma;

	get_device(&ntb->dev);
	dev->ntb = ntb;

	rc = ntc_ntb_dev_init(dev);
	if (rc)
		goto err_init;

	pr_devel("initialized %s\n", dev_name(&dev->ntc.dev));

	dev->ntc.dev.release = ntc_ntb_release;

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
	ntb_register_client(&ntc_ntb_client);
	return 0;
}
module_init(ntc_init);

static void __exit ntc_exit(void)
{
	ntb_unregister_client(&ntc_ntb_client);
}
module_exit(ntc_exit);
