/*
 * Copyright (c) 2019 Dell Technologies, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree.
 */

#ifndef _NTC_PFN_H_
#define _NTC_PFN_H_

#include <linux/mm.h>
#include <linux/hugetlb_inline.h>
#include <linux/spinlock.h>
#include <linux/sched.h>

/*
 * Return the PFN for the specified address in the vma. This only
 * works for a vma that is VM_PFNMAP, VM_IO and not hugetlb.
 */
static inline unsigned long ntc_follow_io_pfn(struct vm_area_struct *vma,
					unsigned long address, int write)
{
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep, pte;
	spinlock_t *ptl;
	unsigned long pfn;
	struct mm_struct *mm = vma->vm_mm;

	if (!(vma->vm_flags & VM_PFNMAP) ||
		!(vma->vm_flags & VM_IO) ||
		is_vm_hugetlb_page(vma))
		return 0;

	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd) || unlikely(pgd_bad(*pgd)))
		return 0;

	p4d = p4d_offset(pgd, address);
	if (p4d_none(*p4d) || unlikely(p4d_bad(*p4d)))
		return 0;

	pud = pud_offset(p4d, address);
	if (pud_none(*pud))
		return 0;
	if (unlikely(pud_bad(*pud)))
		return 0;

	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd))
		return 0;
	if (unlikely(pmd_bad(*pmd)))
		return 0;

	ptep = pte_offset_map_lock(mm, pmd, address, &ptl);
	pte = *ptep;
	if (!pte_present(pte))
		goto bad;
	if (write && !pte_write(pte))
		goto bad;

	pfn = pte_pfn(pte);
	pte_unmap_unlock(ptep, ptl);
	return pfn;
 bad:
	pte_unmap_unlock(ptep, ptl);
	return 0;
}

static inline int ntc_get_io_pfn_segment_locked(struct mm_struct *mm,
						unsigned long addr,
						unsigned long len,
						int write,
						unsigned long *dma_out,
						unsigned long *len_out)
{
	unsigned long end = addr + len;
	unsigned long start = addr & PAGE_MASK;
	unsigned long shift = addr & ~PAGE_MASK;
	unsigned long pfn;
	unsigned long start_pfn;
	struct vm_area_struct *vma;

	if (unlikely(end < addr))
		return -EINVAL;

	vma = find_vma(mm, start);
	if (unlikely(!vma) ||
		(start < vma->vm_start) ||
		unlikely(start >= vma->vm_end))
		return -EINVAL;

	pfn = ntc_follow_io_pfn(vma, start, write);
	if (!pfn)
		return -EINVAL;

	start_pfn = pfn;

	for (start = vma->vm_end; end > start; start = vma->vm_end) {
		cond_resched();

		vma = find_vma(mm, start);
		if (unlikely(!vma) ||
			(start < vma->vm_start) ||
			unlikely(start >= vma->vm_end))
			goto partial;

		pfn = ntc_follow_io_pfn(vma, start, write);
		if (unlikely((pfn != start_pfn + ((start - addr) >> PAGE_SHIFT))
				|| !pfn))
			goto partial;
	}

	*dma_out = (start_pfn << PAGE_SHIFT) + shift;
	*len_out = len;

	return 0;

 partial:
	*dma_out = (start_pfn << PAGE_SHIFT) + shift;
	*len_out = start - addr;

	return 0;
}

static inline int ntc_get_io_pfn_segment(struct mm_struct *mm,
					unsigned long addr,
					unsigned long len,
					int write,
					unsigned long *pfn_out,
					unsigned long *len_out)
{
	int rc;

	down_read(&mm->mmap_sem);

	rc = ntc_get_io_pfn_segment_locked(mm, addr, len, write,
					pfn_out, len_out);

	up_read(&mm->mmap_sem);

	return rc;
}

#endif
