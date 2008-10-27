/** @file
 * Guest management
 */
/*
 * Copyright (C) 2007-2008 Freescale Semiconductor, Inc.
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

#include <libos/percpu.h>
#include <libos/fsl-booke-tlb.h>
#include <libos/core-regs.h>
#include <libos/io.h>
#include <libos/bitops.h>
#include <libos/mpic.h>

#include <hv.h>
#include <paging.h>
#include <timers.h>
#include <byte_chan.h>
#include <vmpic.h>
#include <pamu.h>
#include <ipi_doorbell.h>
#include <devtree.h>
#include <errors.h>
#include <elf.h>
#include <uimage.h>
#include <events.h>
#include <doorbell.h>
#include <gdb-stub.h>

guest_t guests[MAX_PARTITIONS];
unsigned long last_lpid;

#define MAX_PATH 256

static int vcpu_to_cpu(const uint32_t *cpulist, unsigned int len, int vcpu)
{
	unsigned int i, vcpu_base = 0;
	
	for (i = 0; i < len / 4; i += 2) {
		if (vcpu >= vcpu_base && vcpu < vcpu_base + cpulist[i + 1])
			return cpulist[i] + vcpu - vcpu_base;

		vcpu_base += cpulist[i + 1];
	}

	return ERR_RANGE;
}

static int cpu_in_cpulist(const uint32_t *cpulist, unsigned int len, int cpu)
{
	unsigned int i;
	for (i = 0; i < len / 4; i += 2) {
		if (cpu >= cpulist[i] && cpu < cpulist[i] + cpulist[i + 1])
			return 1;
	}

	return 0;
}

static int get_gcpu_num(const uint32_t *cpulist, unsigned int len, int cpu)
{
	unsigned int i;
	unsigned int total = 0; 

	for (i = 0; i < len / 4; i += 2) {
		unsigned int base = cpulist[i];
		unsigned int num = cpulist[i + 1];

		if (cpu >= base && cpu < base + num)
			return total + cpu - base;

		total += num;
	}

	return ERR_RANGE;
}

static unsigned int count_cpus(const uint32_t *cpulist, unsigned int len)
{
	unsigned int i;
	unsigned int total = 0;

	for (i = 0; i < len / 4; i += 2)
		total += cpulist[i + 1];	

	return total;
}

static void map_guest_addr_range(guest_t *guest, phys_addr_t gaddr,
                                 phys_addr_t addr, phys_addr_t size)
{
	unsigned long grpn = gaddr >> PAGE_SHIFT;
	unsigned long rpn = addr >> PAGE_SHIFT;
	unsigned long pages = (gaddr + size -
	                       (grpn << PAGE_SHIFT) +
	                       (PAGE_SIZE - 1)) >> PAGE_SHIFT;

	printlog(LOGTYPE_GUEST_MMU, LOGLEVEL_DEBUG,
	         "cpu%ld mapping guest %lx to real %lx, %lx pages\n",
	         mfspr(SPR_PIR), grpn, rpn, pages);

	vptbl_map(guest->gphys, grpn, rpn, pages, PTE_ALL, PTE_PHYS_LEVELS);
	vptbl_map(guest->gphys_rev, rpn, grpn, pages, PTE_ALL, PTE_PHYS_LEVELS);
}

static int map_guest_reg_one(guest_t *guest, int node,
                             int partition, const uint32_t *reg,
                             uint32_t naddr, uint32_t nsize)
{
	phys_addr_t gaddr, size;
	uint32_t addrbuf[MAX_ADDR_CELLS];
	phys_addr_t rangesize, addr, offset = 0;
	int maplen, ret;
	const uint32_t *physaddrmap;

	physaddrmap = fdt_getprop(fdt, partition, "fsl,hv-physaddr-map", &maplen);
	if (!physaddrmap && maplen != -FDT_ERR_NOTFOUND)
		return maplen;
	if (!physaddrmap || maplen & 3)
		return ERR_BADTREE;

	ret = xlate_reg_raw(guest->devtree, node, reg, addrbuf,
	                    &size, naddr, nsize);
	if (ret < 0)
		return ret;	

	gaddr = ((uint64_t)addrbuf[2] << 32) | addrbuf[3];

	while (offset < size) {
		val_from_int(addrbuf, gaddr + offset);

		ret = xlate_one(addrbuf, physaddrmap, maplen, rootnaddr, rootnsize,
		                guest->naddr, guest->nsize, &rangesize);
		if (ret == -FDT_ERR_NOTFOUND) {
			// FIXME: It is assumed that if the beginning of the reg is not
			// in physaddrmap, then none of it is.

			map_guest_addr_range(guest, gaddr + offset,
			                     gaddr + offset, size - offset);
			return 0;
		}
		
		if (ret < 0)
			return ret;

		if (addrbuf[0] || addrbuf[1])
			return ERR_BADTREE;

		addr = ((uint64_t)addrbuf[2] << 32) | addrbuf[3];

		if (rangesize > size - offset)
			rangesize = size - offset;
		
		map_guest_addr_range(guest, gaddr + offset, addr, rangesize);
		offset += rangesize;
	}

	return 0;
}

static int map_guest_ranges(guest_t *guest, int node, int partition)
{
	int len, ret;
	uint32_t naddr, nsize, caddr, csize;
	char path[MAX_PATH];

	const uint32_t *reg = fdt_getprop(guest->devtree, node,
	                                  "fsl,hv-map-ranges", &len);
	if (!reg) {
		if (len == -FDT_ERR_NOTFOUND)
			return 0;
	
		return len;
	}

	reg = fdt_getprop(guest->devtree, node, "ranges", &len);
	if (!reg) {
		if (len == -FDT_ERR_NOTFOUND)
			return 0;
	
		return len;
	}

	if (len & 3) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
		         "Unaligned ranges length %d\n", len);
		return ERR_BADTREE;
	}

	len >>= 2;

	ret = fdt_get_path(guest->devtree, node, path, sizeof(path));
	if (ret >= 0)
		printlog(LOGTYPE_PARTITION, LOGLEVEL_DEBUG,
		         "found ranges in %s\n", path);

	int parent = fdt_parent_offset(guest->devtree, node);
	if (parent < 0)
		return parent;

	ret = get_addr_format_nozero(guest->devtree, parent, &naddr, &nsize);
	if (ret < 0)
		return ret;

	ret = get_addr_format_nozero(guest->devtree, node, &caddr, &csize);
	if (ret < 0)
		return ret;

	for (int i = 0; i < len; i += caddr + naddr + csize) {
		if (i + caddr + naddr + csize > len) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
			         "Incomplete ranges entry\n");
			return ERR_BADTREE;
		}

		ret = map_guest_reg_one(guest, node, partition,
		                        reg + i + caddr, naddr, csize);
		if (ret < 0 && ret != ERR_NOTRANS)
			return ret;
	}

	return 0;
}

static int map_guest_reg(guest_t *guest, int node, int partition)
{
	int len, ret;
	uint32_t naddr, nsize;
	char path[MAX_PATH];

	const uint32_t *reg = fdt_getprop(guest->devtree, node, "reg", &len);
	if (!reg) {
		if (len == -FDT_ERR_NOTFOUND)
			return 0;
	
		return len;
	}

	if (len & 3)
		return ERR_BADTREE;

	len >>= 2;

	ret = fdt_get_path(guest->devtree, node, path, sizeof(path));
	if (ret >= 0)
		printlog(LOGTYPE_PARTITION, LOGLEVEL_DEBUG, "found reg in %s\n", path);

	int parent = fdt_parent_offset(guest->devtree, node);
	if (parent < 0)
		return parent;

	if (parent) {
		ret = fdt_node_check_compatible(guest->devtree, parent, "simple-bus");
		if (ret)
			return 0;
	}

	ret = get_addr_format_nozero(guest->devtree, parent, &naddr, &nsize);
	if (ret < 0)
		return ret;

	for (int i = 0; i < len; i += naddr + nsize) {
		if (i + naddr + nsize > len) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
			         "Incomplete reg entry\n");
			return ERR_BADTREE;
		}

		ret = map_guest_reg_one(guest, node, partition,
		                        reg + i, naddr, nsize);
		if (ret < 0 && ret != ERR_NOTRANS)
			return ret;
	}

	return 0;
}

static int map_guest_reg_all(guest_t *guest, int partition)
{
	int node = -1;

	while ((node = fdt_next_node(guest->devtree, node, NULL)) >= 0) {
		int ret = map_guest_reg(guest, node, partition);
		if (ret < 0) {
			int len;
			const char *node_name = fdt_get_name(guest->devtree, node, &len);
			if (node_name)
				printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
				         "error: map_guest_reg failed for node %s\n", node_name);
			return ret;
		}

		ret = map_guest_ranges(guest, node, partition);
		if (ret < 0) {
			int len;
			const char *node_name = fdt_get_name(guest->devtree, node, &len);
			if (node_name)
				printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
				         "map_guest_ranges failed for node %s\n", node_name);
			return ret;
		}
	}
	
	if (node != -FDT_ERR_NOTFOUND)
		return node;

	return 0;
}

static void reset_spintbl(guest_t *guest)
{
	struct boot_spin_table *spintbl = guest->spintbl;
	int i;

	for (i = 0; i < guest->cpucnt; i++) {
		spintbl[i].addr_hi = 0;
		spintbl[i].addr_lo = 1;
		spintbl[i].pir = i;
		spintbl[i].r3_hi = 0;
		spintbl[i].r3_lo = 0;
	}
}

static const char *cpu_clocks[] = {
	"clock-frequency",
	"timebase-frequency",
	"bus-frequency",
};

static int copy_cpu_clocks(guest_t *guest, int vnode, int vcpu,
                           const uint32_t *cpulist, int cpulist_len)
{
	int pcpu, node;
	const uint32_t *prop;
	int i;
	
	pcpu = vcpu_to_cpu(cpulist, cpulist_len, vcpu);
	if (pcpu < 0) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "partition %s has no cpu %d\n", guest->name, vcpu);
		return pcpu;
	}

	node = get_cpu_node(fdt, pcpu);
	if (node < 0) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "partition %s vcpu %d maps to non-existent CPU %d\n",
		         guest->name, vcpu, pcpu);
		return node;
	}

	for (i = 0; i < sizeof(cpu_clocks) / sizeof(char *); i++) {
		int ret, len;

		prop = fdt_getprop(fdt, node, cpu_clocks[i], &len);
		if (!prop) {
			printlog(LOGTYPE_MISC, LOGLEVEL_ERROR,
			         "copy_cpu_clocks: failed to read clock property, "
			         "error %d\n", len);

			return len;
		}

		ret = fdt_setprop(guest->devtree, vnode, cpu_clocks[i], prop, len);
		if (ret < 0) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "copy_cpu_clocks: failed to set clock property, "
			         "error %d\n", ret);

			return len;
		}
	}

	return 0;
} 

static int create_guest_spin_table(guest_t *guest,
                                   const uint32_t *cpulist,
                                   int cpulist_len)
{
	unsigned long rpn;
	uint64_t spin_addr;
	int ret, i;
	
	guest->spintbl = alloc(PAGE_SIZE, PAGE_SIZE);
	if (!guest->spintbl)
		return ERR_NOMEM;

	/* FIXME: hardcoded cache line size */
	for (i = 0; i < guest->cpucnt * sizeof(struct boot_spin_table); i += 32)
		asm volatile("dcbf 0, %0" : : "r" ((unsigned long)guest->spintbl + i ) :
		             "memory");

	rpn = virt_to_phys(guest->spintbl) >> PAGE_SHIFT;

	vptbl_map(guest->gphys, 0xfffff, rpn, 1, PTE_ALL, PTE_PHYS_LEVELS);
	vptbl_map(guest->gphys_rev, rpn, 0xfffff, 1, PTE_ALL, PTE_PHYS_LEVELS);

	int off = 0;
	while (1) {
		uint32_t pcpu;
		ret = fdt_node_offset_by_prop_value(guest->devtree, off,
		                                    "device_type", "cpu", 4);
		if (ret == -FDT_ERR_NOTFOUND)
			break;
		if (ret < 0)
			goto fail;

		off = ret;
		const uint32_t *reg = fdt_getprop(guest->devtree, off, "reg", &ret);
		if (!reg)
			goto fail;
		if (ret != 4) {
			printlog(LOGTYPE_MP, LOGLEVEL_ERROR,
			         "create_guest_spin_table(%s): bad cpu reg property\n",
			       guest->name);
			return ERR_BADTREE;
		}

		pcpu = *reg;
		ret = copy_cpu_clocks(guest, off, pcpu, cpulist, cpulist_len);
		if (ret < 0)
			continue;

		if (pcpu == 0) {
			ret = fdt_setprop_string(guest->devtree, off, "status", "okay");
			if (ret < 0)
				goto fail;
			
			continue;
		}

		ret = fdt_setprop_string(guest->devtree, off, "status", "disabled");
		if (ret < 0)
			goto fail;

		ret = fdt_setprop_string(guest->devtree, off,
		                         "enable-method", "spin-table");
		if (ret < 0)
			goto fail;

		spin_addr = 0xfffff000 + pcpu * sizeof(struct boot_spin_table);
		ret = fdt_setprop(guest->devtree, off,
		                  "cpu-release-addr", &spin_addr, 8);
		if (ret < 0)
			goto fail;

		printlog(LOGTYPE_MP, LOGLEVEL_DEBUG,
		         "cpu-release-addr of CPU%u: %llx\n", pcpu, spin_addr);
	}

	return 0;

fail:
	printlog(LOGTYPE_MP, LOGLEVEL_ERROR,
	         "create_guest_spin_table(%s): libfdt error %d (%s)\n",
	       guest->name, ret, fdt_strerror(ret));

	return ret;
}

// FIXME: we're hard-coding the phys address of flash here, it should
// read from the DTS instead.
#define FLASH_ADDR      0xe8000000
#define FLASH_SIZE      (128 * 1024 * 1024)

/*
 * Map flash into memory and return a hypervisor virtual address for the
 * given physical address.
 *
 * @phys: physical address inside flash space
 *
 * The TLB entry created by this function is temporary.
 * FIXME: implement a more generalized I/O mapping mechanism.
 */
static void *map_flash(phys_addr_t phys)
{
	/* Make sure 'phys' points to flash */
	if ((phys < FLASH_ADDR) || (phys > (FLASH_ADDR + FLASH_SIZE - 1))) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "%s: phys addr %llx is out of range\n", __FUNCTION__, phys);
		return NULL;
	}

	/*
	 * There is no permanent TLB entry for flash, so we create a temporary
	 * one here. TEMPTLB2/3 are slots reserved for temporary mappings.  We
	 * can't use TEMPTLB1, because map_gphys() is using that one.
	 */

	tlb1_set_entry(TEMPTLB2, (unsigned long)temp_mapping[1],
	               FLASH_ADDR, TLB_TSIZE_64M, TLB_MAS2_MEM,
	               TLB_MAS3_KERN, 0, 0, TLB_MAS8_HV);
	tlb1_set_entry(TEMPTLB3, (unsigned long)temp_mapping[1] + 64 * 1024 * 1024,
	               FLASH_ADDR, TLB_TSIZE_64M, TLB_MAS2_MEM,
	               TLB_MAS3_KERN, 0, 0, TLB_MAS8_HV);

	return temp_mapping[1] + (phys - FLASH_ADDR);
}

/**
 * create_sdbell_handle - craete the receive handles for a special doorbell
 * @kind - the doorbell name
 * @guest - guest device tree
 * @offset - pointer to the partition node in the guest device tree
 * @dbell - pointer to special doorbell to use
 *
 * This function creates a doorbell receive handle node (under the partition
 * node for each managed partition in a the manager's device tree) for a
 * particular special doorbell.
 *
 * 'kind' is a string that is used to both name the special doorbell and to
 * create the compatible property for the receive handle node.
 *
 * This function must be called after recv_dbell_partition_init() is called,
 * because it creates receive doorbell handles that do not have an fsl,endpoint
 * property.  recv_dbell_partition_init() considers receive doorbell handle
 * nodes without and endpoint to be an error.  And endpoint is required in
 * doorbell handle nodes only when the doorbell is defined in the hypervisor
 * DTS.  Special doorbells are created by the hypervisor in
 * create_guest_special_doorbells(), so they don't exist in any DTS.
 */
static int create_sdbell_handles(const char *kind,
	guest_t *guest,	int offset, struct ipi_doorbell *dbell)
{
	char s[96];	// Should be big enough
	int ret, length;

	// Create the special doorbell receive handle node.
	offset = ret = fdt_add_subnode(guest->devtree, offset, kind);
	if (ret < 0) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			"Couldn't create %s doorbell node: %i\n", kind, ret);
		return ret;
	}
	// 'offset' is now the offset of the new doorbell receive handle node

	// Write the 'compatible' property to the doorbell receive handle node
	// We can't embed a \0 in the format string, because that will confuse
	// snprintf (it will stop scanning when it sees the \0), so we use %c.
	length = snprintf(s, sizeof(s),
		"fsl,hv-%s-doorbell%cfsl,hv-doorbell-receive-handle", kind, 0);
	ret = fdt_setprop(guest->devtree, offset, "compatible", s, length + 1);
	if (ret < 0) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			"Couldn't set 'compatible' property in %s doorbell node: %i\n",
			kind, ret);
		return ret;
	}

	ret = attach_receive_doorbell(guest, dbell, offset);

	return ret;
}

/**
 * create_guest_special_doorbells - create the special doorbells for this guest
 *
 * Each guest gets a number of special doorbells.  These doorbells are run
 * when certain specific events occur, and the manager needs to be notified.
 *
 * For simplicity, these doorbells are always created, even if there is no
 * manager.  The doorbells will still be rung when the corresponding event
 * occurs, but no interrupts will be sent.
 */
static int create_guest_special_doorbells(guest_t *guest)
{
	assert(!guest->dbell_state_change);

	guest->dbell_state_change = alloc_type(ipi_doorbell_t);
	if (!guest->dbell_state_change) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			"%s: out of memory\n", __func__);
		goto error;
	}

	guest->dbell_watchdog_expiration = alloc_type(ipi_doorbell_t);
	if (!guest->dbell_watchdog_expiration) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			"%s: out of memory\n", __func__);
		goto error;
	}

	guest->dbell_restart_request = alloc_type(ipi_doorbell_t);
	if (!guest->dbell_restart_request) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			"%s: out of memory\n", __func__);
		goto error;
	}

	return 0;

error:
	free(guest->dbell_restart_request);
	free(guest->dbell_watchdog_expiration);
	free(guest->dbell_restart_request);

	guest->dbell_restart_request = NULL;
	guest->dbell_watchdog_expiration = NULL;
	guest->dbell_restart_request = NULL;

	return -ERR_NOMEM;
}

/**
 * Load a binary or ELF image into guest memory
 *
 * @guest: guest data structure
 * @image_phys: real physical address of the image
 * @guest_phys: guest physical address to load the image to
 * @length: size of the image, can be -1 if image is an ELF
 *
 * If the image is a plain binary, then 'length' must be the exact size of
 * the image.
 *
 * If the image is an ELF, then 'length' is used only to verify the image
 * data.  To skip verification, set length to -1.
 */
static int load_image_from_flash(guest_t *guest, phys_addr_t image_phys,
                                 phys_addr_t guest_phys, size_t length,
                                 register_t *entry)
{
	int ret;
	void *image = map_flash(image_phys);
	if (!image) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "guest %s: image source address %llx not in flash\n",
		       guest->name, image_phys);
		return ERR_BADTREE;
	}

	if (is_elf(image)) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
		         "guest %s: loading ELF image from flash\n", guest->name);
		return load_elf(guest, image, length, guest_phys, entry);
	}

	ret = load_uimage(guest, image, length, guest_phys, entry);
	if (ret != ERR_UNHANDLED)
		return ret;

	/* Neither an ELF image nor uImage, so it must be a binary. */

	if (!length || (length == (size_t) -1)) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "guest %s: missing or invalid image size\n",
			guest->name);
		return ERR_BADTREE;
	}

	printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
	         "guest %s: loading binary image from flash\n", guest->name);

	if (copy_to_gphys(guest->gphys, guest_phys, image, length) != length)
		return ERR_BADADDR;

	if (entry)
		*entry = guest_phys;

	return 0;
}

/**
 * If an ELF or binary image exists, load it.  This must be called after
 * map_guest_reg_all(), because that function creates some of the TLB
 * mappings we need.
 *
 * Returns 1 if the image is loaded successfully, 0 if no image, negative on
 * error.
 */
static int load_image(guest_t *guest)
{
	int node = guest->partition;
	int ret, size, first = 1;
	const uint32_t *prop, *end;
	uint64_t image_addr;
	uint64_t guest_addr;
	uint64_t length;

	prop = fdt_getprop(fdt, node, "fsl,hv-load-image-table", &size);
	if (!prop) {
		/* 'size' will never equal zero if table is NULL */
		if (size == -FDT_ERR_NOTFOUND)
			return 0;

		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "guest %s: could not read fsl,hv-load-image-table property\n",
		         guest->name);
		return size;
	}

	end = (const uint32_t *)((uintptr_t)prop + size);

	while (prop + rootnaddr + guest->naddr + guest->nsize <= end) {
		image_addr = int_from_tree(&prop, rootnaddr);
		guest_addr = int_from_tree(&prop, guest->naddr);
		length = int_from_tree(&prop, guest->nsize);

		if (length != (size_t)length) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "load_image: guest %s: invalid length %#llx\n",
			         guest->name, length);
			continue;
		}

		ret = load_image_from_flash(guest, image_addr, guest_addr,
		                            length ? length : -1,
		                            first ? &guest->entry : NULL);
		if (ret < 0) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "guest %s: could not load image\n", guest->name);
			return ret;
		}

		first = 0;
	}
	
	return 1;
}
		
static const uint32_t hv_version[4] = {
	CONFIG_HV_MAJOR_VERSION,
	CONFIG_HV_MINOR_VERSION,
	FH_API_VERSION,
	FH_API_COMPAT_VERSION
};

static int process_partition_handles(guest_t *guest)
{
	int off = -1;
	int ret;

	// Add a 'reg' property to every partition-handle node

	while (1) {
		// get a pointer to the first/next partition-handle node
		off = fdt_node_offset_by_compatible(guest->devtree, off,
		                                    "fsl,hv-partition-handle");
		if (off < 0) {
			if (off == -FDT_ERR_NOTFOUND)
				return 0;

			return off;
		}

		// Find the end-point partition node in the hypervisor device tree

		const char *s = fdt_getprop(guest->devtree, off, "fsl,endpoint", &ret);
		if (!s) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "guest %s: partition node missing fsl,endpoint (%d)\n",
			         guest->name, ret);
			continue;
		}

		int endpoint = fdt_path_offset(fdt, s);
		if (endpoint < 0) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "guest %s: partition %s does not exist (%d)\n",
			         guest->name, s, endpoint);
			continue;
		}

		// Get the guest_t for the partition, or create one if necessary
		guest_t *target_guest = node_to_partition(endpoint);
		if (!target_guest) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "guest %s: %s is not a partition\n",
			         guest->name, s);
			continue;
		}

		// Store the pointer to the target guest in our list of handles
		target_guest->handle.guest = target_guest;

		// Allocate a handle
		int32_t ghandle = alloc_guest_handle(guest, &target_guest->handle);
		if (ghandle < 0) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "guest %s: too many handles\n", guest->name);
			return ghandle;
		}

		// Insert a 'reg' property into the partition-handle node of the
		// guest device tree
		ret = fdt_setprop(guest->devtree, off, "reg", &ghandle, sizeof(ghandle));
		if (ret)
			return ret;

		ret = fdt_setprop(guest->devtree, off, "label",
		                  target_guest->name, strlen(target_guest->name) + 1);
		if (ret)
			return ret;

		// Create special doorbells

		ret = create_sdbell_handles("state-change",
			guest, off, target_guest->dbell_state_change);
		if (ret)
			return ret;

		ret = create_sdbell_handles("watchdog-expiration",
			guest, off, target_guest->dbell_watchdog_expiration);
		if (ret)
			return ret;

		ret = create_sdbell_handles("reset-request",
			guest, off, target_guest->dbell_restart_request);
		if (ret)
			return ret;
	}

	return 0;
}

static int register_gcpu_with_guest(guest_t *guest, const uint32_t *cpus,
                                    int len)
{
	int gpir = get_gcpu_num(cpus, len, mfspr(SPR_PIR));
	assert(gpir >= 0);

	while (!guest->gcpus)
		barrier();
	
	guest->gcpus[gpir] = get_gcpu();
	get_gcpu()->gcpu_num = gpir;
	return gpir;
}

int restart_guest(guest_t *guest)
{
	int ret = 0;
	unsigned int i;

	spin_lock(&guest->state_lock);

	if (guest->state != guest_running)
		ret = ERR_INVALID;
	else
		guest->state = guest_stopping;

	spin_unlock(&guest->state_lock);

	if (!ret)
		for (i = 0; i < guest->cpucnt; i++)
			setgevent(guest->gcpus[i], GEV_RESTART);

	return ret;
}

/* Process configuration options in the hypervisor's
 * chosen and hypervisor nodes.
 */
static int partition_config(guest_t *guest)
{
	const uint32_t *prop;
	int hv, ret, len;
	
	hv = fdt_subnode_offset(guest->devtree, 0, "hypervisor");
	if (hv < 0)
		hv = fdt_add_subnode(guest->devtree, 0, "hypervisor");
	if (hv < 0) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "Couldn't create hypervisor node: %d\n", hv);
		return hv;
	}

	/* guest cache lock mode */
	prop = fdt_getprop(guest->devtree, hv, "fsl,hv-guest-cache-lock", &len);
	if (prop)
		guest->guest_cache_lock = 1;

	/* guest debug mode */
	prop = fdt_getprop(guest->devtree, hv, "fsl,hv-guest-debug", &len);
	if (prop)
		guest->guest_debug_mode = 1;

	if (mpic_coreint) {
		ret = fdt_setprop(guest->devtree, hv,
		                  "fsl,hv-pic-coreint", NULL, 0);
		if (ret < 0)
			return ret;
	}

	ret = fdt_setprop(guest->devtree, hv, "fsl,hv-partition-label",
	                  guest->name, strlen(guest->name) + 1);
	if (ret < 0)
		return ret;

	return 0;
}

uint32_t start_guest_lock;

guest_t *node_to_partition(int partition)
{
	int i, len, ret;
	char *name;

	// Verify that 'partition' points to a compatible node
	if (fdt_node_check_compatible(fdt, partition, "fsl,hv-partition")) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "%s: invalid offset %u\n", __FUNCTION__, partition);
		return NULL;
	}

	spin_lock(&start_guest_lock);
	
	for (i = 0; i < last_lpid; i++) {
		assert(guests[i].lpid == i + 1);
		if (guests[i].partition == partition)
			break;
	}
	
	if (i == last_lpid) {
		if (last_lpid >= MAX_PARTITIONS) {
			spin_unlock(&start_guest_lock);
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "node_to_partition: too many partitions\n");
			return NULL;
		}

		// We create the special doorbells here, instead of in
		// init_guest_primary(), because it guarantees that the
		// doorbells will be created for all partitions before the
		// managers start looking for their managed partitions.
		ret = create_guest_special_doorbells(&guests[i]);
		if (ret < 0) {
			spin_unlock(&start_guest_lock);
			return NULL;
		}

		name = fdt_getprop_w(fdt, partition, "label", &len);
		if (len > 0) {
			name[len - 1] = 0;
		} else {
			/* If no label, use the partition node path. */
			name = malloc(MAX_PATH);
			if (!name) {
				spin_unlock(&start_guest_lock);
				printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
				         "node_to_partition: out of memory\n");
				return NULL;
			}

			ret = fdt_get_path(fdt, partition, name, MAX_PATH);
			if (ret >= 0) {
				name[MAX_PATH - 1] = 0;
			} else {
				printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
				         "node_to_partition: Couldn't get partition node path.\n");
				sprintf(name, "partition %d", i);
			}
		}

		guests[i].name = name;
		guests[i].state = guest_starting;
		guests[i].partition = partition;
		guests[i].lpid = ++last_lpid;
	}
	
	spin_unlock(&start_guest_lock);
	return &guests[i];
}

static void guest_core_init(guest_t *guest)
{
	register_t msrp = 0;

	/* Reset the timer control register, reset the watchdog state, and
	 * clear all pending timer interrupts.  This ensures that timers won't
	 * carry over a partition restart.
	 */
	mtspr(SPR_TCR, 0);
	mtspr(SPR_TSR, TSR_ENW | TSR_DIS | TSR_FIS | TSR_WIS);

	mtspr(SPR_MAS5, MAS5_SGS | guest->lpid);
	mtspr(SPR_PID, 0);

	if (!guest->guest_cache_lock)
		msrp |= MSRP_UCLEP;
	if (!guest->guest_debug_mode)
		msrp |= MSRP_DEP;

	mtspr(SPR_MSRP, msrp);
}

static void start_guest_primary_nowait(void)
{
	register register_t r3 asm("r3");
	register register_t r4 asm("r4");
	register register_t r5 asm("r5");
	register register_t r6 asm("r6");
	register register_t r7 asm("r7");
	register register_t r8 asm("r8");
	register register_t r9 asm("r9");
	guest_t *guest = get_gcpu()->guest; 
	int i;
	
	disable_critint();

	if (cpu->ret_user_hook)
		return;

	assert(guest->state == guest_starting);

	guest_core_init(guest);
	reset_spintbl(guest);

#ifdef CONFIG_GDB_STUB
	gdb_stub_init();
#endif

	/* FIXME: append device tree to image, or use address provided by
	 * the device tree, or something sensible like that.
	 */
	int ret = copy_to_gphys(guest->gphys, guest->dtb_gphys,
	                        guest->devtree, fdt_totalsize(guest->devtree));
	if (ret != fdt_totalsize(guest->devtree)) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "Couldn't copy device tree to guest %s, %d\n",
		         guest->name, ret);
		return;
	}

	guest_set_tlb1(0, MAS1_VALID | (TLB_TSIZE_1G << MAS1_TSIZE_SHIFT) |
	                  MAS1_IPROT, 0, 0, TLB_MAS2_MEM, TLB_MAS3_KERN);

	printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
	         "branching to guest %s, %d cpus\n", guest->name, guest->cpucnt);
	guest->active_cpus = guest->cpucnt;
	guest->state = guest_running;
	smp_mbar();
	send_doorbells(guest->dbell_state_change);

	for (i = 1; i < guest->cpucnt; i++) {
		if (!guest->gcpus[i]) {
			enable_critint();
			printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
			         "guest %s waiting for cpu %d...\n", guest->name, i);
		
			while (!guest->gcpus[i]) {
				if (cpu->ret_user_hook)
					break;

				barrier();
			}

			disable_critint();

			if (cpu->ret_user_hook)
				return;
		}

		setgevent(guest->gcpus[i], GEV_START);
	}

	cpu->traplevel = 0;

	if (fdt_get_property(fdt, guest->partition, "fsl,hv-dbg-wait-at-start", NULL)
		&& guest->stub_ops && guest->stub_ops->wait_at_start_hook)
		guest->stub_ops->wait_at_start_hook(guest->entry, MSR_GS);

	// FIXME: This isn't exactly ePAPR compliant.  For starters, we only
	// map 1GiB, so we don't support loading/booting an OS above that
	// address.  Also, we pass the guest physical address even though it
	// should be a guest virtual address, but since we program the TLBs
	// such that guest virtual == guest physical at boot time, this works. 

	r3 = guest->dtb_gphys;
	r4 = 0;
	r5 = 0;
	r6 = 0x45504150; // ePAPR Magic for Book-E
	r7 = 1 << 30;  // 1GB - This must match the TLB_TSIZE_xx value above
	r8 = 0;
	r9 = 0;

	asm volatile("mtsrr0 %0; mtsrr1 %1; rfi" : :
		     "r" (guest->entry), "r" (MSR_GS),
		     "r" (r3), "r" (r4), "r" (r5), "r" (r6), "r" (r7),
		     "r" (r8), "r" (r9)
		     : "memory");

	BUG();
}

static void start_guest_primary(void)
{
	guest_t *guest = get_gcpu()->guest; 
	int ret;

	enable_critint();

	if (cpu->ret_user_hook)
		return;

	assert(guest->state == guest_starting);

	ret = load_image(guest);
	if (ret <= 0) {
		guest->state = guest_stopped;

		/* No hypervisor-loadable image; wait for a manager to start us. */
		printlog(LOGTYPE_PARTITION, LOGLEVEL_DEBUG,
		         "Guest %s waiting for manager start\n", guest->name);

		// Notify the manager(s) that it needs to load images and
		// start this guest.
		send_doorbells(guest->dbell_restart_request);

		return;
	}

	start_guest_primary_nowait();
}

static void start_guest_secondary(void)
{
	register register_t r3 asm("r3");
	register register_t r4 asm("r4");
	register register_t r5 asm("r5");
	register register_t r6 asm("r6");
	register register_t r7 asm("r7");
	register register_t r8 asm("r8");
	register register_t r9 asm("r9");

	unsigned long page;
	gcpu_t *gcpu = get_gcpu();
	guest_t *guest = gcpu->guest;
	int pir = mfspr(SPR_PIR);
	int gpir = gcpu->gcpu_num;

	enable_critint();
		
	mtspr(SPR_GPIR, gpir);

	printlog(LOGTYPE_MP, LOGLEVEL_DEBUG,
	         "cpu %d/%d spinning on table...\n", pir, gpir);

	while (guest->spintbl[gpir].addr_lo & 1) {
		asm volatile("dcbi 0, %0" : : "r" (&guest->spintbl[gpir]) : "memory");
		asm volatile("dcbi 0, %0" : : "r" (&guest->spintbl[gpir + 1]) : "memory");
		smp_mbar();

		if (cpu->ret_user_hook)
			break;
	}

	disable_critint();
	if (cpu->ret_user_hook)
		return;

	guest_core_init(guest);

#ifdef CONFIG_GDB_STUB
	gdb_stub_init();
#endif
	printlog(LOGTYPE_MP, LOGLEVEL_DEBUG,
	         "secondary %d/%d spun up, addr %lx\n",
	         pir, gpir, guest->spintbl[gpir].addr_lo);

	if (guest->spintbl[gpir].pir != gpir)
		printlog(LOGTYPE_MP, LOGLEVEL_ERROR,
		         "WARNING: cpu %d (guest cpu %d) changed spin-table "
		         "PIR to %ld, ignoring\n",
		         pir, gpir, guest->spintbl[gpir].pir);

	/* Mask for 256M mapping */
	page = ((((uint64_t)guest->spintbl[gpir].addr_hi << 32) |
		guest->spintbl[gpir].addr_lo) & ~0xfffffff) >> PAGE_SHIFT;

	guest_set_tlb1(0, MAS1_VALID | (TLB_TSIZE_1G << MAS1_TSIZE_SHIFT) |
	                  MAS1_IPROT, page, page, TLB_MAS2_MEM, TLB_MAS3_KERN);

	if (fdt_get_property(fdt, guest->partition, "fsl,hv-dbg-wait-at-start", NULL)
		&& guest->stub_ops && guest->stub_ops->wait_at_start_hook)
		guest->stub_ops->wait_at_start_hook(guest->entry, MSR_GS);

	r3 = guest->spintbl[gpir].r3_lo;   // FIXME 64-bit
	r4 = 0;
	r5 = 0;
	r6 = 0;
	r7 = 1 << 30;  // 1GB - This must match the TLB_TSIZE_xx value above
	r8 = 0;
	r9 = 0;

	cpu->traplevel = 0;
	asm volatile("mtsrr0 %0; mtsrr1 %1; rfi" : :
	             "r" (guest->spintbl[gpir].addr_lo), "r" (MSR_GS),
	             "r" (r3), "r" (r4), "r" (r5), "r" (r6), "r" (r7),
		     "r" (r8), "r" (r9)
	             : "memory");

	BUG();
}

void start_core(trapframe_t *regs)
{
	printlog(LOGTYPE_MP, LOGLEVEL_DEBUG, "start core %lu\n", mfspr(SPR_PIR));

	gcpu_t *gcpu = get_gcpu();
	guest_t *guest = gcpu->guest;

	if (gcpu == guest->gcpus[0]) {
		assert(guest->state == guest_starting);
		start_guest_primary_nowait();
	} else {
		assert(guest->state == guest_running);
		start_guest_secondary();
	}

	wait_for_gevent(regs);
}

void start_wait_core(trapframe_t *regs)
{
	printlog(LOGTYPE_MP, LOGLEVEL_DEBUG,
	         "start wait core %lu\n", mfspr(SPR_PIR));

	gcpu_t *gcpu = get_gcpu();
	guest_t *guest = gcpu->guest;
	assert(gcpu == guest->gcpus[0]);
	assert(guest->state == guest_starting);

	start_guest_primary();

	assert(guest->state != guest_running);
	wait_for_gevent(regs);
}

void do_stop_core(trapframe_t *regs, int restart)
{
	int i;
	printlog(LOGTYPE_MP, LOGLEVEL_DEBUG,
	         "%s core %lu\n", restart ? "restart" : "stop", mfspr(SPR_PIR));

	gcpu_t *gcpu = get_gcpu();
	guest_t *guest = gcpu->guest;
	assert(guest->state == guest_stopping);

	guest_reset_tlb();

	if (atomic_add(&guest->active_cpus, -1) == 0) {
		for (i = 0; i < MAX_HANDLES; i++) {
			handle_t *h = guest->handles[i];

			if (h && h->ops && h->ops->reset)
				h->ops->reset(h);
		}

		if (restart) {
			guest->state = guest_starting;
			setgevent(guest->gcpus[0], GEV_START_WAIT);
		} else {
			guest->state = guest_stopped;
			send_doorbells(guest->dbell_state_change);
		}
	}

	mpic_reset_core();
	memset(&gcpu->gdbell_pending, 0,
	       sizeof(gcpu_t) - offsetof(gcpu_t, gdbell_pending));

	wait_for_gevent(regs);
}

void stop_core(trapframe_t *regs)
{
	do_stop_core(regs, 0);
}

int stop_guest(guest_t *guest)
{
	unsigned int i, ret = 0;
	spin_lock(&guest->state_lock);

	if (guest->state != guest_running)
		ret = ERR_INVALID;
	else
		guest->state = guest_stopping;

	spin_unlock(&guest->state_lock);
	
	if (ret)
		return ret;

	for (i = 0; i < guest->cpucnt; i++)
		setgevent(guest->gcpus[i], GEV_STOP);

	return ret;
}

int start_guest(guest_t *guest)
{
	int ret = 0;
	spin_lock(&guest->state_lock);

	if (guest->state != guest_stopped)
		ret = ERR_INVALID;
	else
		guest->state = guest_starting;

	spin_unlock(&guest->state_lock);
	
	if (ret)
		return ret;

	setgevent(guest->gcpus[0], GEV_START);
	return ret;
}

static int init_guest_primary(guest_t *guest, int partition,
                              const uint32_t *cpus, int cpus_len)
{
	int ret;
	uint32_t gfdt_size;
	const void *guest_origtree;
	const uint32_t *prop;
	int gpir;

	/* count number of cpus for this partition and alloc data struct */
	guest->cpucnt = count_cpus(cpus, cpus_len);
	guest->gcpus = alloc(sizeof(long) * guest->cpucnt, sizeof(long));
	if (!guest->gcpus) {
		ret = ERR_NOMEM;
		goto fail;
	}

	gpir = register_gcpu_with_guest(guest, cpus, cpus_len);
	assert(gpir == 0);

	guest_origtree = fdt_getprop(fdt, partition, "fsl,hv-dtb", &ret);
	if (!guest_origtree) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "guest missing property fsl,hv-dtb\n");
		goto fail;
	}

	ret = get_addr_format_nozero(guest_origtree, 0,
	                             &guest->naddr, &guest->nsize);
	if (ret < 0)
		goto fail;

	prop = fdt_getprop(fdt, partition, "fsl,hv-dtb-window", &ret);
	if (!prop) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "guest missing property fsl,hv-dtb-window\n");
		goto fail;
	}
	if (ret != (guest->naddr + guest->nsize) * 4) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "guest has invalid property len for fsl,hv-dtb-window\n");
		ret = ERR_BADTREE;
		goto fail;
	}

	guest->dtb_gphys = int_from_tree(&prop, guest->naddr);
	gfdt_size = int_from_tree(&prop, guest->nsize);

	gfdt_size += ret;
	
	guest->devtree = alloc(gfdt_size, 16);
	if (!guest->devtree) {
		ret = ERR_NOMEM;
		goto fail;
	}

	ret = fdt_open_into(guest_origtree, guest->devtree, gfdt_size);
	if (ret < 0)
		goto fail;

	ret = fdt_setprop(guest->devtree, 0, "fsl,hv-version",
	                  hv_version, 16);
	if (ret < 0)
		goto fail;

	guest->gphys = alloc(PAGE_SIZE * 2, PAGE_SIZE * 2);
	guest->gphys_rev = alloc(PAGE_SIZE * 2, PAGE_SIZE * 2);
	if (!guest->gphys || !guest->gphys_rev) {
		ret = ERR_NOMEM;
		goto fail;
	}

	ret = create_guest_spin_table(guest, cpus, cpus_len);
	if (ret < 0)
		goto fail;

#ifdef CONFIG_IPI_DOORBELL
	send_dbell_partition_init(guest);
	recv_dbell_partition_init(guest);
#endif

	ret = process_partition_handles(guest);
	if (ret < 0)
		goto fail;

	ret = partition_config(guest);
	if (ret < 0)
		goto fail;

#ifdef CONFIG_BYTE_CHAN
	byte_chan_partition_init(guest);
#endif

	vmpic_partition_init(guest);

	ret = map_guest_reg_all(guest, partition);
	if (ret < 0)
		goto fail;

#ifdef CONFIG_PAMU
	pamu_partition_init(guest);
#endif

	// Get the watchdog timeout options
	prop = fdt_getprop(fdt, guest->partition, "fsl,hv-wd-mgr-notify", NULL);
	guest->wd_notify = !!prop;

	start_guest_primary();
	return 0;

fail:
	printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
	         "error %d (%s) building guest device tree for %s\n",
	         ret, fdt_strerror(ret), guest->name);

	return ret;
}

__attribute__((noreturn)) void init_guest(void)
{
	int off = -1, partition = 0, ret;
	int pir = mfspr(SPR_PIR);
	const uint32_t *cpus = NULL, *prop;
	guest_t *guest = NULL, *temp_guest;
	int len, cpus_len = 0;
	
	while (1) {
		off = fdt_node_offset_by_compatible(fdt, off, "fsl,hv-partition");
		if (off < 0) {
			if (partition)
				break;

			goto wait;
		}

		prop = fdt_getprop(fdt, off, "fsl,hv-cpus", &len);
		if (!prop) {
			char buf[MAX_PATH];
			ret = fdt_get_path(fdt, off, buf, sizeof(buf));
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "No fsl,hv-cpus in guest %s\n", buf);
			continue;
		}

		if (!cpu_in_cpulist(prop, len, pir))
			continue;

		temp_guest = node_to_partition(off);
		if (!temp_guest)
			continue;

		if (partition) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "extra guest %s on core %d\n", temp_guest->name, pir);
			continue;
		}

		partition = off;
		guest = temp_guest;
		get_gcpu()->guest = guest;
		cpus = prop;
		cpus_len = len;

		mtspr(SPR_LPIDR, guest->lpid);
	
		printlog(LOGTYPE_PARTITION, LOGLEVEL_DEBUG,
		         "guest at %s on core %d\n", guest->name, pir);
	}

	if (pir == cpus[0]) {
		/* Boot CPU */
		init_guest_primary(guest, partition, cpus, cpus_len);
	} else {
		register_gcpu_with_guest(guest, cpus, cpus_len);
	}

wait:	{
		/* Like wait_for_gevent(), but without a
		 * stack frame to return on.
		 */
		register_t new_r1 = (register_t)&cpu->kstack[KSTACK_SIZE - FRAMELEN];

		/* Terminate the callback chain. */
		cpu->kstack[KSTACK_SIZE - FRAMELEN] = 0;

		get_gcpu()->waiting_for_gevent = 1;

		asm volatile("mtsrr0 %0; mtsrr1 %1; mr %%r1, %2; rfi" : :
		             "r" (wait_for_gevent_loop),
		             "r" (mfmsr() | MSR_CE),
		             "r" (new_r1) : "memory");
		BUG();
	}
} 

void *map_gphys(int tlbentry, pte_t *tbl, phys_addr_t addr,
                void *vpage, size_t *len, int maxtsize, int write)
{
	size_t offset, bytesize;
	unsigned long attr;
	unsigned long rpn;
	phys_addr_t physaddr;
	int tsize;

	rpn = vptbl_xlate(tbl, addr >> PAGE_SHIFT, &attr, PTE_PHYS_LEVELS);
	if (!(attr & PTE_VALID) || (attr & PTE_VF))
		return NULL;
	if (write & !(attr & PTE_UW))
		return NULL;

	tsize = attr >> PTE_SIZE_SHIFT;
	if (tsize > maxtsize)
		tsize = maxtsize;

	bytesize = (uintptr_t)tsize_to_pages(tsize) << PAGE_SHIFT;
	offset = addr & (bytesize - 1);
	physaddr = (phys_addr_t)rpn << PAGE_SHIFT;

	if (len)
		*len = bytesize - offset;

	tlb1_set_entry(tlbentry, (unsigned long)vpage, physaddr & ~(bytesize - 1),
	               tsize, TLB_MAS2_MEM, TLB_MAS3_KERN, 0, 0, TLB_MAS8_HV);

	return vpage + offset;
}

/** Copy from a hypervisor virtual address to a guest physical address
 *
 * @param[in] tbl Guest physical page table
 * @param[in] dest Guest physical address to copy to
 * @param[in] src Hypervisor virtual address to copy from
 * @param[in] len Bytes to copy
 * @return number of bytes successfully copied
 */
size_t copy_to_gphys(pte_t *tbl, phys_addr_t dest, void *src, size_t len)
{
	size_t ret = 0;

	while (len > 0) {
		size_t chunk;
		void *vdest;
		
		vdest = map_gphys(TEMPTLB1, tbl, dest, temp_mapping[0],
		                  &chunk, TLB_TSIZE_16M, 1);
		if (!vdest)
			break;

		if (chunk > len)
			chunk = len;

		memcpy(vdest, src, chunk);

		src += chunk;
		dest += chunk;
		ret += chunk;
		len -= chunk;
	}

	return ret;
}

/** Fill a block of guest physical memory with zeroes
 *
 * @param[in] tbl Guest physical page table
 * @param[in] dest Guest physical address to copy to
 * @param[in] len Bytes to zero
 * @return number of bytes successfully zeroed
 */
size_t zero_to_gphys(pte_t *tbl, phys_addr_t dest, size_t len)
{
	size_t ret = 0;

	while (len > 0) {
		size_t chunk;
		void *vdest;

		vdest = map_gphys(TEMPTLB1, tbl, dest, temp_mapping[0],
		                  &chunk, TLB_TSIZE_16M, 1);
		if (!vdest)
			break;

		if (chunk > len)
			chunk = len;

		memset(vdest, 0, chunk);

		dest += chunk;
		ret += chunk;
		len -= chunk;
	}

	return ret;
}


/** Copy from a guest physical address to a hypervisor virtual address 
 *
 * @param[in] tbl Guest physical page table
 * @param[in] dest Hypervisor virtual address to copy to
 * @param[in] src Guest physical address to copy from
 * @param[in] len Bytes to copy
 * @return number of bytes successfully copied
 */
size_t copy_from_gphys(pte_t *tbl, void *dest, phys_addr_t src, size_t len)
{
	size_t ret = 0;

	while (len > 0) {
		size_t chunk;
		void *vsrc;
		
		vsrc = map_gphys(TEMPTLB1, tbl, src, temp_mapping[0],
		                 &chunk, TLB_TSIZE_16M, 0);
		if (!vsrc)
			break;

		if (chunk > len)
			chunk = len;

		memcpy(dest, vsrc, chunk);

		src += chunk;
		dest += chunk;
		ret += chunk;
		len -= chunk;
	}

	return ret;
}

/** Copy from a guest physical address to another guest physical address
 *
 * @param[in] dtbl Guest physical page table of the destination
 * @param[in] dest Guest physical address to copy to
 * @param[in] stbl Guest physical page table of the source
 * @param[in] src Guest physical address to copy from
 * @param[in] len Bytes to copy
 * @return number of bytes successfully copied
 */
size_t copy_between_gphys(pte_t *dtbl, phys_addr_t dest,
                          pte_t *stbl, phys_addr_t src, size_t len)
{
	size_t schunk = 0, dchunk = 0, chunk, ret = 0;
	
	/* Initializiations not needed, but GCC is stupid. */
	void *vdest = NULL, *vsrc = NULL;

	while (len > 0) {
		if (!schunk) {
			vsrc = map_gphys(TEMPTLB1, stbl, src, temp_mapping[0],
			                 &schunk, TLB_TSIZE_16M, 0);
			if (!vsrc)
				break;
		}

		if (!dchunk) {
			vdest = map_gphys(TEMPTLB2, dtbl, dest, temp_mapping[1],
			                  &dchunk, TLB_TSIZE_16M, 1);
			if (!vdest)
				break;
		}

		chunk = min(schunk, dchunk);
		if (chunk > len)
			chunk = len;

		memcpy(vdest, vsrc, chunk);

		src += chunk;
		dest += chunk;
		ret += chunk;
		len -= chunk;
		dchunk -= chunk;
		schunk -= chunk;
	}

	return ret;
}

/* This is a hack to find an upper bound of the hypervisor's memory.
 * It assumes that the main memory segment of each partition will
 * be listed in its physaddr map, and that no shared memory region
 * appears below the lowest private guest memory.
 *
 * Once we have explicit coherency domains, we could perhaps include
 * one labelled for hypervisor use, and maybe one designated for dynamic
 * memory allocation.
 */
phys_addr_t find_lowest_guest_phys(void)
{
	int off = -1, ret;
	phys_addr_t low = (phys_addr_t)-1;
	const uint32_t *prop;
	const void *gtree;

	while (1) {
		uint32_t gnaddr, gnsize;
		int entry_len;
		int i = 0;
	
		off = fdt_node_offset_by_compatible(fdt, off, "fsl,hv-partition");
		if (off < 0)
			return low;

		gtree = fdt_getprop(fdt, off, "fsl,hv-dtb", &ret);
		if (!gtree)
			continue;

		ret = get_addr_format_nozero(gtree, 0, &gnaddr, &gnsize);
		if (ret < 0)
			continue;

		prop = fdt_getprop(fdt, off, "fsl,hv-physaddr-map", &ret);
		if (!prop)
			continue;
		
		entry_len = gnaddr + rootnaddr + gnsize;
		while (i + entry_len <= ret >> 2) {
			phys_addr_t real;
			
			real = prop[i + gnaddr];
			
			if (gnaddr == 2 && sizeof(phys_addr_t) > 4) {
				real <<= 32;
				real += prop[i + gnaddr + 1];
			}
			
			if (real < low)
				low = real;

			i += entry_len;
		}
	}
}
