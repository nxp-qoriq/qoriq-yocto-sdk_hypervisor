/*
 * Instruction emulation
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
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

#include <uv.h>
#include <frame.h>
#include <console.h>
#include <guestmemio.h>
#include <percpu.h>
#include <trap_booke.h>
#include <paging.h>

static int get_ea_indexed(trapframe_t *regs, uint32_t insn)
{
	int ra = (insn >> 16) & 31;
	int rb = (insn >> 11) & 31;
	
	return regs->gpregs[rb] + (ra ? regs->gpregs[ra] : 0);
}

static int emu_tlbivax(trapframe_t *regs, uint32_t insn)
{
	uint32_t va = get_ea_indexed(regs, insn);
	
	if (va & TLBIVAX_RESERVED) {
		printf("tlbivax@0x%08x: reserved bits in EA: 0x%08x\n", regs->srr0, va);
		return 1;
	}

	asm volatile("tlbivax 0, %0" : : "r" (va) : "memory");
	return 0;
}

static int emu_tlbsync(trapframe_t *regs, uint32_t insn)
{
	// FIXME: add lock so only one can be executing at a time
	asm volatile("tlbsync" : : : "memory");
	return 0;
}

static void fixup_tlb_sx_re(void)
{
	if (!(mfspr(SPR_MAS1) & MAS1_VALID))
		return;

	unsigned long mas3 = mfspr(SPR_MAS3);
	unsigned long mas7 = mfspr(SPR_MAS7);

	unsigned long grpn = (mas7 << (32 - PAGE_SHIFT)) |
	                     (mas3 >> MAS3_RPN_SHIFT);

//	printf("sx_re: mas0 %x mas1 %x mas3 %lx mas7 %lx grpn %lx\n",
//	       mfspr(SPR_MAS0), mfspr(SPR_MAS1), mas3, mas7, grpn);

	unsigned long attr;
	unsigned long rpn = vptbl_xlate(hcpu->gcpu->guest->gphys_rev,
	                                grpn, &attr);

	assert(attr & PTE_VALID);

	mtspr(SPR_MAS3, (rpn << PAGE_SHIFT) |
	                (mas3 & (MAS3_FLAGS | MAS3_USER)));
	mtspr(SPR_MAS7, rpn >> (32 - PAGE_SHIFT));

	uint32_t mas0 = mfspr(SPR_MAS0);

	if (MAS0_GET_TLBSEL(mas0) == 1) {
		gcpu_t *gcpu = hcpu->gcpu;
		unsigned int entry = MAS0_GET_TLB1ESEL(mas0);
		mas0 &= ~MAS0_ESEL_MASK;
		mas0 |= MAS0_ESEL(guest_tlb1_to_gtlb1(entry));

		mtspr(SPR_MAS0, mas0);
		mtspr(SPR_MAS1, gcpu->gtlb1[entry].mas1);
		mtspr(SPR_MAS2, gcpu->gtlb1[entry].mas2);
		mtspr(SPR_MAS3, gcpu->gtlb1[entry].mas3);
		mtspr(SPR_MAS7, gcpu->gtlb1[entry].mas7);
	}
}

static int emu_tlbsx(trapframe_t *regs, uint32_t insn)
{
	uint32_t va = get_ea_indexed(regs, insn);

	mtspr(SPR_MAS5, MAS5_SGS | mfspr(SPR_LPID));
	asm volatile("tlbsx 0, %0" : : "r" (va) : "memory");
	fixup_tlb_sx_re();
	return 0;
}

static int emu_tlbre(trapframe_t *regs, uint32_t insn)
{
	gcpu_t *gcpu = hcpu->gcpu;
	uint32_t mas0 = mfspr(SPR_MAS0);
	unsigned int entry, tlb;
	
	if (mas0 & (MAS0_RESERVED | 0x20000000)) {
		printf("tlbre@0x%08x: reserved bits in MAS0: 0x%08x\n", regs->srr0, mas0);
		return 1;
	}

	tlb = MAS0_GET_TLBSEL(mas0);
	if (tlb == 0) {
		asm volatile("tlbre" : : : "memory");
		fixup_tlb_sx_re();
		return 0;
	}

	entry = MAS0_GET_TLB1ESEL(mas0);
	if (entry >= TLB1_GSIZE) {
		printf("tlbre@0x%08x: attempt to read TLB1 entry %d (max %d)\n",
		       regs->srr0, entry, TLB1_GSIZE);
		return 1;
	}

	mtspr(SPR_MAS1, gcpu->gtlb1[entry].mas1);
	mtspr(SPR_MAS2, gcpu->gtlb1[entry].mas2);
	mtspr(SPR_MAS3, gcpu->gtlb1[entry].mas3);
	mtspr(SPR_MAS7, gcpu->gtlb1[entry].mas7);
	return 0;
}

static int emu_tlbwe(trapframe_t *regs, uint32_t insn)
{
	guest_t *guest = hcpu->gcpu->guest;
	unsigned long mas0 = mfspr(SPR_MAS0);
	unsigned long mas1 = mfspr(SPR_MAS1);
	unsigned long mas2 = mfspr(SPR_MAS2);
	unsigned long mas3 = mfspr(SPR_MAS3);
	unsigned long mas7 = mfspr(SPR_MAS7);
	unsigned long grpn = (mas7 << (32 - PAGE_SHIFT)) |
	                     (mas3 >> MAS3_RPN_SHIFT);

	if (mas0 & (MAS0_RESERVED | 0x20000000)) {
		printf("tlbwe@0x%x: reserved bits in MAS0: 0x%lx\n", regs->srr0, mas0);
		return 1;
	}

	if (mas1 & MAS1_RESERVED) {
		printf("tlbwe@0x%x: reserved bits in MAS1: 0x%lx\n", regs->srr0, mas0);
		return 1;
	}

	if (mas2 & MAS2_RESERVED) {
		printf("tlbwe@0x%x: reserved bits in MAS2: 0x%lx\n", regs->srr0, mas0);
		return 1;
	}

	if (mas3 & MAS3_RESERVED) {
		printf("tlbwe@0x%x: reserved bits in MAS3: 0x%lx\n", regs->srr0, mas0);
		return 1;
	}

	if (mas7 & MAS7_RESERVED) {
		printf("tlbwe@0x%x: reserved bits in MAS7: 0x%lx\n", regs->srr0, mas0);
		return 1;
	}

	/* If an entry for this address exists elsewhere in any
	 * TLB, don't let the guest create undefined behavior by
	 * adding a duplicate entry.
	 */
	if (mas1 & MAS1_VALID) {
		mtspr(SPR_MAS5, MAS5_SGS | guest->lpid);
		mtspr(SPR_MAS6, (mas1 & MAS1_TID_MASK) |
		                ((mas1 >> MAS1_TS_SHIFT) & 1));
		asm volatile("tlbsx 0, %0" : : "r" (mas2 & MAS2_EPN) : "memory");

		unsigned long sx_mas0 = mfspr(SPR_MAS0);

		if (likely(!(mfspr(SPR_MAS1) & MAS1_VALID)))
			goto no_dup;

		if (MAS0_GET_TLBSEL(mas0) == 0 && 
		    (mas0 & (MAS0_TLBSEL_MASK | MAS0_ESEL_MASK)) ==
		    (sx_mas0 & (MAS0_TLBSEL_MASK | MAS0_ESEL_MASK)))
			goto no_dup;

		if (MAS0_GET_TLBSEL(mas0) == 1 && MAS0_GET_TLBSEL(sx_mas0) == 1 &&
		    MAS0_GET_TLB1ESEL(sx_mas0) ==
		    guest_tlb1_to_gtlb1(MAS0_GET_TLB1ESEL(mas0)))
			goto no_dup;

		printf("tlbwe@0x%x: duplicate entry for vaddr 0x%lx\n",
		       regs->srr0, mas2 & MAS2_EPN);

		return 1;
	}

no_dup:	
	if (mas0 & MAS0_TLBSEL1) {
		unsigned long epn = mas2 >> PAGE_SHIFT;
		int entry = MAS0_GET_TLB1ESEL(mas0);

		if (entry >= TLB1_GSIZE) {
			printf("tlbwe@0x%x: attempt to write TLB1 entry %d (max %d)\n",
			       regs->srr0, entry, TLB1_GSIZE);
			return 1;
		}
		
		guest_set_tlb1(entry, mas1, epn, grpn, mas2 & MAS2_FLAGS,
		               mas3 & (MAS3_FLAGS | MAS3_USER));
	} else {
		unsigned long rpn;
		unsigned long mas8 = guest->lpid | MAS8_GTS;
	
		mtspr(SPR_MAS0, mas0);
		mtspr(SPR_MAS1, mas1);
		mtspr(SPR_MAS2, mas2);
		mtspr(SPR_MAS3, mas3);
		mtspr(SPR_MAS7, mas7);

		if (likely(mas1 & MAS1_VALID)) {
			unsigned long attr;
			unsigned long rpn = vptbl_xlate(guest->gphys, grpn, &attr);


			// If there's no valid mapping, request a virtualization
			// fault, so a machine check can be reflected upon use.
			if (unlikely(!(attr & PTE_VALID))) {
				printf("tlbwe@0x%x: Invalid gphys %llx for va %lx\n",
				       regs->srr0,
				       ((uint64_t)grpn) << PAGE_SHIFT,
				       mas2 & MAS2_EPN);
				mas8 |= MAS8_VF;
			} else {
				mas3 &= (attr & PTE_MAS3_MASK) | MAS3_USER;
				mas8 |= (attr << PTE_MAS8_SHIFT) & PTE_MAS8_MASK;
			}

			mtspr(SPR_MAS7, rpn >> (32 - PAGE_SHIFT));
			mtspr(SPR_MAS3, (rpn << PAGE_SHIFT) | mas3);
		} else {
			mtspr(SPR_MAS3, mas3);
			mtspr(SPR_MAS7, mas7);
		}

		mtspr(SPR_MAS8, mas8);
		asm volatile("tlbwe" : : : "memory");
	}

	return 0;
}

static int get_spr(uint32_t insn)
{
	return ((insn >> 6) & 0x3e0) |
	       ((insn >> 16) & 0x1f);
}

static int emu_mfspr(trapframe_t *regs, uint32_t insn)
{
	int spr = get_spr(insn);
	int reg = (insn >> 21) & 31;
	uint32_t ret;
	
	switch (spr) {
	case SPR_TLB1CFG:
		ret = mfspr(SPR_TLB1CFG) & ~TLBCFG_NENTRY_MASK;
		ret |= TLB1_GSIZE;
		break;

	case SPR_IVPR:
		ret = hcpu->gcpu->ivpr;
		break;

	case SPR_IVOR0...SPR_IVOR15:
		ret = hcpu->gcpu->ivor[spr - SPR_IVOR0];
		break;

	case SPR_IVOR32...SPR_IVOR37:
		ret = hcpu->gcpu->ivor[spr - SPR_IVOR32 + 32];
		break;

	case SPR_TSR:
		ret = mfspr(SPR_TSR);
		break;

	case SPR_TCR:
		ret = mfspr(SPR_TCR);
		break;

	case SPR_DEC:
		ret = mfspr(SPR_DEC);
		break;

	default:
		printf("mfspr@0x%x: unknown reg %d\n", regs->srr0, spr);
		ret = 0;
//		return 1;
	}
	
	regs->gpregs[reg] = ret;
	return 0;
}

static int emu_mtspr(trapframe_t *regs, uint32_t insn)
{
	int spr = get_spr(insn);
	int reg = (insn >> 21) & 31;
	register_t val = regs->gpregs[reg];

	switch (spr) {
	case SPR_IVPR:
		val &= IVPR_MASK;
		mtspr(SPR_GIVPR, val);
		hcpu->gcpu->ivpr = val;
		break;
	
	case SPR_IVOR2:
		mtspr(SPR_GIVOR2, val);
		goto low_ivor;
		
	case SPR_IVOR3:
		mtspr(SPR_GIVOR3, val);
		goto low_ivor;
		
	case SPR_IVOR4:
		mtspr(SPR_GIVOR4, val);
		goto low_ivor;
		
	case SPR_IVOR8:
		mtspr(SPR_GIVOR8, val);
		goto low_ivor;
		
	case SPR_IVOR13:
		mtspr(SPR_GIVOR13, val);
		goto low_ivor;
		
	case SPR_IVOR14:
		mtspr(SPR_GIVOR14, val);
		goto low_ivor;
		
	case SPR_IVOR0...SPR_IVOR1:
	case SPR_IVOR5...SPR_IVOR7:
	case SPR_IVOR9...SPR_IVOR12:
	case SPR_IVOR15:
	low_ivor:
		hcpu->gcpu->ivor[spr - SPR_IVOR0] = val & IVOR_MASK;
		break;

	case SPR_IVOR32...SPR_IVOR37:
		hcpu->gcpu->ivor[spr - SPR_IVOR32 + 32] = val & IVOR_MASK;
		break;

	case SPR_TSR:
		mtspr(SPR_TSR, val);
		break;

	case SPR_TCR:
		mtspr(SPR_TCR, val);
		break;

	case SPR_DEC:
		mtspr(SPR_DEC, val);
		break;

	default:
		printf("mtspr@0x%x: unknown reg %d, val 0x%x\n",
		       regs->srr0, spr, val);
//		return 1;
	}
	
	return 0;
}

void hvpriv(trapframe_t *regs)
{
	uint32_t insn, op;
	int fault = 1;

	guestmem_set_insn(regs);
//	printf("hvpriv trap from 0x%lx, srr1 0x%lx, eplc 0x%lx\n", regs->srr0,
//	       regs->srr1, mfspr(SPR_EPLC));
	insn = guestmem_in32((uint32_t *)regs->srr0);
	
	if (((insn >> 26) & 0x3f) != 0x1f)
		goto bad;
	
	op = (insn >> 1) & 0x3ff;

	switch (op) {
	case 0x312:
		fault = emu_tlbivax(regs, insn);
		break;
	
	case 0x3b2:
		fault = emu_tlbre(regs, insn);
		break;

	case 0x392:
		fault = emu_tlbsx(regs, insn);
		break;

	case 0x236:
		fault = emu_tlbsync(regs, insn);
		break;

	case 0x3d2:
		fault = emu_tlbwe(regs, insn);
		break;

	case 0x153:
		fault = emu_mfspr(regs, insn);
		break;

	case 0x1d3:
		fault = emu_mtspr(regs, insn);
		break;
	}

	if (likely(!fault)) {
		regs->srr0 += 4;
		return;
	}

bad:
	printf("unhandled hvpriv trap from 0x%x, insn 0x%08x\n", regs->srr0, insn);
	regs->exc = EXC_PGM;
	reflect_trap(regs);
}
