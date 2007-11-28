
#include "uv.h"
#include "frame.h"
#include "trap_booke.h"
#include "console.h"
#include <percpu.h>
#include <spr.h>
#include <timers.h>
#include <guestmemio.h>

struct powerpc_exception {
	int vector;
	char *name;
};

void traceback(trapframe_t *regs)
{
	unsigned long *sp = ((unsigned long *)regs->gpregs[1]);
	printf("sp %p %lx %lx %lx %lx\n", sp, sp[0], sp[1], sp[2], sp[3]);
	sp = ((unsigned long **)regs->gpregs[1])[0];

	printf("Traceback: ");
	
	for (int i = 1; sp != NULL; i++, sp = (unsigned long *)sp[0]) {
		if ((i & 7) == 0)
			printf("\n");
		
		printf("0x%08lx ", sp[1] - 4);
	}

	printf("\n");
}

void dump_regs(trapframe_t *regs)
{
	printf("NIP 0x%08x MSR 0x%08x LR 0x%08x ESR 0x%08x EXC %d\n"
	       "CTR 0x%08x CR 0x%08x XER 0x%08x DEAR 0x%08x\n",
	       regs->srr0, regs->srr1, regs->lr, mfspr(SPR_ESR), regs->exc,
	       regs->ctr, regs->cr, regs->xer, mfspr(SPR_DEAR));

	for (int i = 0; i < 32; i++) {
		printf("r%02d 0x%08x  ", i, regs->gpregs[i]);
		
		if ((i & 3) == 3)
			printf("\n");
	}

	if (!(regs->srr1 & MSR_GS))
		traceback(regs);
}

static const struct powerpc_exception powerpc_exceptions[] = {
	{ EXC_CRIT, "critical input" },
	{ EXC_MCHK, "machine check" },
	{ EXC_DSI, "data storage interrupt" },
	{ EXC_ISI, "instruction storage interrupt" },
	{ EXC_EXI, "external interrupt" },
	{ EXC_ALI, "alignment" },
	{ EXC_PGM, "program" },
	{ EXC_SC, "system call" },
	{ EXC_DECR, "decrementer" },
	{ EXC_FIT, "fixed-interval timer" },
	{ EXC_WDOG, "watchdog timer" },
	{ EXC_DTLB, "data tlb miss" },
	{ EXC_ITLB, "instruction tlb miss" },
	{ EXC_DEBUG, "debug" },
	{ EXC_PERF, "performance monitoring" },
	{ EXC_DOORBELL, "doorbell"},
	{ EXC_DOORBELLC, "doorbell critical"},
	{ EXC_GDOORBELL, "guest doorbell"},
	{ EXC_GDOORBELLC, "guest doorbell critical"},
	{ EXC_HCALL, "hcall"},
	{ EXC_EHPRIV, "ehpriv"},
	{ EXC_LAST, NULL }
};

static const char *trapname(int vector)
{
	const struct powerpc_exception *pe;

	for (pe = powerpc_exceptions; pe->vector != EXC_LAST; pe++) {
		if (pe->vector == vector)
			return (pe->name);
	}

	return "unknown";
}

void unknown_exception(trapframe_t *regs)
{
	printf("unknown exception: %s\n", trapname(regs->exc));
	dump_regs(regs); 
	
	if (regs->srr0 & MSR_GS)
		reflect_trap(regs);
	else
		stopsim();
}

// Do not use this when entering via guest doorbell, since that saves
// state in gsrr rather than srr, despite being directed to the
// hypervisor.

void reflect_trap(trapframe_t *regs)
{
	gcpu_t *gcpu = hcpu->gcpu;

	if (__builtin_expect(!(regs->srr1 & MSR_GS), 0)) {
		printf("unexpected trap in hypervisor\n");
		dump_regs(regs);
		stopsim();
	}

	assert(regs->exc >= 0 && regs->exc < sizeof(gcpu->ivor) / sizeof(int));

	mtspr(SPR_GSRR0, regs->srr0);
	mtspr(SPR_GSRR1, regs->srr1);

	regs->srr0 = gcpu->ivpr | gcpu->ivor[regs->exc];
	regs->srr1 &= MSR_CE | MSR_ME | MSR_DE | MSR_GS | MSR_UCLE;
}

void guest_doorbell(trapframe_t *regs)
{
	gcpu_t *gcpu = hcpu->gcpu;
	unsigned long gsrr1 = mfspr(SPR_GSRR1);

	assert(gsrr1 & MSR_GS);
	assert(gsrr1 & MSR_EE);

	// First, check external interrupts. (TODO).
	if (0) {
	}

	// Then, check for a decrementer.
	if (gcpu->pending & GCPU_PEND_DECR) {
		run_deferred_decrementer();

		regs->srr0 = gcpu->ivpr | gcpu->ivor[EXC_DECR];
		regs->srr1 = gsrr1 & (MSR_CE | MSR_ME | MSR_DE | MSR_GS | MSR_UCLE);
	}
}

void reflect_mcheck(trapframe_t *regs, register_t mcsr, uint64_t mcar)
{
	gcpu_t *gcpu = hcpu->gcpu;

	gcpu->mcsr = mcsr;
	gcpu->mcar = mcar;

	gcpu->mcsrr0 = regs->srr0;
	gcpu->mcsrr1 = regs->srr1;

	regs->srr0 = gcpu->ivpr | gcpu->ivor[EXC_MCHK];
	regs->srr1 &= MSR_GS | MSR_UCLE;
}

typedef struct {
	unsigned long addr, handler;
} extable;

extern extable extable_begin, extable_end;

static void abort_guest_access(trapframe_t *regs, int stat)
{
	regs->gpregs[3] = stat;

	for (extable *ex = &extable_begin; ex < &extable_end; ex++) {
		if (ex->addr == regs->srr0) {
			regs->srr0 = ex->handler;
			return;
		}
	}

	reflect_trap(regs);
}

void data_storage(trapframe_t *regs)
{
	// If it's from the guest, then it was a virtualization
	// fault.  Currently, we only use that for bad mappings.
	if (regs->srr1 & MSR_GS)
		reflect_mcheck(regs, MCSR_MAV | MCSR_MEA, mfspr(SPR_DEAR));
	else if (mfspr(SPR_ESR) & ESR_EPID)
		abort_guest_access(regs, GUESTMEM_TLBERR);
	else
		reflect_trap(regs);
}

void dtlb_miss(trapframe_t *regs)
{
	assert(!(regs->srr1 & MSR_GS));

	if (mfspr(SPR_ESR) & ESR_EPID)
		abort_guest_access(regs, GUESTMEM_TLBMISS);
	else
		reflect_trap(regs);
}
