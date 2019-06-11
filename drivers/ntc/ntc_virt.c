#include <linux/init.h>
#include <linux/module.h>

#include <linux/slab.h>

#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>

#include <linux/ntc.h>

#define DRIVER_NAME "ntc_virt"
#define DRIVER_VERSION  "0.2"
#define DRIVER_RELDATE  "30 September 2015"

MODULE_AUTHOR("Allen Hubbe");
MODULE_DESCRIPTION("NTC virtual channel-mapped buffer support library");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRIVER_VERSION);

static void *ntc_virt_buf_alloc(struct ntc_dev *ntc, u64 size,
				u64 *addr, gfp_t gfp)
{
	void *buf;

	buf = kmalloc_node(size, gfp, dev_to_node(&ntc->dev));

	/* addr must store at least a platform virtual address */
	BUILD_BUG_ON(sizeof(addr) < sizeof(buf));
	*addr = (u64)buf;

	return buf;
}

static void ntc_virt_buf_free(struct ntc_dev *ntc, u64 size,
			      void *buf, u64 addr)
{
	kfree(buf);
}

static u64 ntc_virt_buf_map(struct ntc_dev *ntc, void *buf, u64 size,
			    enum dma_data_direction dir)
{
	/* return value must store at least a platform virtual address */
	BUILD_BUG_ON(sizeof(u64) < sizeof(buf));

	return (u64)buf;
}

static void ntc_virt_buf_unmap(struct ntc_dev *ntc, u64 addr, u64 size,
			       enum dma_data_direction dir)
{
	rmb(); /* read data in the order it is received out of the channel */
}

static void *ntc_virt_umem_get(struct ib_udata *udata,
			       unsigned long uaddr, size_t size,
			       int access, int dmasync)
{
	access |= IB_ACCESS_SOFTWARE;

	return ib_umem_get(udata, uaddr, size, access, dmasync);
}

static void ntc_virt_umem_put(struct ntc_dev *ntc, void *umem)
{
	ib_umem_release(umem);
}

static int ntc_virt_umem_sgl(struct ntc_dev *ntc, void *umem,
			     struct ntc_sge *sgl, int count)
{
	struct ib_umem *ibumem = umem;
	struct scatterlist *sg, *next;
	void *virt_addr, *next_addr;
	size_t virt_len;
	int i, virt_count = 0;

	BUILD_BUG_ON(sizeof(u64) < sizeof(virt_addr));
	BUILD_BUG_ON(sizeof(u64) < sizeof(virt_len));

	for_each_sg(ibumem->sg_head.sgl, sg, ibumem->sg_head.nents, i) {
		/* virt_addr is start addr of the contiguous range */
		virt_addr = page_address(sg_page(sg));
		/* virt_len accumulates the length of the contiguous range */
		virt_len = PAGE_SIZE;

		if (!virt_addr)
			return -ENOMEM;

		for (; i + 1 < ibumem->sg_head.nents; ++i) {
			next = sg_next(sg);
			if (!next)
				break;
			next_addr = page_address(sg_page(next));
			if (next_addr != virt_addr + virt_len)
				break;
			virt_len += PAGE_SIZE;
			sg = next;
		}

		if (sgl && virt_count < count) {
			sgl[virt_count].addr = (u64)virt_addr;
			sgl[virt_count].len = virt_len;
		}

		++virt_count;
	}

	if (virt_count && sgl && count > 0) {
		/* virt_len is start offset in the first page */
		virt_len = ib_umem_offset(ibumem);
		sgl[0].addr += virt_len;
		sgl[0].len -= virt_len;

		if (virt_count <= count) {
			/* virt_len is offset from the end of the last page */
			virt_len = (virt_len + ibumem->length) & ~PAGE_MASK;
			virt_len = (PAGE_SIZE - virt_len) & ~PAGE_MASK;
			sgl[virt_count - 1].len -= virt_len;
		}
	}

	return virt_count;
}

static void ntc_virt_sync_cpu(struct ntc_dev *ntc, u64 addr, u64 size,
			      enum dma_data_direction dir)
{
	rmb(); /* read data in the order it is received out of the channel */
}

struct ntc_map_ops ntc_virt_map_ops = {
	.buf_alloc			= ntc_virt_buf_alloc,
	.buf_free			= ntc_virt_buf_free,
	.buf_map			= ntc_virt_buf_map,
	.buf_unmap			= ntc_virt_buf_unmap,
	.buf_sync_cpu			= ntc_virt_sync_cpu,
	.umem_get			= ntc_virt_umem_get,
	.umem_put			= ntc_virt_umem_put,
	.umem_sgl			= ntc_virt_umem_sgl,
};
EXPORT_SYMBOL(ntc_virt_map_ops);
