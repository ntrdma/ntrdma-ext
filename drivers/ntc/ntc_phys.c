#include <linux/init.h>
#include <linux/module.h>
#include <linux/dma-mapping.h>
#include <rdma/ib_umem.h>
#include <linux/slab.h>
#include <linux/ntc.h>

#define DRIVER_NAME "ntc_phys"
#define DRIVER_VERSION  "0.2"
#define DRIVER_RELDATE  "30 September 2015"

MODULE_AUTHOR("Allen Hubbe");
MODULE_DESCRIPTION("NTC physical channel-mapped buffer support library");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_VERSION(DRIVER_VERSION);

static int ntc_phys_local_buf_alloc(struct ntc_local_buf *buf, gfp_t gfp)
{
	struct device *dev = buf->dma_engine_dev;

	buf->ptr = kmalloc_node(buf->size, gfp, dev_to_node(dev));
	if (!buf->ptr)
		return -ENOMEM;

	return 0;
}

static int ntc_phys_local_buf_map(struct ntc_local_buf *buf)
{
	struct device *dev = buf->dma_engine_dev;

	buf->dma_addr = dma_map_single(dev, buf->ptr, buf->size,
				DMA_TO_DEVICE);
	if (dma_mapping_error(dev, buf->dma_addr))
		return -EIO;

	return 0;
}

static void ntc_phys_local_buf_unmap(struct ntc_local_buf *buf)
{
	dma_unmap_single(buf->dma_engine_dev, buf->dma_addr, buf->size,
			DMA_TO_DEVICE);
}

static void ntc_phys_local_buf_free(struct ntc_local_buf *buf)
{
	if (buf->owned && buf->ptr)
		kfree(buf->ptr);
}

static void ntc_phys_local_buf_prepare_to_copy(const struct ntc_local_buf *buf,
					u64 offset, u64 len)
{
	dma_sync_single_for_device(buf->dma_engine_dev, buf->dma_addr + offset,
				len, DMA_TO_DEVICE);
}

static int ntc_phys_export_buf_alloc(struct ntc_export_buf *buf)
{
	struct device *dev = buf->ntb_dev;
	dma_addr_t dma_addr;

	if (buf->gfp)
		buf->ptr = kmalloc_node(buf->size, buf->gfp,
					dev_to_node(dev));
	if (!buf->ptr)
		return -ENOMEM;

	dma_addr = dma_map_single(dev, buf->ptr, buf->size, DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, dma_addr)) {
		if (buf->gfp)
			kfree(buf->ptr);
		return -EIO;
	}

	buf->chan_addr.mw_desc = NTC_CHAN_MW1;
	buf->chan_addr.offset = dma_addr;

	return 0;
}

static void ntc_phys_export_buf_free(struct ntc_export_buf *buf)
{
	struct device *dev = buf->ntb_dev;
	dma_addr_t dma_addr;

	if (buf->chan_addr.mw_desc == NTC_CHAN_MW1) {
		dma_addr = buf->chan_addr.offset;
		dma_unmap_single(dev, dma_addr, buf->size, DMA_FROM_DEVICE);
	}

	if (buf->gfp && buf->ptr)
		kfree(buf->ptr);
}

static const void *ntc_phys_export_buf_const_deref(struct ntc_export_buf *buf,
						u64 offset, u64 len)
{
	struct device *dev = buf->ntb_dev;
	dma_addr_t dma_addr;

	if (buf->chan_addr.mw_desc == NTC_CHAN_MW1) {
		dma_addr = buf->chan_addr.offset;
		dma_sync_single_for_cpu(dev, dma_addr, len, DMA_FROM_DEVICE);
	}

	return buf->ptr + offset;
}

static int ntc_phys_bidir_buf_alloc(struct ntc_bidir_buf *buf)
{
	struct device *ntb_dev = buf->ntb_dev;
	struct device *dma_engine_dev = buf->dma_engine_dev;
	dma_addr_t ntb_dma_addr;
	dma_addr_t dma_engine_addr;
	int rc;

	if (buf->gfp)
		buf->ptr = kmalloc_node(buf->size, buf->gfp,
					dev_to_node(ntb_dev));
	if (!buf->ptr)
		return -ENOMEM;

	ntb_dma_addr =
		dma_map_single(ntb_dev, buf->ptr, buf->size, DMA_FROM_DEVICE);
	if (dma_mapping_error(ntb_dev, ntb_dma_addr)) {
		rc = -EIO;
		goto err_ntb_map;
	}

	dma_engine_addr = dma_map_single(dma_engine_dev, buf->ptr, buf->size,
					DMA_TO_DEVICE);
	if (dma_mapping_error(dma_engine_dev, dma_engine_addr)) {
		rc = -EIO;
		goto err_dma_engine_map;
	}

	buf->chan_addr.mw_desc = NTC_CHAN_MW1;
	buf->chan_addr.offset = ntb_dma_addr;
	buf->dma_addr = dma_engine_addr;

	return 0;

 err_dma_engine_map:
	dma_unmap_single(ntb_dev, ntb_dma_addr, buf->size, DMA_FROM_DEVICE);

 err_ntb_map:
	if (buf->gfp)
		kfree(buf->ptr);

	return rc;
}

static void ntc_phys_bidir_buf_free(struct ntc_bidir_buf *buf)
{
	struct device *ntb_dev = buf->ntb_dev;
	struct device *dma_engine_dev = buf->dma_engine_dev;
	dma_addr_t ntb_dma_addr;
	dma_addr_t dma_engine_addr = buf->dma_addr;

	if (buf->chan_addr.mw_desc == NTC_CHAN_MW1) {
		ntb_dma_addr = buf->chan_addr.offset;
		dma_unmap_single(ntb_dev, ntb_dma_addr, buf->size,
				DMA_FROM_DEVICE);
	}
	dma_unmap_single(dma_engine_dev, dma_engine_addr, buf->size,
			DMA_TO_DEVICE);

	if (buf->gfp && buf->ptr)
		kfree(buf->ptr);
}

static void *ntc_phys_bidir_buf_deref(struct ntc_bidir_buf *buf,
				u64 offset, u64 len)
{
	struct device *ntb_dev = buf->ntb_dev;
	dma_addr_t ntb_dma_addr;

	if (buf->chan_addr.mw_desc == NTC_CHAN_MW1) {
		ntb_dma_addr = buf->chan_addr.offset;
		dma_sync_single_for_cpu(ntb_dev, ntb_dma_addr, len,
					DMA_FROM_DEVICE);
	}

	return buf->ptr + offset;
}

static void ntc_phys_bidir_buf_unref(struct ntc_bidir_buf *buf,
				u64 offset, u64 len)
{
	struct device *ntb_dev = buf->ntb_dev;
	dma_addr_t ntb_dma_addr;

	if (buf->chan_addr.mw_desc == NTC_CHAN_MW1) {
		ntb_dma_addr = buf->chan_addr.offset;
		dma_sync_single_for_device(ntb_dev, ntb_dma_addr, len,
					DMA_FROM_DEVICE);
	}
}

static const void *ntc_phys_bidir_buf_const_deref(struct ntc_bidir_buf *buf,
						u64 offset, u64 len)
{
	struct device *ntb_dev = buf->ntb_dev;
	dma_addr_t ntb_dma_addr;

	if (buf->chan_addr.mw_desc == NTC_CHAN_MW1) {
		ntb_dma_addr = buf->chan_addr.offset;
		dma_sync_single_for_cpu(ntb_dev, ntb_dma_addr, len,
					DMA_FROM_DEVICE);
	}

	return buf->ptr + offset;
}

static void ntc_phys_bidir_buf_prepare_to_copy(const struct ntc_bidir_buf *buf,
					u64 offset, u64 len)
{
	dma_sync_single_for_device(buf->dma_engine_dev, buf->dma_addr + offset,
				len, DMA_TO_DEVICE);
}

static int ntc_phys_remote_buf_map(struct ntc_remote_buf *buf,
				const struct ntc_remote_buf_desc *desc)
{
	struct device *dev = buf->dma_engine_dev;

	buf->ptr = ntc_peer_addr(buf->ntc, &desc->chan_addr);
	if (!buf->ptr)
		return -EIO;

	buf->dma_addr = dma_map_resource(dev, buf->ptr, desc->size,
					DMA_FROM_DEVICE, 0);

	if (dma_mapping_error(dev, buf->dma_addr))
		return -EIO;

	buf->size = desc->size;

	return 0;
}

static int ntc_phys_remote_buf_map_phys(struct ntc_remote_buf *buf,
					phys_addr_t ptr, u64 size)
{
	struct device *dev = buf->dma_engine_dev;

	if (!ptr)
		return -EIO;

	buf->ptr = ptr;
	buf->dma_addr = dma_map_resource(dev, ptr, size, DMA_FROM_DEVICE, 0);

	if (dma_mapping_error(dev, buf->dma_addr))
		return -EIO;

	buf->size = size;

	return 0;
}

static void ntc_phys_remote_buf_unmap(struct ntc_remote_buf *buf)
{
	struct device *dev = buf->dma_engine_dev;

	dma_unmap_resource(dev, buf->dma_addr, buf->size, DMA_FROM_DEVICE, 0);
}

struct ntc_map_ops ntc_phys_map_ops = {
	.local_buf_alloc		= ntc_phys_local_buf_alloc,
	.local_buf_free			= ntc_phys_local_buf_free,
	.local_buf_map			= ntc_phys_local_buf_map,
	.local_buf_unmap		= ntc_phys_local_buf_unmap,
	.local_buf_prepare_to_copy	= ntc_phys_local_buf_prepare_to_copy,
	.export_buf_alloc		= ntc_phys_export_buf_alloc,
	.export_buf_free		= ntc_phys_export_buf_free,
	.export_buf_const_deref		= ntc_phys_export_buf_const_deref,
	.bidir_buf_alloc		= ntc_phys_bidir_buf_alloc,
	.bidir_buf_free			= ntc_phys_bidir_buf_free,
	.bidir_buf_deref		= ntc_phys_bidir_buf_deref,
	.bidir_buf_unref		= ntc_phys_bidir_buf_unref,
	.bidir_buf_const_deref		= ntc_phys_bidir_buf_const_deref,
	.bidir_buf_prepare_to_copy	= ntc_phys_bidir_buf_prepare_to_copy,
	.remote_buf_map			= ntc_phys_remote_buf_map,
	.remote_buf_map_phys		= ntc_phys_remote_buf_map_phys,
	.remote_buf_unmap		= ntc_phys_remote_buf_unmap,
};
EXPORT_SYMBOL(ntc_phys_map_ops);
