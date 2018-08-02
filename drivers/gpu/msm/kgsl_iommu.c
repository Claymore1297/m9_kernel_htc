/* Copyright (c) 2011-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/genalloc.h>
#include <linux/slab.h>
#include <linux/iommu.h>
#include <linux/msm_kgsl.h>
#include <linux/msm_iommu_domains.h>
#include <stddef.h>

#include "kgsl.h"
#include "kgsl_device.h"
#include "kgsl_mmu.h"
#include "kgsl_sharedmem.h"
#include "kgsl_iommu.h"
#include "adreno_pm4types.h"
#include "adreno.h"
#include "kgsl_trace.h"
#include "kgsl_cffdump.h"
#include "kgsl_pwrctrl.h"

static struct kgsl_iommu_register_list kgsl_iommuv0_reg[KGSL_IOMMU_REG_MAX] = {
	{ 0, 0 },			
	{ 0x0, 1 },			
	{ 0x10, 1 },			
	{ 0x14, 1 },			
	{ 0x20, 1 },			
	{ 0x28, 1 },			
	{ 0x800, 1 },			
	{ 0x820, 1 },			
	{ 0x03C, 1 },			
	{ 0x818, 1 },			
	{ 0x2C, 1 },			
	{ 0x30, 1 },			
	{ 0, 0 },			
	{ 0, 0 },			
	{ 0, 0 }			
};

static struct kgsl_iommu_register_list kgsl_iommuv1_reg[KGSL_IOMMU_REG_MAX] = {
	{ 0, 0 },			
	{ 0x0, 1 },			
	{ 0x20, 1 },			
	{ 0x28, 1 },			
	{ 0x58, 1 },			
	{ 0x60, 1 },			
	{ 0x618, 1 },			
	{ 0x008, 1 },			
	{ 0, 0 },			
	{ 0, 0 },			
	{ 0x68, 1 },			
	{ 0x6C, 1 },			
	{ 0x7F0, 1 },			
	{ 0x7F4, 1 },			
	{ 0x2000, 0 }			
};

static struct iommu_access_ops *iommu_access_ops;

static int kgsl_iommu_flush_pt(struct kgsl_mmu *mmu);
static phys_addr_t
kgsl_iommu_get_current_ptbase(struct kgsl_mmu *mmu);

static void _iommu_lock(struct kgsl_iommu const *iommu)
{
	if (iommu_access_ops && iommu_access_ops->iommu_lock_acquire)
		iommu_access_ops->iommu_lock_acquire(
						iommu->sync_lock_initialized);
}

static void _iommu_unlock(struct kgsl_iommu const *iommu)
{
	if (iommu_access_ops && iommu_access_ops->iommu_lock_release)
		iommu_access_ops->iommu_lock_release(
						iommu->sync_lock_initialized);
}

struct remote_iommu_petersons_spinlock kgsl_iommu_sync_lock_vars;


static struct page *kgsl_guard_page;
static struct kgsl_memdesc kgsl_secure_guard_page_memdesc;

static int get_iommu_unit(struct device *dev, struct kgsl_mmu **mmu_out,
			struct kgsl_iommu_unit **iommu_unit_out)
{
	int i, j, k;

	for (i = 0; i < KGSL_DEVICE_MAX; i++) {
		struct kgsl_mmu *mmu;
		struct kgsl_iommu *iommu;

		if (kgsl_driver.devp[i] == NULL)
			continue;

		mmu = kgsl_get_mmu(kgsl_driver.devp[i]);
		if (mmu == NULL || mmu->priv == NULL)
			continue;

		iommu = mmu->priv;

		for (j = 0; j < iommu->unit_count; j++) {
			struct kgsl_iommu_unit *iommu_unit =
				&iommu->iommu_units[j];
			for (k = 0; k < iommu_unit->dev_count; k++) {
				if (iommu_unit->dev[k].dev == dev) {
					*mmu_out = mmu;
					*iommu_unit_out = iommu_unit;
					return 0;
				}
			}
		}
	}

	return -EINVAL;
}

static struct kgsl_iommu_device *get_iommu_device(struct kgsl_iommu_unit *unit,
		struct device *dev)
{
	int k;

	for (k = 0; unit && k < unit->dev_count; k++) {
		if (unit->dev[k].dev == dev)
			return &(unit->dev[k]);
	}

	return NULL;
}



struct _mem_entry {
	unsigned int gpuaddr;
	unsigned int size;
	unsigned int flags;
	unsigned int priv;
	int pending_free;
	pid_t pid;
};


static void _prev_entry(struct kgsl_process_private *priv,
	unsigned int faultaddr, struct _mem_entry *ret)
{
	struct rb_node *node;
	struct kgsl_mem_entry *entry;

	for (node = rb_first(&priv->mem_rb); node; ) {
		entry = rb_entry(node, struct kgsl_mem_entry, node);

		if (entry->memdesc.gpuaddr > faultaddr)
			break;


		if (entry->memdesc.gpuaddr > ret->gpuaddr) {
			ret->gpuaddr = entry->memdesc.gpuaddr;
			ret->size = entry->memdesc.size;
			ret->flags = entry->memdesc.flags;
			ret->priv = entry->memdesc.priv;
			ret->pending_free = entry->pending_free;
			ret->pid = priv->pid;
		}

		node = rb_next(&entry->node);
	}
}


static void _next_entry(struct kgsl_process_private *priv,
	unsigned int faultaddr, struct _mem_entry *ret)
{
	struct rb_node *node;
	struct kgsl_mem_entry *entry;

	for (node = rb_last(&priv->mem_rb); node; ) {
		entry = rb_entry(node, struct kgsl_mem_entry, node);

		if (entry->memdesc.gpuaddr < faultaddr)
			break;


		if (entry->memdesc.gpuaddr < ret->gpuaddr) {
			ret->gpuaddr = entry->memdesc.gpuaddr;
			ret->size = entry->memdesc.size;
			ret->flags = entry->memdesc.flags;
			ret->priv = entry->memdesc.priv;
			ret->pending_free = entry->pending_free;
			ret->pid = priv->pid;
		}

		node = rb_prev(&entry->node);
	}
}

static void _find_mem_entries(struct kgsl_mmu *mmu, unsigned int faultaddr,
	unsigned int ptbase, struct _mem_entry *preventry,
	struct _mem_entry *nextentry)
{
	struct kgsl_process_private *private;
	int id = kgsl_mmu_get_ptname_from_ptbase(mmu, ptbase);

	memset(preventry, 0, sizeof(*preventry));
	memset(nextentry, 0, sizeof(*nextentry));

	
	nextentry->gpuaddr = 0xFFFFFFFF;

	mutex_lock(&kgsl_driver.process_mutex);

	list_for_each_entry(private, &kgsl_driver.process_list, list) {

		if (private->pagetable && (private->pagetable->name != id))
			continue;

		spin_lock(&private->mem_lock);
		_prev_entry(private, faultaddr, preventry);
		_next_entry(private, faultaddr, nextentry);
		spin_unlock(&private->mem_lock);
	}

	mutex_unlock(&kgsl_driver.process_mutex);
}

static void _print_entry(struct kgsl_device *device, struct _mem_entry *entry)
{
	char name[32];
	memset(name, 0, sizeof(name));

	kgsl_get_memory_usage(name, sizeof(name) - 1, entry->flags);

	KGSL_LOG_DUMP(device,
		"[%8.8X - %8.8X] %s %s (pid = %d) (%s)\n",
		entry->gpuaddr,
		entry->gpuaddr + entry->size,
		entry->priv & KGSL_MEMDESC_GUARD_PAGE ? "(+guard)" : "",
		entry->pending_free ? "(pending free)" : "",
		entry->pid, name);
}

static void _check_if_freed(struct kgsl_iommu_device *iommu_dev,
	unsigned long addr, unsigned int pid)
{
	unsigned long gpuaddr = addr;
	unsigned long size = 0;
	unsigned int flags = 0;

	char name[32];
	memset(name, 0, sizeof(name));

	if (kgsl_memfree_find_entry(pid, &gpuaddr, &size, &flags)) {
		kgsl_get_memory_usage(name, sizeof(name) - 1, flags);
		KGSL_LOG_DUMP(iommu_dev->kgsldev, "---- premature free ----\n");
		KGSL_LOG_DUMP(iommu_dev->kgsldev,
			"[%8.8lX-%8.8lX] (%s) was already freed by pid %d\n",
			gpuaddr, gpuaddr + size, name, pid);
	}
}

static int kgsl_iommu_fault_handler(struct iommu_domain *domain,
	struct device *dev, unsigned long addr, int flags, void *token)
{
	int ret = 0;
	struct kgsl_mmu *mmu;
	struct kgsl_iommu *iommu;
	struct kgsl_iommu_unit *iommu_unit;
	struct kgsl_iommu_device *iommu_dev;
	unsigned int ptbase, fsr;
	unsigned int pid;
	struct _mem_entry prev, next;
	unsigned int fsynr0, fsynr1;
	int write;
	struct kgsl_device *device;
	struct adreno_device *adreno_dev;
	unsigned int no_page_fault_log = 0;
	unsigned int curr_context_id = 0;
	struct kgsl_context *context;

	ret = get_iommu_unit(dev, &mmu, &iommu_unit);
	if (ret)
		goto done;

	device = mmu->device;
	adreno_dev = ADRENO_DEVICE(device);
	if (1 == atomic_cmpxchg(&mmu->fault, 0, 1))
		goto done;

	iommu_dev = get_iommu_device(iommu_unit, dev);
	if (!iommu_dev) {
		KGSL_CORE_ERR("Invalid IOMMU device %p\n", dev);
		ret = -ENOSYS;
		goto done;
	}
	iommu = mmu->priv;

	fsr = KGSL_IOMMU_GET_CTX_REG(iommu, iommu_unit,
		iommu_dev->ctx_id, FSR);
	if (!fsr) {
		atomic_set(&mmu->fault, 0);
		goto done;
	}
	kgsl_sharedmem_readl(&device->memstore, &curr_context_id,
		KGSL_MEMSTORE_OFFSET(KGSL_MEMSTORE_GLOBAL, current_context));

	context = kgsl_context_get(device, curr_context_id);

	if (context != NULL) {
		
		set_bit(KGSL_CONTEXT_PRIV_PAGEFAULT, &context->priv);

		kgsl_context_put(context);
		context = NULL;
	}

	iommu_dev->fault = 1;

	if (adreno_dev->ft_pf_policy & KGSL_FT_PAGEFAULT_GPUHALT_ENABLE) {
		adreno_set_gpu_fault(adreno_dev, ADRENO_IOMMU_PAGE_FAULT);
		
		kgsl_pwrctrl_change_state(device, KGSL_STATE_AWARE);
		adreno_dispatcher_schedule(device);
	}

	ptbase = KGSL_IOMMU_GET_CTX_REG_Q(iommu, iommu_unit,
				iommu_dev->ctx_id, TTBR0);

	fsynr0 = KGSL_IOMMU_GET_CTX_REG(iommu, iommu_unit,
		iommu_dev->ctx_id, FSYNR0);
	fsynr1 = KGSL_IOMMU_GET_CTX_REG(iommu, iommu_unit,
		iommu_dev->ctx_id, FSYNR1);

	if (msm_soc_version_supports_iommu_v0())
		write = ((fsynr1 & (KGSL_IOMMU_FSYNR1_AWRITE_MASK <<
			KGSL_IOMMU_FSYNR1_AWRITE_SHIFT)) ? 1 : 0);
	else
		write = ((fsynr0 & (KGSL_IOMMU_V1_FSYNR0_WNR_MASK <<
			KGSL_IOMMU_V1_FSYNR0_WNR_SHIFT)) ? 1 : 0);

	pid = kgsl_mmu_get_ptname_from_ptbase(mmu, ptbase);

	if (adreno_dev->ft_pf_policy & KGSL_FT_PAGEFAULT_LOG_ONE_PER_PAGE)
		no_page_fault_log = kgsl_mmu_log_fault_addr(mmu, ptbase, addr);

	if (!no_page_fault_log) {
		KGSL_MEM_CRIT(iommu_dev->kgsldev,
			"GPU PAGE FAULT: addr = %lX pid = %d\n", addr, pid);
		KGSL_MEM_CRIT(iommu_dev->kgsldev,
		 "context = %d TTBR0 = %X FSR = %X FSYNR0 = %X FSYNR1 = %X(%s fault)\n",
			iommu_dev->ctx_id, ptbase, fsr, fsynr0, fsynr1,
			write ? "write" : "read");

		_check_if_freed(iommu_dev, addr, pid);

		KGSL_LOG_DUMP(iommu_dev->kgsldev, "---- nearby memory ----\n");

		_find_mem_entries(mmu, addr, ptbase, &prev, &next);

		if (prev.gpuaddr)
			_print_entry(iommu_dev->kgsldev, &prev);
		else
			KGSL_LOG_DUMP(iommu_dev->kgsldev, "*EMPTY*\n");

		KGSL_LOG_DUMP(iommu_dev->kgsldev, " <- fault @ %8.8lX\n", addr);

		if (next.gpuaddr != 0xFFFFFFFF)
			_print_entry(iommu_dev->kgsldev, &next);
		else
			KGSL_LOG_DUMP(iommu_dev->kgsldev, "*EMPTY*\n");

	}

	trace_kgsl_mmu_pagefault(iommu_dev->kgsldev, addr,
			kgsl_mmu_get_ptname_from_ptbase(mmu, ptbase),
			write ? "write" : "read");

	if (adreno_dev->ft_pf_policy & KGSL_FT_PAGEFAULT_GPUHALT_ENABLE)
		ret = -EBUSY;
done:
	return ret;
}

static void kgsl_iommu_disable_clk(struct kgsl_mmu *mmu, int unit)
{
	struct kgsl_iommu *iommu = mmu->priv;
	int i, j;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];

		
		if ((unit != i) && (unit != KGSL_IOMMU_MAX_UNITS))
			continue;

		atomic_dec(&iommu_unit->clk_enable_count);
		BUG_ON(atomic_read(&iommu_unit->clk_enable_count) < 0);

		for (j = (KGSL_IOMMU_MAX_CLKS - 1); j >= 0; j--)
			if (iommu_unit->clks[j])
				clk_disable_unprepare(iommu_unit->clks[j]);
	}
}


static int kgsl_iommu_clk_prepare_enable(struct clk *clk)
{
	int num_retries = 4;

	while (num_retries--) {
		if (!clk_prepare_enable(clk))
			return 0;
	}

	return 1;
}

static void kgsl_iommu_enable_clk(struct kgsl_mmu *mmu,
				int unit)
{
	int i, j;
	struct kgsl_iommu *iommu = mmu->priv;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];

		
		if ((unit != i) && (unit != KGSL_IOMMU_MAX_UNITS))
			continue;

		for (j = 0; j < KGSL_IOMMU_MAX_CLKS; j++) {
			if (iommu_unit->clks[j])
				if (kgsl_iommu_clk_prepare_enable(
						iommu_unit->clks[j]))
						goto done;
		}
		atomic_inc(&iommu_unit->clk_enable_count);
	}
	return;
done:
	KGSL_CORE_ERR("IOMMU clk enable failed\n");
	BUG();
}

static int kgsl_iommu_pt_equal(struct kgsl_mmu *mmu,
				struct kgsl_pagetable *pt,
				phys_addr_t pt_base)
{
	struct kgsl_iommu_pt *iommu_pt = pt ? pt->priv : NULL;
	phys_addr_t domain_ptbase;

	if (iommu_pt == NULL)
		return 0;

	domain_ptbase = iommu_get_pt_base_addr(iommu_pt->domain)
			& KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;

	pt_base &= KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;

	return (domain_ptbase == pt_base);

}

static phys_addr_t kgsl_iommu_get_ptbase(struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt = pt ? pt->priv : NULL;
	phys_addr_t domain_ptbase;

	if (iommu_pt == NULL)
		return 0;

	domain_ptbase = iommu_get_pt_base_addr(iommu_pt->domain);

	return domain_ptbase & KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
}

static void kgsl_iommu_destroy_pagetable(struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;

	if (iommu_pt->domain) {
		phys_addr_t domain_ptbase =
			iommu_get_pt_base_addr(iommu_pt->domain);
		trace_kgsl_pagetable_destroy(domain_ptbase, pt->name);
		msm_unregister_domain(iommu_pt->domain);
	}

	kfree(iommu_pt);
	iommu_pt = NULL;
}

static void *kgsl_iommu_create_pagetable(void)
{
	int domain_num;
	struct kgsl_iommu_pt *iommu_pt;

	struct msm_iova_layout kgsl_layout = {
		
		.partitions = NULL,
		.npartitions = 0,
		.client_name = "kgsl",
		.domain_flags = 0,
	};

	iommu_pt = kzalloc(sizeof(struct kgsl_iommu_pt), GFP_KERNEL);
	if (!iommu_pt)
		return NULL;

	
	if (msm_soc_version_supports_iommu_v0())
		kgsl_layout.domain_flags = MSM_IOMMU_DOMAIN_PT_CACHEABLE;

	domain_num = msm_register_domain(&kgsl_layout);
	if (domain_num >= 0) {
		iommu_pt->domain = msm_get_iommu_domain(domain_num);

		if (iommu_pt->domain) {
			iommu_set_fault_handler(iommu_pt->domain,
				kgsl_iommu_fault_handler, NULL);

			return iommu_pt;
		}
	}

	KGSL_CORE_ERR("Failed to create iommu domain\n");
	kfree(iommu_pt);
	return NULL;
}

static void *kgsl_iommu_create_secure_pagetable(void)
{
	int domain_num;
	struct kgsl_iommu_pt *iommu_pt;

	struct msm_iova_layout kgsl_secure_layout = {
		
		.partitions = NULL,
		.npartitions = 0,
		.client_name = "kgsl_secure",
		.domain_flags = 0,
		.is_secure = 1,
	};

	iommu_pt = kzalloc(sizeof(struct kgsl_iommu_pt), GFP_KERNEL);
	if (!iommu_pt)
		return NULL;

	domain_num = msm_register_domain(&kgsl_secure_layout);
	if (domain_num >= 0) {
		iommu_pt->domain = msm_get_iommu_domain(domain_num);

		if (iommu_pt->domain)
			return iommu_pt;
	}

	KGSL_CORE_ERR("Failed to create secure iommu domain\n");
	kfree(iommu_pt);
	return NULL;
}

static void kgsl_detach_pagetable_iommu_domain(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu_pt *iommu_pt;
	struct kgsl_iommu *iommu = mmu->priv;
	int i, j;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		iommu_pt = mmu->defaultpagetable->priv;
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (mmu->priv_bank_table &&
				(KGSL_IOMMU_CONTEXT_PRIV == j))
				iommu_pt = mmu->priv_bank_table->priv;
			if (mmu->securepagetable &&
				(KGSL_IOMMU_CONTEXT_SECURE == j))
				iommu_pt = mmu->securepagetable->priv;
			if (iommu_unit->dev[j].attached) {
				iommu_detach_device(iommu_pt->domain,
						iommu_unit->dev[j].dev);
				iommu_unit->dev[j].attached = false;
				KGSL_MEM_INFO(mmu->device, "iommu %p detached "
					"from user dev of MMU: %p\n",
					iommu_pt->domain, mmu);
			}
		}
	}
}

static int kgsl_attach_pagetable_iommu_domain(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu_pt *iommu_pt;
	struct kgsl_iommu *iommu = mmu->priv;
	struct msm_iommu_drvdata *drvdata = 0;
	int i, j, ret = 0;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		iommu_pt = mmu->defaultpagetable->priv;
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (KGSL_IOMMU_CONTEXT_PRIV == j) {
				if (mmu->priv_bank_table)
					iommu_pt = mmu->priv_bank_table->priv;
				else
					continue;
			}

			if (KGSL_IOMMU_CONTEXT_SECURE == j) {
				if (mmu->securepagetable)
					iommu_pt = mmu->securepagetable->priv;
				else
					continue;
			}

			if (!iommu_unit->dev[j].attached) {
				ret = iommu_attach_device(iommu_pt->domain,
							iommu_unit->dev[j].dev);
				if (ret) {
					KGSL_MEM_ERR(mmu->device,
						"Failed to attach device, err %d\n",
						ret);
					goto done;
				}
				iommu_unit->dev[j].attached = true;
				KGSL_MEM_INFO(mmu->device,
				"iommu pt %p attached to dev %p, ctx_id %d\n",
				iommu_pt->domain, iommu_unit->dev[j].dev,
				iommu_unit->dev[j].ctx_id);
				
				if (!drvdata) {
					drvdata = dev_get_drvdata(
					iommu_unit->dev[j].dev->parent);
					iommu_unit->clks[0] = drvdata->pclk;
					iommu_unit->clks[1] = drvdata->clk;
					iommu_unit->clks[2] = drvdata->aclk;
					iommu_unit->clks[3] =
							iommu->gtcu_iface_clk;
				}
			}
		}
	}
done:
	return ret;
}

static int _get_iommu_ctxs(struct kgsl_mmu *mmu,
	struct kgsl_device_iommu_data *data, unsigned int unit_id)
{
	struct kgsl_iommu *iommu = mmu->priv;
	struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[unit_id];
	int i, j;
	int found_ctx;
	int ret = 0;

	for (j = 0; j < KGSL_IOMMU_CONTEXT_MAX; j++) {
		found_ctx = 0;
		for (i = 0; i < data->iommu_ctx_count; i++) {
			if (j == data->iommu_ctxs[i].ctx_id) {
				found_ctx = 1;
				break;
			}
		}
		if (!found_ctx)
			break;
		if (!data->iommu_ctxs[i].iommu_ctx_name) {
			KGSL_CORE_ERR("Context name invalid\n");
			ret = -EINVAL;
			goto done;
		}
		atomic_set(&(iommu_unit->clk_enable_count), 0);

		iommu_unit->dev[iommu_unit->dev_count].dev =
			msm_iommu_get_ctx(data->iommu_ctxs[i].iommu_ctx_name);
		if (NULL == iommu_unit->dev[iommu_unit->dev_count].dev)
			ret = -EINVAL;
		if (IS_ERR(iommu_unit->dev[iommu_unit->dev_count].dev)) {
			ret = PTR_ERR(
				iommu_unit->dev[iommu_unit->dev_count].dev);
			iommu_unit->dev[iommu_unit->dev_count].dev = NULL;
		}
		if (ret)
			goto done;
		iommu_unit->dev[iommu_unit->dev_count].ctx_id =
						data->iommu_ctxs[i].ctx_id;
		iommu_unit->dev[iommu_unit->dev_count].kgsldev = mmu->device;

		KGSL_DRV_INFO(mmu->device,
				"Obtained dev handle %p for iommu context %s\n",
				iommu_unit->dev[iommu_unit->dev_count].dev,
				data->iommu_ctxs[i].iommu_ctx_name);

		iommu_unit->dev_count++;
	}
done:
	if (!iommu_unit->dev_count && !ret)
		ret = -EINVAL;
	if (ret) {
		if (!msm_soc_version_supports_iommu_v0() &&
			iommu_unit->dev_count)
			ret = 0;
		else
			KGSL_CORE_ERR(
			"Failed to initialize iommu contexts, err: %d\n", ret);
	}

	return ret;
}

static int kgsl_iommu_start_sync_lock(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	uint32_t lock_gpu_addr = 0;

	if (KGSL_DEVICE_3D0 != mmu->device->id ||
		!msm_soc_version_supports_iommu_v0() ||
		!kgsl_mmu_is_perprocess(mmu) ||
		iommu->sync_lock_vars)
		return 0;

	if (!(mmu->flags & KGSL_MMU_FLAGS_IOMMU_SYNC)) {
		KGSL_DRV_ERR(mmu->device,
		"The GPU microcode does not support IOMMUv1 sync opcodes\n");
		return -ENXIO;
	}
	
	lock_gpu_addr = (iommu->sync_lock_desc.gpuaddr +
			iommu->sync_lock_offset);

	kgsl_iommu_sync_lock_vars.flag[PROC_APPS] = (lock_gpu_addr +
		(offsetof(struct remote_iommu_petersons_spinlock,
			flag[PROC_APPS])));
	kgsl_iommu_sync_lock_vars.flag[PROC_GPU] = (lock_gpu_addr +
		(offsetof(struct remote_iommu_petersons_spinlock,
			flag[PROC_GPU])));
	kgsl_iommu_sync_lock_vars.turn = (lock_gpu_addr +
		(offsetof(struct remote_iommu_petersons_spinlock, turn)));

	iommu->sync_lock_vars = &kgsl_iommu_sync_lock_vars;

	return 0;
}

#ifdef CONFIG_MSM_IOMMU_GPU_SYNC

static int kgsl_iommu_init_sync_lock(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	int status = 0;
	uint32_t lock_phy_addr = 0;
	uint32_t page_offset = 0;

	if (!msm_soc_version_supports_iommu_v0() ||
		!kgsl_mmu_is_perprocess(mmu))
		return status;

	
	if (iommu->sync_lock_initialized)
		return status;

	iommu_access_ops = msm_get_iommu_access_ops();

	if (iommu_access_ops && iommu_access_ops->iommu_lock_initialize) {
		lock_phy_addr = (uint32_t)
				iommu_access_ops->iommu_lock_initialize();
		if (!lock_phy_addr) {
			iommu_access_ops = NULL;
			return status;
		}
		lock_phy_addr = lock_phy_addr - (uint32_t)MSM_SHARED_RAM_BASE +
				(uint32_t)msm_shared_ram_phys;
	}

	
	page_offset = (lock_phy_addr & (PAGE_SIZE - 1));
	lock_phy_addr = (lock_phy_addr & ~(PAGE_SIZE - 1));
	iommu->sync_lock_desc.physaddr = (unsigned int)lock_phy_addr;
	iommu->sync_lock_offset = page_offset;

	iommu->sync_lock_desc.size =
				PAGE_ALIGN(sizeof(kgsl_iommu_sync_lock_vars));
	status =  memdesc_sg_phys(&iommu->sync_lock_desc,
				 iommu->sync_lock_desc.physaddr,
				 iommu->sync_lock_desc.size);

	if (status) {
		iommu_access_ops = NULL;
		return status;
	}

	
	iommu->sync_lock_desc.priv |= KGSL_MEMDESC_PRIVATE;
	status = kgsl_add_global_pt_entry(mmu->device, &iommu->sync_lock_desc);
	if (status) {
		kgsl_sg_free(iommu->sync_lock_desc.sg,
			iommu->sync_lock_desc.sglen);
		iommu_access_ops = NULL;
		return status;
	}

	
	iommu->sync_lock_initialized = 1;

	return status;
}
#else
static int kgsl_iommu_init_sync_lock(struct kgsl_mmu *mmu)
{
	return 0;
}
#endif

static unsigned int kgsl_iommu_sync_lock(struct kgsl_mmu *mmu,
						unsigned int *cmds)
{
	struct kgsl_device *device = mmu->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct kgsl_iommu *iommu = mmu->device->mmu.priv;
	struct remote_iommu_petersons_spinlock *lock_vars =
					iommu->sync_lock_vars;
	unsigned int *start = cmds;

	if (!iommu->sync_lock_initialized)
		return 0;

	*cmds++ = cp_type3_packet(CP_MEM_WRITE, 2);
	*cmds++ = lock_vars->flag[PROC_GPU];
	*cmds++ = 1;

	cmds += adreno_add_idle_cmds(adreno_dev, cmds);

	*cmds++ = cp_type3_packet(CP_WAIT_REG_MEM, 5);
	
	*cmds++ = 0x13;
	*cmds++ = lock_vars->flag[PROC_GPU];
	*cmds++ = 0x1;
	*cmds++ = 0x1;
	*cmds++ = 0x1;

	
	*cmds++ = cp_type3_packet(CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 0;

	*cmds++ = cp_type3_packet(CP_MEM_WRITE, 2);
	*cmds++ = lock_vars->turn;
	*cmds++ = 0;

	cmds += adreno_add_idle_cmds(adreno_dev, cmds);

	*cmds++ = cp_type3_packet(CP_WAIT_REG_MEM, 5);
	
	*cmds++ = 0x13;
	*cmds++ = lock_vars->flag[PROC_GPU];
	*cmds++ = 0x1;
	*cmds++ = 0x1;
	*cmds++ = 0x1;

	
	*cmds++ = cp_type3_packet(CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 0;

	*cmds++ = cp_type3_packet(CP_TEST_TWO_MEMS, 3);
	*cmds++ = lock_vars->flag[PROC_APPS];
	*cmds++ = lock_vars->turn;
	*cmds++ = 0;

	
	*cmds++ = cp_type3_packet(CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 0;

	cmds += adreno_add_idle_cmds(adreno_dev, cmds);

	return cmds - start;
}

static unsigned int kgsl_iommu_sync_unlock(struct kgsl_mmu *mmu,
					unsigned int *cmds)
{
	struct kgsl_device *device = mmu->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct kgsl_iommu *iommu = mmu->device->mmu.priv;
	struct remote_iommu_petersons_spinlock *lock_vars =
						iommu->sync_lock_vars;
	unsigned int *start = cmds;

	if (!iommu->sync_lock_initialized)
		return 0;

	*cmds++ = cp_type3_packet(CP_MEM_WRITE, 2);
	*cmds++ = lock_vars->flag[PROC_GPU];
	*cmds++ = 0;

	*cmds++ = cp_type3_packet(CP_WAIT_REG_MEM, 5);
	
	*cmds++ = 0x13;
	*cmds++ = lock_vars->flag[PROC_GPU];
	*cmds++ = 0x0;
	*cmds++ = 0x1;
	*cmds++ = 0x1;

	
	*cmds++ = cp_type3_packet(CP_SET_PROTECTED_MODE, 1);
	*cmds++ = 0;

	cmds += adreno_add_idle_cmds(adreno_dev, cmds);

	return cmds - start;
}

static int kgsl_get_iommu_ctxt(struct kgsl_mmu *mmu)
{
	struct kgsl_device_platform_data *pdata =
		dev_get_platdata(&mmu->device->pdev->dev);
	struct kgsl_iommu *iommu = mmu->device->mmu.priv;
	int i, ret = 0;

	
	if (KGSL_IOMMU_MAX_UNITS < pdata->iommu_count) {
		KGSL_CORE_ERR("Too many IOMMU units defined\n");
		ret = -EINVAL;
		goto  done;
	}

	for (i = 0; i < pdata->iommu_count; i++) {
		ret = _get_iommu_ctxs(mmu, &pdata->iommu_data[i], i);
		if (ret)
			break;
	}
	iommu->unit_count = pdata->iommu_count;
done:
	return ret;
}

static int kgsl_set_register_map(struct kgsl_mmu *mmu)
{
	struct kgsl_device_platform_data *pdata =
		dev_get_platdata(&mmu->device->pdev->dev);
	struct kgsl_iommu *iommu = mmu->device->mmu.priv;
	struct kgsl_iommu_unit *iommu_unit;
	int i = 0, ret = 0;
	struct kgsl_device *device = mmu->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);

	for (; i < pdata->iommu_count; i++) {
		struct kgsl_device_iommu_data data = pdata->iommu_data[i];
		iommu_unit = &iommu->iommu_units[i];
		
		if (!data.physstart || !data.physend) {
			KGSL_CORE_ERR("The register range for IOMMU unit not"
					" specified\n");
			ret = -EINVAL;
			goto err;
		}
		
		iommu_unit->reg_map.hostptr = ioremap(data.physstart,
					data.physend - data.physstart + 1);
		if (!iommu_unit->reg_map.hostptr) {
			KGSL_CORE_ERR("Failed to map SMMU register address "
				"space from %x to %x\n", data.physstart,
				data.physend - data.physstart + 1);
			ret = -ENOMEM;
			i--;
			goto err;
		}
		iommu_unit->reg_map.size = data.physend - data.physstart + 1;
		iommu_unit->reg_map.physaddr = data.physstart;
		ret = memdesc_sg_phys(&iommu_unit->reg_map, data.physstart,
				iommu_unit->reg_map.size);
		if (ret)
			goto err;

		if (msm_soc_version_supports_iommu_v0()) {
			iommu_unit->reg_map.priv |= KGSL_MEMDESC_PRIVATE;
			kgsl_add_global_pt_entry(mmu->device,
					&iommu_unit->reg_map);

		}

		if (!msm_soc_version_supports_iommu_v0())
			iommu_unit->iommu_halt_enable = 1;

		if (kgsl_msm_supports_iommu_v2())
			if (adreno_is_a405(adreno_dev)) {
				iommu_unit->ahb_base =
					KGSL_IOMMU_V2_AHB_BASE_A405;
			} else
				iommu_unit->ahb_base = KGSL_IOMMU_V2_AHB_BASE;
		else
			iommu_unit->ahb_base =
				data.physstart - mmu->device->reg_phys;
	}
	iommu->unit_count = pdata->iommu_count;
	return ret;
err:
	
	for (; i >= 0; i--) {
		iommu_unit = &iommu->iommu_units[i];

		kgsl_remove_global_pt_entry(&iommu_unit->reg_map);
		iounmap(iommu_unit->reg_map.hostptr);
		iommu_unit->reg_map.size = 0;
		iommu_unit->reg_map.physaddr = 0;
	}
	return ret;
}

static phys_addr_t kgsl_iommu_get_pt_base_addr(struct kgsl_mmu *mmu,
						struct kgsl_pagetable *pt)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	return iommu_get_pt_base_addr(iommu_pt->domain) &
			KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
}

static uint64_t kgsl_iommu_get_default_ttbr0(struct kgsl_mmu *mmu,
				unsigned int unit_id,
				enum kgsl_iommu_context_id ctx_id)
{
	struct kgsl_iommu *iommu = mmu->priv;
	int i, j;
	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++)
			if (unit_id == i &&
				ctx_id == iommu_unit->dev[j].ctx_id)
				return iommu_unit->dev[j].default_ttbr0;
	}
	return 0;
}

static unsigned int kgsl_iommu_get_reg_ahbaddr(struct kgsl_mmu *mmu,
					int iommu_unit, int ctx_id,
					enum kgsl_iommu_reg_map reg)
{
	struct kgsl_iommu *iommu = mmu->priv;

	if (iommu->iommu_reg_list[reg].ctx_reg)
		return iommu->iommu_units[iommu_unit].ahb_base +
			iommu->iommu_reg_list[reg].reg_offset +
			(ctx_id << KGSL_IOMMU_CTX_SHIFT) +
			iommu->ctx_ahb_offset;
	else
		return iommu->iommu_units[iommu_unit].ahb_base +
			iommu->iommu_reg_list[reg].reg_offset;
}

static int kgsl_iommu_init(struct kgsl_mmu *mmu)
{
	int status = 0;
	struct kgsl_iommu *iommu;
	struct platform_device *pdev = mmu->device->pdev;
	size_t secured_pool_sz = 0;

	atomic_set(&mmu->fault, 0);
	iommu = kzalloc(sizeof(struct kgsl_iommu), GFP_KERNEL);
	if (!iommu)
		return -ENOMEM;

	mmu->priv = iommu;
	status = kgsl_get_iommu_ctxt(mmu);
	if (status)
		goto done;
	status = kgsl_set_register_map(mmu);
	if (status)
		goto done;

	if (mmu->secured)
		secured_pool_sz = KGSL_IOMMU_SECURE_MEM_SIZE;

	if (KGSL_MMU_USE_PER_PROCESS_PT &&
		of_property_match_string(pdev->dev.of_node, "clock-names",
						"gtcu_iface_clk") >= 0)
		iommu->gtcu_iface_clk = clk_get(&pdev->dev, "gtcu_iface_clk");

	mmu->pt_base = KGSL_MMU_MAPPED_MEM_BASE;
	mmu->pt_size = (KGSL_MMU_MAPPED_MEM_SIZE - secured_pool_sz);

	status = kgsl_iommu_init_sync_lock(mmu);
	if (status)
		goto done;

	if (kgsl_msm_supports_iommu_v2()) {
		iommu->iommu_reg_list = kgsl_iommuv1_reg;
		iommu->ctx_offset = KGSL_IOMMU_CTX_OFFSET_V2;
		iommu->ctx_ahb_offset = KGSL_IOMMU_CTX_AHB_OFFSET_V2;
	} else if (msm_soc_version_supports_iommu_v0()) {
		iommu->iommu_reg_list = kgsl_iommuv0_reg;
		iommu->ctx_offset = KGSL_IOMMU_CTX_OFFSET_V0;
		iommu->ctx_ahb_offset = KGSL_IOMMU_CTX_OFFSET_V0;
	} else {
		iommu->iommu_reg_list = kgsl_iommuv1_reg;
		iommu->ctx_offset = KGSL_IOMMU_CTX_OFFSET_V1;
		iommu->ctx_ahb_offset = KGSL_IOMMU_CTX_OFFSET_V1;
	}

	kgsl_sharedmem_writel(mmu->device, &mmu->setstate_memory,
				KGSL_IOMMU_SETSTATE_NOP_OFFSET,
				cp_nop_packet(1));

	if (kgsl_guard_page == NULL) {
		kgsl_guard_page = alloc_page(GFP_KERNEL | __GFP_ZERO |
				__GFP_HIGHMEM);
		if (kgsl_guard_page == NULL) {
			status = -ENOMEM;
			goto done;
		}
	}

done:
	if (status) {
		kfree(iommu);
		mmu->priv = NULL;
	}
	return status;
}

static int kgsl_iommu_setup_defaultpagetable(struct kgsl_mmu *mmu)
{
	int status = 0;

	if (msm_soc_version_supports_iommu_v0()) {
		mmu->priv_bank_table =
			kgsl_mmu_getpagetable(mmu, KGSL_MMU_PRIV_PT);
		if (mmu->priv_bank_table == NULL) {
			status = -ENOMEM;
			goto err;
		}
	}
	mmu->defaultpagetable = kgsl_mmu_getpagetable(mmu, KGSL_MMU_GLOBAL_PT);
	
	if (mmu->defaultpagetable == NULL) {
		status = -ENOMEM;
		goto err;
	}

	if (mmu->secured) {
		mmu->securepagetable = kgsl_mmu_getpagetable(mmu,
				KGSL_MMU_SECURE_PT);
		
		if (mmu->securepagetable == NULL) {
			KGSL_DRV_ERR(mmu->device,
			"Unable to create secure pagetable, disable content protection\n");
			status = -ENOMEM;
			goto err;
		}
	}
	return status;
err:
	if (mmu->priv_bank_table) {
		kgsl_mmu_putpagetable(mmu->priv_bank_table);
		mmu->priv_bank_table = NULL;
	}
	if (mmu->defaultpagetable) {
		kgsl_mmu_putpagetable(mmu->defaultpagetable);
		mmu->defaultpagetable = NULL;
	}
	return status;
}

static void kgsl_iommu_lock_rb_in_tlb(struct kgsl_mmu *mmu)
{
	struct kgsl_device *device = mmu->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct adreno_ringbuffer *rb;
	struct kgsl_iommu *iommu = mmu->priv;
	unsigned int num_tlb_entries;
	unsigned int tlblkcr = 0;
	unsigned int v2pxx = 0;
	unsigned int vaddr = 0;
	int i, j, k, l;

	if (!iommu->sync_lock_initialized)
		return;

	rb = ADRENO_CURRENT_RINGBUFFER(adreno_dev);
	num_tlb_entries = rb->buffer_desc.size / PAGE_SIZE;

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (!iommu_unit->dev[j].attached)
				continue;
			tlblkcr = 0;
			tlblkcr |= (((num_tlb_entries *
				iommu_unit->dev_count) &
				KGSL_IOMMU_TLBLKCR_FLOOR_MASK) <<
				KGSL_IOMMU_TLBLKCR_FLOOR_SHIFT);
			
			tlblkcr	|= ((1 & KGSL_IOMMU_TLBLKCR_TLBIALLCFG_MASK)
				<< KGSL_IOMMU_TLBLKCR_TLBIALLCFG_SHIFT);
			tlblkcr	|= ((1 & KGSL_IOMMU_TLBLKCR_TLBIASIDCFG_MASK)
				<< KGSL_IOMMU_TLBLKCR_TLBIASIDCFG_SHIFT);
			tlblkcr	|= ((1 & KGSL_IOMMU_TLBLKCR_TLBIVAACFG_MASK)
				<< KGSL_IOMMU_TLBLKCR_TLBIVAACFG_SHIFT);
			
			tlblkcr |= ((1 & KGSL_IOMMU_TLBLKCR_LKE_MASK)
				<< KGSL_IOMMU_TLBLKCR_LKE_SHIFT);
			KGSL_IOMMU_SET_CTX_REG(iommu, iommu_unit,
					iommu_unit->dev[j].ctx_id,
					TLBLKCR, tlblkcr);
		}
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (!iommu_unit->dev[j].attached)
				continue;
			
			vaddr = rb->buffer_desc.gpuaddr;
			for (k = 0; k < num_tlb_entries; k++) {
				v2pxx = 0;
				v2pxx |= (((k + j * num_tlb_entries) &
					KGSL_IOMMU_V2PXX_INDEX_MASK)
					<< KGSL_IOMMU_V2PXX_INDEX_SHIFT);
				v2pxx |= vaddr & (KGSL_IOMMU_V2PXX_VA_MASK <<
						KGSL_IOMMU_V2PXX_VA_SHIFT);

				KGSL_IOMMU_SET_CTX_REG(iommu, iommu_unit,
						iommu_unit->dev[j].ctx_id,
						V2PUR, v2pxx);
				mb();
				vaddr += PAGE_SIZE;
				for (l = 0; l < iommu_unit->dev_count; l++) {
					tlblkcr = KGSL_IOMMU_GET_CTX_REG(iommu,
						iommu_unit,
						iommu_unit->dev[l].ctx_id,
						TLBLKCR);
					mb();
					tlblkcr &=
					~(KGSL_IOMMU_TLBLKCR_VICTIM_MASK
					<< KGSL_IOMMU_TLBLKCR_VICTIM_SHIFT);
					tlblkcr |= (((k + 1 +
					(j * num_tlb_entries)) &
					KGSL_IOMMU_TLBLKCR_VICTIM_MASK) <<
					KGSL_IOMMU_TLBLKCR_VICTIM_SHIFT);
					KGSL_IOMMU_SET_CTX_REG(iommu,
						iommu_unit,
						iommu_unit->dev[l].ctx_id,
						TLBLKCR, tlblkcr);
				}
			}
		}
		for (j = 0; j < iommu_unit->dev_count; j++) {
			if (!iommu_unit->dev[j].attached)
				continue;
			tlblkcr = KGSL_IOMMU_GET_CTX_REG(iommu, iommu_unit,
						iommu_unit->dev[j].ctx_id,
						TLBLKCR);
			mb();
			
			tlblkcr &= ~(KGSL_IOMMU_TLBLKCR_LKE_MASK
				<< KGSL_IOMMU_TLBLKCR_LKE_SHIFT);
			KGSL_IOMMU_SET_CTX_REG(iommu, iommu_unit,
				iommu_unit->dev[j].ctx_id, TLBLKCR, tlblkcr);
		}
	}
}

static int kgsl_iommu_start(struct kgsl_mmu *mmu)
{
	int status;
	struct kgsl_iommu *iommu = mmu->priv;
	int i, j;
	int sctlr_val = 0;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(mmu->device);

	if (mmu->defaultpagetable == NULL) {
		status = kgsl_iommu_setup_defaultpagetable(mmu);
		if (status)
			return -ENOMEM;
	}
	status = kgsl_iommu_start_sync_lock(mmu);
	if (status)
		return status;

	status = kgsl_attach_pagetable_iommu_domain(mmu);
	if (status)
		goto done;

	kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

	_iommu_lock(iommu);
	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++) {

			if ((!iommu_unit->dev[j].attached) ||
				(KGSL_IOMMU_CONTEXT_SECURE == j))
				continue;

			if ((!msm_soc_version_supports_iommu_v0()) &&
				(!(adreno_dev->ft_pf_policy &
				   KGSL_FT_PAGEFAULT_GPUHALT_ENABLE))) {
				sctlr_val = KGSL_IOMMU_GET_CTX_REG(iommu,
						iommu_unit,
						iommu_unit->dev[j].ctx_id,
						SCTLR);
				sctlr_val |= (0x1 <<
						KGSL_IOMMU_SCTLR_HUPCF_SHIFT);
				KGSL_IOMMU_SET_CTX_REG(iommu,
						iommu_unit,
						iommu_unit->dev[j].ctx_id,
						SCTLR, sctlr_val);
			}
			iommu_unit->dev[j].default_ttbr0 =
				KGSL_IOMMU_GET_CTX_REG_Q(iommu,
					iommu_unit,
					iommu_unit->dev[j].ctx_id, TTBR0);
		}
	}
	kgsl_iommu_lock_rb_in_tlb(mmu);
	_iommu_unlock(iommu);

	
	kgsl_cffdump_write(mmu->device, mmu->setstate_memory.gpuaddr +
				KGSL_IOMMU_SETSTATE_NOP_OFFSET,
				cp_nop_packet(1));

	kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

done:
	return status;
}

static void kgsl_iommu_flush_tlb_pt_current(struct kgsl_pagetable *pt,
				struct kgsl_memdesc *memdesc)
{
	struct kgsl_iommu *iommu = pt->mmu->priv;

	if (kgsl_memdesc_is_secured(memdesc))
		return;

	mutex_lock(&pt->mmu->device->mutex);
	if (kgsl_mmu_is_perprocess(pt->mmu) &&
		iommu->iommu_units[0].dev[KGSL_IOMMU_CONTEXT_USER].attached &&
		kgsl_iommu_pt_equal(pt->mmu, pt,
		kgsl_iommu_get_current_ptbase(pt->mmu)))
		kgsl_iommu_flush_pt(pt->mmu);
	mutex_unlock(&pt->mmu->device->mutex);
}

static int
kgsl_iommu_unmap(struct kgsl_pagetable *pt,
		struct kgsl_memdesc *memdesc)
{
	struct kgsl_device *device = pt->mmu->device;
	int ret = 0;
	unsigned int range = memdesc->size;
	struct kgsl_iommu_pt *iommu_pt = pt->priv;


	unsigned int gpuaddr = PAGE_ALIGN(memdesc->gpuaddr);

	if (range == 0 || gpuaddr == 0)
		return 0;

	if (kgsl_memdesc_has_guard_page(memdesc))
		range += kgsl_memdesc_guard_page_size(memdesc);

	if (kgsl_memdesc_is_secured(memdesc)) {

		if (!kgsl_mmu_is_secured(pt->mmu))
			return -EINVAL;

		mutex_lock(&device->mutex);
		ret = kgsl_active_count_get(device);
		if (!ret) {
			ret = iommu_unmap_range(iommu_pt->domain,
						gpuaddr, range);
			kgsl_active_count_put(device);
		}
		mutex_unlock(&device->mutex);
	} else
		ret = iommu_unmap_range(iommu_pt->domain, gpuaddr, range);
	if (ret) {
		KGSL_CORE_ERR("iommu_unmap_range(%p, %x, %d) failed "
			"with err: %d\n", iommu_pt->domain, gpuaddr,
			range, ret);
		return ret;
	}

	if (!kgsl_memdesc_is_global(memdesc))
		kgsl_iommu_flush_tlb_pt_current(pt, memdesc);

	return ret;
}

struct scatterlist *_create_sg_no_large_pages(struct kgsl_memdesc *memdesc)
{
	struct page *page;
	struct scatterlist *s, *s_temp, *sg_temp;
	int sglen_alloc = 0;
	uint64_t offset, pg_size;
	int i;

	for_each_sg(memdesc->sg, s, memdesc->sglen, i) {
		if (SZ_1M <= s->length) {
			sglen_alloc += s->length >> 16;
			sglen_alloc += ((s->length & 0xF000) >> 12);
		} else {
			sglen_alloc++;
		}
	}
	
	if (sglen_alloc == memdesc->sglen)
		return NULL;

	sg_temp = kgsl_malloc(sglen_alloc * sizeof(struct scatterlist));
	if (NULL == sg_temp)
		return ERR_PTR(-ENOMEM);

	sg_init_table(sg_temp, sglen_alloc);
	s_temp = sg_temp;

	for_each_sg(memdesc->sg, s, memdesc->sglen, i) {
		page = sg_page(s);
		if (SZ_1M <= s->length) {
			for (offset = 0; offset < s->length; s_temp++) {
				pg_size = ((s->length - offset) >= SZ_64K) ?
						SZ_64K : SZ_4K;
				sg_set_page(s_temp, page, pg_size, offset);
				offset += pg_size;
			}
		} else {
			sg_set_page(s_temp, page, s->length, 0);
			s_temp++;
		}
	}
	return sg_temp;
}

int _iommu_add_guard_page(struct kgsl_pagetable *pt,
						   struct kgsl_memdesc *memdesc,
						   unsigned int gpuaddr,
						   unsigned int protflags)
{
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	phys_addr_t physaddr = page_to_phys(kgsl_guard_page);
	int ret;

	if (kgsl_memdesc_has_guard_page(memdesc)) {

		if (kgsl_memdesc_is_secured(memdesc)) {
			if (!kgsl_secure_guard_page_memdesc.physaddr) {
				if (kgsl_cma_alloc_secure(pt->mmu->device,
					&kgsl_secure_guard_page_memdesc,
					SZ_1M)) {
					KGSL_CORE_ERR(
					"Secure guard page alloc failed\n");
					return -ENOMEM;
				}
			}

			physaddr = kgsl_secure_guard_page_memdesc.physaddr;
		}

		ret = iommu_map(iommu_pt->domain, gpuaddr, physaddr,
				kgsl_memdesc_guard_page_size(memdesc),
				protflags & ~IOMMU_WRITE);
		if (ret) {
			KGSL_CORE_ERR(
			"iommu_map(%p, addr %x, flags %x) err: %d\n",
			iommu_pt->domain, gpuaddr, protflags & ~IOMMU_WRITE,
			ret);
			return ret;
		}
	}

	return 0;
}

static int
kgsl_iommu_map(struct kgsl_pagetable *pt,
			struct kgsl_memdesc *memdesc)
{
	int ret = 0;
	unsigned int iommu_virt_addr;
	struct kgsl_iommu_pt *iommu_pt = pt->priv;
	size_t size = memdesc->size;
	unsigned int protflags;
	struct kgsl_device *device = pt->mmu->device;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(device);
	struct scatterlist *sg_temp = NULL;

	BUG_ON(NULL == iommu_pt);

	iommu_virt_addr = memdesc->gpuaddr;

	
	protflags = IOMMU_READ;
	if (!(memdesc->flags & KGSL_MEMFLAGS_GPUREADONLY))
		protflags |= IOMMU_WRITE;

	if (memdesc->priv & KGSL_MEMDESC_PRIVILEGED)
		protflags |= IOMMU_PRIV;

	if (kgsl_memdesc_is_secured(memdesc)) {

		if (!kgsl_mmu_is_secured(pt->mmu))
			return -EINVAL;

		mutex_lock(&device->mutex);
		ret = kgsl_active_count_get(device);
		if (!ret) {
			ret = iommu_map_range(iommu_pt->domain, iommu_virt_addr,
				memdesc->sg, size, protflags);
			kgsl_active_count_put(device);
		}
		mutex_unlock(&device->mutex);
	} else {
		sg_temp = _create_sg_no_large_pages(memdesc);

		if (IS_ERR(sg_temp))
			return PTR_ERR(sg_temp);

		ret = iommu_map_range(iommu_pt->domain, iommu_virt_addr,
				sg_temp ? sg_temp : memdesc->sg,
				size, protflags);
	}

	if (ret)
		KGSL_CORE_ERR("iommu_map_range(%p, %x, %p, %zd, %x) err: %d\n",
			iommu_pt->domain, iommu_virt_addr,
			sg_temp != NULL ? sg_temp : memdesc->sg, size,
			protflags, ret);

	kgsl_free(sg_temp);

	if (ret)
		return ret;

	ret = _iommu_add_guard_page(pt, memdesc, iommu_virt_addr + size,
								protflags);
	if (ret)
		
		iommu_unmap_range(iommu_pt->domain, iommu_virt_addr, size);


	if (ADRENO_FEATURE(adreno_dev, IOMMU_FLUSH_TLB_ON_MAP)
		&& !kgsl_memdesc_is_global(memdesc))
		kgsl_iommu_flush_tlb_pt_current(pt, memdesc);

	return ret;
}

static void kgsl_iommu_pagefault_resume(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	int i, j;

	if (atomic_read(&mmu->fault)) {
		kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_MAX_UNITS);
		for (i = 0; i < iommu->unit_count; i++) {
			struct kgsl_iommu_unit *iommu_unit =
						&iommu->iommu_units[i];
			for (j = 0; j < iommu_unit->dev_count; j++) {

				if ((!iommu_unit->dev[j].attached) ||
					(KGSL_IOMMU_CONTEXT_SECURE == j))
					continue;

				if (iommu_unit->dev[j].fault) {
					_iommu_lock(iommu);
					KGSL_IOMMU_SET_CTX_REG(iommu,
						iommu_unit,
						iommu_unit->dev[j].ctx_id,
						RESUME, 1);
					KGSL_IOMMU_SET_CTX_REG(iommu,
						iommu_unit,
						iommu_unit->dev[j].ctx_id,
						FSR, 0);
					_iommu_unlock(iommu);
					iommu_unit->dev[j].fault = 0;
				}
			}
		}
		kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);
		atomic_set(&mmu->fault, 0);
	}
}


static void kgsl_iommu_stop(struct kgsl_mmu *mmu)
{
	
	kgsl_detach_pagetable_iommu_domain(mmu);

	kgsl_iommu_pagefault_resume(mmu);
}

static int kgsl_iommu_close(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	int i;

	if (mmu->priv_bank_table != NULL)
		kgsl_mmu_putpagetable(mmu->priv_bank_table);

	if (mmu->defaultpagetable != NULL)
		kgsl_mmu_putpagetable(mmu->defaultpagetable);

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_memdesc *reg_map = &iommu->iommu_units[i].reg_map;

		
		kgsl_remove_global_pt_entry(reg_map);

		if (reg_map->hostptr)
			iounmap(reg_map->hostptr);
		kgsl_free(reg_map->sg);
	}
	

	kgsl_remove_global_pt_entry(&iommu->sync_lock_desc);
	kgsl_free(iommu->sync_lock_desc.sg);
	memset(&iommu->sync_lock_desc, 0, sizeof(iommu->sync_lock_desc));
	iommu->sync_lock_vars = NULL;

	kfree(iommu);

	if (kgsl_guard_page != NULL) {
		__free_page(kgsl_guard_page);
		kgsl_guard_page = NULL;
	}

	kgsl_sharedmem_free(&kgsl_secure_guard_page_memdesc);

	return 0;
}

static phys_addr_t
kgsl_iommu_get_current_ptbase(struct kgsl_mmu *mmu)
{
	phys_addr_t pt_base;
	struct kgsl_iommu *iommu = mmu->priv;
	if (in_interrupt())
		return 0;
	
	kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_MAX_UNITS);
	pt_base = KGSL_IOMMU_GET_CTX_REG_Q(iommu,
				(&iommu->iommu_units[0]),
				KGSL_IOMMU_CONTEXT_USER, TTBR0);
	kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);
	return pt_base & KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
}

static int kgsl_iommu_flush_pt(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	unsigned long wait_for_flush;
	unsigned int tlbflush_ctxt = KGSL_IOMMU_CONTEXT_USER;
	int i;
	int ret = 0;

	
	if (msm_soc_version_supports_iommu_v0()) {
		ret = adreno_spin_idle(mmu->device);
		if (ret)
			return ret;
	}
	kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

	
	_iommu_lock(iommu);

	for (i = 0; i < iommu->unit_count; i++) {
		KGSL_IOMMU_SET_CTX_REG(iommu, (&iommu->iommu_units[i]),
			tlbflush_ctxt, TLBIALL, 1);
		mb();
		if (!msm_soc_version_supports_iommu_v0()) {
			wait_for_flush = jiffies + msecs_to_jiffies(2000);
			KGSL_IOMMU_SET_CTX_REG(iommu,
				(&iommu->iommu_units[i]),
				tlbflush_ctxt, TLBSYNC, 0);
			while (KGSL_IOMMU_GET_CTX_REG(iommu,
				(&iommu->iommu_units[i]),
				tlbflush_ctxt, TLBSTATUS) &
				(KGSL_IOMMU_CTX_TLBSTATUS_SACTIVE)) {
				if (time_after(jiffies,
					wait_for_flush)) {
					KGSL_DRV_WARN(mmu->device,
					"Wait limit reached for IOMMU tlb flush\n");
					break;
				}
				cpu_relax();
			}
		}
	}
	
	_iommu_unlock(iommu);

	
	kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

	return ret;
}

static int kgsl_iommu_set_pt(struct kgsl_mmu *mmu,
				struct kgsl_pagetable *pt)
{
	struct kgsl_iommu *iommu = mmu->priv;
	int temp;
	int i;
	int ret = 0;
	phys_addr_t pt_base;
	uint64_t pt_val;

	kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

	pt_base = kgsl_iommu_get_pt_base_addr(mmu, pt);

	ret = adreno_spin_idle(mmu->device);
	if (ret)
		return ret;

	
	_iommu_lock(iommu);

	for (i = 0; i < iommu->unit_count; i++) {
		pt_val = kgsl_iommu_get_default_ttbr0(mmu, i,
					KGSL_IOMMU_CONTEXT_USER);

		pt_base &= KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
		pt_val &= ~KGSL_IOMMU_CTX_TTBR0_ADDR_MASK;
		pt_val |= pt_base;
		KGSL_IOMMU_SET_CTX_REG_Q(iommu,
				(&iommu->iommu_units[i]),
				KGSL_IOMMU_CONTEXT_USER, TTBR0, pt_val);

		mb();
		temp = KGSL_IOMMU_GET_CTX_REG_Q(iommu,
			(&iommu->iommu_units[i]),
			KGSL_IOMMU_CONTEXT_USER, TTBR0);
	}
	
	_iommu_unlock(iommu);

	kgsl_iommu_flush_pt(mmu);

	
	kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

	return ret;
}

static unsigned int kgsl_iommu_get_reg_gpuaddr(struct kgsl_mmu *mmu,
					int iommu_unit, int ctx_id, int reg)
{
	struct kgsl_iommu *iommu = mmu->priv;

	if (KGSL_IOMMU_GLOBAL_BASE == reg)
		return iommu->iommu_units[iommu_unit].reg_map.gpuaddr;

	if (iommu->iommu_reg_list[reg].ctx_reg)
		return iommu->iommu_units[iommu_unit].reg_map.gpuaddr +
			iommu->iommu_reg_list[reg].reg_offset +
			(ctx_id << KGSL_IOMMU_CTX_SHIFT) + iommu->ctx_offset;
	else
		return iommu->iommu_units[iommu_unit].reg_map.gpuaddr +
			iommu->iommu_reg_list[reg].reg_offset;
}
static int kgsl_iommu_hw_halt_supported(struct kgsl_mmu *mmu, int iommu_unit)
{
	struct kgsl_iommu *iommu = mmu->priv;
	return iommu->iommu_units[iommu_unit].iommu_halt_enable;
}

static int kgsl_iommu_get_num_iommu_units(struct kgsl_mmu *mmu)
{
	struct kgsl_iommu *iommu = mmu->priv;
	return iommu->unit_count;
}

static int kgsl_iommu_set_pf_policy(struct kgsl_mmu *mmu,
				unsigned int pf_policy)
{
	int i, j;
	struct kgsl_iommu *iommu = mmu->priv;
	struct adreno_device *adreno_dev = ADRENO_DEVICE(mmu->device);
	int ret = 0;
	unsigned int sctlr_val;

	if ((adreno_dev->ft_pf_policy & KGSL_FT_PAGEFAULT_GPUHALT_ENABLE) ==
		(pf_policy & KGSL_FT_PAGEFAULT_GPUHALT_ENABLE))
		return ret;
	if (msm_soc_version_supports_iommu_v0())
		return ret;

	kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

	
	ret = mmu->device->ftbl->idle(mmu->device);
	if (ret) {
		kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);
		return ret;
	}

	for (i = 0; i < iommu->unit_count; i++) {
		struct kgsl_iommu_unit *iommu_unit = &iommu->iommu_units[i];
		for (j = 0; j < iommu_unit->dev_count; j++) {

			if ((!iommu_unit->dev[j].attached) ||
				(KGSL_IOMMU_CONTEXT_SECURE == j))
				continue;

			sctlr_val = KGSL_IOMMU_GET_CTX_REG(iommu,
					iommu_unit,
					iommu_unit->dev[j].ctx_id,
					SCTLR);
			if (pf_policy & KGSL_FT_PAGEFAULT_GPUHALT_ENABLE)
				sctlr_val &= ~(0x1 <<
					KGSL_IOMMU_SCTLR_HUPCF_SHIFT);
			else
				sctlr_val |= (0x1 <<
					KGSL_IOMMU_SCTLR_HUPCF_SHIFT);
			KGSL_IOMMU_SET_CTX_REG(iommu,
					iommu_unit,
					iommu_unit->dev[j].ctx_id,
					SCTLR, sctlr_val);
		}
	}

	kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);
	return ret;
}

static void kgsl_iommu_set_pagefault(struct kgsl_mmu *mmu)
{
	int i, j;
	struct kgsl_iommu *iommu = mmu->priv;
	unsigned int fsr;

	
	if (atomic_read(&mmu->fault))
		return;

	kgsl_iommu_enable_clk(mmu, KGSL_IOMMU_MAX_UNITS);

	
	for (i = 0; i < iommu->unit_count; i++) {
		for (j = 0; j < iommu->iommu_units[i].dev_count; j++) {

			if ((!iommu->iommu_units[i].dev[j].attached) ||
				(KGSL_IOMMU_CONTEXT_SECURE == j))
				continue;

			fsr = KGSL_IOMMU_GET_CTX_REG(iommu,
				(&(iommu->iommu_units[i])),
				iommu->iommu_units[i].dev[j].ctx_id, FSR);
			if (fsr) {
				uint64_t far =
					KGSL_IOMMU_GET_CTX_REG_Q(iommu,
					(&(iommu->iommu_units[i])),
					iommu->iommu_units[i].dev[j].ctx_id,
					FAR);
				kgsl_iommu_fault_handler(NULL,
				iommu->iommu_units[i].dev[j].dev, far, 0, NULL);
				break;
			}
		}
	}

	kgsl_iommu_disable_clk(mmu, KGSL_IOMMU_MAX_UNITS);
}

struct kgsl_protected_registers *kgsl_iommu_get_prot_regs(struct kgsl_mmu *mmu)
{
	static struct kgsl_protected_registers iommuv1_regs = { 0x4000, 14 };
	static struct kgsl_protected_registers iommuv2_regs;

	if (msm_soc_version_supports_iommu_v0())
		return NULL;
	if (kgsl_msm_supports_iommu_v2()) {

		struct kgsl_iommu *iommu = mmu->priv;

		
		iommuv2_regs.base = iommu->iommu_units[0].ahb_base >> 2;
		iommuv2_regs.range = 10;
		return &iommuv2_regs;
	}
	else
		return &iommuv1_regs;
}

struct kgsl_mmu_ops kgsl_iommu_ops = {
	.mmu_init = kgsl_iommu_init,
	.mmu_close = kgsl_iommu_close,
	.mmu_start = kgsl_iommu_start,
	.mmu_stop = kgsl_iommu_stop,
	.mmu_set_pt = kgsl_iommu_set_pt,
	.mmu_pagefault_resume = kgsl_iommu_pagefault_resume,
	.mmu_get_current_ptbase = kgsl_iommu_get_current_ptbase,
	.mmu_enable_clk = kgsl_iommu_enable_clk,
	.mmu_disable_clk = kgsl_iommu_disable_clk,
	.mmu_get_default_ttbr0 = kgsl_iommu_get_default_ttbr0,
	.mmu_get_reg_gpuaddr = kgsl_iommu_get_reg_gpuaddr,
	.mmu_get_reg_ahbaddr = kgsl_iommu_get_reg_ahbaddr,
	.mmu_get_num_iommu_units = kgsl_iommu_get_num_iommu_units,
	.mmu_pt_equal = kgsl_iommu_pt_equal,
	.mmu_get_pt_base_addr = kgsl_iommu_get_pt_base_addr,
	.mmu_hw_halt_supported = kgsl_iommu_hw_halt_supported,
	
	.mmu_sync_lock = kgsl_iommu_sync_lock,
	.mmu_sync_unlock = kgsl_iommu_sync_unlock,
	.mmu_set_pf_policy = kgsl_iommu_set_pf_policy,
	.mmu_set_pagefault = kgsl_iommu_set_pagefault,
	.mmu_get_prot_regs = kgsl_iommu_get_prot_regs
};

struct kgsl_mmu_pt_ops iommu_pt_ops = {
	.mmu_map = kgsl_iommu_map,
	.mmu_unmap = kgsl_iommu_unmap,
	.mmu_create_pagetable = kgsl_iommu_create_pagetable,
	.mmu_create_secure_pagetable = kgsl_iommu_create_secure_pagetable,
	.mmu_destroy_pagetable = kgsl_iommu_destroy_pagetable,
	.get_ptbase = kgsl_iommu_get_ptbase,
};