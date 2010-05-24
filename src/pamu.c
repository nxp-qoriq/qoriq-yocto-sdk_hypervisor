/** peripheral access management unit (PAMU) support
 *
 * @file
 *
 */

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

#include <libos/pamu.h>
#include <libos/bitops.h>
#include <libos/percpu.h>
#include <libos/console.h>
#include <libos/alloc.h>
#include <libos/platform_error.h>

#include <pamu.h>
#include <percpu.h>
#include <errors.h>
#include <devtree.h>
#include <paging.h>
#include <events.h>
#include <ccm.h>
#include <cpc.h>
#include <limits.h>
#include <error_log.h>
#include <error_mgmt.h>

/* mcheck-safe lock, used to ensure atomicity of reassignment */
static uint32_t pamu_error_lock;

uint32_t liodn_to_handle[PAACE_NUMBER_ENTRIES];
static guest_t *liodn_to_guest[PAACE_NUMBER_ENTRIES];
static uint32_t pamu_lock;

static unsigned int map_addrspace_size_to_wse(phys_addr_t addrspace_size)
{
	assert(!(addrspace_size & (addrspace_size - 1)));

	/* window size is 2^(WSE+1) bytes */
	return count_lsb_zeroes(addrspace_size >> PAGE_SHIFT) + 11;
}

static unsigned int map_subwindow_cnt_to_wce(uint32_t subwindow_cnt)
{
	/* window count is 2^(WCE+1) bytes */
	return count_lsb_zeroes_32(subwindow_cnt) - 1;
}

static int is_subwindow_count_valid(int subwindow_cnt)
{
	if (subwindow_cnt <= 1 || subwindow_cnt > 16)
		return 0;
	if (subwindow_cnt & (subwindow_cnt - 1))
		return 0;
	return 1;
}

#define L1 1
#define L2 2
#define L3 3

/*
 * Given the stash-dest enumeration value and a hw node of the device
 * being configured, return the cache-stash-id property value for
 * the associated cpu.  Assumption is that a cpu-handle property
 * points to that cpu.
 *
 * stash-dest values in the config tree are defined as:
 *    1 : L1 cache
 *    2 : L2 cache
 *    3 : L3/CPC cache
 *
 */
static uint32_t get_stash_dest(uint32_t stash_dest, dt_node_t *hwnode)
{
	dt_prop_t *prop;
	dt_node_t *node;

	/* Fastpath, exit early if L3/CPC cache is target for stashing */
	if (stash_dest == L3) {
		node = dt_get_first_compatible(hw_devtree,
				"fsl,p4080-l3-cache-controller");
		if (node) {
			if (!cpcs_enabled()) {
				printlog(LOGTYPE_DEVTREE, LOGLEVEL_WARN,
					"%s: %s not enabled\n",
					__func__, node->name);
				return ~(uint32_t)0;
			}
			prop = dt_get_prop(node, "cache-stash-id", 0);
			if (!prop) {
				printlog(LOGTYPE_DEVTREE, LOGLEVEL_WARN,
					"%s: missing cache-stash-id in %s\n",
					__func__, node->name);
				return ~(uint32_t)0;
			}
			return *(const uint32_t *)prop->data;
		}
		return ~(uint32_t)0;
	}

	prop = dt_get_prop(hwnode, "cpu-handle", 0);
	if (!prop || prop->len != 4)
		return ~(uint32_t)0;  /* if no cpu-phandle assume that this is
			      not a per-cpu portal */

	node = dt_lookup_phandle(hw_devtree, *(const uint32_t *)prop->data);
	if (!node) {
		printlog(LOGTYPE_DEVTREE, LOGLEVEL_ERROR,
		         "%s: bad cpu phandle reference in %s \n",
		          __func__, hwnode->name);
		return ~(uint32_t)0;
	}

	/* find the hwnode that represents the cache */
	for (uint32_t cache_level = L1; cache_level <= L3; cache_level++) {
		if (stash_dest == cache_level) {
			prop = dt_get_prop(node, "cache-stash-id", 0);
			if (!prop || prop->len != 4) {
				printlog(LOGTYPE_DEVTREE, LOGLEVEL_ERROR,
				         "%s: missing/bad cache-stash-id at %s \n",
				          __func__, node->name);
				return ~(uint32_t)0;
			}
			return *(const uint32_t *)prop->data;
		}

		prop = dt_get_prop(node, "next-level-cache", 0);
		if (!prop || prop->len != 4) {
			printlog(LOGTYPE_DEVTREE, LOGLEVEL_ERROR,
			         "%s: can't find next-level-cache at %s \n",
			          __func__, node->name);
			return ~(uint32_t)0;  /* can't traverse any further */
		}

		/* advance to next node in cache hierarchy */
		node = dt_lookup_phandle(hw_devtree, *(const uint32_t *)prop->data);
		if (!node) {
			printlog(LOGTYPE_DEVTREE, LOGLEVEL_ERROR,
			         "%s: bad cpu phandle reference in %s \n",
			          __func__, hwnode->name);
			return ~(uint32_t)0;
		}
	}

	printlog(LOGTYPE_DEVTREE, LOGLEVEL_ERROR,
	         "%s: stash dest not found for %d on %s \n",
	          __func__, stash_dest, hwnode->name);
	return ~(uint32_t)0;
}

static uint32_t get_snoop_id(dt_node_t *gnode, guest_t *guest)
{
	dt_prop_t *prop;
	dt_node_t *node;

	prop = dt_get_prop(gnode, "cpu-handle", 0);
	if (!prop || prop->len != 4)
		return ~(uint32_t)0;

	node = dt_lookup_phandle(hw_devtree, *(const uint32_t *)prop->data);
	if (!node) {
		printlog(LOGTYPE_DEVTREE, LOGLEVEL_ERROR,
		         "%s: bad cpu phandle reference in %s \n",
		          __func__, gnode->name);
		return ~(uint32_t)0;
	}
	prop = dt_get_prop(node, "reg", 0);
	if (!prop || prop->len != 4) {
		printlog(LOGTYPE_DEVTREE, LOGLEVEL_ERROR,
		         "%s: bad reg in cpu node %s \n",
		          __func__, node->name);
		return ~(uint32_t)0;
	}
	return *(const uint32_t *)prop->data;
}

#define PEXIWBAR 0xDA8
#define PEXIWBEAR 0xDAC
#define PEXIWAR 0xDB0
#define PEXI_EN 0x80000000
#define PEXI_IWS 0x3F

static unsigned long setup_pcie_msi_subwin(guest_t *guest, dt_node_t *cfgnode,
                             dt_node_t *node, uint64_t gaddr,
                             uint64_t *size)
{
	int ret;
	dt_prop_t *prop;
	uint32_t phandle;
	uint32_t msi_bank_addr;
	char buf[32];
	uint32_t reg[2];
	dt_prop_t *regprop;
	uint64_t msi_addr = 0;
	dt_node_t *msi_gnode, *msi_node;
	unsigned long rpn = ULONG_MAX;
	uint8_t *pci_ctrl;
	phys_addr_t pcie_addr, pcie_size;

	prop = dt_get_prop(node, "fsl,msi", 0);
	if (prop) {
		int i;

		phandle = *(const uint32_t *)prop->data;
		msi_gnode = dt_lookup_phandle(guest->devtree, phandle);
		msi_node = dt_lookup_phandle(hw_devtree, phandle);

		if (!msi_gnode)
			return ULONG_MAX;
		
		if (!msi_node ||
		    !dt_node_is_compatible(msi_node, "fsl,mpic-msi")) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: Bad fsl,msi phandle in %s\n",
			         __func__, node->name);
			return ULONG_MAX;
		}

		ret = dt_get_reg(msi_node, 0, &msi_addr, NULL);
		if (ret < 0) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: Could not get reg in %s\n",
			         __func__, msi_node->name);
			return ULONG_MAX;
		}

		msi_bank_addr = msi_addr & (PAGE_SIZE - 1);
		rpn = msi_addr >> PAGE_SHIFT;
		msi_addr = gaddr + msi_bank_addr;
		if (*size > PAGE_SIZE)
			*size = PAGE_SIZE;

		ret = snprintf(buf, sizeof(buf), "fsl,vmpic-msi");
		ret = dt_set_prop(msi_gnode, "compatible", buf, ret + 1);
		if (ret < 0)
			return ULONG_MAX;
		regprop = dt_get_prop(msi_gnode, "reg", 0);
		dt_delete_prop(regprop);

		reg[0] = msi_addr >> 32;
		reg[1] = msi_addr & 0xffffffff;

		// FIXME: This needs to be done via u-boot
		reg[1] += 0x140;

		ret = dt_set_prop(node, "msi-address-64", reg, rootnaddr * 4);
		if (ret < 0)
			goto nomem;

		ret = dt_get_reg(node, 0, &pcie_addr, &pcie_size);
		if (ret < 0 || pcie_size < 0x1000) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: bad/missing reg\n", __func__);
			return ULONG_MAX;
		}

		size_t len = 0x1000;
		pci_ctrl = map_gphys(TEMPTLB1, guest->gphys, pcie_addr,
		                     temp_mapping[0], &len, TLB_TSIZE_4K,
		                     TLB_MAS2_IO, 0);
		if (!pci_ctrl || len < 0x1000) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: Couldn't map reg %llx (guest phys) of %s\n",
			         __func__, pcie_addr, msi_node->name);
			return ULONG_MAX;
		}

		for (i = 0; i <= 2; i++) {
			uint32_t piwar, piwbear, piwbar;

			piwar = in32((uint32_t *)
				(pci_ctrl + PEXIWAR + i * 0x20));

			if (piwar & PEXI_EN) {
				piwbar = in32((uint32_t *)
					(pci_ctrl + PEXIWBAR + i * 0x20));
				piwbear = in32((uint32_t *) (pci_ctrl +
						PEXIWBEAR + i * 0x20));

				/* logic works for undefined PEXIWBEAR1 */
				uint64_t inb_win_addr = ((uint64_t)
					((piwbear & 0xFFFFF) << 12 |
				          piwbar >> 20)  << 32) |
					(piwbar & 0xFFFFF) << 12;

				uint32_t inb_win_size = 1 <<
					((piwar & PEXI_IWS) + 1);

				if (msi_addr >= inb_win_addr &&
				    msi_addr <= inb_win_addr + inb_win_size - 1)
					break;
			}
		}

		tlb1_clear_entry(TEMPTLB1);

		if (i > 2) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: %s: msi-address 0x%llx outside inbound memory windows\n",
			         __func__, node->name, msi_addr);

			return ULONG_MAX;
		}
	}

	return rpn;

nomem:
	printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
	         "%s: out of memory\n", __func__);
	return ULONG_MAX;
}

#define MAX_SUBWIN_CNT 16

static int setup_subwins(guest_t *guest, dt_node_t *parent,
                         uint32_t liodn, phys_addr_t primary_base,
                         phys_addr_t primary_size,
                         uint32_t subwindow_cnt,
                         uint32_t omi, uint32_t stash_dest,
                         ppaace_t *ppaace, dt_node_t *cfgnode,
                         dt_node_t *hwnode)
{
	unsigned long fspi;
	phys_addr_t subwindow_size = primary_size / subwindow_cnt;

	/* The first entry is in the primary PAACE instead */
	fspi = get_fspi_and_increment(subwindow_cnt - 1);
	if (fspi == ULONG_MAX) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: spaace indexes exhausted %s\n",
		         __func__, parent->name);
		return ERR_BUSY;
	}

	list_for_each(&parent->children, i) {
		dt_node_t *node = to_container(i, dt_node_t, child_node);
		dt_prop_t *prop;
		uint64_t gaddr, size;
		unsigned long rpn;
		int subwin, swse;

		prop = dt_get_prop(node, "guest-addr", 0);
		if (!prop || prop->len != 8) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: bad/missing guest-addr in %s/%s\n",
			         __func__, parent->name, node->name);
			continue;
		}
		
		gaddr = *(uint64_t *)prop->data;
		if (gaddr < primary_base ||
		    gaddr > primary_base + primary_size) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: guest-addr %llx in %s/%s out of bounds\n",
			         __func__, gaddr, parent->name, node->name);
			continue;
		}

		if (gaddr & (subwindow_size - 1)) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: guest-addr %llx in %s/%s misaligned\n"
			         "subwindow size %llx\n",
			         __func__, gaddr, parent->name,
			         node->name, subwindow_size);
			continue;
		}
		
		subwin = (gaddr - primary_base) / subwindow_size;
		assert(primary_base + subwin * subwindow_size == gaddr);

		prop = dt_get_prop(node, "size", 0);
		if (!prop || prop->len != 8) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_WARN,
			         "%s: warning: missing/bad size prop at %s/%s\n",
			         __func__, parent->name,
			         node->name);
			size = subwindow_size;
		} else {
			size = *(uint64_t *)prop->data;
		}

		if (size & (size - 1) || size > subwindow_size ||
		    size < PAGE_SIZE) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: size %llx in %s/%s "
			         "out of range, or not a power of 2\n",
			         __func__, size, parent->name, node->name);
			continue;
		}

		if (!dt_get_prop(node, "pcie-msi-subwindow", 0))
			rpn = get_rpn(guest, gaddr >> PAGE_SHIFT,
				size >> PAGE_SHIFT);
		else
			rpn = setup_pcie_msi_subwin(guest, cfgnode, hwnode,
					gaddr, &size);

		if (rpn == ULONG_MAX)
			continue;

		swse = map_addrspace_size_to_wse(size);

		/* If we merge ppaace_t and spaace_t, we could
		 * simplify this a bit.
		 */
		if (subwin == 0) {
			ppaace->swse = swse;
			ppaace->atm = PAACE_ATM_WINDOW_XLATE;
			ppaace->twbah = rpn >> 20;
			ppaace->twbal = rpn & 0xfffff;
			ppaace->ap = PAACE_AP_PERMS_ALL; /* FIXME read-only gpmas */
		} else {
			spaace_t *spaace = pamu_get_spaace(fspi, subwin - 1);
			setup_default_xfer_to_host_spaace(spaace);

			spaace->swse = swse;
			spaace->atm = PAACE_ATM_WINDOW_XLATE;
			spaace->twbah = rpn >> 20;
			spaace->twbal = rpn & 0xfffff;
			spaace->ap = PAACE_AP_PERMS_ALL; /* FIXME read-only gpmas */
			spaace->liodn = liodn;

			if (~omi != 0) {
				spaace->otm = PAACE_OTM_INDEXED;
				spaace->op_encode.index_ot.omi = omi;
				spaace->impl_attr.cid = stash_dest;
			}

			lwsync();
			spaace->v = 1;
		}
	}

	ppaace->wce = map_subwindow_cnt_to_wce(subwindow_cnt);
	ppaace->mw = 1;
	ppaace->fspi = fspi;
	return 0;
}

/*
 * Given a device liodn and a device config node setup
 * the paace entry for this device.
 *
 * Before this function is called the caller should have
 * established whether there is an associated dma-window
 * so it is an error if a dma-window prop is not found.
 */
int pamu_config_liodn(guest_t *guest, uint32_t liodn, dt_node_t *hwnode, dt_node_t *cfgnode)
{
	struct ppaace_t *ppaace;
	unsigned long rpn;
	dt_prop_t *prop, *stash_prop;
	dt_prop_t *gaddr;
	dt_prop_t *size;
	dt_node_t *dma_window;
	phys_addr_t window_addr = ~(phys_addr_t)0;
	phys_addr_t window_size = 0;
	uint32_t stash_dest = ~(uint32_t)0;
	uint32_t omi = ~(uint32_t)0;
	uint32_t subwindow_cnt = 0;
	register_t saved;
	int ret;

	prop = dt_get_prop(cfgnode, "dma-window", 0);
	if (!prop || prop->len != 4) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_WARN,
		         "%s: warning: missing dma-window at %s\n",
		         __func__, cfgnode->name);
		return 0;
	}

	dma_window = dt_lookup_phandle(config_tree, *(const uint32_t *)prop->data);
	if (!dma_window) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: bad dma-window phandle ref at %s\n",
		         __func__, cfgnode->name);
		return ERR_BADTREE;
	}
	gaddr = dt_get_prop(dma_window, "guest-addr", 0);
	if (!gaddr || gaddr->len != 8) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: warning: missing/bad guest-addr at %s\n",
		         __func__, dma_window->name);
		return ERR_BADTREE;
	}
	window_addr = *(const uint64_t *)gaddr->data;

	size = dt_get_prop(dma_window, "size", 0);
	if (!size || size->len != 8) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: missing/bad size prop at %s\n",
		         __func__, dma_window->name);
		return ERR_BADTREE;
	}
	window_size = *(const uint64_t *)size->data;

	if ((window_size & (window_size - 1)) || window_size < PAGE_SIZE) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: %s size too small or not a power of two\n",
		         __func__, dma_window->name);
		return ERR_BADTREE;
	}

	if (window_addr & (window_size - 1)) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: %s is not aligned with window size\n",
		         __func__, dma_window->name);
		return ERR_BADTREE;
	}

	saved = spin_lock_intsave(&pamu_lock);
	ppaace = pamu_get_ppaace(liodn);
	if (!ppaace) {
		spin_unlock_intsave(&pamu_lock, saved);
		return ERR_NOMEM;
	}

	if (ppaace->wse) {
		spin_unlock_intsave(&pamu_lock, saved);
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: liodn %d or device in use\n", __func__, liodn);
		return ERR_BUSY;
	}

	ppaace->wse = map_addrspace_size_to_wse(window_size);
	spin_unlock_intsave(&pamu_lock, saved);

	liodn_to_guest[liodn] = guest;

	setup_default_xfer_to_host_ppaace(ppaace);

	ppaace->wbah = window_addr >> (PAGE_SHIFT + 20);
	ppaace->wbal = (window_addr >> PAGE_SHIFT) & 0xfffff;

	/* set up operation mapping if it's configured */
	prop = dt_get_prop(cfgnode, "operation-mapping", 0);
	if (prop) {
		if (prop->len == 4) {
			omi = *(const uint32_t *)prop->data;
			if (omi <= OMI_MAX) {
				ppaace->otm = PAACE_OTM_INDEXED;
				ppaace->op_encode.index_ot.omi = omi;
			} else {
				printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
				         "%s: bad operation mapping index at %s\n",
				         __func__, cfgnode->name);
			}
		} else {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			         "%s: bad operation mapping index at %s\n",
			         __func__, cfgnode->name);
		}
	}

	/* configure stash id */
	stash_prop = dt_get_prop(cfgnode, "stash-dest", 0);
	if (stash_prop && stash_prop->len == 4) {
		stash_dest = get_stash_dest( *(const uint32_t *)
				stash_prop->data , hwnode);

		if (~stash_dest != 0)
			ppaace->impl_attr.cid = stash_dest;
	}

	/* configure snoop-id if needed */
	prop = dt_get_prop(cfgnode, "snoop-cpu-only", 0);
	if (prop && ~stash_dest != 0) {
		if ((*(const uint32_t *)stash_prop->data) >= L3) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
				"%s: %s snoop-cpu-only property must have stash-dest as L1 or L2 cache\n",
				 __func__, cfgnode->name);
			goto skip_snoop_id;
		}

		if (!dt_get_prop(cfgnode->parent, "vcpu", 0)) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
				"%s: missing vcpu property in %s node corresponding to snoop-cpu-only property defined in %s node\n",
				__func__, cfgnode->parent->name, cfgnode->name);
			goto skip_snoop_id;
		}

		ppaace->domain_attr.to_host.snpid =
				get_snoop_id(hwnode, guest) + 1;
	}
skip_snoop_id:

	prop = dt_get_prop(dma_window, "subwindow-count", 0);
	if (prop) {
		if (prop->len != 4) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
				"%s: bad subwindow-count length %zu in %s\n",
				 __func__, prop->len, cfgnode->name);
			return ERR_BADTREE;
		}
	
		subwindow_cnt = *(const uint32_t *)prop->data;

		if (!is_subwindow_count_valid(subwindow_cnt)) {
			printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
				"%s: bad subwindow-count %d in %s\n",
				 __func__, subwindow_cnt, cfgnode->name);
			return ERR_BADTREE;
		}

		ret = setup_subwins(guest, dma_window, liodn, window_addr,
		                    window_size, subwindow_cnt, omi,
		                    stash_dest, ppaace, cfgnode, hwnode);
		if (ret < 0)
			return ret;
	} else {
		/* No subwindows */
		rpn = get_rpn(guest, window_addr >> PAGE_SHIFT,
		              window_size >> PAGE_SHIFT);
		if (rpn == ULONG_MAX)
			return ERR_NOTRANS;

		ppaace->atm = PAACE_ATM_WINDOW_XLATE;
		ppaace->twbah = rpn >> 20;
		ppaace->twbal = rpn;
		ppaace->ap = PAACE_AP_PERMS_ALL; /* FIXME read-only gpmas */
	}

	lwsync();

	/* PAACE is invalid, validated by enable hcall */
	ppaace->v = 1; // FIXME: for right now we are leaving PAACE
	               // entries enabled by default.

	return 0;
}

int pamu_enable_liodn(unsigned int handle)
{
	guest_t *guest = get_gcpu()->guest;
	struct ppaace_t *current_ppaace;
	pamu_handle_t *pamu_handle;
	unsigned long liodn;

	// FIXME: race against handle closure
	if (handle >= MAX_HANDLES || !guest->handles[handle]) {
		return EV_EINVAL;
	}

	pamu_handle = guest->handles[handle]->pamu;
	if (!pamu_handle) {
		return EV_EINVAL;
	}

	liodn = pamu_handle->assigned_liodn;

#ifdef CONFIG_CLAIMABLE_DEVICES
	if (liodn_to_guest[liodn] != guest)
		return EV_INVALID_STATE;
#endif

	current_ppaace = pamu_get_ppaace(liodn);
	current_ppaace->v = 1;

	return 0;
}

int pamu_disable_liodn(unsigned int handle)
{
	guest_t *guest = get_gcpu()->guest;
	struct ppaace_t *current_ppaace;
	pamu_handle_t *pamu_handle;
	unsigned long liodn;

	// FIXME: race against handle closure
	if (handle >= MAX_HANDLES || !guest->handles[handle]) {
		return EV_EINVAL;
	}

	pamu_handle = guest->handles[handle]->pamu;
	if (!pamu_handle) {
		return EV_EINVAL;
	}

	liodn = pamu_handle->assigned_liodn;

#ifdef CONFIG_CLAIMABLE_DEVICES
	if (liodn_to_guest[liodn] != guest)
		return EV_INVALID_STATE;
#endif

	current_ppaace = pamu_get_ppaace(liodn);
	current_ppaace->v = 0;

	/*
	 * FIXME : Need to wait or synchronize with any pending DMA flush
	 * operations on disabled liodn's, as once we disable PAMU
	 * window here any pending DMA flushes will fail.
	 */

	return 0;
}

static void setup_omt(void)
{
	ome_t *ome;

	/* Configure OMI_QMAN */
	ome = pamu_get_ome(OMI_QMAN);
	if (!ome) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: failed to read operation mapping table\n", __func__);
		return;
	}

	ome->moe[IOE_READ_IDX] = EOE_VALID | EOE_READ;
	ome->moe[IOE_EREAD0_IDX] = EOE_VALID | EOE_RSA;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
	ome->moe[IOE_EWRITE0_IDX] = EOE_VALID | EOE_WWSAO;

	/*
	 * When it comes to stashing DIRECTIVEs, the QMan BG says
	 * (1.5.6.7.1:  FQD Context_A field used for dequeued etc.
	 * etc. stashing control):
	 * - AE/DE/CE == 0:  don't stash exclusive.  Use DIRECT0,
	 *                   which should be a non-PE LOADEC.
	 * - AE/DE/CE == 1:  stash exclusive via DIRECT1, i.e.
	 *                   LOADEC-PE
	 * If one desires to alter how the three different types of
	 * stashing are done, please alter rx_conf.exclusive in
	 * ipfwd_a.c (that specifies the 3-bit AE/DE/CE field), and
	 * do not alter the settings here.  - bgrayson
	 */
	ome->moe[IOE_DIRECT0_IDX] = EOE_VALID | EOE_LDEC;
	ome->moe[IOE_DIRECT1_IDX] = EOE_VALID | EOE_LDECPE;

	/* Configure OMI_FMAN */
	ome = pamu_get_ome(OMI_FMAN);
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READI;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;

	/* Configure OMI_QMAN private */
	ome = pamu_get_ome(OMI_QMAN_PRIV);
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READ;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
	ome->moe[IOE_EREAD0_IDX] = EOE_VALID | EOE_RSA;
	ome->moe[IOE_EWRITE0_IDX] = EOE_VALID | EOE_WWSA;

	/* Configure OMI_CAAM */
	ome = pamu_get_ome(OMI_CAAM);
	ome->moe[IOE_READ_IDX]  = EOE_VALID | EOE_READI;
	ome->moe[IOE_WRITE_IDX] = EOE_VALID | EOE_WRITE;
}

static void pamu_error_log(hv_error_t *err, guest_t *guest)
{
	/* Access violations go to per guest and the global error queue*/
	if (guest)
		error_log(&guest->error_event_queue, err, &guest->error_log_prod_lock);

	if (error_manager_guest)
		error_log(&global_event_queue, err, &global_event_prod_lock);

	error_log(&hv_global_event_queue, err, &hv_queue_prod_lock);
}

static int pamu_error_isr(void *arg)
{
	hv_error_t err = { };
	device_t *dev = arg;
	interrupt_t *irq;

	irq = dev->irqs[1];
	irq->ops->disable(irq);

	strncpy(err.domain, get_domain_str(error_pamu), sizeof(err.domain));
	strncpy(err.error, get_domain_str(error_pamu), sizeof(err.error));

	pamu_error_log(&err, NULL);

	return 0;
}

static int pamu_av_isr(void *arg)
{
	device_t *dev = arg;
	void *reg_base = dev->regs[0].virt;
	phys_addr_t reg_size = dev->regs[0].size;
	unsigned long reg_off;
	int ret = -1;
	hv_error_t err = { };
	dt_node_t *pamu_node;
	uint32_t av_liodn, avs1;

	for (reg_off = 0; reg_off < reg_size; reg_off += PAMU_OFFSET) {
		void *reg = reg_base + reg_off;

		uint32_t pics = in32((uint32_t *)(reg + PAMU_PICS));
		if (pics & PAMU_ACCESS_VIOLATION_STAT) {
			avs1 = in32 ((uint32_t *) (reg + PAMU_AVS1));
			av_liodn = avs1 >> PAMU_AVS1_LIODN_SHIFT;
			ppaace_t *ppaace = pamu_get_ppaace(av_liodn);
			/* We may get access violations for invalid LIODNs, just ignore them */
			if (ppaace->v) {
				strncpy(err.domain, get_domain_str(error_pamu), sizeof(err.domain));
				strncpy(err.error, get_error_str(error_pamu, pamu_access_violation),
						sizeof(err.error));
				pamu_node = to_container(dev, dt_node_t, dev);
				dt_get_path(NULL, pamu_node, err.hdev_tree_path, sizeof(err.hdev_tree_path));
				err.pamu.avs1 = avs1;
				err.pamu.access_violation_addr = ((phys_addr_t) in32 ((uint32_t *) (reg + PAMU_AVAH)))
									 << 32 | in32 ((uint32_t *) (reg + PAMU_AVAL));
				err.pamu.avs2 = in32 ((uint32_t *) (reg + PAMU_AVS2));
				printlog(LOGTYPE_PAMU, LOGLEVEL_DEBUG,
						"PAMU access violation on PAMU#%ld, liodn = %x\n",
						 reg_off / PAMU_OFFSET, av_liodn);
				printlog(LOGTYPE_PAMU, LOGLEVEL_DEBUG,
						"PAMU access violation avs1 = %x, avs2 = %x, av_addr = %llx\n",
						 err.pamu.avs1, err.pamu.avs2, err.pamu.access_violation_addr);

				/*FIXME : LIODN index not in PPAACT table*/
				assert(!(avs1 & PAMU_LAV_LIODN_NOT_IN_PPAACT));

				spin_lock(&pamu_error_lock);

				guest_t *guest = liodn_to_guest[av_liodn];
				if (guest) {
					err.pamu.lpid = guest->lpid;
					err.pamu.liodn_handle = liodn_to_handle[av_liodn];
				}

				spin_unlock(&pamu_error_lock);

				pamu_error_log(&err, guest);
				ppaace->v = 0;
			}

			/* Clear the write one to clear bits in AVS1, mask out the LIODN */
			out32((uint32_t *) (reg + PAMU_AVS1), (avs1 & PAMU_AV_MASK));
			/* De-assert access violation pin */
			out32((uint32_t *)(reg + PAMU_PICS), pics);

#ifdef CONFIG_P4080_ERRATUM_PAMU3
			/* erratum -- do it twice */
			out32((uint32_t *)(reg + PAMU_PICS), pics);
#endif

			ret = 0;
		}
	}

	return ret;
}

static int pamu_probe(driver_t *drv, device_t *dev);

static driver_t __driver pamu = {
	.compatible = "fsl,p4080-pamu",
	.probe = pamu_probe
};

#define PAMUBYPENR 0x604
static int pamu_probe(driver_t *drv, device_t *dev)
{
	int ret;
	phys_addr_t addr, size;
	unsigned long pamu_reg_base, pamu_reg_off;
	unsigned long pamumem_size;
	void *pamumem;
	dt_node_t *guts_node, *pamu_node;
	phys_addr_t guts_addr, guts_size;
	uint32_t pamubypenr, pamu_counter, *pamubypenr_ptr;
	interrupt_t *irq;

	/*
	 * enumerate all PAMUs and allocate and setup PAMU tables
	 * for each of them.
	 */

	if (dev->num_regs < 1 || !dev->regs[0].virt) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
			"%s: PAMU reg initialization failed\n", __func__);
		return ERR_INVALID;
	}
	pamu_node = to_container(dev, dt_node_t, dev);
	addr = dev->regs[0].start;
	size = dev->regs[0].size;

	guts_node = dt_get_first_compatible(hw_devtree, "fsl,qoriq-device-config-1.0");
	if (!guts_node) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: pamu present, but no guts node found\n", __func__);
		return ERR_UNHANDLED;
	}

	ret = dt_get_reg(guts_node, 0, &guts_addr, &guts_size);
	if (ret < 0) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR,
		         "%s: no guts reg found\n", __func__);
		return ERR_UNHANDLED;
	}

	pamumem_size = align(PAACT_SIZE + 1, PAMU_TABLE_ALIGNMENT) +
			 align(SPAACT_SIZE + 1, PAMU_TABLE_ALIGNMENT) + OMT_SIZE;

	pamumem_size = 1 << ilog2_roundup(pamumem_size);
	pamumem = alloc(pamumem_size, pamumem_size);
	if (!pamumem) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR, "%s: Unable to allocate space for PAMU tables.\n",
				__func__);
		return ERR_NOMEM;
	}

	pamu_node->pma = alloc_type(pma_t);
	if (!pamu_node->pma) {
		printlog(LOGTYPE_PAMU, LOGLEVEL_ERROR, "%s: out of memory\n", __func__);
		goto fail_mem;
	}

	pamu_node->pma->start = virt_to_phys(pamumem);
	pamu_node->pma->size = pamumem_size;

	pamubypenr_ptr = map(guts_addr + PAMUBYPENR, 4,
	                     TLB_MAS2_IO, TLB_MAS3_KERN);
	pamubypenr = in32(pamubypenr_ptr);

	for (pamu_reg_off = 0, pamu_counter = 0x80000000; pamu_reg_off < size;
	     pamu_reg_off += PAMU_OFFSET, pamu_counter >>= 1) {

		pamu_reg_base = (unsigned long) addr + pamu_reg_off;
		ret = pamu_hw_init(pamu_reg_base - CCSRBAR_PA, pamu_reg_off,
					pamumem, pamumem_size);
		if (ret < 0) {
			/* This can only fail for the first instance due to
			 * memory alignment issues, hence this failure
			 * implies global pamu init failure and let all
			 * PAMU(s) remain in bypass mode.
			 */
			goto fail_pma;
		}

		/* Disable PAMU bypass for this PAMU */
		pamubypenr &= ~pamu_counter;
	}

	setup_pamu_law(pamu_node);
	setup_omt();

	if (dev->num_irqs >= 1 && dev->irqs[0]) {
		irq = dev->irqs[0];
		if (irq && irq->ops->register_irq)
			irq->ops->register_irq(irq, pamu_av_isr, dev, TYPE_MCHK);
	}

	if (dev->num_irqs >= 2 && dev->irqs[1]) {
		irq = dev->irqs[1];
		if (irq && irq->ops->register_irq)
			irq->ops->register_irq(irq, pamu_error_isr, dev, TYPE_MCHK);
	}

	/* Enable all relevant PAMU(s) */
	out32(pamubypenr_ptr, pamubypenr);

	for (int i = 0; i < PAACE_NUMBER_ENTRIES; i++)
		liodn_to_handle[i] = -1;

	return 0;

fail_pma:
	free(pamu_node->pma);

fail_mem:
	free(pamumem);
	return ERR_NOMEM;
}

#ifdef CONFIG_CLAIMABLE_DEVICES
static int claim_dma(claim_action_t *action, dev_owner_t *owner,
                     dev_owner_t *prev)
{
	pamu_handle_t *ph = to_container(action, pamu_handle_t, claim_action);
	uint32_t liodn = ph->assigned_liodn;
	uint32_t saved;
	
	saved = spin_lock_mchksave(&pamu_error_lock);

	liodn_to_handle[liodn] = ph->user.id;
	liodn_to_guest[liodn] = owner->guest;

	spin_unlock_mchksave(&pamu_error_lock, saved);
	return 0;
}
#endif

static dt_node_t *find_cfgnode(dt_node_t *hwnode, dev_owner_t *owner)
{
	dt_prop_t *prop;

	if (!dt_node_is_compatible(hwnode->parent, "fsl,qman-portal")) {
		return owner->cfgnode;  /* normal case */
	}

	/* if parent of hwnode is fsl,qman-portal it's a special case */

	/* iterate over all children of the config node */
	list_for_each(&owner->cfgnode->children, i) {
		dt_node_t *config_child = to_container(i, dt_node_t, child_node);

		/* look for nodes with a "device" property */
		const char *s = dt_get_prop_string(config_child, "device");
		if (!s)
			continue;

		dt_node_t *devnode = dt_lookup_alias(hw_devtree, s);
		if (!devnode) {
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "%s: missing device (%s) reference at %s\n",
			         __func__, s, config_child->name);
			continue;
		}
		uint32_t hw_phandle_cfg = dt_get_phandle(devnode, 0);

		prop = dt_get_prop(hwnode, "dev-handle", 0);
		if (prop) {
			if (prop->len != 4) {
				printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
				         "%s: missing/bad dev-handle on %s\n",
				         __func__, owner->hwnode->name);
				continue;
			}
			uint32_t hw_phandle = *(const uint32_t *)prop->data;

			if (hw_phandle == hw_phandle_cfg)
				return config_child; /* found match */
		}
	}

	printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
	         "%s: no config node found for %s\n",
	         __func__, owner->hwnode->name);

	return NULL;
}

typedef struct search_liodn_ctx {
	dt_node_t *cfgnode;
	uint32_t liodn_index;
} search_liodn_ctx_t;

static int search_liodn_index(dt_node_t *cfgnode, void *arg)
{
	search_liodn_ctx_t *ctx = arg;
	dt_prop_t *prop;

	prop = dt_get_prop(cfgnode, "liodn-index", 0);
	if (prop) {
		if (prop->len != 4) {
			ctx->cfgnode = NULL;
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "%s: bad liodn-index on %s\n",
			         __func__, cfgnode->name);
			return 0;
		}
		if (ctx->liodn_index == *(const uint32_t *)prop->data) {
			ctx->cfgnode = cfgnode;
			return 1; /* found match */
		}
	}

	return 0;
}

static int configure_liodn(dt_node_t *hwnode, dev_owner_t *owner,
                           uint32_t liodn, dt_node_t *cfgnode)
{
	int ret = pamu_config_liodn(owner->guest, liodn,
	                            hwnode, cfgnode);
	if (ret < 0) {
		if (ret == ERR_NOMEM)
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
			         "paact table not found-- pamu may not be"
			         " assigned to hypervisor\n");
		else
			printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		                 "%s: config of liodn failed (rc=%d)\n",
		                 __func__, ret);
	}

	return ret;
}

int configure_dma(dt_node_t *hwnode, dev_owner_t *owner)
{
	dt_node_t *cfgnode;
	dt_prop_t *liodn_prop = NULL;
	const uint32_t *liodn;
	int liodn_cnt, i;
	int ret;
	uint32_t *dma_handles = NULL;
	pamu_handle_t *pamu_handle;
	claim_action_t *claim_action;
	int standby = get_claimable(owner) == claimable_standby;

	cfgnode = find_cfgnode(hwnode, owner);
	if (!cfgnode)
		return 0;

	/* get the liodn property on the hw node */
	liodn_prop = dt_get_prop(hwnode, "fsl,liodn", 0);
	if (!liodn_prop)
		return 0;  /* continue */

	if (liodn_prop->len & 3 || liodn_prop->len == 0) {
		printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
		         "%s: bad liodn on %s\n",
		         __func__, hwnode->name);
		return 0;
	}
	liodn = (const uint32_t *)liodn_prop->data;

	/* get a count of liodns */
	liodn_cnt = liodn_prop->len / 4;

	dma_handles = malloc(liodn_cnt * sizeof(uint32_t));
	if (!dma_handles)
		goto nomem;

	for (i = 0; i < liodn_cnt; i++) {
		search_liodn_ctx_t ctx;

		/* search for an liodn-index that matches this liodn */
		ctx.liodn_index = i;
		ctx.cfgnode = cfgnode;  /* default */
		dt_for_each_node(cfgnode, &ctx, search_liodn_index, NULL);

		if (standby) {
			if (ctx.cfgnode &&
			    dt_get_prop(ctx.cfgnode, "dma-window", 0)) {
				printlog(LOGTYPE_PARTITION, LOGLEVEL_NORMAL,
				         "%s: %s: warning: standby owners "
				         "should not have a DMA config\n",
				         __func__, cfgnode->name);
			}
		} else {
			if (!ctx.cfgnode) { /* error */
				free(dma_handles);
				return 0;
			}

			if (configure_liodn(hwnode, owner, liodn[i],
			                    ctx.cfgnode)) {
				free(dma_handles);
				return 0;
			}
		}

		pamu_handle = alloc_type(pamu_handle_t);
		if (!pamu_handle)
			goto nomem;

		pamu_handle->assigned_liodn = liodn[i];
		pamu_handle->user.pamu = pamu_handle;

		ret = alloc_guest_handle(owner->guest, &pamu_handle->user);
		if (ret < 0) {
			free(dma_handles);
			return ret;
		}

		dma_handles[i] = ret;

#ifdef CONFIG_CLAIMABLE_DEVICES
		pamu_handle->claim_action.claim = claim_dma;
		pamu_handle->claim_action.next = owner->claim_actions;
		owner->claim_actions = &pamu_handle->claim_action;
#endif

		if (!standby)
			liodn_to_handle[liodn[i]] = ret;
	}

	if (dt_set_prop(owner->gnode, "fsl,hv-dma-handle", dma_handles,
				i * sizeof(uint32_t)) < 0)
		goto nomem;

	free(dma_handles);
	return 0;

nomem:
	free(dma_handles);
	printlog(LOGTYPE_PARTITION, LOGLEVEL_ERROR,
        	 "%s: out of memory\n", __func__);
	return ERR_NOMEM;
}
