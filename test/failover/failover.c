/*
 * Copyright (C) 2008-2010 Freescale Semiconductor, Inc.
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

#include <libos/alloc.h>
#include <libos/fsl_hcalls.h>
#include <libos/epapr_hcalls.h>
#include <libos/core-regs.h>
#include <libos/trapframe.h>
#include <libos/bitops.h>
#include <libos/fsl-booke-tlb.h>
#include <libos/platform_error.h>
#include <libos/io.h>
#include <libfdt.h>
#include <hvtest.h>

#define DMA_CHAN_BUSY      0x00000004
#define DMA_END_OF_SEGMENT 0x00000002
#define PAGE_SIZE 4096

void init(unsigned long devtree_ptr);

static phys_addr_t dma_phys;
static uint32_t *dma_virt;
static uint32_t liodn;
static volatile int mcheck_int, crit_int;
static hv_error_t mcheck_err, crit_err;

static int state_change_irq, dma_irq;
static int managed_partition;
static volatile int standby_go;

/* zero if no irq yet, positive if good irq, negative if bad irq */
static volatile int got_dma_irq;

void ext_int_handler(trapframe_t *frameptr)
{
	unsigned int vector;
	int ret;

	if (coreint)
		vector = mfspr(SPR_EPR);
	else
		vector = ev_int_iack(0, &vector);

	if (vector == 0xffff)
		return;

	if (vector == state_change_irq) {
		unsigned int status;

		ret = fh_partition_get_status(managed_partition, &status);
		if (ret) {
			printf("BROKEN: error %d reading partition status\n",
			       ret);
			goto out;
		}

		if (status == FH_PARTITION_STOPPED)
			standby_go = 1;
	} else if (vector == dma_irq) {
		printf("DMA interrupt\n");

		uint32_t stat = in32(&dma_virt[1]);
		out32(&dma_virt[1], stat);
		
		if (stat & ~(DMA_END_OF_SEGMENT | DMA_CHAN_BUSY)) {
			printf("dma stat %x\n", stat);
			got_dma_irq = -1;
		} else if (stat & DMA_END_OF_SEGMENT) {
			got_dma_irq = 1;
		}
	} else {
		printf("BROKEN: Unexpected extint %u\n", vector);
	}

out:
		ev_int_eoi(vector);
}

void crit_int_handler(trapframe_t *regs)
{
	hv_error_t *ptr;
	uint32_t bufsize = sizeof(hv_error_t);

	int ret = fh_err_get_info(global_error_queue, &bufsize, 0, virt_to_phys(&crit_err), 0);
	if (!ret)
		crit_int++;
}

void mcheck_interrupt(trapframe_t *regs)
{
	uint32_t bufsize = sizeof(hv_error_t);

	if (!(mfspr(SPR_MCSR) & MCSR_MCP))
		return;

	int ret = fh_err_get_info(guest_error_queue, &bufsize, 0, virt_to_phys(&mcheck_err), 0);
	if (!ret)
		mcheck_int++;

	mtspr(SPR_MCSR, mfspr(SPR_MCSR));
}

/* First word of shmem is iteration counter, rest of page used for DMA */
static volatile uint32_t *shmem;
static phys_addr_t shmem_phys;

static int dma_init(void)
{
	int node, parent, len;
	const uint32_t *prop;
	const char *str;
	int wait_for_irq = 0;
	int ret;

	node = fdt_node_offset_by_compatible(fdt, -1, "fsl,eloplus-dma-channel");
	if (node < 0)
		return -1;

	if (dt_get_reg(fdt, node, 0, &dma_phys, NULL) < 0)
		return -2;

	parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return -3;

	dma_virt = valloc(4096, 4096);
	if (!dma_virt)
		return -4;

	dma_virt = (void *)dma_virt + (dma_phys & (PAGE_SIZE - 1));

	tlb1_set_entry(4, (uintptr_t)dma_virt, dma_phys,
	               TLB_TSIZE_4K, TLB_MAS2_IO,
	               TLB_MAS3_KERN, 0, 0, 0);

	/* FIXME: reset all channels of DMA block before enabling liodn */
	
	prop = fdt_getprop(fdt, parent, "fsl,hv-dma-handle", &len);
	if (!prop || len != 4) {
		printf("bad/missing fsl,hv-dma-handle property: %d\n", len);
		return -5;
	}

	liodn = *prop;

	dma_irq = get_vmpic_irq(node, 0);
	if (dma_irq < 0) {
		printf("BROKEN: no state change irq\n");
		return -1;
	}

	str = fdt_getprop(fdt, node, "status", &len);
	if (str && strcmp(str, "okay")) {
		str = fdt_getprop(fdt, node, "fsl,hv-claimable", &len);
		if (!str || !len || strcmp(str, "standby")) {
			printf("FAILED: missing/bad claimable %s in standby\n",
			       str ? str : "<none>");
			return -6;
		}

		ret = set_vmpic_irq_priority(dma_irq, 15);
		if (ret != EV_INVALID_STATE) {
			printf("FAILED: set prio on standby irq returned %d\n",
			       ret);
			return -14;
		}

		ret = fh_dma_enable(liodn);
		if (ret != EV_INVALID_STATE) {
			printf("FAILED: enable standby dma returned %d\n",
			       ret);
			return -15;
		}

		printf("Standby waiting...");

		/* Wait until other guest stops, then claim and continue */
		while (!standby_go);
		
		printf("done\n");

		printf("Failover #%u:\n\n", *shmem);

		prop = fdt_getprop(fdt, node, "fsl,hv-device-handle", &len);
		if (!prop || len != 4) {
			printf("FAILED: missing/bad fsl,hv-device-handle "
			       "in standby channel\n");
			return -9;
		}

		/* Claim the channel, it has the interrupt. */
		ret = fh_claim_device(*prop);
		if (ret) {
			printf("FAILED: error %d claiming dma channel node\n", ret);
			return -10;
		}

		prop = fdt_getprop(fdt, parent, "fsl,hv-device-handle", &len);
		if (!prop || len != 4) {
			printf("FAILED: missing/bad fsl,hv-device-handle "
			       "in standby controller\n");
			return -11;
		}

		/* Claim the controller, it has the DMA. */
		ret = fh_claim_device(*prop);
		if (ret) {
			printf("FAILED: error %d claiming dma node\n", ret);
			return -12;
		}

		if (*shmem < 5) {
			ret = fh_partition_start(managed_partition, 0);
			if (ret) {
				printf("BROKEN: error %d starting partition\n", ret);
				return -13;
			}

			/* expected to fail until partition has started. */
			do {
				ret = fh_partition_restart(managed_partition);
			} while (ret != 0);
		}
	
		/* temporary until queued error migration is implemented */
		ret = fh_dma_enable(liodn);
		if (ret) {
			printf("FAILED: couldn't enable DMA\n");
			return -16;
		}

		if (*shmem == 3 || *shmem == 4)
			wait_for_irq = 1;
	} else {
		str = fdt_getprop(fdt, node, "fsl,hv-claimable", &len);
		if (!str || strcmp(str, "active")) {
			printf("FAILED: bad claimable %s in active\n",
			       str ? str : "<none>");
			return -7;
		}

		ret = fh_dma_enable(liodn);
		if (ret) {
			printf("FAILED: couldn't enable DMA\n");
			return -8;
		}
	}
	
	ret = set_vmpic_irq_priority(dma_irq, 15);
	if (ret) {
		printf("FAILED: couldn't set dma irq prio\n");
		return -17;
	}

	ret = ev_int_set_mask(dma_irq, 0);
	if (ret) {
		printf("FAILED: couldn't unmask dma irq\n");
		return -17;
	}

	if (wait_for_irq) {
		/* Previous run should have left us a pending interrupt
		 * for a good DMA transfer.
		 */
		while (!got_dma_irq);
	
		if (got_dma_irq > 0)
			printf("PASSED: previous owner left good irq\n");
		else if (got_dma_irq < 0)
			printf("FAILED: previous owner left bad irq\n");
	}

	return 0;
}

static int test_dma_memcpy(int valid, int wait)
{
	int count = 256;
	unsigned char *gva_src, *gva_dst;
	phys_addr_t gpa_src, gpa_dst;
	uint32_t stat;

	gva_src = (unsigned char *)&shmem[64];
	gva_dst = (unsigned char *)&shmem[128];

	gpa_src = shmem_phys + 64 * 4;

	if (valid)
		gpa_dst = shmem_phys + 128 * 4;
	else
		gpa_dst = 0xdeadbeef;

	for (int i = 0; i < count; i++)
		gva_src[i] = i;
	memset(gva_dst, 0xeb, count);

	got_dma_irq = 0;

	/* basic DMA direct mode test */
	out32(&dma_virt[1], in32(&dma_virt[1])); /* clear old status */
	out32(&dma_virt[0], 0x00000644);
	out32(&dma_virt[8], count); /* count */
	out32(&dma_virt[4], 0x00050000); /* read,snoop */
	out32(&dma_virt[6], 0x00050000); /* write,snoop */
	out32(&dma_virt[5], gpa_src);
	out32(&dma_virt[7], gpa_dst);

	if (!wait)
		return 0;

	while (!got_dma_irq);
	
	if (got_dma_irq < 0)
		return valid ? -1 : 0;
	

	if (valid)
		return memcmp(gva_src, gva_dst, count);

	return 0;
}

#define tophys(x) (((uintptr_t)(x)) - PHYSBASE)

static int map_shmem(void)
{
	const uint32_t *prop;
	int node, len;

	node = fdt_node_offset_by_compatible(fdt, -1, "failover-shmem");
	if (node < 0) {	
		printf("BROKEN: no failover-shmem\n");
		return -1;
	}

	if (dt_get_reg(fdt, node, 0, &shmem_phys, NULL) < 0) {
		printf("BROKEN: missing/bad reg in failover-shmem\n");
		return -1;
	}	

	shmem = valloc(4096, 4096);
	tlb1_set_entry(2, (uintptr_t)shmem, shmem_phys,
	               TLB_TSIZE_4K, TLB_MAS2_MEM,
	               TLB_MAS3_KERN, 0, 0, 0);

	prop = fdt_getprop(fdt, node, "initialize-me", &len);
	if (prop && len == 4 && *prop == 1) {
		static char shmem_path[256];
		uint32_t zero = 0;
		int ret;

		memset((void *)shmem, 0, 4096);

		ret = fdt_get_path(fdt, node, shmem_path, sizeof(shmem_path));
		if (ret) {
			printf("BROKEN: can't get shmem path\n");
			return -1;
		}

		ret = fh_partition_set_dtprop(-1, tophys(shmem_path),
		                              tophys("initialize-me"),
		                              tophys(&zero), 4);
		if (ret) {
			printf("BROKEN: can't set initialize-me\n");
			return -1;
		}
	}

	return 0;
}

static int setup_managed(void)
{
	int node, len, ret;
	const uint32_t *prop;

	node = fdt_node_offset_by_compatible(fdt, -1, "fsl,hv-partition-handle");
	if (node < 0) {
		printf("BROKEN: no managed partition\n");
		return -1;
	}

	prop = fdt_getprop(fdt, node, "reg", &len);
	if (!prop || len != 4) {
		printf("BROKEN: missing/bad reg in managed partition\n");
		return -1;
	}

	managed_partition = *prop;

	int subnode = fdt_node_offset_by_compatible
		(fdt, node, "fsl,hv-state-change-doorbell");
	if (subnode < 0) {
		printf("BROKEN: no state change doorbell\n");
		return -1;
	}

	state_change_irq = get_vmpic_irq(subnode, 0);
	if (state_change_irq < 0) {
		printf("BROKEN: no state change irq\n");
		return -1;
	}

	ret = set_vmpic_irq_priority(state_change_irq, 15);
	if (ret) {
		printf("BROKEN: couldn't set state change prio\n");
		return -1;
	}

	ev_int_set_mask(state_change_irq, 0);
	return 0;
}

void libos_client_entry(unsigned long devtree_ptr)
{
	int ret, len, i;

	init(devtree_ptr);

	printf("\nFailover test:\n");

	if (init_error_queues() < 0)
		return;

	enable_extint();
	enable_critint();
	enable_mcheck();

	if (map_shmem() < 0)
		return;

	if (setup_managed() < 0)
		return;

	ret = dma_init();
	if (ret < 0) {
		printf("dma_init failed = %d\n", ret);
		return;
	}

	if (mcheck_int != 0) {
		printf("FAILED: pre-existing mcheck %d\n", mcheck_int);
		return;
	}

	crit_int = 0;

	/* test access violation failure and pass cases */
	ret = test_dma_memcpy(0, 1);
	printf("DMA invalid access test #1: %s\n", ret ? "FAILED" : "PASSED");
	if (ret)
		return;

	while (mcheck_int < 1);

	printf("Received in local error queue:\n");
	printf("domain: %s, error: %s, path: %s\n", mcheck_err.domain, mcheck_err.error,
		 mcheck_err.hdev_tree_path);
	printf("PAMU access violation avs1 = %x, avs2 = %x, av_addr = %llx\n",
		 mcheck_err.pamu.avs1, mcheck_err.pamu.avs2, mcheck_err.pamu.access_violation_addr);
	printf("PAMU access violation lpid = %x, handle = %x\n\n",
		mcheck_err.pamu.lpid, mcheck_err.pamu.liodn_handle);

	if (!((*shmem) & 1)) {
		while (crit_int < 1);
	
		printf("Received in global error queue:\n");
		printf("domain: %s, error: %s, path: %s\n", crit_err.domain, crit_err.error,
			crit_err.hdev_tree_path);
		printf("PAMU access violation avs1 = %x, avs2 = %x, av_addr = %llx\n",
			crit_err.pamu.avs1, crit_err.pamu.avs2, crit_err.pamu.access_violation_addr);
		printf("PAMU access violation lpid = %x, handle = %x\n\n",
			crit_err.pamu.lpid, crit_err.pamu.liodn_handle);
	}

	/* PPAACE entry correponding to access violation LIODN is marked
	 * invalid by the hypervisor. We need to explicitly mark the entry
	 * as valid using the hcall.
	 */
	ret = fh_dma_enable(liodn);
	if (ret) {
		printf("fh_dma_enable: failed %d\n", liodn);
		return;
	}

	(*shmem)++;

	if (*shmem <= 2) {
		/* Do one more valid access, but leave it in the queues for
		 * the claiming partition.
		 */

		ret = test_dma_memcpy(1, 1);
		printf("DMA valid access test: %s\n", ret ? "FAILED" : "PASSED");
		if (ret)
			return;
	
		disable_mcheck();
		disable_critint();
		ret = test_dma_memcpy(0, 1);
		printf("DMA invalid access test #2: %s\n",
		       ret ? "FAILED" : "PASSED");
		if (ret)
			return;
	} else if (*shmem <= 4) {
		/* Leave an interrupt pending */
		disable_extint();
		test_dma_memcpy(1, 0);
	} else if (*shmem == 6) {
		printf("Test Complete\n");
	}

	fh_partition_stop(-1);
}
