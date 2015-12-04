#include <linux/init.h>
#include <linux/module.h>

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <rdma/ib_verbs.h>

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
	/* nothing to do */
}

struct ntc_vmem {
	void *buf;
	size_t size;
	void *kvaddr;
	unsigned long npages;
	struct page *pages[];
};

static void *ntc_virt_umem_get(struct ntc_dev *ntc, struct ib_ucontext *uctx,
			       unsigned long uaddr, size_t size,
			       int access, int dmasync)
{
	struct ntc_vmem *vmem;
	unsigned long npages, locked, lock_limit;
	int readonly, rc, i;

	if (!size)
		return ERR_PTR(-EINVAL);

	if (uaddr + size < uaddr || PAGE_ALIGN(uaddr + size) < uaddr + size)
		return ERR_PTR(-EINVAL);

	if (!can_do_mlock())
		return ERR_PTR(-EPERM);

	npages = 1 + ((uaddr + size) >> PAGE_SHIFT) - (uaddr >> PAGE_SHIFT);

	readonly = !(access &
		     (IB_ACCESS_LOCAL_WRITE   | IB_ACCESS_REMOTE_WRITE |
		      IB_ACCESS_REMOTE_ATOMIC | IB_ACCESS_MW_BIND));

	vmem = kmalloc_node(sizeof(*vmem) + npages * sizeof(*vmem->pages),
			    GFP_KERNEL, dev_to_node(&ntc->dev));
	if (!vmem)
		return ERR_PTR(-ENOMEM);

	vmem->npages = npages;

	down_write(&current->mm->mmap_sem);

	locked     = npages + current->mm->pinned_vm;
	lock_limit = rlimit(RLIMIT_MEMLOCK) >> PAGE_SHIFT;

	if ((locked > lock_limit) && !capable(CAP_IPC_LOCK)) {
		rc = -ENOMEM;
		goto err_pages;
	}

	rc = get_user_pages(current, current->mm, uaddr & PAGE_MASK,
			     npages, 1, readonly, vmem->pages, NULL);
	up_write(&current->mm->mmap_sem);
	if (rc < 0)
		goto err_pages;

	vmem->kvaddr = vmap(vmem->pages, npages, VM_MAP, PAGE_KERNEL);
	if (!vmem->kvaddr) {
		rc = -ENOMEM;
		goto err_vmap;
	}

	vmem->buf = vmem->kvaddr + (uaddr & ~PAGE_MASK);
	vmem->size = size;

	return vmem;

err_vmap:
	for (i = 0; i < npages; ++i)
		put_page(vmem->pages[i]);
err_pages:
	kfree(vmem);
	return ERR_PTR(rc);
}

static void ntc_virt_umem_put(struct ntc_dev *ntc, void *umem)
{
	struct ntc_vmem *vmem = umem;
	int i, npages = vmem->npages;

	vunmap(vmem->kvaddr);
	for (i = 0; i < npages; ++i)
		put_page(vmem->pages[i]);
	kfree(vmem);
}

static int ntc_virt_umem_sgl(struct ntc_dev *ntc, void *umem,
			     struct ntc_sge *sgl, int count)
{
	struct ntc_vmem *vmem = umem;

	if (count >= 1) {
		/* virtual pointer and size must fit in the sg fields */
		BUILD_BUG_ON(sizeof(u64) < sizeof(vmem->buf));
		BUILD_BUG_ON(sizeof(u64) < sizeof(vmem->size));

		sgl[0].addr = (u64)vmem->buf;
		sgl[0].len = (u64)vmem->size;
	}

	return 1;
}

static int ntc_virt_umem_count(struct ntc_dev *ntc, void *umem)
{
	return 1;
}

struct ntc_map_ops ntc_virt_map_ops = {
	.buf_alloc			= ntc_virt_buf_alloc,
	.buf_free			= ntc_virt_buf_free,
	.buf_map			= ntc_virt_buf_map,
	.buf_unmap			= ntc_virt_buf_unmap,
	.umem_get			= ntc_virt_umem_get,
	.umem_put			= ntc_virt_umem_put,
	.umem_sgl			= ntc_virt_umem_sgl,
	.umem_count			= ntc_virt_umem_count,
};
EXPORT_SYMBOL(ntc_virt_map_ops);
