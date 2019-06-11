#include <linux/init.h>
#include <linux/module.h>

#include <linux/dma-mapping.h>
#include <rdma/ib_umem.h>

#include <linux/ntc.h>

#define DRIVER_NAME "ntc_phys"
#define DRIVER_VERSION  "0.2"
#define DRIVER_RELDATE  "30 September 2015"

MODULE_AUTHOR("Allen Hubbe");
MODULE_DESCRIPTION("NTC physical channel-mapped buffer support library");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRIVER_VERSION);

static void *ntc_phys_buf_alloc(struct ntc_dev *ntc, u64 size,
				u64 *addr, gfp_t gfp)
{
	struct device *dev = ntc_map_dev(ntc);
	dma_addr_t dma_addr;
	void *buf;

	buf = dma_alloc_coherent(dev, size, &dma_addr, gfp);

	/* addr must store at least a platform dma addr */
	BUILD_BUG_ON(sizeof(*addr) < sizeof(dma_addr));
	*addr = (u64)dma_addr;

	return buf;
}

static void ntc_phys_buf_free(struct ntc_dev *ntc, u64 size,
			      void *buf, u64 addr)
{
	struct device *dev = ntc_map_dev(ntc);

	dma_free_coherent(dev, size, buf, addr);
}

static u64 ntc_phys_buf_map(struct ntc_dev *ntc, void *buf, u64 size,
			    enum dma_data_direction dir)
{
	struct device *dev = ntc_map_dev(ntc);

	/* return value must store at least a platform dma addr */
	BUILD_BUG_ON(sizeof(u64) < sizeof(dma_addr_t));

	return dma_map_single(dev, buf, size, dir);
}

static void ntc_phys_buf_unmap(struct ntc_dev *ntc, u64 addr, u64 size,
			       enum dma_data_direction dir)
{
	struct device *dev = ntc_map_dev(ntc);

	dma_unmap_single(dev, addr, size, dir);
}

static void ntc_phys_buf_sync_cpu(struct ntc_dev *ntc, u64 addr, u64 size,
				  enum dma_data_direction dir)
{
	struct device *dev = ntc_map_dev(ntc);

	dma_sync_single_for_cpu(dev, addr, size, dir);
}

static void ntc_phys_buf_sync_dev(struct ntc_dev *ntc, u64 addr, u64 size,
				  enum dma_data_direction dir)
{
	struct device *dev = ntc_map_dev(ntc);

	dma_sync_single_for_device(dev, addr, size, dir);
}

static void *ntc_phys_umem_get(struct ib_udata *udata,
			       unsigned long uaddr, size_t size,
			       int access, int dmasync)
{
	return ib_umem_get(udata, uaddr, size, access, dmasync);
}

static void ntc_phys_umem_put(struct ntc_dev *ntc, void *umem)
{
	ib_umem_release(umem);
}

static int ntc_phys_umem_sgl(struct ntc_dev *ntc, void *umem,
			     struct ntc_sge *sgl, int count)
{
	struct ib_umem *ibumem = umem;
	struct scatterlist *sg, *next;
	dma_addr_t dma_addr, next_addr;
	size_t dma_len;
	int i, dma_count = 0;

	BUILD_BUG_ON(sizeof(u64) < sizeof(dma_addr));
	BUILD_BUG_ON(sizeof(u64) < sizeof(dma_len));

	for_each_sg(ibumem->sg_head.sgl, sg, ibumem->sg_head.nents, i) {
		/* dma_addr is start addr of the contiguous range */
		dma_addr = sg_dma_address(sg);
		/* dma_len accumulates the length of the contiguous range */
		dma_len = sg_dma_len(sg);

		for (; i + 1 < ibumem->sg_head.nents; ++i) {
			next = sg_next(sg);
			if (!next)
				break;
			next_addr = sg_dma_address(next);
			if (next_addr != dma_addr + dma_len)
				break;
			dma_len += sg_dma_len(next);
			sg = next;
		}

		if (sgl && dma_count < count) {
			sgl[dma_count].addr = dma_addr;
			sgl[dma_count].len = dma_len;
		}

		++dma_count;
	}

	if (dma_count && sgl && count > 0) {
		/* dma_len is start offset in the first page */
		dma_len = ib_umem_offset(ibumem);
		sgl[0].addr += dma_len;
		sgl[0].len -= dma_len;

		if (dma_count <= count) {
			/* dma_len is offset from the end of the last page */
			dma_len = (dma_len + ibumem->length) & ~PAGE_MASK;
			dma_len = (PAGE_SIZE - dma_len) & ~PAGE_MASK;
			sgl[dma_count - 1].len -= dma_len;
		}
	}

	return dma_count;
}

struct ntc_map_ops ntc_phys_map_ops = {
	.buf_alloc			= ntc_phys_buf_alloc,
	.buf_free			= ntc_phys_buf_free,
	.buf_map			= ntc_phys_buf_map,
	.buf_unmap			= ntc_phys_buf_unmap,
	.buf_sync_cpu			= ntc_phys_buf_sync_cpu,
	.buf_sync_dev			= ntc_phys_buf_sync_dev,
	.umem_get			= ntc_phys_umem_get,
	.umem_put			= ntc_phys_umem_put,
	.umem_sgl			= ntc_phys_umem_sgl,
};
EXPORT_SYMBOL(ntc_phys_map_ops);
