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

static int ntc_virt_local_buf_alloc(struct ntc_local_buf *buf, gfp_t gfp)
{
	buf->ptr = kmalloc_node(buf->size, gfp, dev_to_node(&buf->ntc->dev));
	if (!buf->ptr)
		return -ENOMEM;

	return 0;
}

static int ntc_virt_local_buf_map(struct ntc_local_buf *buf)
{
	buf->dma_addr = (typeof(buf->dma_addr))buf->ptr;

	return 0;
}

static void ntc_virt_local_buf_free(struct ntc_local_buf *buf)
{
	rmb(); /* read data in the order it is received out of the channel */

	if (buf->owned && buf->ptr)
		kfree(buf->ptr);
}

static int ntc_virt_export_buf_alloc(struct ntc_export_buf *buf)
{
	if (buf->gfp)
		buf->ptr = kmalloc_node(buf->size, buf->gfp,
					dev_to_node(&buf->ntc->dev));
	if (!buf->ptr)
		return -ENOMEM;

	buf->chan_addr = (typeof(buf->chan_addr))buf->ptr;

	return 0;
}

static void ntc_virt_export_buf_free(struct ntc_export_buf *buf)
{
	rmb(); /* read data in the order it is received out of the channel */

	if (buf->gfp && buf->ptr)
		kfree(buf->ptr);
}

static int ntc_virt_bidir_buf_alloc(struct ntc_bidir_buf *buf)
{
	if (buf->gfp)
		buf->ptr = kmalloc_node(buf->size, buf->gfp,
					dev_to_node(&buf->ntc->dev));
	if (!buf->ptr)
		return -ENOMEM;

	buf->chan_addr = (typeof(buf->chan_addr))buf->ptr;
	buf->dma_addr = (typeof(buf->dma_addr))buf->ptr;

	return 0;
}

static void ntc_virt_bidir_buf_free(struct ntc_bidir_buf *buf)
{
	rmb(); /* read data in the order it is received out of the channel */

	if (buf->gfp && buf->ptr)
		kfree(buf->ptr);
}

static int ntc_virt_remote_buf_map(struct ntc_remote_buf *buf,
				const struct ntc_remote_buf_desc *desc)
{
	buf->ptr = ntc_peer_addr(buf->ntc, desc->chan_addr);
	if (!buf->ptr)
		return -EIO;

	buf->dma_addr = buf->ptr;
	buf->size = desc->size;

	return 0;
}

static int ntc_virt_remote_buf_map_phys(struct ntc_remote_buf *buf,
					phys_addr_t ptr, u64 size)
{
	if (!ptr)
		return -EIO;

	buf->ptr = ptr;
	buf->dma_addr = ptr;
	buf->size = size;

	return 0;
}

static void ntc_virt_remote_buf_unmap(struct ntc_remote_buf *buf)
{
	rmb(); /* read data in the order it is received out of the channel */
}

struct ntc_map_ops ntc_virt_map_ops = {
	.local_buf_alloc	= ntc_virt_local_buf_alloc,
	.local_buf_free		= ntc_virt_local_buf_free,
	.local_buf_map		= ntc_virt_local_buf_map,
	.export_buf_alloc	= ntc_virt_export_buf_alloc,
	.export_buf_free	= ntc_virt_export_buf_free,
	.bidir_buf_alloc	= ntc_virt_bidir_buf_alloc,
	.bidir_buf_free		= ntc_virt_bidir_buf_free,
	.remote_buf_map		= ntc_virt_remote_buf_map,
	.remote_buf_map_phys	= ntc_virt_remote_buf_map_phys,
	.remote_buf_unmap	= ntc_virt_remote_buf_unmap,
};
EXPORT_SYMBOL(ntc_virt_map_ops);
