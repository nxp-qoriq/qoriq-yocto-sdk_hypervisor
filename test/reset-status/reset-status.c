
/*
 * Copyright (C) 2009 Freescale Semiconductor, Inc.
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



#include <libos/libos.h>
#include <libos/hcalls.h>
#include <libos/core-regs.h>
#include <libos/trapframe.h>
#include <libos/bitops.h>
#include <libfdt.h>
#include <hvtest.h>

static int irq;

void ext_int_handler(trapframe_t *frameptr)
{
	unsigned int vector;

	if (coreint)
		vector = mfspr(SPR_EPR);
	else
		fh_vmpic_iack(&vector);

	printf("ext int %d\n",vector);
	fh_vmpic_eoi(irq);
}

void libos_client_entry(unsigned long devtree_ptr)
{
	uint32_t status;
	const char *str;
	uint32_t handle;
	int node = -1;
	int len;

	init(devtree_ptr);

	printf("Reset status test\n");

	node = fdt_path_offset(fdt, "/hypervisor");
	if (node < 0) {
		printf("no /hypervisor node: FAILED\n");
		return;
	}

	printf("reset status property: ");
	str = fdt_getprop(fdt, node, "fsl,hv-sys-reset-status", &len);
	if (!str) {
		printf(" FAILED\n");
	} else {
		printf(" %s\n",str);
	}

	printf("reason stopped property: ");
	str = fdt_getprop(fdt, node, "fsl,hv-reason-stopped", &len);
	if (!str) {
		printf(" <no property present>\n");
	} else {
		printf(" %s\n",str);
	}

	printf("stopped by property: ");
	str = fdt_getprop(fdt, node, "fsl,hv-stopped-by", &len);
	if (!str) {
		printf(" <no property present>\n");
	} else {
		printf(" %s\n",str);
	}


	printf("Test Complete\n");
}
