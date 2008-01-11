/*
 * Paging, including guest phys to real phys translation.
 *
 * Copyright (C) 2007 Freescale Semiconductor, Inc.
 * Author: Scott Wood <scottwood@freescale.com>
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <paging.h>
#include <libos/fsl-booke-tlb.h>

// epn is already shifted by levels that the caller deals with.
static pte_t *vptbl_get_ptep(pte_t *tbl, int *levels, unsigned long epn,
                             int insert)
{
	while (--*levels >= 0) {
		int idx = (epn >> (PGDIR_SHIFT * *levels)) & (PGDIR_SIZE - 1);
		pte_t *ptep = &tbl[idx];
#if 0
		printf("pte %lx attr %lx epn %lx level %d\n", ptep->page, ptep->attr,
		       epn, *levels);
#endif
		if (!(ptep->attr & PTE_VALID)) {
			if (!insert)
				return NULL;

			if (*levels == 0)
				return ptep;

			tbl = alloc(PAGE_SIZE * 2, PAGE_SIZE * 2);
			assert(tbl);

			ptep->page = (unsigned long)tbl;
			ptep->attr = PTE_VALID;
		}

		if (ptep->attr & PTE_SIZE)
			return ptep;

		tbl = (pte_t *)ptep->page;
	}

	BUG();
}

unsigned long vptbl_xlate(pte_t *tbl, unsigned long epn,
                          unsigned long *attr, int level)
{
	pte_t *ptep = vptbl_get_ptep(tbl, &level, epn, 0);

	if (unlikely(!ptep)) {
		*attr = 0;
//		printf("2 vtable xlate %p 0x%lx %i\n", tbl, epn << PAGE_SHIFT, level);
		return (1UL << (PGDIR_SHIFT * level)) - 1;
	}

	pte_t pte = *ptep;
	unsigned int size = pte.attr >> PTE_SIZE_SHIFT;
	unsigned long size_pages;

//	printf("vtable xlate %p 0x%lx 0x%lx\n", tbl, epn << PAGE_SHIFT, pte.attr);

	size = pte.attr >> PTE_SIZE_SHIFT;

	if (level == 0) {
		assert(size < TLB_TSIZE_4M);
	} else {
		assert(level == 1);
		assert(size >= TLB_TSIZE_4M);
	}

	*attr = pte.attr;

	if (unlikely(!(pte.attr & PTE_VALID)))
		return (1UL << (PGDIR_SHIFT * level)) - 1;
	
	size_pages = tsize_to_pages(size);
	return (pte.page & ~(size_pages - 1)) | (epn & (size_pages - 1));
}

// Large mappings are a latency source -- this should only be done
// at initialization, when processing the device tree.

void vptbl_map(pte_t *tbl, unsigned long epn, unsigned long rpn,
               unsigned long npages, unsigned long attr, int levels)
{
	unsigned long end = epn + npages;

	while (epn < end) {
		unsigned int size = min(max_page_size(epn, end - epn),
		                        natural_alignment(rpn));
		unsigned long size_pages = tsize_to_pages(size);
		unsigned long sub_end = epn + size_pages;

		assert(size_pages <= end - epn);
		attr = (attr & ~PTE_SIZE) | (size << PTE_SIZE_SHIFT);

#if 0
		printf("max_page_size(epn, end - epn) %u\n", max_page_size(epn, end - epn));

		printf("epn %lx rpn %lx end %lx size %u size_pages %lx sub_end %lx\n", epn, rpn, end,
		       size, size_pages, sub_end);
#endif

		assert(size > 0);
		int largepage = size >= TLB_TSIZE_4M;
		int incr = largepage ? PGDIR_SIZE - 1 : 0;
		
		while (epn < sub_end) {
			int level = levels - largepage;
			pte_t *ptep = vptbl_get_ptep(tbl, &level,
			                             epn >> (PGDIR_SHIFT * largepage),
			                             1);

			if (largepage && ptep->page && !(ptep->attr & PTE_SIZE)) {
				printf("vptbl_map: Tried to overwrite a small page with "
				       "a large page at %llx\n",
				       ((uint64_t )epn) << PAGE_SHIFT);
				return;
			}

			if (!largepage && level != 0) {
				printf("vptbl_map: Tried to overwrite a large page with "
				       "a small page at %llx\n",
				       ((uint64_t )epn) << PAGE_SHIFT);
				return;
			}

			ptep->page = rpn;
			ptep->attr = attr;
			
//			printf("epn %lx: setting rpn %lx attr %lx\n", epn, rpn, attr);

			epn = (epn | incr) + 1;
			rpn = (rpn | incr) + 1;
		}
	}
}
