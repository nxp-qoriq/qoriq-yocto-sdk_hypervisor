/*-
 * Copyright (c) 1982, 1990 The Regents of the University of California.
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * William Jolitz.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)genassym.c	5.11 (Berkeley) 5/10/91
 * $FreeBSD: src/sys/powerpc/powerpc/genassym.c,v 1.58 2006/11/30 04:17:05 jb Exp $
 */

#include <stddef.h>
#include "assym.h"
#include "pcpu.h"
#include "frame.h"

ASSYM(CPUSAVE_R28, CPUSAVE_R28*4);
ASSYM(CPUSAVE_R29, CPUSAVE_R29*4);
ASSYM(CPUSAVE_R30, CPUSAVE_R30*4);
ASSYM(CPUSAVE_R31, CPUSAVE_R31*4);
ASSYM(CPUSAVE_DEAR, CPUSAVE_DEAR*4);
ASSYM(CPUSAVE_ESR, CPUSAVE_ESR*4);
ASSYM(CPUSAVE_SRR0, CPUSAVE_SRR0*4);
ASSYM(CPUSAVE_SRR1, CPUSAVE_SRR1*4);

ASSYM(PCPU_CRITSAVE, offsetof(pcpu_t, critsave));
ASSYM(UVSTACK, offsetof(pcpu_t, uvstack));

ASSYM(FRAMELEN, FRAMELEN);
ASSYM(FRAME_0, offsetof(trapframe_t, fixreg[0]));
ASSYM(FRAME_1, offsetof(trapframe_t, fixreg[1]));
ASSYM(FRAME_2, offsetof(trapframe_t, fixreg[2]));
ASSYM(FRAME_3, offsetof(trapframe_t, fixreg[3]));
ASSYM(FRAME_LR, offsetof(trapframe_t, lr));
ASSYM(FRAME_CR, offsetof(trapframe_t, cr));
ASSYM(FRAME_DEAR, offsetof(trapframe_t, dear));
ASSYM(FRAME_ESR, offsetof(trapframe_t, esr));
ASSYM(FRAME_CTR, offsetof(trapframe_t, ctr));
ASSYM(FRAME_XER, offsetof(trapframe_t, xer));
ASSYM(FRAME_SRR0, offsetof(trapframe_t, srr0));
ASSYM(FRAME_SRR1, offsetof(trapframe_t, srr1));
ASSYM(FRAME_EXC, offsetof(trapframe_t, exc));

#if 0

#include <sys/param.h>
#include <sys/assym.h>
#include <sys/errno.h>
#include <sys/ktr.h>
#include <sys/proc.h>
#include <sys/queue.h>
#include <sys/signal.h>
#include <sys/smp.h>
#include <sys/systm.h>
#include <sys/ucontext.h>
#include <sys/ucontext.h>
#include <sys/vmmeter.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>

#include <machine/pcb.h>
#include <machine/pmap.h>
#include <machine/sigframe.h>

ASSYM(PC_CURTHREAD, offsetof(struct pcpu, pc_curthread));
ASSYM(PC_CURPCB, offsetof(struct pcpu, pc_curpcb));
ASSYM(PC_CURPMAP, offsetof(struct pcpu, pc_curpmap));
ASSYM(PC_TEMPSAVE, offsetof(struct pcpu, pc_tempsave));
ASSYM(PC_DISISAVE, offsetof(struct pcpu, pc_disisave));
ASSYM(PC_DBSAVE, offsetof(struct pcpu, pc_dbsave));
#if defined(E500)
ASSYM(PC_CRITSAVE, offsetof(struct pcpu, pc_critsave));
ASSYM(PC_MCHKSAVE, offsetof(struct pcpu, pc_mchksave));
ASSYM(PC_TLBSAVE, offsetof(struct pcpu, pc_tlbsave));
ASSYM(PC_TLB_LEVEL, offsetof(struct pcpu, pc_tlb_level));
#endif

ASSYM(CPUSAVE_R28, CPUSAVE_R28*4);
ASSYM(CPUSAVE_R29, CPUSAVE_R29*4);
ASSYM(CPUSAVE_R30, CPUSAVE_R30*4);
ASSYM(CPUSAVE_R31, CPUSAVE_R31*4);
#if defined(AIM)
ASSYM(CPUSAVE_DAR, CPUSAVE_DAR*4);
ASSYM(CPUSAVE_DSISR, CPUSAVE_DSISR*4);
#elif defined(E500)
ASSYM(CPUSAVE_DEAR, CPUSAVE_DEAR*4);
ASSYM(CPUSAVE_ESR, CPUSAVE_ESR*4);
#endif
ASSYM(CPUSAVE_SRR0, CPUSAVE_SRR0*4);
ASSYM(CPUSAVE_SRR1, CPUSAVE_SRR1*4);
#if defined(E500)
ASSYM(TLBSAVE_LR, TLBSAVE_LR*4);
ASSYM(TLBSAVE_CR, TLBSAVE_CR*4);
ASSYM(TLBSAVE_SRR0, TLBSAVE_SRR0*4);
ASSYM(TLBSAVE_SRR1, TLBSAVE_SRR1*4);
ASSYM(TLBSAVE_R20, TLBSAVE_R20*4);
ASSYM(TLBSAVE_R21, TLBSAVE_R21*4);
ASSYM(TLBSAVE_R22, TLBSAVE_R22*4);
ASSYM(TLBSAVE_R23, TLBSAVE_R23*4);
ASSYM(TLBSAVE_R24, TLBSAVE_R24*4);
ASSYM(TLBSAVE_R25, TLBSAVE_R25*4);
ASSYM(TLBSAVE_R26, TLBSAVE_R26*4);
ASSYM(TLBSAVE_R27, TLBSAVE_R27*4);
ASSYM(TLBSAVE_R28, TLBSAVE_R28*4);
ASSYM(TLBSAVE_R29, TLBSAVE_R29*4);
ASSYM(TLBSAVE_R30, TLBSAVE_R30*4);
ASSYM(TLBSAVE_R31, TLBSAVE_R31*4);
#endif

ASSYM(MTX_LOCK, offsetof(struct mtx, mtx_lock));
ASSYM(MTX_RECURSECNT, offsetof(struct mtx, mtx_recurse));

#if defined(AIM)
ASSYM(PM_KERNELSR, offsetof(struct pmap, pm_sr[KERNEL_SR]));
ASSYM(PM_USRSR, offsetof(struct pmap, pm_sr[USER_SR]));
ASSYM(PM_SR, offsetof(struct pmap, pm_sr));
#elif defined(E500)
ASSYM(PM_PDIR, offsetof(struct pmap, pm_pdir));
#endif

#if defined(E500)
ASSYM(PTE_RPN, offsetof(struct pte_entry, rpn));
ASSYM(PTE_FLAGS, offsetof(struct pte_entry, flags));
ASSYM(TLB0TABLE_MAS1, offsetof(struct tlb_entry, mas1));
ASSYM(TLB0TABLE_MAS2, offsetof(struct tlb_entry, mas2));
ASSYM(TLB0TABLE_MAS3, offsetof(struct tlb_entry, mas3));
ASSYM(TLB0_ENTRY_SIZE, sizeof(struct tlb_entry));
#endif

ASSYM(FSP, 8);
ASSYM(FRAMELEN, FRAMELEN);
ASSYM(FRAME_0, offsetof(struct trapframe, fixreg[0]));
ASSYM(FRAME_1, offsetof(struct trapframe, fixreg[1]));
ASSYM(FRAME_2, offsetof(struct trapframe, fixreg[2]));
ASSYM(FRAME_3, offsetof(struct trapframe, fixreg[3]));
ASSYM(FRAME_4, offsetof(struct trapframe, fixreg[4]));
ASSYM(FRAME_5, offsetof(struct trapframe, fixreg[5]));
ASSYM(FRAME_6, offsetof(struct trapframe, fixreg[6]));
ASSYM(FRAME_7, offsetof(struct trapframe, fixreg[7]));
ASSYM(FRAME_8, offsetof(struct trapframe, fixreg[8]));
ASSYM(FRAME_9, offsetof(struct trapframe, fixreg[9]));
ASSYM(FRAME_10, offsetof(struct trapframe, fixreg[10]));
ASSYM(FRAME_11, offsetof(struct trapframe, fixreg[11]));
ASSYM(FRAME_12, offsetof(struct trapframe, fixreg[12]));
ASSYM(FRAME_13, offsetof(struct trapframe, fixreg[13]));
ASSYM(FRAME_14, offsetof(struct trapframe, fixreg[14]));
ASSYM(FRAME_15, offsetof(struct trapframe, fixreg[15]));
ASSYM(FRAME_16, offsetof(struct trapframe, fixreg[16]));
ASSYM(FRAME_17, offsetof(struct trapframe, fixreg[17]));
ASSYM(FRAME_18, offsetof(struct trapframe, fixreg[18]));
ASSYM(FRAME_19, offsetof(struct trapframe, fixreg[19]));
ASSYM(FRAME_20, offsetof(struct trapframe, fixreg[20]));
ASSYM(FRAME_21, offsetof(struct trapframe, fixreg[21]));
ASSYM(FRAME_22, offsetof(struct trapframe, fixreg[22]));
ASSYM(FRAME_23, offsetof(struct trapframe, fixreg[23]));
ASSYM(FRAME_24, offsetof(struct trapframe, fixreg[24]));
ASSYM(FRAME_25, offsetof(struct trapframe, fixreg[25]));
ASSYM(FRAME_26, offsetof(struct trapframe, fixreg[26]));
ASSYM(FRAME_27, offsetof(struct trapframe, fixreg[27]));
ASSYM(FRAME_28, offsetof(struct trapframe, fixreg[28]));
ASSYM(FRAME_29, offsetof(struct trapframe, fixreg[29]));
ASSYM(FRAME_30, offsetof(struct trapframe, fixreg[30]));
ASSYM(FRAME_31, offsetof(struct trapframe, fixreg[31]));
ASSYM(FRAME_LR, offsetof(struct trapframe, lr));
ASSYM(FRAME_CR, offsetof(struct trapframe, cr));
#if defined(AIM)
ASSYM(FRAME_DAR, offsetof(struct trapframe, dar));
ASSYM(FRAME_DSISR, offsetof(struct trapframe, dsisr));
#elif defined(E500)
#endif

ASSYM(CF_FUNC, offsetof(struct callframe, cf_func));
ASSYM(CF_ARG0, offsetof(struct callframe, cf_arg0));
ASSYM(CF_ARG1, offsetof(struct callframe, cf_arg1));
ASSYM(CF_SIZE, sizeof(struct callframe));

ASSYM(PCB_CONTEXT, offsetof(struct pcb, pcb_context));
ASSYM(PCB_CR, offsetof(struct pcb, pcb_cr));
#if defined(E500)
ASSYM(PCB_CTR, offsetof(struct pcb, pcb_ctr));
ASSYM(PCB_XER, offsetof(struct pcb, pcb_xer));
#endif
#if defined(AIM)
ASSYM(PCB_PMR, offsetof(struct pcb, pcb_pmreal));
#endif
ASSYM(PCB_SP, offsetof(struct pcb, pcb_sp));
ASSYM(PCB_LR, offsetof(struct pcb, pcb_lr));
#if defined(AIM)
ASSYM(PCB_USR, offsetof(struct pcb, pcb_usr));
#endif
ASSYM(PCB_ONFAULT, offsetof(struct pcb, pcb_onfault));
ASSYM(PCB_FLAGS, offsetof(struct pcb, pcb_flags));
#if defined(AIM)
ASSYM(PCB_FPU, PCB_FPU);
#endif

ASSYM(TD_PROC, offsetof(struct thread, td_proc));
ASSYM(TD_PCB, offsetof(struct thread, td_pcb));

ASSYM(P_VMSPACE, offsetof(struct proc, p_vmspace));

ASSYM(VM_PMAP, offsetof(struct vmspace, vm_pmap));

ASSYM(TD_FLAGS, offsetof(struct thread, td_flags));

ASSYM(TDF_ASTPENDING, TDF_ASTPENDING);
ASSYM(TDF_NEEDRESCHED, TDF_NEEDRESCHED);

ASSYM(SF_UC, offsetof(struct sigframe, sf_uc));

ASSYM(MAXCOMLEN, MAXCOMLEN);

#endif
