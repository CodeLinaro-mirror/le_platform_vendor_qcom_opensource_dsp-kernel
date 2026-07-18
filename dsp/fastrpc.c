// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2011-2018, The Linux Foundation. All rights reserved.
 * Copyright (c) 2018, Linaro Limited
 * Copyright (c) 2022-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */
#include <linux/devcoredump.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/dma-resv.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/sort.h>
#include <linux/of_platform.h>
#include <linux/iommu.h>
#include <linux/msm_dma_iommu_mapping.h>
#include <linux/genalloc.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/pm_qos.h>
#include "../include/uapi/misc/fastrpc.h"
#include "../include/linux/fastrpc.h"
#include <linux/of_reserved_mem.h>
#include <linux/cred.h>
#include <linux/arch_topology.h>
#include <linux/mem-buf.h>
#include <soc/qcom/secure_buffer.h>
#include "fastrpc_shared.h"
#include "fastrpc_scheduler.h"
#include <linux/platform_device.h>
#include <linux/types.h>
#if FRPC_RING_BUFFER_ENABLED
#include <linux/ring_buffer.h>
#endif
#include <linux/version.h>
#define CREATE_TRACE_POINTS
#include "fastrpc_trace.h"
#include "fastrpc_timeline.h"

/*
 * The size of the hash table used to store fastrpc domains.
 * This value determines the max number of domains that can be
 * added to the table..
 */
#define DOMAINS_TABLE_SIZE 8

/* Struct to hold globally used variables */
struct fastrpc_common {
	/*
	 * use spin lock to protect global resources that are also accessed
	 * in interrupt context
	 */
	spinlock_t glock;

	/*
	 * use mutex to protect global resources that will never be accessed
	 * in interrupt context
	 */
	struct mutex gmut;

	/* Mutex to protect access of global domains hash tables */
	struct mutex hmut;

	/*
	 * Declare a hash table to store fastrpc domains.
	 * The hash table is used to efficiently manage and look up fastrpc domains.
	 */
	DECLARE_HASHTABLE(fastrpc_domains_table, DOMAINS_TABLE_SIZE);

	/* Global counter for number of dsp's of each type that booted up */
	int dsp_counter[FASTRPC_MAX_DSP_TYPE];

	/* global list of multidomain context ids */
	struct idr mdctx_idr;

	/* Flag to check if the kernel is in trusted VM */
	bool is_trusted_vm;

	/*
	 * Debug-only flag: when true, BUG_ON() on a FastRPC SSR timeout
	 * instead of the normal recovery (bypass or real SSR). Off by
	 * default; only settable via the debugfs node below, which is
	 * only meaningful on debug builds.
	 */
	bool debug_mode_enable;

#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_root;
	struct dentry *debugfs_global_file;
#endif
};

/* Global fastrpc driver object */
struct fastrpc_common g_frpc;

bool fastrpc_debug_mode_enabled(void)
{
	return g_frpc.debug_mode_enable;
}

static void fastrpc_user_release(struct kref *ref);

int fastrpc_file_get(struct fastrpc_user *fl)
{
	return kref_get_unless_zero(&fl->refcount) ? 0 : -ENOENT;
}

void fastrpc_file_put(struct fastrpc_user *fl, bool worker)
{
	if (worker) {
		struct kref *kref = &fl->refcount;
		u32 old, new, val = atomic_read(&kref->refcount.refs);

		/*
		 * Schedule the job to a worker thread if the user-object
		 * reference count is 1; otherwise, simply decrease the
		 * refcount.
		 */
		if (val > 1 && likely(val != UINT_MAX)) {

			/* Loop until the refcount is successfully decremented */
			for (;;) {
				new = val - 1;

				/* Schedule the job to a worker thread if refcount is 1 */
				if (val == 1)
					goto schedule_work;

				/*
				 * Atomically compare the reference counter with "val" and
				 * replace it with "new" only if they match. The
				 * atomic_cmpxchg_release function always returns the
				 * previous value. If the old value matches the expected
				 * "val," the atomic_cmpxchg_release operation succeeds.
				 */
				old = atomic_cmpxchg_release(&kref->refcount.refs, val, new);

				/*
				 * Break the loop if the atomic operation was successful
				 * to decrement the user-object refcount.
				 */
				if (old == val)
					break;

				/*
				 * Update "val" with the most recent value read during
				 * the failed atomic operation to retry.
				 */
				val = old;
			}
		} else if (val == 1) {
			goto schedule_work;
		}
	} else {
		kref_put(&fl->refcount, fastrpc_user_release);
	}
	return;
schedule_work:

	/*
	 * In case the reference count is 1 for the user-object,
	 * its release function cannot be called in an interrupt context.
	 * So schedule the job to a worker thread.
	 */
	schedule_work(&fl->put_work);

}

/*
 * Extracts a job from the worker and releases the user-object reference.
 *
 * @param work Pointer to the worker.
 *
 * @return None.
 */
static void fastrpc_file_put_worker(struct work_struct *work)
{
	struct fastrpc_user *fl =
			container_of(work, struct fastrpc_user, put_work);

	fastrpc_file_put(fl, false);
}

static inline int64_t getnstimediff(struct timespec64 *start)
{
	int64_t ns;
	struct timespec64 ts, b;

	ktime_get_real_ts64(&ts);
	b = timespec64_sub(ts, *start);
	ns = timespec64_to_ns(&b);
	return ns;
}

static int fastrpc_device_create(struct fastrpc_user *fl);

static int fastrpc_multidomain_ctx_cleanup(struct fastrpc_user *fl,
	uint32_t req, uint64_t ctx);

static struct fastrpc_domain *fastrpc_lookup_domain_in_table(u32 key,
	bool use_phy_id);

static int fastrpc_convert_legacy_id_to_logical_id(u32 legacy_id,
	u32 *logical_id);
static int fastrpc_release_current_dsp_process(struct fastrpc_user *fl);

void fastrpc_queue_pd_status(struct fastrpc_user *fl, int domain,
	int status, int sessionid);

/*
 * Checks if a given logical domain id is valid.
 *
 * @param domain_id Logical domain ID to check.
 *
 * @return true if the domain ID is valid, false otherwise.
 */
static bool fastrpc_is_valid_logical_domain_id(u32 domain_id)
{
	struct fastrpc_domain *domain = fastrpc_lookup_domain_in_table(domain_id,
		false);
	return domain ? true : false;
}

/*
 * Retrieves the fastrpc channel context for a given Logical domain ID.
 *
 * @param domain_id Logical domain id of channel context
 *
 * @return A pointer to the fastrpc channel context for the
 *         specified domain or NULL if the domain is not found.
 */
static inline struct fastrpc_channel_ctx
	*fastrpc_get_domain_channel_ctx(int domain_id)
{
	struct fastrpc_domain *domain = fastrpc_lookup_domain_in_table(domain_id,
		false);
	return domain ? domain->cctx : NULL;
}

void __dma_buf_unmap_attachment_wrap(struct dma_buf_attachment *attach,
	struct sg_table *table)
{
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,2,0))
	dma_buf_unmap_attachment_unlocked(attach, table, DMA_BIDIRECTIONAL);
#else
	dma_buf_unmap_attachment(attach, table, DMA_BIDIRECTIONAL);
#endif
}

static void dma_buf_unmap_attachment_wrap(struct fastrpc_map *map)
{
	trace_fastrpc_dma_unmap(map->fl->cctx->domain_id, map->phys,
		map->size, map->fd);

	__dma_buf_unmap_attachment_wrap(map->attach, map->table);
}

struct sg_table *__dma_buf_map_attachment_wrap(struct dma_buf_attachment *attach)
{
	struct sg_table *table;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,2,0))
	table = dma_buf_map_attachment_unlocked(attach,
		DMA_BIDIRECTIONAL);
#else
	table = dma_buf_map_attachment(attach,
		DMA_BIDIRECTIONAL);
#endif

	return table;
}

static int dma_buf_map_attachment_wrap(struct fastrpc_map *map)
{
	int err = 0;
	struct sg_table *table;

	table = __dma_buf_map_attachment_wrap(map->attach);
	if (IS_ERR(table)) {
		err = PTR_ERR(table);
		return err;
	}

	map->table = table;

	return 0;
}

static inline void __fastrpc_dma_map_free(struct fastrpc_map *map)
{
	dma_buf_unmap_attachment_wrap(map);
	dma_buf_detach(map->buf, map->attach);
	dma_buf_put(map->buf);
}

static void __fastrpc_free_map(struct fastrpc_map *map)
{
	struct fastrpc_user *fl = NULL;
	struct fastrpc_smmu *smmucb = NULL;
	__maybe_unused bool iova_in_use = false;
	bool retain_iova = false;

	if (!map)
		return;

	fl = map->fl;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0))
	iova_in_use = dma_use_iova(&map->iova_state);
#endif
	retain_iova = (map->attr == FASTRPC_MAP_ATTR_RETAIN_IOVA);

	/*
	 * If mapping is being removed but IOVA is retained, skip
	 * removing corresponding entry from map-list.
	 */
	if (fl && !retain_iova) {
		spin_lock(&map->fl->lock);
		list_del(&map->node);
		spin_unlock(&map->fl->lock);
	}

	if (map->table) {
		if (fl && (map->attr & FASTRPC_ATTR_SECUREMAP)) {
			struct qcom_scm_vmperm perm;
			int vmid = fl->cctx->vmperms[0].vmid;
			u64 src_perms = BIT(QCOM_SCM_VMID_HLOS) | BIT(vmid);
			int err = 0;

			perm.vmid = QCOM_SCM_VMID_HLOS;
			perm.perm = QCOM_SCM_PERM_RWX;
			err = qcom_scm_assign_mem(map->phys, map->size,
				&src_perms, &perm, 1);
			if (err) {
				dev_err(fl->cctx->dev,
					"Failed to assign memory phys 0x%llx size 0x%llx err %d",
						map->phys, map->size, err);
				goto free_map;
			}
		}
		/*
		 * FASTRPC_MAP_FD_NOMAP and FASTRPC_ATTR_NOMAP
		 * is not mapped on SMMU CB device
		 */
		if (map->attr & FASTRPC_ATTR_NOMAP ||
			map->flags == FASTRPC_MAP_FD_NOMAP) {
			__fastrpc_dma_map_free(map);
		} else {
			smmucb = map->smmucb;
			mutex_lock(&smmucb->map_mutex);
			if (!smmucb->dev) {
				mutex_unlock(&smmucb->map_mutex);
				goto free_map;
			}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0))
			if (iova_in_use) {
				if (map->phys) {
					/*
					 * Remove smmu mapping.
					 * This does NOT release iova region.
					 */
					dma_iova_unlink(smmucb->dev, &map->iova_state,
						0, map->size, DMA_BIDIRECTIONAL, 0);
					map->phys = 0;
				}

				/*
				 * If buffer is being unmapped without 'retain iova' attribute,
				 * then release the iova region as well.
				 */
				if (!retain_iova)
					dma_iova_free(smmucb->dev, &map->iova_state);
			}
#endif

			if (map->buf) {
				__fastrpc_dma_map_free(map);
				map->buf = NULL;
			}
			if (!retain_iova) {
				/* Do not update smmu device stats if iova region was retained */
				smmucb->allocatedbytes -= SMMU_ALIGN(map->size);
			}

			mutex_unlock(&smmucb->map_mutex);
		}
	}

free_map:
	if (!retain_iova)
		kfree(map);
}

static void fastrpc_free_map(struct kref *ref)
{
	struct fastrpc_map *map = NULL;

	map = container_of(ref, struct fastrpc_map, refcount);
	__fastrpc_free_map(map);
}

static void fastrpc_map_put(struct fastrpc_map *map)
{
	if (map)
		kref_put(&map->refcount, fastrpc_free_map);
}

static int fastrpc_map_get(struct fastrpc_map *map)
{
	if (!map)
		return -ENOENT;

	return kref_get_unless_zero(&map->refcount) ? 0 : -ENOENT;
}


static int fastrpc_map_lookup(struct fastrpc_user *fl, int fd,
			    u64 va, u64 len, struct dma_buf *buf, int mflags,
			    struct fastrpc_map **ppmap, bool take_ref,
				uint32_t attrs)
{
	struct fastrpc_pool_ctx *sess = fl->sctx;
	struct fastrpc_map *map = NULL, *found = NULL;
	int ret = -ENOENT;
	bool buf_put_needed = false,
		retained_map = (attrs == FASTRPC_MAP_ATTR_RETAIN_IOVA);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0))
	if (retained_map)
		return -EOPNOTSUPP;
#endif
	if (mflags == ADSP_MMAP_DMA_BUFFER) {
		if (!buf)
			return -ENOENT;
	} else {
		/* Fetch DMA buffer from fd */
		buf = dma_buf_get(fd);
		if (IS_ERR(buf))
			return PTR_ERR(buf);

		buf_put_needed = true;
	}

	spin_lock(&fl->lock);
	list_for_each_entry(map, &fl->maps, node) {
		/*
		 * For regular map, multiple fd's can point to same dma-buf.
		 * Create separate mapping for each duplicated fd.
		 *
		 * For map with 'retain-iova' attribute, client can request
		 * to map the same fd which is now pointing to a different
		 * dma-buf. So skip the dma-buf check.
		 */
		if (map->fd == fd && (retained_map || map->buf == buf)) {
			found = map;
			ret = 0;
			break;
		}
	}

	if (!found)
		goto bail;

	/*
	 * Validate that the found map has same attributes as the one passed
	 * by user. In case of 'retain-iova' attribute, also validate that
	 * the current ref-count of map has been reset to 0.
	 */
	if (found->attr != attrs ||
		(retained_map && (kref_read(&map->refcount) != 0))) {
		found = NULL;
		ret = -EBADFD;
		goto bail;
	}

	/*
	 * For buffers with 'retain-iova' attribute, refcount can be only
	 * 1 or 0 i.e. only 1 mapping request can be made.
	 * For all other attributes, multiple mapping requests can be made on
	 * the same buffer, and its ref-count will be incremented in
	 * subsequent requests.
	 */
	if (take_ref && !retained_map) {
		ret = fastrpc_map_get(map);
		if (ret) {
			dev_dbg(sess->smmucb[DEFAULT_SMMU_IDX].dev,
				"%s: Failed to get map fd=%d ret=%d\n",
				__func__, fd, ret);
			found = NULL;
			ret = -ENOENT;
		}
	}
bail:
	spin_unlock(&fl->lock);

	/* Drop the DMA buf ref */
	if (buf_put_needed)
		dma_buf_put(buf);

	if (!ret)
		*ppmap = found;

	return ret;
}

static bool fastrpc_get_persistent_buf(struct fastrpc_user *fl,
		size_t size, int buf_type, struct fastrpc_buf **obuf)
{
	u32 i = 0;
	bool found = false;
	struct fastrpc_buf *buf = NULL;

	spin_lock(&fl->lock);
	/*
	 * Persistent header buffer can be used only if
	 * metadata length is less than 1 page size.
	 */
	if (!fl->num_pers_hdrs || buf_type != METADATA_BUF || size > PAGE4K_SIZE) {
		spin_unlock(&fl->lock);
		return found;
	}

	for (i = 0; i < fl->num_pers_hdrs; i++) {
		buf = &fl->hdr_bufs[i];
		/* If buffer not in use, then assign it for requested alloc */
		if (!buf->in_use) {
			buf->in_use = true;
			*obuf = buf;
			found = true;
			break;
		}
	}
	spin_unlock(&fl->lock);
	return found;
}

static inline void fastrpc_dma_buf_free(struct fastrpc_buf *buf)
{
	trace_fastrpc_dma_free(buf->domain_id, buf->phys, buf->size);

	__fastrpc_dma_buf_free(buf);

	kfree(buf);
}

static void __fastrpc_buf_free(struct fastrpc_buf *buf)
{
	struct fastrpc_smmu *smmucb = NULL;

	/* REMOTEHEAP_BUF is not mapped on SMMU device */
	if (buf->type == REMOTEHEAP_BUF) {
		fastrpc_dma_buf_free(buf);
	} else {
		smmucb = buf->smmucb;
		mutex_lock(&smmucb->map_mutex);
		if (smmucb->dev) {
			smmucb->allocatedbytes -= SMMU_ALIGN(buf->size);
			fastrpc_dma_buf_free(buf);
		}
		mutex_unlock(&smmucb->map_mutex);
	}
}

static void fastrpc_cached_buf_list_add(struct fastrpc_buf *buf)
{
	struct fastrpc_user *fl = buf->fl;

	if (buf->size < FASTRPC_MAX_CACHE_BUF_SIZE) {
		spin_lock(&fl->lock);
		if (fl->num_cached_buf > FASTRPC_MAX_CACHED_BUFS) {
			spin_unlock(&fl->lock);
			goto skip_buf_cache;
		}

		list_add_tail(&buf->node, &fl->cached_bufs);
		fl->num_cached_buf++;
		buf->type = -1;
		spin_unlock(&fl->lock);
		return;
	}

skip_buf_cache:
	__fastrpc_buf_free(buf);
	return;
}

void fastrpc_buf_free(struct fastrpc_buf *buf, bool cache)
{
	struct fastrpc_user *fl = buf->fl;

	if (buf->in_use) {
		/* Don't free persistent header buf. Just mark as available */
		spin_lock(&fl->lock);
		buf->in_use = false;
		spin_unlock(&fl->lock);
		return;
	}
	if (cache)
		fastrpc_cached_buf_list_add(buf);
	else
		__fastrpc_buf_free(buf);
}

static inline bool fastrpc_get_cached_buf(struct fastrpc_user *fl,
		size_t size, int buf_type, struct fastrpc_buf **obuf)
{
	bool found = false;
	struct fastrpc_buf *buf, *n, *cbuf = NULL;

	if (buf_type == USER_BUF || buf_type == REMOTEHEAP_BUF)
		return found;

	/* find the smallest buffer that fits in the cache */
	spin_lock(&fl->lock);
	list_for_each_entry_safe(buf, n, &fl->cached_bufs, node) {
		if (buf->size >= size && (!cbuf || cbuf->size > buf->size))
			cbuf = buf;
	}
	if (cbuf) {
		list_del_init(&cbuf->node);
		fl->num_cached_buf--;
	}
	spin_unlock(&fl->lock);
	if (cbuf) {
		cbuf->type = buf_type;
		*obuf = cbuf;
		found = true;
	}

	return found;
}

static void fastrpc_buf_list_free(struct fastrpc_user *fl,
	struct list_head *buf_list, bool is_cached_buf)
{
	struct fastrpc_buf *buf = NULL, *n = NULL, *free = NULL;

	do {
		free = NULL;
		spin_lock(&fl->lock);
		list_for_each_entry_safe(buf, n, buf_list, node) {
			list_del(&buf->node);
			if (is_cached_buf)
				fl->num_cached_buf--;
			free = buf;
			break;
		}
		spin_unlock(&fl->lock);
		if (free)
			fastrpc_buf_free(free, false);
	} while (free);
}

/*
 * Free list of buffers donated for rootheap
 * @arg1: channel context.
 * @arg2: rootpd session context
 *
 * Returns void
 */
static void fastrpc_rootheap_buf_list_free(struct fastrpc_channel_ctx *cctx)
{
	struct fastrpc_buf *buf = NULL, *n = NULL, *free = NULL;
	unsigned long flags = 0;

	/* Return if no rootheap buffers were donated */
	if (!cctx->rootheap_bufs.num)
		return;

	do {
		free = NULL;
		spin_lock_irqsave(&cctx->lock, flags);
		list_for_each_entry_safe(buf, n, &cctx->rootheap_bufs.list, node) {
			list_del(&buf->node);
			cctx->rootheap_bufs.num--;
			free = buf;
			break;
		}
		spin_unlock_irqrestore(&cctx->lock, flags);

		if (free)
			__fastrpc_buf_free(free);
	} while (free);
}

static inline int fastrpc_dma_alloc(struct fastrpc_buf *buf)
{
	struct fastrpc_smmu *smmucb = buf->smmucb;
	int err = 0;

	if (!buf->dev)
		return -EINVAL;

	err = __fastrpc_dma_alloc(buf);
	if (err)
		return err;

	if (buf->type == REMOTEHEAP_BUF)
		return 0;

	smmucb->allocatedbytes += SMMU_ALIGN(buf->size);
	RECONSTRUCT_IOVA_FROM_SID_PA((u64)smmucb->sid,
		buf->phys, smmucb->sid_pos);

	return 0;
}

static int __fastrpc_buf_alloc(struct fastrpc_user *fl,
		struct fastrpc_smmu *smmucb, u32 domain_id,
		u64 size, struct fastrpc_buf **obuf, u32 buf_type)
{
	struct fastrpc_buf *buf;
	struct timespec64 start_ts, end_ts;
	int err = 0;

	/* Check if the size is valid (non-zero and within integer range) */
	if (!size || size > INT_MAX)
		return -EFAULT;
	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	INIT_LIST_HEAD(&buf->attachments);
	INIT_LIST_HEAD(&buf->node);
	mutex_init(&buf->lock);

	buf->fl = fl;
	buf->virt = NULL;
	buf->phys = 0;
	buf->size = size;
	buf->raddr = 0;
	buf->type = buf_type;
	buf->domain_id = domain_id;
	ktime_get_boottime_ts64(&start_ts);

	/* REMOTEHEAP_BUF is allocated using cctx device */
	if (buf_type == REMOTEHEAP_BUF) {
		buf->dev = fl->cctx->dev;
		/*
		 * Do not acquire spinlock with IRQ disabled
		 * as "dma_alloc_coherent" locks a mutex
		 */
		err = fastrpc_dma_alloc(buf);
	} else {
		buf->dev = smmucb->dev;
		buf->smmucb = smmucb;
		mutex_lock(&smmucb->map_mutex);
		err = fastrpc_dma_alloc(buf);
		mutex_unlock(&smmucb->map_mutex);
	}

	if (err) {
		mutex_destroy(&buf->lock);
		kfree(buf);
		return -ENOMEM;
	}

	*obuf = buf;

	trace_fastrpc_dma_alloc(domain_id, (uint64_t)buf->phys, buf->size,
								(unsigned long)buf->type, 0);
	ktime_get_boottime_ts64(&end_ts);
	buf->alloc_time = timespec64_sub(end_ts, start_ts);
	return 0;
}

static int fastrpc_buf_alloc(struct fastrpc_user *fl,
			struct fastrpc_smmu *smmucb, u64 size,
			u32 buf_type, struct fastrpc_buf **obuf)
{
	int ret;

	if (fastrpc_get_persistent_buf(fl, size, buf_type, obuf))
		return 0;
	if (fastrpc_get_cached_buf(fl, size, buf_type, obuf))
		return 0;
	ret = __fastrpc_buf_alloc(fl, smmucb, fl->cctx->domain_id,
						size, obuf, buf_type);
	if (ret == -ENOMEM) {
		fastrpc_buf_list_free(fl, &fl->cached_bufs, true);
		ret = __fastrpc_buf_alloc(fl, smmucb, fl->cctx->domain_id,
					size, obuf, buf_type);
		if (ret)
			return ret;
	}

	return ret;
}

/**
 * fastrpc_smmu_device_lookup() -
 * Function to get IOMMU device index from the session pool
 * @arg1: Fastrpc pool session
 * @arg2: Allocation/map buffer size
 * @arg3: Current IOMMU pool device index
 *
 * Starting from current IOMMU pool device index, function
 * finds a IOMMU CB where there is enough virtual space available
 * to allocate/map buffer size.
 *
 * Return: Returns IOMMU pool device index,
 *         where virtual space is available
 */
static u32 fastrpc_smmu_device_lookup(struct fastrpc_pool_ctx *sess,
						u64 size, u32 smmuidx)
{
	struct fastrpc_smmu *smmucb = NULL;

	for (; smmuidx < sess->smmucount; smmuidx++) {
		smmucb = &sess->smmucb[smmuidx];
		/*
		 * Use the SMMU index device, if the SMMU pool
		 * alloc ranges are not defined.
		 */
		if (smmucb->maxallocsize == 0)
			break;

		if (size >= smmucb->minallocsize &&
			size < (smmucb->totalbytes - smmucb->allocatedbytes))
			break;
	}

	return smmuidx;
}

/**
 * fastrpc_smmu_buf_alloc() - Allocates memory on IOMMU CB
 * @arg1: Fastrpc user file pointer
 * @arg2: Allocation buffer size
 * @arg3: Allocation buffer type
 * @arg4: Output argument pointer to the fastrpc_buf
 *
 * Return: Returns 0 on success, error code on failure
 */
int fastrpc_smmu_buf_alloc(struct fastrpc_user *fl, u64 size,
						u32 buf_type, struct fastrpc_buf **obuf)
{
	int err = 0;
	struct fastrpc_pool_ctx *sess = NULL;
	struct fastrpc_smmu *smmucb = NULL;
	u32 smmuidx = DEFAULT_SMMU_IDX;

	sess = fl->sctx;
retry_alloc:
	smmuidx = fastrpc_smmu_device_lookup(sess, size, smmuidx);
	if (smmuidx >= sess->smmucount) {
		dev_err(fl->cctx->dev,
			"%s: No valid smmu context bank found for size 0x%llx\n",
			__func__, size);
		err = -ENOSR;
		return err;
	} else {
		smmucb = &sess->smmucb[smmuidx];
	}

	err = fastrpc_buf_alloc(fl, smmucb, size, buf_type, obuf);
	/*
	 * Retry allocation on next availale IOMMU CB,
	 * if there is no enough virtual space available on current IOMMU CB
	 */
	if (err == -ENOMEM || err == -EINVAL) {
		smmuidx++;
		goto retry_alloc;
	}
	return err;
}

static void fastrpc_channel_ctx_free(struct kref *ref)
{
	struct fastrpc_channel_ctx *cctx;
	int i, j;

	cctx = container_of(ref, struct fastrpc_channel_ctx, refcount);
	fastrpc_channel_default_user_delete(cctx);
	mutex_destroy(&cctx->wake_mutex);

	/*
	 * Free the NPU priority table. In the normal teardown path (no SSR)
	 * this is the only place it gets freed. In the SSR path,
	 * fastrpc_rpmsg_remove already freed it and NULLed the pointer,
	 * so this block is skipped.
	 */
	if (cctx->npu_app_prio) {
		kfree(cctx->npu_app_prio->entries);
		kfree(cctx->npu_app_prio);
		cctx->npu_app_prio = NULL;
	}



	for (i = 0; i < FASTRPC_MAX_SESSIONS; i++)
		for (j = 0; j < cctx->session[i].smmucount; j++)
			mutex_destroy(&cctx->session[i].smmucb[j].map_mutex);
	ida_destroy(&cctx->tgid_frpc_ida);
	kvfree(cctx);
}

void fastrpc_channel_ctx_get(struct fastrpc_channel_ctx *cctx)
{
	kref_get(&cctx->refcount);
}

void fastrpc_channel_ctx_put(struct fastrpc_channel_ctx *cctx)
{
	kref_put(&cctx->refcount, fastrpc_channel_ctx_free);
}

static void fastrpc_context_free(struct kref *ref)
{
	struct fastrpc_invoke_ctx *ctx;
	struct fastrpc_channel_ctx *cctx;
	unsigned long flags;
	int i;

	ctx = container_of(ref, struct fastrpc_invoke_ctx, refcount);
	cctx = ctx->cctx;

	mutex_lock(&ctx->fl->map_mutex);
	for (i = 0; i < ctx->nbufs; i++)
		fastrpc_map_put(ctx->maps[i]);
	mutex_unlock(&ctx->fl->map_mutex);
	trace_fastrpc_msg("fastrpc_context_free: free_maps");

	if (ctx->buf)
		fastrpc_buf_free(ctx->buf, true);

	if (ctx->fl->profile)
		kfree(ctx->perf);

	spin_lock_irqsave(&cctx->lock, flags);
	idr_remove(&cctx->ctx_idr, FASTRPC_GET_IDR_FROM_CTXID(ctx->ctxid));
	spin_unlock_irqrestore(&cctx->lock, flags);

	trace_fastrpc_context_free((uint64_t)ctx,
		ctx->ctxid, ctx->handle, ctx->sc);

	kfree(ctx->maps);
	kfree(ctx->olaps);
	kfree(ctx->args);
	kfree(ctx->outbufs);
	kfree(ctx);

	fastrpc_channel_ctx_put(cctx);
}

// static void fastrpc_context_get(struct fastrpc_invoke_ctx *ctx)
// {
	// kref_get(&ctx->refcount);
// }

static void fastrpc_context_put(struct fastrpc_invoke_ctx *ctx)
{
	kref_put(&ctx->refcount, fastrpc_context_free);
}

// static void fastrpc_context_put_wq(struct work_struct *work)
// {
	// struct fastrpc_invoke_ctx *ctx =
			// container_of(work, struct fastrpc_invoke_ctx, put_work);

	// fastrpc_context_put(ctx);
// }

#define CMP(aa, bb) ((aa) == (bb) ? 0 : (aa) < (bb) ? -1 : 1)

static u32 sorted_lists_intersection(u32 *listA,
		u32 lenA, u32 *listB, u32 lenB)
{
	u32 i = 0, j = 0;

	while (i < lenA && j < lenB) {
		if (listA[i] < listB[j])
			i++;
		else if (listA[i] > listB[j])
			j++;
		else
			return listA[i];
	}
	return 0;
}

static int uint_cmp_func(const void *p1, const void *p2)
{
	u32 a1 = *((u32 *)p1);
	u32 a2 = *((u32 *)p2);

	return CMP(a1, a2);
}

static int olaps_cmp(const void *a, const void *b)
{
	struct fastrpc_buf_overlap *pa = (struct fastrpc_buf_overlap *)a;
	struct fastrpc_buf_overlap *pb = (struct fastrpc_buf_overlap *)b;
	/* sort with lowest starting buffer first */
	int st = CMP(pa->start, pb->start);
	/* sort with highest ending buffer first */
	int ed = CMP(pb->end, pa->end);

	return st == 0 ? ed : st;
}

/**
 * fastrpc_get_buff_overlaps - Detect and handle buffer overlaps in RPC args
 * @ctx: The invoke context containing buffer information
 *
 * This function detects overlapping memory regions in the RPC arguments and
 * adjusts the memory mapping accordingly. It handles ION and non-ION buffers
 * separately to prevent incorrect overlap detection between different buf types.
 * For each buffer type:
 * - If a buffer overlaps with a previous buffer of the same type, it adjusts
 *   the mapping to avoid the overlap
 * - If no overlap is detected, it uses the full buffer range
 *
 * Return: 0 on success, error code on failure
 */
static int fastrpc_get_buff_overlaps(struct fastrpc_invoke_ctx *ctx)
{
	u64 ion_buf_end_pos = 0, non_ion_buf_end_pos = 0;
	int i;
	struct device *dev = ctx->fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;

	for (i = 0; i < ctx->nbufs; ++i) {
		ctx->olaps[i].start = ctx->args[i].ptr;
		/* Check the overflow for user buffer */
		if (ctx->olaps[i].start > (ULLONG_MAX - ctx->args[i].length)) {
			dev_dbg(dev,
				"user passed invalid non ion buffer addr 0x%llx, size %llx\n",
				ctx->args[i].ptr, ctx->args[i].length);
			return -EFAULT;
		}
		ctx->olaps[i].end = ctx->olaps[i].start + ctx->args[i].length;
		ctx->olaps[i].raix = i;
	}

	sort(ctx->olaps, ctx->nbufs, sizeof(*ctx->olaps), olaps_cmp, NULL);

	for (i = 0; i < ctx->nbufs; ++i) {
		/* Separate ION and non-ION buffers; fd <= 0 indicates non-ION */
		u64 *last_buf_end = (ctx->args[ctx->olaps[i].raix].fd <= 0) ?
				&non_ion_buf_end_pos : &ion_buf_end_pos;

		if (ctx->olaps[i].start < *last_buf_end) {
			/* Overlap detected within same buffer type */
			ctx->olaps[i].mstart = *last_buf_end;
			ctx->olaps[i].mend = ctx->olaps[i].end;
			ctx->olaps[i].offset = *last_buf_end - ctx->olaps[i].start;

			if (ctx->olaps[i].end > *last_buf_end) {
				*last_buf_end = ctx->olaps[i].end;
			} else {
				ctx->olaps[i].mend = 0;
				ctx->olaps[i].mstart = 0;
			}

		} else  {
			/* No overlap, assign full range */
			ctx->olaps[i].mend = ctx->olaps[i].end;
			ctx->olaps[i].mstart = ctx->olaps[i].start;
			ctx->olaps[i].offset = 0;
			*last_buf_end = ctx->olaps[i].end;
		}
	}
	return 0;
}

static struct fastrpc_invoke_ctx *fastrpc_context_alloc(
			struct fastrpc_user *user, u32 kernel, u32 sc,
			struct fastrpc_enhanced_invoke *invoke)
{
	struct fastrpc_channel_ctx *cctx = user->cctx;
	struct fastrpc_invoke_ctx *ctx = NULL;
	unsigned long flags;
	int ret;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&ctx->node);
	ctx->fl = user;
	ctx->nscalars = REMOTE_SCALARS_LENGTH(sc);
	ctx->nbufs = REMOTE_SCALARS_INBUFS(sc) +
		     REMOTE_SCALARS_OUTBUFS(sc);

	if (ctx->nscalars) {
		ctx->maps = kcalloc(ctx->nscalars,
				    sizeof(*ctx->maps), GFP_KERNEL);
		if (!ctx->maps) {
			ret = -ENOMEM;
			goto err_alloc;
		}
		ctx->olaps = kcalloc(ctx->nscalars,
				    sizeof(*ctx->olaps), GFP_KERNEL);
		if (!ctx->olaps) {
			ret = -ENOMEM;
			goto err_alloc;
		}
		ctx->args = kcalloc(ctx->nscalars,
				    sizeof(*ctx->args), GFP_KERNEL);
		if (!ctx->args) {
			ret = -ENOMEM;
			goto err_alloc;
		}
		if (!kernel) {
			if (copy_from_user((void *)ctx->args,
					(void __user *)(uintptr_t)invoke->inv.args,
					ctx->nscalars * sizeof(*ctx->args))) {
				ret = -EFAULT;
				goto err_alloc;
			}
		} else {
			memcpy((void *)ctx->args,
					(void *)(uintptr_t)invoke->inv.args,
					ctx->nscalars * sizeof(*ctx->args));
		}
		invoke->inv.args = (__u64)ctx->args;
		ret = fastrpc_get_buff_overlaps(ctx);
		if (ret)
			goto err_alloc;
	}

	/* Released in fastrpc_context_put() */
	fastrpc_channel_ctx_get(cctx);

	ctx->crc = (u32 *)(uintptr_t)invoke->crc;
	ctx->perf_dsp = (u64 *)(uintptr_t)invoke->perf_dsp;
	ctx->perf_kernel = (u64 *)(uintptr_t)invoke->perf_kernel;
	if (ctx->fl->profile) {
		ctx->perf = kzalloc(sizeof(*(ctx->perf)), GFP_KERNEL);
		if (!ctx->perf) {
			ret = -ENOMEM;
			goto err_perf_alloc;
		}
		ctx->perf->tid = ctx->fl->tgid_app;
	}
	ctx->handle = invoke->inv.handle;
	ctx->sc = sc;
	ctx->retval = -1;
	ctx->pid = current->pid;
	ctx->tgid = user->tgid_app;
	ctx->cctx = cctx;
	ctx->rsp_flags = NORMAL_RESPONSE;
	ctx->is_work_done = false;
	init_completion(&ctx->work);
	// INIT_WORK(&ctx->put_work, fastrpc_context_put_wq);

	spin_lock(&user->lock);
	list_add_tail(&ctx->node, &user->pending);
	spin_unlock(&user->lock);

	spin_lock_irqsave(&cctx->lock, flags);
	ret = idr_alloc_cyclic(&cctx->ctx_idr, ctx, 1,
			       FASTRPC_CTX_MAX, GFP_ATOMIC);
	if (ret < 0) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		goto err_idr;
	}
	cctx->jobid++;
	ctx->ctxid = FASTRPC_PACK_JOBID_IN_CTXID(ctx->ctxid, cctx->jobid);
	ctx->ctxid = FASTRPC_PACK_IDR_IN_CTXID(ctx->ctxid, ret);
	spin_unlock_irqrestore(&cctx->lock, flags);

	trace_fastrpc_context_alloc((uint64_t)ctx,
				ctx->ctxid, ctx->handle, ctx->sc);
	kref_init(&ctx->refcount);

	return ctx;
err_idr:
	spin_lock(&user->lock);
	list_del(&ctx->node);
	spin_unlock(&user->lock);
err_perf_alloc:
	fastrpc_channel_ctx_put(cctx);
err_alloc:
	kfree(ctx->maps);
	kfree(ctx->olaps);
	kfree(ctx->args);
	kfree(ctx);

	return ERR_PTR(ret);
}

static struct fastrpc_invoke_ctx *fastrpc_context_restore_interrupted(
			struct fastrpc_user *fl, struct fastrpc_invoke *inv)
{
	struct fastrpc_invoke_ctx *ctx = NULL, *ictx = NULL, *n;

	spin_lock(&fl->lock);
	list_for_each_entry_safe(ictx, n, &fl->interrupted, node) {
		if (ictx->pid == current->pid) {
			if (inv->sc != ictx->sc || ictx->fl != fl) {
				dev_err(ictx->fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev,
					"interrupted sc (0x%x) or fl (%pK) does not match with invoke sc (0x%x) or fl (%pK)\n",
					ictx->sc, ictx->fl, inv->sc, fl);
				spin_unlock(&fl->lock);
				return ERR_PTR(-EINVAL);
			} else {
				ctx = ictx;
				list_del(&ctx->node);
				list_add_tail(&ctx->node, &fl->pending);
			}
			break;
		}
	}
	spin_unlock(&fl->lock);
	return ctx;
}

static void fastrpc_context_save_interrupted(
			struct fastrpc_invoke_ctx *ctx)
{
	trace_fastrpc_context_interrupt(ctx->cctx->domain_id, (uint64_t)ctx,
					ctx->msg.ctx, ctx->msg.handle, ctx->msg.sc);
	spin_lock(&ctx->fl->lock);
	list_del(&ctx->node);
	list_add_tail(&ctx->node, &ctx->fl->interrupted);
	spin_unlock(&ctx->fl->lock);
}

static struct sg_table *
fastrpc_map_dma_buf(struct dma_buf_attachment *attachment,
		    enum dma_data_direction dir)
{
	struct fastrpc_dma_buf_attachment *a = attachment->priv;
	struct sg_table *table;
	int ret;

	table = &a->sgt;

	ret = dma_map_sgtable(attachment->dev, table, dir, 0);
	if (ret)
		table = ERR_PTR(ret);
	return table;
}

static void fastrpc_unmap_dma_buf(struct dma_buf_attachment *attach,
				  struct sg_table *table,
				  enum dma_data_direction dir)
{
	dma_unmap_sgtable(attach->dev, table, dir, 0);
}

static void fastrpc_release(struct dma_buf *dmabuf)
{
	struct fastrpc_buf *buffer = dmabuf->priv;

	fastrpc_buf_free(buffer, false);
}

static int fastrpc_dma_buf_attach(struct dma_buf *dmabuf,
				  struct dma_buf_attachment *attachment)
{
	struct fastrpc_dma_buf_attachment *a;
	struct fastrpc_buf *buffer = dmabuf->priv;
	int ret;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	ret = dma_get_sgtable(buffer->dev, &a->sgt, buffer->virt,
			IOVA_TO_PHYSADDR(buffer->phys, buffer->smmucb->sid_pos),
			buffer->size);
	if (ret < 0) {
		dev_err(buffer->dev, "failed to get scatterlist from DMA API\n");
		kfree(a);
		return -EINVAL;
	}

	a->dev = attachment->dev;
	INIT_LIST_HEAD(&a->node);
	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->node, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void fastrpc_dma_buf_detatch(struct dma_buf *dmabuf,
				    struct dma_buf_attachment *attachment)
{
	struct fastrpc_dma_buf_attachment *a = attachment->priv;
	struct fastrpc_buf *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->node);
	mutex_unlock(&buffer->lock);
	sg_free_table(&a->sgt);
	kfree(a);
}

static int fastrpc_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct fastrpc_buf *buf = dmabuf->priv;

	iosys_map_set_vaddr(map, buf->virt);

	return 0;
}

static int fastrpc_mmap(struct dma_buf *dmabuf,
			struct vm_area_struct *vma)
{
	struct fastrpc_buf *buf = dmabuf->priv;
	size_t size = vma->vm_end - vma->vm_start;

	return dma_mmap_coherent(buf->dev, vma, buf->virt,
				IOVA_TO_PHYSADDR(buf->phys,
					buf->smmucb->sid_pos), size);
}

static const struct dma_buf_ops fastrpc_dma_buf_ops = {
	.attach = fastrpc_dma_buf_attach,
	.detach = fastrpc_dma_buf_detatch,
	.map_dma_buf = fastrpc_map_dma_buf,
	.unmap_dma_buf = fastrpc_unmap_dma_buf,
	.mmap = fastrpc_mmap,
	.vmap = fastrpc_vmap,
	.release = fastrpc_release,
};

static struct fastrpc_pool_ctx *fastrpc_session_alloc(
				struct fastrpc_user *fl, bool secure, int pd_type)
{

	struct fastrpc_pool_ctx *session = NULL, *isess = NULL;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	unsigned long flags;
	bool sharedcb = fl->sharedcb;
	int i;

	if (!cctx->dev)
		return session;

	/*
	 * No dedicated context bank exists for root PD on trusted VMs
	 * Use the unsigned pool for root PD sessions.
	 */
	if (g_frpc.is_trusted_vm && pd_type == ROOT_PD)
		pd_type = USER_UNSIGNEDPD_POOL;

	/*
	 * If PD type is configured for context banks in device tree,
	 * use CPZ_USERPD, to allocate secure context bank type.
	 */
	if (secure && cctx->pd_type) {
		pd_type = CPZ_USERPD;
		sharedcb = true;
	} else if (secure)
		/* Legacy case, where pd_type is not configured in device tree */
		pd_type = DEFAULT_UNUSED;

	/*
	 * If session is being requested for the default non-secure pd-type of
	 * the process and it has been allocated already, then reuse that
	 * session.
	 */
	if (fl->sctx && !secure && fl->sctx->pd_type == pd_type)
		return fl->sctx;

	spin_lock_irqsave(&cctx->lock, flags);
	for (i = 0; i < cctx->sesscount; i++) {
		/*
		 * Session is chosen based on following conditions:
		 * 1. If session is SID pooled (smmucount > 1), then any number of applications
		 *    can use session, else only one application (usecount == 0) is allowed to
		 *    use session
		 * AND
		 * 2. SMMU CB should always be valid, should not have been unregistered.
		 * AND
		 * 3. If process is secure usecase (CPZ usecase), then session also
		 *    should have secure parameter set.
		 * AND
		 * 4. If process needs to share CB (for SID sharing):
		 *    - For sensors PD, session will have sharedcb parameter set
		 *    - For some extended map usecases, the sharedcb parameter cannot be set
		 *      in the code, but the CB can be shared
		 * AND
		 * 5. If pd_type is configured, then process pd_type needs to match with
		 *    session pd_type, else pd_type check is ignored
		 */
		isess = &cctx->session[i];

		if ((isess->usecount == 0 || isess->smmucount > 1) &&
			isess->smmucb[DEFAULT_SMMU_IDX].valid &&
			isess->secure == secure &&
			((isess->pd_type == EXT_MAP_PD_TYPE && isess->sharedcb) ||
			(isess->sharedcb == sharedcb)) &&
			(pd_type == DEFAULT_UNUSED || isess->pd_type == pd_type || secure)) {
			session = isess;
			/*
			 * Increment number of apps using session.
			 * Will be max 1 for sessions that don't have
			 * pooled context banks or a shared context bank.
			 */
			session->usecount++;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, flags);

	return session;
}

static void fastrpc_session_free(struct fastrpc_channel_ctx *cctx,
				 struct fastrpc_pool_ctx *session)
{
	unsigned long flags;

	spin_lock_irqsave(&cctx->lock, flags);
	if (session->usecount > 0)
		session->usecount--;
	spin_unlock_irqrestore(&cctx->lock, flags);
}

static void fastrpc_pm_awake(struct fastrpc_user *fl)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct wakeup_source *wake_source = cctx->wake_source;

	if (!fl->wake_enable)
		return;

	/*
	 * Vote with PM to abort any suspend in progress and
	 * keep system awake for specified timeout
	 */
	if (wake_source)
		pm_wakeup_ws_event(wake_source, fl->ws_timeout, true);
	else
		dev_warn_ratelimited(cctx->dev, "wake_source is not registered\n");
}

static void fastrpc_pm_relax(struct fastrpc_user *fl)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct wakeup_source *wake_source = cctx->wake_source;

	if (!fl->wake_enable)
		return;

	mutex_lock(&cctx->wake_mutex);
	if (wake_source)
		__pm_relax(wake_source);
	else
		dev_warn_ratelimited(cctx->dev, "wake_source is not registered\n");
	mutex_unlock(&cctx->wake_mutex);
}

static int get_buffer_attr(struct dma_buf *buf, bool *exclusive_access)
{
	const int *vmids_list = NULL;
	const int  *perms = NULL;
	int err = 0;
	int vmids_list_len = 0;
	*exclusive_access = false;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
	/*
	 * If buf lacks the Qualcomm extension of mem-buf,
	 * No support for accesing by vm related function.
	 * Make exclusive access as true to select non-secure smmu cb.
	 * And bypass following vm related functions.
	 */
	if (!is_mem_buf_dma_buf(buf))
	{
		*exclusive_access = true;
		return 0;
	}
#endif

	err = mem_buf_dma_buf_get_vmperm(buf, &vmids_list, &perms, &vmids_list_len);
	if (err)
		return err;
	/*
	 * If one VM has access to buffer and is the current VM,
	 * then VM has exclusive access to buffer
	 */
	if (vmids_list_len == 1 && vmids_list[0] == mem_buf_current_vmid())
		*exclusive_access = true;

	return err;
}

static int set_buffer_secure_type(struct fastrpc_map *map)
{
	int err = 0;
	bool exclusive_access = false;
	struct device *dev = map->fl->cctx->dev;

	err = get_buffer_attr(map->buf, &exclusive_access);
	if (err) {
		dev_err(dev, "failed to obtain buffer attributes for fd %d ret %d\n", map->fd, err);
		return -EBADFD;
	}
	/*
	 * Secure buffers would always be owned by multiple VMs.
	 * If current VM is the exclusive owner of a buffer, it is considered non-secure.
	 * In PVM:
	 *	- CPZ buffers are secure
	 *	- All other buffers are non-secure
	 * In TVM:
	 *	- Since it is a secure environment by default, there are no explicit "secure" buffers
	 *	- All buffers are marked "non-secure"
	 */
	if (g_frpc.is_trusted_vm)
		map->secure = 0;
	else
		map->secure = (exclusive_access) ? 0 : 1;

	return err;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0))
/**
 * fastrpc_map_reserve_iova() -
 * Function to reserve iova region first (or use previously
 * reserved region for same buffer) and map each SGL entry of dma
 * buffer to that iova region.
 * @arg1: SMMU context bank
 * @arg2: FastRPC map structure
 *
 * Return: Returns 0 on Success, error code on failure
 */

static int fastrpc_map_reserve_iova(struct fastrpc_smmu *smmucb,
		struct fastrpc_map *map)
{
	struct scatterlist *sgl = NULL;
	int err = 0, sgl_index = 0;
	bool iova_reserved = false;
	size_t offset = 0;

	/* Compute the total size of all SGL entries of buffer */
	for_each_sgtable_sg(map->table, sgl, sgl_index)
		map->size += sgl->length;

	if (map->size < map->len) {
		err = -EBADF;
		dev_err(smmucb->dev,
		"Error: %s: passed dma buffer size %llu mismatch with actual size %llu for fd %d\n",
		__func__, map->len, map->size, map->fd);
		goto iova_reserve_err;
	}

	/* Check if map has an iova region reserved already */
	if (!dma_use_iova(&map->iova_state)) {
		/*
		 * If no iova region reserved, then reserve a
		 * new region of buffer's size.
		 */
		iova_reserved = dma_iova_try_alloc(smmucb->dev,
						&(map->iova_state),
						0, map->size);
		if (!iova_reserved) {
			err = -ENOMEM;
			goto iova_reserve_err;
		}
	}

	sgl_index = 0;
	for_each_sgtable_sg(map->table, sgl, sgl_index) {
		/*
		 * Create smmu mapping of each SGL entry of the dma buffer at
		 * appropriate offsets within the reserved iova region.
		 */
		err = dma_iova_link(smmucb->dev, &map->iova_state,
				sg_phys(sgl), offset,
				sgl->length, DMA_BIDIRECTIONAL, 0);
		if (err) {
			dev_err(smmucb->dev,
			"Error %d: %s: dma_iova_link failed for fd %d, sgl phys 0x%llx, offset 0x%zx\n",
			err, __func__, map->fd, sg_phys(sgl), offset);
			goto dma_link_err;
		}
		offset += sgl->length;
	}

	/*
	 * Synchronize the IOMMU’s TLB to make recent IOMMU page‑table
	 * updates visible to the device
	 */
	err = dma_iova_sync(smmucb->dev,
			&map->iova_state, 0, map->size);
	if (err)
		goto dma_sync_err;

	map->phys = map->iova_state.addr;
	RECONSTRUCT_IOVA_FROM_SID_PA((u64)smmucb->sid,
		map->phys, smmucb->sid_pos);
	if (iova_reserved) {
		/*
		 * If previously reserved iova region for same buffer was
		 * reused for creating smmu mapping, then no need to update
		 * smmu device memory stats.
		 */
		smmucb->allocatedbytes += SMMU_ALIGN(map->size);
	}

	return 0;
dma_sync_err:
	dma_iova_unlink(smmucb->dev, &map->iova_state, 0, map->size,
				DMA_BIDIRECTIONAL, 0);
dma_link_err:
	if (iova_reserved)
		dma_iova_free(smmucb->dev, &map->iova_state);
iova_reserve_err:
	return err;
}
#endif

static int fastrpc_map_create(struct fastrpc_user *fl, int fd,
			      u64 va, struct dma_buf *buf, u64 len,
			      u32 attr, int mflags, struct fastrpc_map **ppmap,
				  bool take_ref)
{
	struct fastrpc_pool_ctx *sess = NULL;
	struct fastrpc_pool_ctx **pool_ctx = NULL;
	struct fastrpc_map *map = NULL;
	struct scatterlist *sgl = NULL;
	int err = 0, sgl_index = 0, ret = 0;
	struct device *dev = NULL;
	struct fastrpc_smmu *smmucb = NULL;
	u32 smmuidx = DEFAULT_SMMU_IDX, pd_type = 0;
	bool secure = false, retained_map = false,
		retain_iova_attr = (attr == FASTRPC_MAP_ATTR_RETAIN_IOVA);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6,13,0))
	if (retain_iova_attr)
		return -EOPNOTSUPP;
#endif
	ret = fastrpc_map_lookup(fl, fd, va, len, buf, mflags,
			ppmap, take_ref, attr);

	if (ret == -EBADFD) {
		/*
		 * For EBADFD, return error to user,
		 * For ENOENT, proceed to create new map.
		 */
		return -EBADFD;
	} else if (ret == 0){
		if (retain_iova_attr) {
			/* Map with 'retain-iova' flag. Skip new map allocation */
			map = *ppmap;
			retained_map = true;
			goto skip_map_alloc;
		}
		/* Return map found */
		return 0;
	}

	map = kzalloc(sizeof(*map), GFP_KERNEL);
	if (!map)
		return -ENOMEM;

	INIT_LIST_HEAD(&map->node);

skip_map_alloc:
	kref_init(&map->refcount);

	map->fl = fl;
	map->fd = fd;
	map->flags = mflags;
	map->len = len;
	map->size = 0;
	map->attr = attr;

	if(mflags == ADSP_MMAP_DMA_BUFFER) {
		if (!buf) {
			err = -EFAULT;
			goto get_err;
		}
		map->buf = buf;
		get_dma_buf(map->buf);

	} else {
		map->buf = dma_buf_get(fd);
		if (IS_ERR(map->buf)) {
			err = PTR_ERR(map->buf);
			goto get_err;
		}
	}

	err = set_buffer_secure_type(map);
	if (err)
		goto attach_err;

	sess = fl->sctx;
	pd_type = fl->pd_type;
	pool_ctx = &fl->sctx;
	if (map->secure && (!(attr & FASTRPC_ATTR_NOMAP
				|| mflags == FASTRPC_MAP_FD_NOMAP))) {
		sess = fl->secsctx;
		pool_ctx = &fl->secsctx;
		secure = true;
	} else if (IS_EXTENDED_MAP_FLAG(mflags)) {
		/*
		 * Min page-size for extended mappings on DSP is 1MB and it increases in
 		 * multiples of 1MB. Min page-size for SMMU mappings is 2MB. Due to these
		 * HW constraints, following size constraints are imposed for extended
		 * mapping requests.
		 */
		if ((len < SMMU_2M) || (len % SMMU_1M != 0)) {
			err = -EOPNOTSUPP;
			dev_err(fl->cctx->dev,
				"Error 0x%x: %s: Invalid size 0x%llx. Size should be a multiple of 0x%llx and greater than 0x%llx for map flag %d",
				err, __func__, len, SMMU_1M, SMMU_2M, mflags);
			goto attach_err;
		}
		sess = fl->extctx;
		pool_ctx = &fl->extctx;
		pd_type = EXT_MAP_PD_TYPE;
	}
	if (!sess) {
		sess = fastrpc_session_alloc(fl, secure, pd_type);
		if (!sess) {
			dev_err(fl->cctx->dev,
				"%s: no session available, pd type %d, secure %d\n",
				__func__, pd_type, secure);
			err = -EBUSY;
			goto attach_err;
		}
		*pool_ctx = sess;
	}

map_retry:
	smmuidx = fastrpc_smmu_device_lookup(sess, len, smmuidx);
	if (smmuidx >= sess->smmucount) {
		dev_err(fl->cctx->dev,
			"%s: No valid smmu context bank found for len 0x%llx\n",
			__func__, len);
		err = -ENOSR;
		goto attach_err;
	} else {
		smmucb = &sess->smmucb[smmuidx];
	}

	if (attr & FASTRPC_ATTR_NOMAP || mflags == FASTRPC_MAP_FD_NOMAP) {
		dev = fl->cctx->dev;
	} else {
		/*
		 * For 'retain iova' attr, use parent SMMU dev,
		 * to get sgl without mapping on IOMMU.
		 */
		dev = retain_iova_attr ? smmucb->dev->parent : smmucb->dev;
		map->smmucb = smmucb;
	}

	mutex_lock(&smmucb->map_mutex);
	if (!smmucb->dev) {
		err = -ENODEV;
		mutex_unlock(&smmucb->map_mutex);
		goto attach_err;
	}

	map->attach = dma_buf_attach(map->buf, dev);
	if (IS_ERR(map->attach)) {
		dev_err(dev, "Failed to attach dmabuf\n");
		err = PTR_ERR(map->attach);
		mutex_unlock(&smmucb->map_mutex);
		goto attach_err;
	}

	err = dma_buf_map_attachment_wrap(map);
	/*
	 * Retry allocation on next availale IOMMU CB,
	 * if there is no enough virtual space available on current IOMMU CB.
	 * Detach from current IOMMU CB.
	 */
	if (err == -ENOMEM || err == -EINVAL) {
		mutex_unlock(&smmucb->map_mutex);
		dma_buf_detach(map->buf, map->attach);
		smmuidx++;
		goto map_retry;
	} else if (err) {
		mutex_unlock(&smmucb->map_mutex);
		goto map_err;
	}

	if (retain_iova_attr) {
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6,16,0))
		err = fastrpc_map_reserve_iova(smmucb, map);
#else
		err = -EOPNOTSUPP;
#endif
		if (err) {
			mutex_unlock(&smmucb->map_mutex);
			goto assign_err;
		}
	} else if (attr & FASTRPC_ATTR_SECUREMAP) {
		map->phys = sg_phys(map->table->sgl);
		for_each_sg(map->table->sgl, sgl, map->table->nents,
			sgl_index)
			map->size += sg_dma_len(sgl);
		smmucb->allocatedbytes += SMMU_ALIGN(map->size);
	} else if (attr & FASTRPC_ATTR_NOMAP || mflags == FASTRPC_MAP_FD_NOMAP){
		map->phys = sg_dma_address(map->table->sgl);
		map->size = sg_dma_len(map->table->sgl);
	} else {
		map->phys = sg_dma_address(map->table->sgl);
		RECONSTRUCT_IOVA_FROM_SID_PA((u64)smmucb->sid,
			map->phys, smmucb->sid_pos);
		for_each_sg(map->table->sgl, sgl, map->table->nents,
			sgl_index)
			map->size += sg_dma_len(sgl);
		smmucb->allocatedbytes += SMMU_ALIGN(map->size);
	}
	map->va = (void *) (uintptr_t) va;

	trace_fastrpc_dma_map(map->fl->cctx->domain_id, map->fd, map->phys,
		map->size, map->len, map->attach->dma_map_attrs, map->flags);
	mutex_unlock(&smmucb->map_mutex);

	/*
	 * Any mapping request with size > 2MB would be aligned to 2MB by SMMU.
	 * Since that is the minimum size allowed for extended mapping requests,
	 * validate that the returned iova is aligned as expected.
	 */
	if (IS_EXTENDED_MAP_FLAG(mflags) && !IS_ALIGNED(map->phys, SMMU_2M)) {
		err = -EOPNOTSUPP;
		dev_err(dev, "Error %d: %s: iova 0x%llx not aligned to 0x%llx for map flag %d",
			err, __func__, map->phys, SMMU_2M, mflags);
		goto assign_err;
	}

	if (attr & FASTRPC_ATTR_SECUREMAP) {
		/*
		 * If subsystem VMIDs are defined in DTSI, then do
		 * hyp_assign from HLOS to those VM(s)
		 */
		u64 src_perms = BIT(QCOM_SCM_VMID_HLOS);
		struct qcom_scm_vmperm dst_perms[2] = {0};

		dst_perms[0].vmid = QCOM_SCM_VMID_HLOS;
		dst_perms[0].perm = QCOM_SCM_PERM_RW;
		dst_perms[1].vmid = fl->cctx->vmperms[0].vmid;
		dst_perms[1].perm = QCOM_SCM_PERM_RWX;
		err = qcom_scm_assign_mem(map->phys, (u64)map->size, &src_perms, dst_perms, 2);
		if (err) {
			dev_err(smmucb->dev,
			"Failed to assign memory with phys 0x%llx size 0x%llx err %d",
			map->phys, map->size, err);
			goto assign_err;
		}
	}
	spin_lock(&fl->lock);
	if (!retained_map) {
		/*
		 * If an fd previously mapped with 'retain-iova' attribute is
		 * being mapped again, it should already be part of the list.
		 */
		list_add_tail(&map->node, &fl->maps);
	}
	spin_unlock(&fl->lock);
	*ppmap = map;

	return 0;
assign_err:
	dma_buf_unmap_attachment_wrap(map);
map_err:
	dma_buf_detach(map->buf, map->attach);
attach_err:
	dma_buf_put(map->buf);
get_err:
	if (!retained_map) {
		/*
		 * If an fd that was previously mapped with the 'retain-iova'
		 * attribute is being mapped again, it will already be part
		 * of the list, so it cannot be freed.
		 */
		kfree(map);
	}

	return err;
}

/*
 * Fastrpc payload buffer with metadata looks like:
 *
 * >>>>>>  START of METADATA <<<<<<<<<
 * +---------------------------------+
 * |           Arguments             |
 * | type:(union fastrpc_remote_arg)|
 * |             (0 - N)             |
 * +---------------------------------+
 * |         Invoke Buffer list      |
 * | type:(struct fastrpc_invoke_buf)|
 * |           (0 - N)               |
 * +---------------------------------+
 * |         Page info list          |
 * | type:(struct fastrpc_phy_page)  |
 * |             (0 - N)             |
 * +---------------------------------+
 * |         Optional info           |
 * |(can be specific to SoC/Firmware)|
 * +---------------------------------+
 * >>>>>>>>  END of METADATA <<<<<<<<<
 * +---------------------------------+
 * |         Inline ARGS             |
 * |            (0-N)                |
 * +---------------------------------+
 */

static int fastrpc_get_meta_size(struct fastrpc_invoke_ctx *ctx)
{
	int size = 0;

	size = (sizeof(struct fastrpc_remote_buf) +
		sizeof(struct fastrpc_invoke_buf) +
		sizeof(struct fastrpc_phy_page)) * ctx->nscalars +
		sizeof(u64) * FASTRPC_MAX_FDLIST +
		sizeof(u32) * FASTRPC_MAX_CRCLIST +
		sizeof(u32) + sizeof(u64) * FASTRPC_DSP_PERF_LIST;

	return size;
}

static u64 fastrpc_get_payload_size(struct fastrpc_invoke_ctx *ctx, int metalen)
{
	u64 size = 0, len;
	int oix;

	size = ALIGN(metalen, FASTRPC_ALIGN);
	for (oix = 0; oix < ctx->nbufs; oix++) {
		int i = ctx->olaps[oix].raix;

		if (ctx->args[i].fd == 0 || ctx->args[i].fd == -1) {

			if (ctx->olaps[oix].offset == 0) {
				/* Check for overflow before align. */
				if (size > (ULLONG_MAX - (FASTRPC_ALIGN - 1)))
					return 0;
				size = ALIGN(size, FASTRPC_ALIGN);
			}

			len = (ctx->olaps[oix].mend - ctx->olaps[oix].mstart);
			/* Check the overflow for payload */
			if (size > (ULLONG_MAX - len))
				return 0;
			size += len;
		}
	}

	return size;
}

static int fastrpc_create_maps(struct fastrpc_invoke_ctx *ctx)
{
	struct device *dev = ctx->fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	struct fastrpc_channel_ctx *cctx = ctx->fl->cctx;
	int i, err;

	for (i = 0; i < ctx->nscalars; ++i) {
		bool take_ref = true;
		int mflags = 0;

		if (ctx->args[i].fd == 0 || ctx->args[i].fd == -1 ||
		   (i >= ctx->nbufs && cctx->dsp_attributes[DMA_HANDLE_REVERSE_RPC_CAP]) ||
                    ctx->args[i].length == 0)
			continue;

		if (i >= ctx->nbufs) {
			take_ref = false;
			/* Set the DMA handle mapping flag for DMA handles */
			mflags = FASTRPC_MAP_LEGACY_DMA_HANDLE;
		}
		mutex_lock(&ctx->fl->map_mutex);
		err = fastrpc_map_create(ctx->fl, ctx->args[i].fd, (u64)ctx->args[i].ptr, NULL,
			 ctx->args[i].length, ctx->args[i].attr, mflags, &ctx->maps[i], take_ref);
		mutex_unlock(&ctx->fl->map_mutex);
		if (err) {
			dev_err(dev, "Error Creating map %d\n", err);
			return -EINVAL;
		}

	}
	return 0;
}

static struct fastrpc_invoke_buf *fastrpc_invoke_buf_start(union fastrpc_remote_arg *pra, int len)
{
	return (struct fastrpc_invoke_buf *)(&pra[len]);
}

static struct fastrpc_phy_page *fastrpc_phy_page_start(struct fastrpc_invoke_buf *buf, int len)
{
	return (struct fastrpc_phy_page *)(&buf[len]);
}

static int fastrpc_get_args(u32 kernel, struct fastrpc_invoke_ctx *ctx)
{
	struct device *dev = ctx->fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	union fastrpc_remote_arg *rpra;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	int inbufs, outbufs, i, oix, err = 0;
	u64 len, rlen, pkt_size, outbufslen;
	u64 pg_start, pg_end;
	u64 *perf_counter = NULL;
	uintptr_t args;
	int metalen;

	if (ctx->fl->profile)
		perf_counter = (u64 *)ctx->perf + PERF_COUNT;

	inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(ctx->sc);
	metalen = fastrpc_get_meta_size(ctx);
	pkt_size = fastrpc_get_payload_size(ctx, metalen);
	outbufslen = sizeof(struct fastrpc_remote_buf) * outbufs;
	ctx->outbufs = kzalloc(outbufslen, GFP_KERNEL);
	if (!ctx->outbufs) {
		err = -ENOMEM;
		goto bail;
	}
	if (!pkt_size) {
		dev_err(dev, "invalid payload size for handle 0x%x, sc 0x%x\n",
			ctx->handle, ctx->sc);
		return -EFAULT;
	}

	PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_MAP),
	err = fastrpc_create_maps(ctx);
	if (err)
		return err;
	PERF_END);

	ctx->msg_sz = metalen;

	err = fastrpc_smmu_buf_alloc(ctx->fl, pkt_size, METADATA_BUF, &ctx->buf);
	if (err)
		return err;

	memset(ctx->buf->virt, 0, pkt_size);
	rpra = ctx->buf->virt;
	list = fastrpc_invoke_buf_start(rpra, ctx->nscalars);
	pages = fastrpc_phy_page_start(list, ctx->nscalars);
	args = (uintptr_t)ctx->buf->virt + metalen;
	rlen = pkt_size - metalen;
	ctx->rpra = rpra;

	for (oix = 0; oix < ctx->nbufs; ++oix) {
		u64 mlen;
		u64 offset = 0;

		i = ctx->olaps[oix].raix;
		len = ctx->args[i].length;

		rpra[i].buf.pv = 0;
		rpra[i].buf.len = len;
		list[i].num = len ? 1 : 0;
		list[i].pgidx = i;

		if (!len)
			continue;

		if (ctx->maps[i]) {
			struct vm_area_struct *vma = NULL;
			u64 addr = (u64)ctx->args[i].ptr & PAGE4K_MASK, vm_start = 0,
			vm_end = 0;

			PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_MAP),

			rpra[i].buf.pv = (u64) ctx->args[i].ptr;
			pages[i].addr = ctx->maps[i]->phys;

			if (len > ctx->maps[i]->size) {
				err = -EFAULT;
				dev_err(dev,
					"Invalid buffer addr 0x%llx len 0x%llx IPA 0x%llx size 0x%llx fd %d\n",
					ctx->args[i].ptr, len, ctx->maps[i]->phys,
					ctx->maps[i]->size, ctx->maps[i]->fd);
				goto bail;
			}
			if (!(ctx->maps[i]->attr & FASTRPC_ATTR_NOVA)) {
				mmap_read_lock(current->mm);
				vma = find_vma(current->mm, ctx->args[i].ptr);
				if (vma) {
					vm_start = vma->vm_start;
					vm_end = vma->vm_end;
				}
				mmap_read_unlock(current->mm);
				if (addr < vm_start || addr + len > vm_end ||
					(addr - vm_start) + len > ctx->maps[i]->size) {
					err = -EFAULT;
					dev_err(dev,
						"Invalid buffer addr 0x%llx len 0x%llx vm start 0x%llx vm end 0x%llx IPA 0x%llx size 0x%llx\n",
						ctx->args[i].ptr, len, vm_start, vm_end,
						ctx->maps[i]->phys, ctx->maps[i]->size);
					goto bail;
				}
				else
					offset = addr - vm_start;
				pages[i].addr += offset;
			}

			pg_start = addr >> PAGE4K_SHIFT;
			pg_end = ((ctx->args[i].ptr + len - 1) & PAGE4K_MASK) >>
				PAGE4K_SHIFT;
			pages[i].size = (pg_end - pg_start + 1) * PAGE4K_SIZE;
			PERF_END);
			/*
			 * Check for page range overflow and validate page
			 * range is not greater than map buffer range.
			 * This prevents potential buffer overflow
			 * and memory corruption that could be exploited.
			 */
			if (pages[i].addr > (ULLONG_MAX - pages[i].size) ||
			   (pages[i].addr + pages[i].size) >
					(ctx->maps[i]->phys + ctx->maps[i]->size)) {
				err = -EFAULT;
				dev_err(dev,
					"Invalid buffer addr 0x%llx len 0x%llx IPA 0x%llx size 0x%llx fd %d\n",
					ctx->args[i].ptr, len, ctx->maps[i]->phys,
					ctx->maps[i]->size, ctx->maps[i]->fd);
				goto bail;
			}
		} else {
			PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_COPY),
			if (ctx->olaps[oix].offset == 0) {
				rlen -= ALIGN(args, FASTRPC_ALIGN) - args;
				args = ALIGN(args, FASTRPC_ALIGN);
			}

			mlen = ctx->olaps[oix].mend - ctx->olaps[oix].mstart;

			if (mlen > COPY_BUF_WARN_LIMIT)
				dev_dbg(dev, "user passed non ion buffer size 0x%llx, mend 0x%llx mstart 0x%llx, sc 0x%x\n",
					mlen, ctx->olaps[oix].mend, ctx->olaps[oix].mstart, ctx->sc);

			if (rlen < mlen) {
				err = -EFAULT;
				goto bail;
			}

			if (i >= inbufs) {
				int j = i - inbufs;
				ctx->outbufs[j].buf.pv = args - ctx->olaps[oix].offset;
				ctx->outbufs[j].buf.len = len;
			}
			rpra[i].buf.pv = args - ctx->olaps[oix].offset;
			pages[i].addr = ctx->buf->phys -
					ctx->olaps[oix].offset +
					(pkt_size - rlen);
			pages[i].addr = pages[i].addr &	PAGE4K_MASK;

			pg_start = (rpra[i].buf.pv & PAGE4K_MASK) >> PAGE4K_SHIFT;
			pg_end = ((rpra[i].buf.pv + len - 1) & PAGE4K_MASK) >> PAGE4K_SHIFT;
			pages[i].size = (pg_end - pg_start + 1) * PAGE4K_SIZE;
			args = args + mlen;
			rlen -= mlen;
			PERF_END);
		}

		if (i < inbufs && !ctx->maps[i]) {
			void *dst = (void *)(uintptr_t)rpra[i].buf.pv;
			void *src = (void *)(uintptr_t)ctx->args[i].ptr;
			PERF(ctx->fl->profile, GET_COUNTER(perf_counter, PERF_COPY),

			if (!kernel) {
				if (copy_from_user(dst, (void __user *)src, len)) {
					dev_err(dev, "invalid buffer length 0x%llx\n", len);
					err = -EFAULT;
					goto bail;
				}
			} else {
				memcpy(dst, src, len);
			}
			PERF_END);
		}
	}
	trace_fastrpc_get_args((uint64_t)ctx,
		ctx->ctxid, ctx->handle, ctx->sc);

	for (i = ctx->nbufs; i < ctx->nscalars; ++i) {
		list[i].num = ctx->args[i].length ? 1 : 0;
		list[i].pgidx = i;
		if (ctx->maps[i]) {
			/* It is possible that map is created using mflag
			 * is FASTRPC_MAP_LEGACY_DMA_HANDLE and take_ref
			 * is false. Check if map is still exist or is
			 * being freed as take_ref is false
			 */
			mutex_lock(&ctx->fl->map_mutex);
			if (!fastrpc_map_lookup(ctx->fl, ctx->args[i].fd,
				 0, 0, NULL, 0 , &ctx->maps[i],
				 false, FASTRPC_MAP_ATTR_DEFAULT)) {
				pages[i].addr = ctx->maps[i]->phys;
				pages[i].size = ctx->maps[i]->size;
			}
			mutex_unlock(&ctx->fl->map_mutex);
		}
		rpra[i].dma.fd = ctx->args[i].fd;
		rpra[i].dma.len = ctx->args[i].length;
		rpra[i].dma.offset = (u64) ctx->args[i].ptr;
	}

bail:
	if (err)
		dev_err(dev, "Error: get invoke args failed:%d\n", err);

	return err;
}

static int fastrpc_put_args(struct fastrpc_invoke_ctx *ctx,
			    u32 kernel)
{
	union fastrpc_remote_arg *rpra = ctx->rpra;
	struct fastrpc_user *fl = ctx->fl;
	struct fastrpc_map *mmap = NULL;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	u64 *fdlist, *perf_dsp_list;
	u32 *crclist, *poll;
	int i, inbufs, outbufs, handles, perferr;

	inbufs = REMOTE_SCALARS_INBUFS(ctx->sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(ctx->sc);
	handles = REMOTE_SCALARS_INHANDLES(ctx->sc) + REMOTE_SCALARS_OUTHANDLES(ctx->sc);
	list = fastrpc_invoke_buf_start(rpra, ctx->nscalars);
	pages = fastrpc_phy_page_start(list, ctx->nscalars);
	fdlist = (u64 *)(pages + inbufs + outbufs + handles);
	crclist = (u32 *)(fdlist + FASTRPC_MAX_FDLIST);
	poll = (u32 *)(crclist + FASTRPC_MAX_CRCLIST);
	perf_dsp_list = (u64 *)(poll + 1);

	for (i = inbufs; i < ctx->nbufs; ++i) {
		if (!ctx->maps[i]) {
			int j = i - inbufs;
			void *src = (void *)(uintptr_t)ctx->outbufs[j].buf.pv;
			void *dst = (void *)(uintptr_t)ctx->args[i].ptr;
			u64 len = ctx->outbufs[j].buf.len;

			if (!kernel) {
				if (copy_to_user((void __user *)dst, src, len))
					return -EFAULT;
			} else {
				memcpy(dst, src, len);
			}
		}
	}

	for (i = 0; i < FASTRPC_MAX_FDLIST; i++) {
		if (!fdlist[i])
			break;
		mutex_lock(&fl->map_mutex);
		if (!fastrpc_map_lookup(fl, (int)fdlist[i],
			0, 0, NULL, 0, &mmap, false, FASTRPC_MAP_ATTR_DEFAULT))
			/* Validate the map flags for DMA handles and skip freeing map if invalid */
			if (mmap->flags == FASTRPC_MAP_LEGACY_DMA_HANDLE) {
				/* Allow DMA handle maps to free only once */
				mmap->flags = 0;
				fastrpc_map_put(mmap);
			}
		mutex_unlock(&fl->map_mutex);
	}
	if (ctx->crc && crclist && rpra) {
		if (copy_to_user((void __user *)ctx->crc, crclist, FASTRPC_MAX_CRCLIST * sizeof(u32)))
			return -EFAULT;
	}
	if (ctx->perf_dsp && perf_dsp_list) {
		if (0 != (perferr = copy_to_user((void __user *)ctx->perf_dsp, perf_dsp_list, FASTRPC_DSP_PERF_LIST * sizeof(u64)))) {
			pr_err("failed to copy perf data %d\n", perferr);
		}
	}
	return 0;
}

static s64 get_timestamp_in_ns(void)
{
	s64 ns = 0;
	struct timespec64 ts;

	ktime_get_boottime_ts64(&ts);
	ns = timespec64_to_ns(&ts);
	return ns;
}

static void fastrpc_update_txmsg_buf(struct fastrpc_channel_ctx *chan,
				struct fastrpc_msg *msg, int rpmsg_send_err, s64 ns)
{
	unsigned long flags = 0;
	u32 tx_index = 0;
	struct fastrpc_tx_msg *tx_msg = NULL;

	spin_lock_irqsave(&(chan->gmsg_log.tx_lock), flags);

	tx_index = chan->gmsg_log.tx_index;
	tx_msg = &(chan->gmsg_log.tx_msgs[tx_index]);

	memcpy(&tx_msg->msg, msg, sizeof(struct fastrpc_msg));
	tx_msg->rpmsg_send_err = rpmsg_send_err;
	tx_msg->ns = ns;

	tx_index++;
	chan->gmsg_log.tx_index =
		(tx_index > (GLINK_MSG_HISTORY_LEN - 1)) ? 0 : tx_index;

	spin_unlock_irqrestore(&(chan->gmsg_log.tx_lock), flags);
}

static void fastrpc_update_rxmsg_buf(struct fastrpc_channel_ctx *chan,
							u64 ctx, int retval, u32 rsp_flags,
							u32 early_wake_time, u32 ver, s64 ns)
{
	unsigned long flags = 0;
	u32 rx_index = 0;
	struct fastrpc_rx_msg *rx_msg = NULL;
	struct fastrpc_invoke_rspv2 *rsp = NULL;

	spin_lock_irqsave(&(chan->gmsg_log.rx_lock), flags);

	rx_index = chan->gmsg_log.rx_index;
	rx_msg = &(chan->gmsg_log.rx_msgs[rx_index]);
	rsp = &rx_msg->rsp;

	rsp->ctx = ctx;
	rsp->retval = retval;
	rsp->flags = rsp_flags;
	rsp->early_wake_time = early_wake_time;
	rsp->version = ver;
	rx_msg->ns = ns;

	rx_index++;
	chan->gmsg_log.rx_index =
		(rx_index > (GLINK_MSG_HISTORY_LEN - 1)) ? 0 : rx_index;

	spin_unlock_irqrestore(&(chan->gmsg_log.rx_lock), flags);
}

/*
 * fastrpc_getpd_msgidx()
 * Function returns msg index that is embedded in rpc msg ctx sent to dsp
 */
static inline int fastrpc_getpd_msgidx(u32 pd_type) {
	if (pd_type == ROOT_PD)
		return 0;
	else if (pd_type == SENSORS_STATICPD)
		return 2;
	else
		return 1;
}

static int fastrpc_invoke_send(struct fastrpc_pool_ctx *sctx,
				u32 priority, struct fastrpc_invoke_ctx *ctx,
				u32 kernel, uint32_t handle)
{
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_user *fl = ctx->fl;
	struct fastrpc_msg *msg = &ctx->msg;
	struct fastrpc_ipcmsg ipcmsg = {0};
	int ret;

	cctx = fl->cctx;
	msg->pid = fl->tgid_frpc;
	msg->tid = current->pid;
	if (kernel == KERNEL_MSG_WITH_ZERO_PID)
		msg->pid = 0;
	if (kernel == KERNEL_MSG_WITH_ZERO_PID_ZERO_TID)
		msg->tid = 0;

	/* Last 2 ctx ID bits, to route glink msg to appropriate PD type on DSP */
	msg->ctx = FASTRPC_PACK_PD_IN_CTXID(ctx->ctxid,
				fastrpc_getpd_msgidx(fl->pd_type));
	msg->handle = handle;
	msg->sc = ctx->sc;
	msg->addr = ctx->buf ? ctx->buf->phys : 0;
	msg->size = roundup(ctx->msg_sz, PAGE4K_SIZE);
	// fastrpc_context_get(ctx);

	/*
	 * If DSP supports new fastrpc_ipcmsg format, then send message
	 * with IPCMSG_TX_PRIORITY_REQ type and struct transport_req
	 * payload. Priority is passed as part of the payload.
	 */
	if (fl->cctx->dsp_attributes[FASTRPC_IPCMSG_SUPPORT]) {
		ipcmsg.type = IPCMSG_TX_PRIORITY_REQ;
		ipcmsg.payload.req.msg = *msg;
		ipcmsg.payload.req.priority = priority;
		ipcmsg.size = sizeof(ipcmsg.payload.req);
		ret = fastrpc_transport_send(cctx, (void *)&ipcmsg,
			sizeof(ipcmsg));
		trace_fastrpc_transport_send_ipcmsg(ipcmsg.type, cctx->domain_id,
			(uint64_t)ctx, msg->ctx, msg->handle, msg->sc, msg->addr,
			msg->size, ipcmsg.payload.req.priority);
	} else {
		/*
		 * If DSP does not support new fastrpc_ipcmsg struct,
		 * then send message using older fastrpc_msg struct,
		 * handle priority is encoded into bits [31-26] of tid
		 */
		if (!VALIDATE_PRIORITY_BITS_IN_TID(msg->tid)) {
			dev_err(fl->cctx->dev, "Error: %s: priority bits in tid %d are non-zero (prio %u)",
				__func__, msg->tid, priority);
			return -EFAULT;
		}
		msg->tid = GENERATE_FRPC_TID_WITH_PRIORITY(msg->tid, priority);

		ret = fastrpc_transport_send(cctx, (void *)msg, sizeof(*msg));
		trace_fastrpc_transport_send(cctx->domain_id, (uint64_t)ctx,
			msg->ctx, msg->handle, msg->sc, msg->addr, msg->size);
	}

	fastrpc_update_txmsg_buf(cctx, msg, ret, get_timestamp_in_ns());
	return ret;
}

static int poll_for_remote_response(struct fastrpc_invoke_ctx *ctx, u32 timeout)
{
	int err = -EIO, ii = 0, jj = 0;
	u32 sc = ctx->sc;
	struct fastrpc_invoke_buf *list;
	struct fastrpc_phy_page *pages;
	u64 *fdlist = NULL;
	u32 *crclist = NULL, *poll = NULL;
	unsigned int inbufs, outbufs, handles;

	/* calculate poll memory location */
	inbufs = REMOTE_SCALARS_INBUFS(sc);
	outbufs = REMOTE_SCALARS_OUTBUFS(sc);
	handles = REMOTE_SCALARS_INHANDLES(sc) + REMOTE_SCALARS_OUTHANDLES(sc);
	list = fastrpc_invoke_buf_start(ctx->rpra, ctx->nscalars);
	pages = fastrpc_phy_page_start(list, ctx->nscalars);
	fdlist = (u64 *)(pages + inbufs + outbufs + handles);
	crclist = (u32 *)(fdlist + FASTRPC_MAX_FDLIST);
	poll = (u32 *)(crclist + FASTRPC_MAX_CRCLIST);

	/* poll on memory for DSP response. Return failure on timeout */
	for (ii = 0, jj = 0; ii < timeout; ii++, jj++) {
		if (*poll == FASTRPC_EARLY_WAKEUP_POLL) {
			/* Remote processor sent early response */
			err = 0;
			break;
		} else if (*poll == FASTRPC_POLL_RESPONSE) {
			err = 0;
			ctx->is_work_done = true;
			ctx->retval = 0;
			fastrpc_update_rxmsg_buf(ctx->fl->cctx, ctx->msg.ctx, 0,
			POLL_MODE, 0, FASTRPC_RSP_VERSION2, get_timestamp_in_ns());
			break;
		}
		if (jj == FASTRPC_POLL_TIME_MEM_UPDATE) {
			/* Wait for DSP to finish updating poll memory */
			rmb();
			jj = 0;
		}
		udelay(1);
	}
	return err;
}


static int fastrpc_wait_for_response(struct fastrpc_invoke_ctx *ctx,
						u32 kernel)
{
	int interrupted = 0, err = 0;
	long timeleft = 0;
	bool is_timer_set = false;
	struct fastrpc_user *fl = ctx->fl;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	const unsigned int FASTRPC_NONINTERRUPT_CALL_TIMEOUT = 5000;

	if (kernel) {
		/*
		 * For all non-interruptible kernel rpc calls (like process
		 * spawn or kill, map / unmap), in case of a timeout,
		 * trigger an SSR on the dsp from a kernel-worker thread
		 * Certain trusted applications can disable this recovery
		 * mechanism by configuring an environment variable.
		 */
		if (((cctx->domain->type == FASTRPC_NSP &&
			(fl->pd_type == USERPD ||
			fl->pd_type == USER_UNSIGNEDPD_POOL)) ||
			(cctx->domain->type == FASTRPC_LPASS &&
			(fl->pd_type == SENSORS_STATICPD ||
			fl->pd_type == AUDIO_STATICPD ||
			fl->pd_type == OIS_STATICPD))) &&
			fl->dsp_recovery && !g_frpc.is_trusted_vm &&
			!atomic_read(&cctx->teardown)) {
			/*
			 * Start timer that will trigger ssr when kernel rpc
			 * call times out
			 */
			timer_setup(&ctx->ssr_timer, ssr_timer_callback, 0);
			mod_timer(&ctx->ssr_timer, jiffies +
			msecs_to_jiffies(FASTRPC_NONINTERRUPT_CALL_TIMEOUT));
			is_timer_set = true;
			dev_dbg(cctx->dev,
				"%s: started timer for domain %d, handle 0x%x, sc 0x%x, pid %d, tid %d\n",
				__func__, cctx->domain_id, ctx->handle,
				ctx->sc, ctx->tgid, ctx->pid);
		}

		wait_for_completion(&ctx->work);

		if (is_timer_set) {
			// Delete timer after ssr callback is completed
			#if (KERNEL_VERSION(6, 15, 0) > LINUX_VERSION_CODE)
			del_timer_sync(&ctx->ssr_timer);
			#else
			timer_delete_sync(&ctx->ssr_timer);
			#endif
			dev_dbg(cctx->dev,
				"%s: deleted timer for domain %d, handle 0x%x, sc 0x%x, pid %d, tid %d\n",
				__func__, cctx->domain_id, ctx->handle,
				ctx->sc, ctx->tgid, ctx->pid);
		}
	} else {
		/*
		 * For interruptible user rpc calls, after the user-specified
		 * timeout, issue a kill rpc call for the corresponding user pd
		 * as it is in an unresponsive state.
		 */
		if (ctx->handle > FASTRPC_MAX_STATIC_HANDLE &&
			cctx->domain->type == FASTRPC_NSP && fl->timeout) {
			/*
			 * User has specified an rpc timeout. So wait for dsp response
			 * with that timeout.
			 */
			timeleft =
				wait_for_completion_interruptible_timeout(
					&ctx->work,
					msecs_to_jiffies(fl->timeout));
			if (timeleft == 0) {
				dev_err(cctx->dev,
					"%s: user-call timed out after %ums for domain %d, handle 0x%x, sc 0x%x, pid %d, tid %d\n",
					__func__, fl->timeout, cctx->domain_id,
					ctx->handle, ctx->sc, ctx->tgid, ctx->pid);

				/* Send rpc timeout notification to user app */
				fastrpc_queue_pd_status(fl, cctx->domain_id,
					FASTRPC_USERPD_TIMEOUT, fl->sessionid);

				/* Close corresponding user-process on dsp */
				err = fastrpc_release_current_dsp_process(fl);
				if (err == -ETIME) {
					pr_err("%s: release dsp process timed out after %ums for domain %d, handle 0x%x, sc 0x%x, pid %d, tid %d\n",
						__func__, FASTRPC_NONINTERRUPT_CALL_TIMEOUT,
						cctx->domain_id, ctx->handle, ctx->sc,
						ctx->tgid, ctx->pid);
				}

				atomic_set(&fl->state, DSP_EXIT_COMPLETE);
				if (IS_DYNAMIC_PD(fl->pd_type))
					fastrpc_sysfs_notify_pids(cctx->domain);

				interrupted = -ETIME;
			} else if (timeleft < 0) {
				/* RPC call interrupted */
				interrupted = timeleft;
			}
		} else {
			interrupted =
				wait_for_completion_interruptible(&ctx->work);
		}
	}
	return interrupted;
}

static void fastrpc_wait_for_completion(struct fastrpc_invoke_ctx *ctx,
			int *ptr_interrupted, u32 kernel)
{
	int err = 0, jj = 0;
	bool wait_resp = false;
	u32 wTimeout = FASTRPC_USER_EARLY_HINT_TIMEOUT;
	u32 wakeTime = 0;
	struct fastrpc_user *fl = ctx->fl;
	struct fastrpc_timeline *timeline = fl->fastrpc_timeline_obj;

	do {
		switch (ctx->rsp_flags) {
		/* try polling on completion with timeout */
		case USER_EARLY_SIGNAL:
			wakeTime = ctx->early_wake_time;
			/* disable preempt to avoid context switch latency */
			preempt_disable();
			jj = 0;
			wait_resp = false;
			fastrpc_timeline_record(48, fl->tgid_app, timeline);
			for (; jj < wakeTime && jj < wTimeout; jj++) {
				wait_resp = try_wait_for_completion(&ctx->work);
				if (wait_resp)
					break;
				udelay(1);
			}
			fastrpc_timeline_record(49, fl->tgid_app, timeline);
			preempt_enable();
			if (!wait_resp) {
				fastrpc_timeline_record(50, fl->tgid_app, timeline);
				*ptr_interrupted = fastrpc_wait_for_response(ctx, kernel);
				fastrpc_timeline_record(51, fl->tgid_app, timeline);
				if (*ptr_interrupted || ctx->is_work_done)
					return;
			}
			break;
		/* busy poll on memory for actual job done */
		case EARLY_RESPONSE:
			trace_fastrpc_msg("early_response: poll_begin");
			fastrpc_timeline_record(24, fl->tgid_app, timeline);
			err = poll_for_remote_response(ctx, FASTRPC_POLL_TIME);
			/* Mark job done if poll on memory successful */
			/* Wait for completion if poll on memory timeout */
			if (!err) {
				ctx->is_work_done = true;
				fastrpc_timeline_record(44, fl->tgid_app, timeline);
				return;
			}
			fastrpc_timeline_record(25, fl->tgid_app, timeline);
			trace_fastrpc_msg("early_response: poll_timeout");
			if (!ctx->is_work_done) {
				if (ctx->rsp_flags == COMPLETE_SIGNAL)
					fastrpc_timeline_record(26, fl->tgid_app, timeline);
				*ptr_interrupted = fastrpc_wait_for_response(ctx, kernel);
				if (ctx->rsp_flags == COMPLETE_SIGNAL)
					fastrpc_timeline_record(39, fl->tgid_app, timeline);
				if (*ptr_interrupted || ctx->is_work_done)
					return;
			}
			break;
		case COMPLETE_SIGNAL:
		case NORMAL_RESPONSE:
			if (ctx->rsp_flags == NORMAL_RESPONSE)
				fastrpc_timeline_record(9, fl->tgid_app, timeline);
			if (ctx->rsp_flags == COMPLETE_SIGNAL)
				fastrpc_timeline_record(53, fl->tgid_app, timeline);
			*ptr_interrupted = fastrpc_wait_for_response(ctx, kernel);
			fastrpc_timeline_record(23, fl->tgid_app, timeline);
			if (*ptr_interrupted || ctx->is_work_done) {
				fastrpc_timeline_record(37, fl->tgid_app, timeline);
				return;
			}
			break;
		case POLL_MODE:
			trace_fastrpc_msg("poll_mode: begin");
			fastrpc_timeline_record(55, fl->tgid_app, timeline);
			err = poll_for_remote_response(ctx, ctx->fl->poll_timeout);
			fastrpc_timeline_record(56, fl->tgid_app, timeline);

			/* If polling timed out, move to normal response state */
			if (err) {
				trace_fastrpc_msg("poll_mode: timeout");
				ctx->rsp_flags = NORMAL_RESPONSE;
			} else {
				*ptr_interrupted = 0;
			}
			break;
		default:
			*ptr_interrupted = -EBADR;
			pr_err("unsupported response type:0x%x\n", ctx->rsp_flags);
			break;
		}
	} while (!ctx->is_work_done);
}

static void fastrpc_update_invoke_count(u32 handle, u64 *perf_counter,
					struct timespec64 *invoket)
{
	/* update invoke count for dynamic handles */
	u64 *invcount, *count;
	invcount = GET_COUNTER(perf_counter, PERF_INVOKE);
	if (invcount)
		*invcount += getnstimediff(invoket);

	count = GET_COUNTER(perf_counter, PERF_COUNT);
	if (count)
		*count += 1;
}

static int fastrpc_internal_invoke(struct fastrpc_user *fl,  u32 kernel,
				   struct fastrpc_enhanced_invoke *invoke)
{
	struct fastrpc_invoke_ctx *ctx = NULL;
	struct fastrpc_invoke *inv = &invoke->inv;
	u32 handle, sc;
	int err = 0, perferr = 0, interrupted = 0;
	u64 *perf_counter = NULL;
	struct timespec64 invoket = {0};
	struct device *dev = NULL;
	u32 priority = invoke->priority;

	if (atomic_read(&fl->cctx->teardown))
		return -EPIPE;

	if (fl->profile)
		ktime_get_real_ts64(&invoket);

	if (!fl->sctx)
		return -EINVAL;

	dev = fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	if ((!fl->cctx->dev) || (!dev))
		return -EPIPE;

	handle = inv->handle;
	sc = inv->sc;
	if (handle == FASTRPC_INIT_HANDLE && !kernel) {
		dev_warn_ratelimited(dev,
		"user app trying to send a kernel RPC message (%d)\n",  handle);
		return -EPERM;
	}

	/*
	 * After PDR, for Audio & OIS PD, kill call is still needed to clean
	 * the Audio & OIS PD process in root PD. For Sensors PD, no cleanup
	 * is needed in root PD of DSP.
	 */
	if (IS_PDR(fl) && fl->pd_type == SENSORS_STATICPD) {
		err = -EPIPE;
		return err;
	}

	if (!kernel) {
		ctx = fastrpc_context_restore_interrupted(fl, inv);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);
		if (ctx) {
			trace_fastrpc_context_restore(ctx->cctx->domain_id, (uint64_t)ctx,
					ctx->msg.ctx, ctx->msg.handle, ctx->msg.sc);
			goto wait;
		}
	}

	trace_fastrpc_msg("context_alloc: begin");
	fastrpc_timeline_record(3, fl->tgid_app, fl->fastrpc_timeline_obj);
	ctx = fastrpc_context_alloc(fl, kernel, sc, invoke);
	fastrpc_timeline_record(4, fl->tgid_app, fl->fastrpc_timeline_obj);
	trace_fastrpc_msg("context_alloc: end");
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	if (fl->profile)
		perf_counter = (u64 *)ctx->perf + PERF_COUNT;
	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_GETARGS),
	fastrpc_timeline_record(5, fl->tgid_app, fl->fastrpc_timeline_obj);
	err = fastrpc_get_args(kernel, ctx);
	if (err)
		goto bail;
	PERF_END);
	fastrpc_timeline_record(6, fl->tgid_app, fl->fastrpc_timeline_obj);
	trace_fastrpc_msg("get_args: end");

	/*
	 * For untrusted processes (opened by DSP HAL on behalf of another
	 * process), only permit invocations of dynamic handles that have an
	 * active admitted job in the scheduler executing list.  Static handles
	 * (<= FASTRPC_MAX_STATIC_HANDLE) and kernel-initiated calls are exempt.
	 */
	if (!kernel && invoke->appid >= 0 && fl->untrusted_process &&
		handle > FASTRPC_MAX_STATIC_HANDLE) {
		if (!fastrpc_scheduler_handle_is_executing(
				&fl->cctx->scheduler, fl, (u64)handle)) {
			dev_err(dev, "%s: untrusted process %d invoking handle 0x%x without active executing work",
				__func__, fl->tgid_frpc, handle);
			err = -EPERM;
			goto bail;
		}
	}

	/* make sure that all CPU memory writes are seen by DSP */
	dma_wmb();
	/* Send invoke buffer to remote dsp */
	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_LINK),
	fastrpc_timeline_record(7, fl->tgid_app, fl->fastrpc_timeline_obj);
	err = fastrpc_invoke_send(fl->sctx, priority, ctx, kernel, handle);
	if (err)
		goto bail;
	PERF_END);
	fastrpc_timeline_record(8, fl->tgid_app, fl->fastrpc_timeline_obj);
	trace_fastrpc_msg("invoke_send: end");
wait:
	if (fl->poll_mode &&
		handle > FASTRPC_MAX_STATIC_HANDLE &&
		(fl->cctx->domain->type == FASTRPC_NSP ||
		fl->cctx->domain->type == FASTRPC_LPASS) &&
		(fl->pd_type == USERPD || fl->pd_type == USER_UNSIGNEDPD_POOL))
		ctx->rsp_flags = POLL_MODE;

	fastrpc_wait_for_completion(ctx, &interrupted, kernel);
	if (interrupted != 0) {
		trace_fastrpc_msg("wait_for_completion: interrupted");
		err = interrupted;
		goto bail;
	}
	trace_fastrpc_msg("wait_for_completion: end");
	if (!ctx->is_work_done) {
		err = -ETIMEDOUT;
		dev_err(dev, "Error: Invalid workdone state for handle 0x%x, sc 0x%x\n",
			handle, sc);
		goto bail;
	}

	/* make sure that all memory writes by DSP are seen by CPU */
	dma_rmb();
	fastrpc_timeline_record(40, fl->tgid_app, fl->fastrpc_timeline_obj);
	/* populate all the output buffers with results */
	PERF(fl->profile, GET_COUNTER(perf_counter, PERF_PUTARGS),
	err = fastrpc_put_args(ctx, kernel);
	if (err)
		goto bail;
	PERF_END);
	trace_fastrpc_msg("put_args: end");
	/* Check the response from remote dsp */
	err = ctx->retval;
	if (err)
		goto bail;
	fastrpc_timeline_record(41, fl->tgid_app, fl->fastrpc_timeline_obj);

bail:
	if (ctx && interrupted == -ERESTARTSYS) {
		fastrpc_context_save_interrupted(ctx);
	} else if (ctx) {
		if (fl->profile && !interrupted)
			fastrpc_update_invoke_count(handle, perf_counter, &invoket);
		if (fl->profile && ctx->perf && handle > FASTRPC_RMID_INIT_MAX) {
			trace_fastrpc_perf_counters(handle, ctx->sc,
			ctx->perf->count, ctx->perf->flush, ctx->perf->map,
			ctx->perf->copy, ctx->perf->link, ctx->perf->getargs,
			ctx->perf->putargs, ctx->perf->invargs,
			ctx->perf->invoke, ctx->perf->tid);
			if (fl->profile && ctx->perf && ctx->perf_kernel)
				if (0 != (perferr = copy_to_user((void __user *)ctx->perf_kernel, ctx->perf, FASTRPC_KERNEL_PERF_LIST * sizeof(u64)))) {
					pr_warn("failed to copy perf data err 0x%x\n", perferr);
				}
		}
		spin_lock(&fl->lock);
		list_del(&ctx->node);
		spin_unlock(&fl->lock);
		fastrpc_context_put(ctx);
		trace_fastrpc_msg("context_free: end");
	}

	if (err)
		dev_dbg(dev, "Error: Invoke Failed %d\n", err);

	return err;
}

static int fastrpc_mem_map_to_dsp(struct fastrpc_user *fl, int fd, int offset,
				u32 flags, u64 va, u64 phys,
				size_t size, uintptr_t *raddr)
{
	struct fastrpc_invoke_args args[4] = { [0 ... 3] = { 0 } };
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_mem_map_req_msg req_msg = { 0 };
	struct fastrpc_mmap_rsp_msg rsp_msg = { 0 };
	struct fastrpc_phy_page pages = { 0 };
	struct device *dev = fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	int err = 0;

	if (!fl) {
		err = -EBADF;
		return err;
	}

	req_msg.pgid = fl->tgid_frpc;
	req_msg.fd = fd;
	req_msg.offset = offset;
	req_msg.vaddrin = va;
	req_msg.flags = flags;
	req_msg.num = sizeof(pages);
	req_msg.data_len = 0;

	args[0].ptr = (u64) (uintptr_t) &req_msg;
	args[0].length = sizeof(req_msg);

	pages.addr = phys;
	pages.size = size;

	args[1].ptr = (u64) (uintptr_t) &pages;
	args[1].length = sizeof(pages);

	args[2].ptr = (u64) (uintptr_t) &pages;
	args[2].length = 0;

	args[3].ptr = (u64) (uintptr_t) &rsp_msg;
	args[3].length = sizeof(rsp_msg);

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MEM_MAP, 3, 1);
	ioctl.inv.args = (__u64)args;
	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
	if (err) {
		dev_err(dev, "mem mmap error, fd %d, vaddr %llx, size %zx, err 0x%x\n",
			fd, va, size, err);
		return err;
	}
	*raddr = rsp_msg.vaddr;

	return 0;
}

static int fastrpc_create_persistent_headers(struct fastrpc_user *fl)
{
	int err = 0;
	int i = 0;
	u64 virtb = 0;
	struct device *dev = fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	struct fastrpc_buf *hdr_bufs, *buf, *pers_hdr_buf = NULL;
	u32 num_pers_hdrs = 0;
	size_t hdr_buf_alloc_len = 0;

	/*
	 * Pre-allocate memory for persistent header buffers based
	 * on concurrency info passed by user. Upper limit enforced.
	 */
	num_pers_hdrs = FASTRPC_MAX_PERSISTENT_HEADERS;
	hdr_buf_alloc_len = num_pers_hdrs * PAGE4K_SIZE;

	err = fastrpc_smmu_buf_alloc(fl, hdr_buf_alloc_len,
			METADATA_BUF, &pers_hdr_buf);
	if (err)
		return err;

	virtb = (u64) (uintptr_t)(pers_hdr_buf->virt);
	err = fastrpc_mem_map_to_dsp(fl, -1, 0,
				ADSP_MMAP_PERSIST_HDR, 0, (u64) (uintptr_t)(pers_hdr_buf->phys),
				pers_hdr_buf->size, &pers_hdr_buf->raddr);
	if (err)
		goto err_dsp_map;

	hdr_bufs = kcalloc(num_pers_hdrs, sizeof(struct fastrpc_buf),
				GFP_KERNEL);
	if (!hdr_bufs)
		return -ENOMEM;

	spin_lock(&fl->lock);
	fl->pers_hdr_buf = pers_hdr_buf;
	fl->num_pers_hdrs = num_pers_hdrs;
	fl->hdr_bufs = hdr_bufs;
	for (i = 0; i < num_pers_hdrs; i++) {
		buf = &fl->hdr_bufs[i];
		buf->fl = fl;
		buf->virt = (void *)(virtb + (i * PAGE4K_SIZE));
		buf->phys = pers_hdr_buf->phys + (i * PAGE4K_SIZE);
		buf->size = PAGE4K_SIZE;
		buf->type = pers_hdr_buf->type;
		buf->in_use = false;
	}
	spin_unlock(&fl->lock);

	return 0;
err_dsp_map:
	dev_err(dev, "Warning: failed to map len %zu, flags %d, num headers %u with err %d\n",
			hdr_buf_alloc_len, ADSP_MMAP_PERSIST_HDR,
			num_pers_hdrs, err);
	fastrpc_buf_free(pers_hdr_buf, 0);
	return err;
}

static bool is_session_rejected(struct fastrpc_user *fl, bool unsigned_pd_request)
{
	/* Check if the device node is non-secure and channel is secure */
	if (!fl->is_secure_dev && fl->cctx->secure) {
		/*
		 * Allow untrusted applications to offload only to Unsigned PD when
		 * channel is configured as secure and block untrusted apps on channel
		 * that does not support unsigned PD offload
		 */
		if (!fl->cctx->unsigned_support || !unsigned_pd_request)
			goto reject_session;
	}
	/* Check if untrusted process is trying to offload to signed PD */
	if (fl->untrusted_process && !unsigned_pd_request)
		goto reject_session;

	return false;
reject_session:
	dev_err(fl->cctx->dev, "Error: Untrusted application trying to offload to signed PD");
	return true;
}

static int fastrpc_get_process_gids(struct gid_list *gidlist)
{
	struct group_info *group_info = current_cred()->group_info;
	int i, num_gids;
	u32 *gids = NULL;

	if (!group_info)
		return -EFAULT;

	num_gids = group_info->ngroups + 1;
	gids = kcalloc(num_gids, sizeof(u32), GFP_KERNEL);
	if (!gids)
		return -ENOMEM;

	/* Get the real GID */
	gids[0] = __kgid_val(current_gid());

	/* Get the supplemental GIDs */
	for (i = 1; i < num_gids; i++)
		gids[i] = __kgid_val(group_info->gid[i - 1]);

	sort(gids, num_gids, sizeof(*gids), uint_cmp_func, NULL);
	gidlist->gids = gids;
	gidlist->gidcount = num_gids;

	return 0;
}

static void fastrpc_check_privileged_process(struct fastrpc_user *fl,
				struct fastrpc_init_create *init)
{
	struct gid_list gidlist = {0};
	u32 gid;

	/* disregard any privilege bits from userspace */
	init->attrs &= (~FASTRPC_MODE_PRIVILEGED);

	if (fastrpc_get_process_gids(&gidlist)) {
		dev_info(fl->cctx->dev, "%s failed to get gidlist\n",
				__func__);
		return;
	}

	gid = sorted_lists_intersection(gidlist.gids,
			gidlist.gidcount, fl->cctx->gidlist.gids,
			fl->cctx->gidlist.gidcount);


	if (gid) {
		dev_info(fl->cctx->dev, "%s: %s (PID %d, GID %u) is a privileged process\n",
				__func__, current->comm, current->tgid, gid);
		init->attrs |= FASTRPC_MODE_PRIVILEGED;
	}
	/* Free memory for gid allocated in fastrpc_get_process_gids */
	kfree(gidlist.gids);
}

static int fastrpc_remote_heap_unassign(struct fastrpc_channel_ctx *cctx, struct fastrpc_buf *buf)
{
	u64 src_perms = 0;
	struct qcom_scm_vmperm dst_perms;
	int i, err = 0;

	if (cctx->vmcount) {
		for (i = 0; i < cctx->vmcount; i++)
			src_perms |= BIT(cctx->vmperms[i].vmid);

		dst_perms.vmid = QCOM_SCM_VMID_HLOS;
		dst_perms.perm = QCOM_SCM_PERM_RWX;
		err = qcom_scm_assign_mem(buf->phys, (u64)buf->size,
					&src_perms, &dst_perms, 1);
		if (err) {
			dev_err(cctx->dev, "%s: Failed to assign memory with phys 0x%llx size 0x%llx err %d\n",
				__func__, buf->phys, buf->size, err);
			BUG_ON(1);
			return err;
		}
	}
	return 0;
}

int fastrpc_mmap_remove_ssr(struct fastrpc_channel_ctx *cctx)
{
	struct fastrpc_buf *buf, *b, *match;
	unsigned long flags;
	int err = 0;

	do {
		match = NULL;
		spin_lock_irqsave(&cctx->lock, flags);
		list_for_each_entry_safe(buf, b, &cctx->gmaps, node) {
			match = buf;
			list_del(&buf->node);
			break;
		}
		spin_unlock_irqrestore(&cctx->lock, flags);
		if (!match)
			return 0;

		err = fastrpc_remote_heap_unassign(cctx, match);
		if (err) {
			spin_lock_irqsave(&cctx->lock, flags);
			list_add_tail(&match->node, &cctx->gmaps);
			spin_unlock_irqrestore(&cctx->lock, flags);
			return err;
		}

		__fastrpc_buf_free(match);

	} while (match);

	return 0;
}

/*
 * Function to get static PD for process trying to attach,
 * by comparing fixed pid.
 */
static int fastrpc_get_static_pd_session(struct fastrpc_user *fl, u32 *session)
{
	int i, err = 0;
	struct fastrpc_static_pd *spd = NULL;

	if (!fl)
		return -EBADF;

	for (i = 0; i < FASTRPC_MAX_SPD ; i++) {
		spd = &fl->cctx->spd[i];
		if (!spd->spd_id)
			continue;
		if (fl->spd_id == spd->spd_id) {
			*session = i;
			break;
		}
	}

	if (i >= FASTRPC_MAX_SPD)
		return -EUSERS;

	if (spd && atomic_read(&spd->ispdup) == 0)
		return -ENOTCONN;

	return err;
}

/* Function to check if static PD is up on remote subsystem */
static int fastrpc_check_static_pd_status(struct fastrpc_user *fl, u32 session)
{
	if (atomic_read(&fl->cctx->spd[session].ispdup) == 0)
		return -ENOTCONN;
	return 0;
}

/*
 * Function to get static PD to attach to and check its status.
 * Only one application can attach to Audio & OIS PD.
 */
static int fastrpc_init_static_pd_status(struct fastrpc_user *fl)
{
	int err = 0;
	u32 session = 0;

	if (!fl)
		return -EBADF;

	err = fastrpc_get_static_pd_session(fl, &session);
	if (err)
		return err;

	err = fastrpc_check_static_pd_status(fl, session);
	if (err)
		return err;

	// Allow only one application to connect to audio & OIS PD
	if (atomic_add_unless(&fl->cctx->spd[session].is_attached, 1, 1)) {
		fl->spd = &fl->cctx->spd[session];
	} else {
		dev_err(fl->cctx->dev,"Application already attached to audio PD\n");
		return -ECONNREFUSED;
	}

	return err;
}

/*
 * Function to get static PD to attach to and check its status.
 * Multiple applications can attach to sensors PD
 */
static int fastrpc_init_sensor_static_pd_status(struct fastrpc_user *fl)
{
	int err = 0;
	u32 session = 0;

	if (!fl)
		return -EBADF;

	err = fastrpc_get_static_pd_session(fl, &session);
	if (err)
		return err;

	err = fastrpc_check_static_pd_status(fl, session);
	if (err)
		return err;

	fl->spd = &fl->cctx->spd[session];

	// Update PDR count, to check for any PDR.
	fl->spd->prevpdrcount = fl->spd->pdrcount;

	return err;
}

static void print_buf_info(struct seq_file *s_file, struct fastrpc_buf *buf)
{
    seq_printf(s_file,"\n %s %2s 0x%p", "virt", ":", buf->virt);
	seq_printf(s_file,"\n %s %2s 0x%llx", "phys", ":", buf->phys);
	seq_printf(s_file,"\n %s %2s 0x%lx", "raddr", ":", buf->raddr);
	seq_printf(s_file,"\n %s %2s 0x%x", "type", ":", buf->type);
	seq_printf(s_file,"\n %s %2s 0x%llx", "size", ":", buf->size);
	seq_printf(s_file,"\n %s %s %d", "in_use", ":", buf->in_use);
}

static void print_ictx_info(struct seq_file *s_file, struct fastrpc_invoke_ctx *ictx)
{
	seq_printf(s_file,"\n %s %7s %d", "nscalars", ":", ictx->nscalars);
	seq_printf(s_file,"\n %s %10s %d", "nbufs", ":", ictx->nbufs);
	seq_printf(s_file,"\n %s %10s %d", "retval", ":", ictx->retval);
	seq_printf(s_file,"\n %s %12s %px", "crc", ":", ictx->crc);
	seq_printf(s_file,"\n %s %1s %d", "early_wake_time", ":", ictx->early_wake_time);
	seq_printf(s_file,"\n %s %5s %px", "perf_kernel", ":", ictx->perf_kernel);
	seq_printf(s_file,"\n %s %7s %px", "perf_dsp", ":", ictx->perf_dsp);
	seq_printf(s_file,"\n %s %12s %d", "pid", ":", ictx->pid);
	seq_printf(s_file,"\n %s %11s %d", "tgid", ":", ictx->tgid);
	seq_printf(s_file,"\n %s %13s 0x%x", "sc", ":", ictx->sc);
	seq_printf(s_file,"\n %s %10s %llu", "ctxid", ":", ictx->ctxid);
	seq_printf(s_file,"\n %s %3s %d", "is_work_done", ":", ictx->is_work_done);
	seq_printf(s_file,"\n %s %9s %llu", "msg_sz", ":", ictx->msg_sz);
}

static void print_sctx_info(struct seq_file *s_file, struct fastrpc_pool_ctx *sctx)
{
	int i;
	struct fastrpc_smmu *s = NULL;

	seq_printf(s_file,"%s %9s %d\n", "pd_type", ":", sctx->pd_type);
	seq_printf(s_file,"%s %10s %d\n", "secure", ":", sctx->secure);
	seq_printf(s_file,"%s %8s %d\n", "sharedcb", ":", sctx->sharedcb);
	seq_printf(s_file,"%s %7s %d\n", "smmucount", ":", sctx->smmucount);
	seq_printf(s_file,"%s %8s %d\n", "usecount", ":", sctx->usecount);

	for (i = 0; i < sctx->smmucount; i++) {
		s = &sctx->smmucb[i];
		seq_printf(s_file,"\n========== SMMU context bank %d=============\n", i);
		seq_printf(s_file,"%s %13s %d\n", "sid", ":", s->sid);
		seq_printf(s_file,"%s %11s %d\n", "valid", ":", s->valid);
		seq_printf(s_file,"%s %4s %lu\n", "genpool_iova", ":",
								s->genpool_iova);
		seq_printf(s_file,"%s %4s %zu\n", "genpool_size", ":",
								s->genpool_size);
		seq_printf(s_file,"%s %2s %llx\n", "allocatedbytes", ":",
								s->allocatedbytes);
		seq_printf(s_file,"%s %6s %llx\n", "totalbytes", ":", s->totalbytes);
		seq_printf(s_file,"%s %4s %llx\n", "minallocsize", ":",
								s->minallocsize);
		seq_printf(s_file,"%s %4s %llx\n", "maxallocsize", ":",
								s->maxallocsize);
	}
}

static void print_ctx_info(struct seq_file *s_file, struct fastrpc_channel_ctx *ctx)
{
	seq_printf(s_file,"%s %8s %d\n", "domain_id", ":", ctx->domain_id);
	seq_printf(s_file,"%s %8s %d\n", "sesscount", ":", ctx->sesscount);
	seq_printf(s_file,"%s %10s %d\n", "vmcount", ":", ctx->vmcount);
	seq_printf(s_file,"%s %12s %llu\n", "perms", ":", ctx->perms);
	seq_printf(s_file,"%s %s %d\n", "valid_attributes", ":", ctx->valid_attributes);
	seq_printf(s_file,"%s %3s %d\n", "cpuinfo_status", ":", ctx->cpuinfo_status);
	seq_printf(s_file,"%s %2s %d\n", "staticpd_status", ":", ctx->staticpd_status);
	seq_printf(s_file,"%s %11s %d\n", "secure", ":", ctx->secure);
	seq_printf(s_file,"%s %s %d\n", "unsigned_support", ":", ctx->unsigned_support);
	seq_printf(s_file,"%s %4s %d\n",  "startshutdown", ":", ctx->startshutdown);
	seq_printf(s_file,"%s %9s %d\n",  "teardown", ":", atomic_read(&ctx->teardown));
	seq_printf(s_file,"%s %7s %u\n",  "invoke_cnt", ":", ctx->invoke_cnt);

	if (ctx->valid_attributes) {
		seq_printf(s_file, "\n=============== DSP Attributes ===============\n");
		for (int i = 0; i < FASTRPC_MAX_DSP_ATTRIBUTES; i++)
			seq_printf(s_file, "dsp_attributes[%d] : %u\n", i, ctx->dsp_attributes[i]);
	}
}

static void print_map_info(struct seq_file *s_file, struct fastrpc_map *map)
{
	seq_printf(s_file,"%s %4s %d\n", "fd", ":", map->fd);
	seq_printf(s_file,"%s %s 0x%llx\n", "phys", ":", map->phys);
	seq_printf(s_file,"%s %s 0x%llx\n", "size", ":", map->size);
	seq_printf(s_file,"%s %4s 0x%p\n", "va", ":", map->va);
	seq_printf(s_file,"%s %3s 0x%llx\n", "len", ":", map->len);
	seq_printf(s_file,"%s %2s 0x%llx\n", "raddr", ":", map->raddr);
	seq_printf(s_file,"%s %2s 0x%x\n", "attr", ":", map->attr);
	seq_printf(s_file,"%s %2s 0x%x\n", "flags", ":", map->flags);
}

static void print_session_info(struct seq_file *s_file, struct fastrpc_user *fl)
{
	seq_printf(s_file,"%s %2s %s\n", "process_name", ":", fl->name);
	seq_printf(s_file,"%s %12s %d\n", "tgid", ":", fl->tgid);
	seq_printf(s_file,"%s %7s %d\n", "tgid_frpc", ":", fl->tgid_frpc);
	seq_printf(s_file,"%s %3s %d\n", "is_secure_dev", ":", fl->is_secure_dev);
	seq_printf(s_file,"%s %3s %d\n", "num_pers_hdrs", ":", fl->num_pers_hdrs);
	seq_printf(s_file,"%s %2s %d\n", "num_cached_buf", ":", fl->num_cached_buf);
	seq_printf(s_file,"%s %5s %d\n", "wake_enable", ":", fl->wake_enable);
	seq_printf(s_file,"%s %2s %d\n",  "is_unsigned_pd", ":", fl->is_unsigned_pd);
	seq_printf(s_file,"%s %7s %d\n",  "sessionid", ":", fl->sessionid);
	seq_printf(s_file,"%s %9s %d\n", "pd_type", ":", fl->pd_type);
	seq_printf(s_file,"%s %9s %d\n",  "profile", ":", fl->profile);
	seq_printf(s_file,"%s %7s %d\n", "poll_mode", ":", fl->poll_mode);
	seq_printf(s_file,"%s %8s %d\n", "sharedcb", ":", fl->sharedcb);
	seq_printf(s_file,"%s %11s %d\n", "state", ":", atomic_read(&fl->state));
	seq_printf(s_file,"%s %9s %u\n", "timeout", ":", fl->timeout);
	seq_printf(s_file,"%s %s %d\n", "multi_session_support", ":", fl->multi_session_support);
	seq_printf(s_file,"%s %4s %d\n", "dsp_recovery", ":", fl->dsp_recovery);
}

static void print_tx_msgs(struct seq_buf *gmsgbuf, struct fastrpc_tx_msg *tx_msg)
{
	int i;

	seq_buf_printf(gmsgbuf, "\n=============== glink tx_msgs ===============\n");

	for (i = 0; i < GLINK_MSG_HISTORY_LEN; i++) {
		seq_buf_printf(gmsgbuf,
			"pid = %d, tid = %d, ctx = %llu, handle = %u, sc = %u, addr = %llu, size = %llu, rpmsg_send_err = %d, ns = %lld\n",
			tx_msg[i].msg.pid,
			tx_msg[i].msg.tid,
			(unsigned long long)tx_msg[i].msg.ctx,
			tx_msg[i].msg.handle,
			tx_msg[i].msg.sc,
			(unsigned long long)tx_msg[i].msg.addr,
			(unsigned long long)tx_msg[i].msg.size,
			tx_msg[i].rpmsg_send_err,
			(long long)tx_msg[i].ns);
	}
}

static void print_rx_msgs(struct seq_buf *gmsgbuf, struct fastrpc_rx_msg *rx_msg)
{
	int i;

	seq_buf_printf(gmsgbuf, "\n=============== glink rx_msgs ===============\n");

	for (i = 0; i < GLINK_MSG_HISTORY_LEN; i++) {
		seq_buf_printf(gmsgbuf,
			"ctx = %llu, retval = %d, flags = %u, early_wake_time = %u, version = %u, ns = %lld\n",
			(unsigned long long)rx_msg[i].rsp.ctx,
			rx_msg[i].rsp.retval,
			rx_msg[i].rsp.flags,
			rx_msg[i].rsp.early_wake_time,
			rx_msg[i].rsp.version,
			(long long)rx_msg[i].ns);
	}
}

static void print_rpmsg_glink_logs(struct seq_buf *gmsgbuf, struct fastrpc_rpmsg_log *log)
{
	unsigned long flags_tx, flags_rx;
	struct fastrpc_tx_msg *tx_copy;
	struct fastrpc_rx_msg *rx_copy;

	tx_copy = vmalloc(sizeof(struct fastrpc_tx_msg) * GLINK_MSG_HISTORY_LEN);
	if (!tx_copy) {
		pr_warn("%s: Failed to allocate tx_copy, skipping RPMSG logs\n", __func__);
		return;
	}

	rx_copy = vmalloc(sizeof(struct fastrpc_rx_msg) * GLINK_MSG_HISTORY_LEN);
	if (!rx_copy) {
		pr_warn("%s: Failed to allocate rx_copy, skipping RPMSG logs\n", __func__);
		vfree(tx_copy);
		return;
	}

	spin_lock_irqsave(&log->tx_lock, flags_tx);
	memcpy(tx_copy, log->tx_msgs, sizeof(struct fastrpc_tx_msg) * GLINK_MSG_HISTORY_LEN);
	spin_unlock_irqrestore(&log->tx_lock, flags_tx);
	spin_lock_irqsave(&log->rx_lock, flags_rx);
	memcpy(rx_copy, log->rx_msgs, sizeof(struct fastrpc_rx_msg) * GLINK_MSG_HISTORY_LEN);
	spin_unlock_irqrestore(&log->rx_lock, flags_rx);

	seq_buf_printf(gmsgbuf, "\n=============== RPMSG GLINK Logs ===============\n");
	print_tx_msgs(gmsgbuf, tx_copy);
	print_rx_msgs(gmsgbuf, rx_copy);

	vfree(tx_copy);
	vfree(rx_copy);
}

static int fastrpc_debugfs_show(struct seq_file *s_file, void *data)
{
	struct fastrpc_user *fl = s_file->private;
	struct fastrpc_map *map;
	struct fastrpc_channel_ctx *ctx;
	struct fastrpc_pool_ctx *sctx = NULL;
	struct fastrpc_invoke_ctx *ictx, *m;
	struct fastrpc_buf *buf, *n;
	int i, ret;
	unsigned long irq_flags = 0;

	if (fl != NULL) {
		ret = fastrpc_file_get(fl);
		if (ret) {
			/* User object being released as ref-count is already 0 */
			return 0;
		}

		print_session_info(s_file, fl);

		if(fl->cctx) {
			seq_printf(s_file,"\n=============== Channel Context ===============\n");
			ctx = fl->cctx;
			print_ctx_info(s_file, ctx);
		}
		if(fl->sctx) {
			seq_printf(s_file,"\n=============== Session Context ===============\n");
			sctx = fl->sctx;
			print_sctx_info(s_file, sctx);
		}
		if(fl->secsctx) {
			seq_printf(s_file,"\n=============== Secure Session Context ===============\n");
			sctx = fl->secsctx;
			print_sctx_info(s_file, sctx);
		}

		spin_lock(&fl->lock);
		if (fl->init_mem) {
			seq_printf(s_file,"\n=============== Init Mem ===============\n");
			buf = fl->init_mem;
			print_buf_info(s_file, buf);
		}
		if (fl->pers_hdr_buf) {
			seq_printf(s_file,"\n=============== Persistent Header Buf ===============\n");
			buf = fl->pers_hdr_buf;
			print_buf_info(s_file, buf);
		}
		if (fl->hdr_bufs) {
			seq_printf(s_file,"\n=============== Pre-allocated Header Buf ===============\n");
			buf = fl->hdr_bufs;
			print_buf_info(s_file, buf);
		}
		spin_unlock(&fl->lock);

		seq_printf(s_file,"\n=============== Global Maps ===============\n");
		spin_lock_irqsave(&fl->cctx->lock, irq_flags);
		list_for_each_entry_safe(buf, n, &fl->cctx->gmaps, node) {
			print_buf_info(s_file, buf);
		}
		spin_unlock_irqrestore(&fl->cctx->lock, irq_flags);
		seq_printf(s_file,"\n=============== DSP Signal Status ===============\n");
		spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
		for (i = 0; i < FASTRPC_DSPSIGNAL_NUM_SIGNALS/FASTRPC_DSPSIGNAL_GROUP_SIZE; i++) {
			if (fl->signal_groups[i] != NULL)
				seq_printf(s_file,"%d : %d ",i, fl->signal_groups[i]->state);
		}
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		seq_printf(s_file,"\n=============== User space maps ===============\n");
		spin_lock(&fl->lock);
		list_for_each_entry(map, &fl->maps, node) {
			if (map)
				print_map_info(s_file, map);
		}
		seq_printf(s_file,"\n=============== Kernel maps ===============\n");
		list_for_each_entry(buf, &fl->mmaps, node) {
			if (buf)
				print_buf_info(s_file, buf);
		}
		seq_printf(s_file,"\n=============== Cached Bufs ===============\n");
		list_for_each_entry_safe(buf, n, &fl->cached_bufs, node) {
			if(buf)
				print_buf_info(s_file, buf);
		}
		seq_printf(s_file,"\n=============== Pending contexts ===============\n");
		list_for_each_entry_safe(ictx, m, &fl->pending, node) {
			if (ictx)
				print_ictx_info(s_file, ictx);
		}
		seq_printf(s_file,"\n=============== Interrupted contexts ===============\n");
		list_for_each_entry_safe(ictx, m, &fl->interrupted, node) {
			if (ictx)
				print_ictx_info(s_file, ictx);
		}
		spin_unlock(&fl->lock);
		fastrpc_file_put(fl, false);
	}
	return 0;
}
#ifdef CONFIG_DEBUG_FS
DEFINE_SHOW_ATTRIBUTE(fastrpc_debugfs);

static int fastrpc_create_session_debugfs(struct fastrpc_user *fl)
{
	char cur_comm[TASK_COMM_LEN];
	int domain_id = -1, size = 0;
	struct dentry *debugfs_root = g_frpc.debugfs_root;

	if (atomic_cmpxchg(&fl->debugfs_file_create, 0, 1))
		return 0;
	memcpy(cur_comm, current->comm, TASK_COMM_LEN);
	cur_comm[TASK_COMM_LEN-1] = '\0';
	if (debugfs_root != NULL) {
		domain_id = fl->cctx->domain_id;
		size = strlen(cur_comm) + strlen("_")
			+ COUNT_OF(current->pid) + strlen("_")
			+ COUNT_OF(fl->tgid_frpc) + strlen("_")
			+ COUNT_OF(domain_id)
			+ 1;

		fl->debugfs_buf = kzalloc(size, GFP_KERNEL);
		if (fl->debugfs_buf == NULL) {
			return -ENOMEM;
		}
		/*
		 * Use HLOS process name, HLOS PID, unique fastrpc PID
		 * domain_id in debugfs filename to create unique file name
		 */
		snprintf(fl->debugfs_buf, size, "%.10s%s%d%s%d%s%d",
			cur_comm, "_", current->pid, "_",
			fl->tgid_frpc, "_", domain_id);
		fl->debugfs_file = debugfs_create_file(fl->debugfs_buf, 0644,
			debugfs_root, fl, &fastrpc_debugfs_fops);
		if (IS_ERR_OR_NULL(fl->debugfs_file)) {
			pr_warn("Error: %s: %s: failed to create debugfs file %s\n",
					cur_comm, __func__, fl->debugfs_buf);
			fl->debugfs_file = NULL;
		}
		kfree(fl->debugfs_buf);
	}
return 0;
}
#endif

static int fastrpc_init_create_static_process(struct fastrpc_user *fl,
					      char __user *argp)
{
	struct fastrpc_init_create_static init;
	struct fastrpc_invoke_args args[FASTRPC_CREATE_STATIC_PROCESS_NARGS] = {0};
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_phy_page pages[1];
	struct fastrpc_buf *buf = NULL;
	struct fastrpc_smmu *smmucb = NULL;
	struct fastrpc_pool_ctx *sctx = NULL;
	u64 phys = 0, size = 0;
	char *name;
	int err = 0;
	bool scm_done = false;
	bool is_audiopd = false;
	unsigned long flags;
	struct {
		int pgid;
		u32 namelen;
		u32 pageslen;
	} inbuf;

	if (!fl->is_secure_dev) {
		dev_err(fl->cctx->dev, "untrusted app trying to attach to privileged DSP PD\n");
		return -EACCES;
	}

	if (copy_from_user(&init, argp, sizeof(init)))
		return -EFAULT;

	if ((init.namelen > INIT_FILE_NAMELEN_MAX) || (!init.namelen))
		return -EINVAL;

	name = memdup_user_nul(u64_to_user_ptr(init.name), init.namelen);
	/* ret -ENOMEM for malloc failure, -EFAULT for copy_from_user failure */
	if (IS_ERR(name))
		return PTR_ERR(name);

	sctx = fastrpc_session_alloc(fl, false, fl->pd_type);
	if (!sctx) {
		dev_err(fl->cctx->dev, "No session available\n");
		err = -EBUSY;
		goto err_name;
	}
	fl->sctx = sctx;

	smmucb = &fl->sctx->smmucb[DEFAULT_SMMU_IDX];

	/*
	 * Update the pd_type, to direct the messages to correct PD, when
	 * fastrpc_getpd_msgidx is queried. Update pd_type only after session
	 * allocation. Session is allocated based on user configured pd_type
	 */
	if (!strcmp(name, AUDIOPD)) {
		fl->pd_type = AUDIO_STATICPD;
		fl->servloc_name = AUDIO_PDR_SERVICE_LOCATION_CLIENT_NAME;
		fl->spd_id = AUDIO_STATIC_ID;
		is_audiopd = true;
	} else if (!strcmp(name, OISPD)) {
		fl->pd_type = OIS_STATICPD;
		fl->servloc_name = OIS_PDR_ADSP_SERVICE_LOCATION_CLIENT_NAME;
		fl->spd_id = OIS_STATIC_ID;
	} else if (!strcmp(name, ASCPD)) {
		fl->pd_type = ASC_STATICPD;
		fl->spd_id = ASC_STATIC_ID;
	} else {
		dev_err(smmucb->dev,
		"Create static process is failed for proc_name %s", name);
		err = -EINVAL;
		goto err_name;
	}

	err = fastrpc_init_static_pd_status(fl);
	if (err)
		goto err_name;
	if (is_audiopd && IS_PDR(fl)) {
		/*
		 * Remove any previous remote heap mappings in case process is trying
		 * to reconnect after a PD restart on remote subsystem.
		 */
		err = fastrpc_mmap_remove_ssr(fl->cctx);
		if (err) {
			pr_warn("%s: %s: failed to unmap remote heap (err %d)\n",
				current->comm, __func__, err);
			goto err_name;
		}
	}
	// Update PDR count, to check for any PDR.
	fl->spd->prevpdrcount =	fl->spd->pdrcount;

	inbuf.pgid = fl->tgid_frpc;
	inbuf.namelen = init.namelen;
	inbuf.pageslen = 0;

	// Remote heap feature is available only for audio static PD
	if (!fl->cctx->staticpd_status && is_audiopd) {
		inbuf.pageslen = 1;
		err = fastrpc_buf_alloc(fl, NULL, init.memlen, REMOTEHEAP_BUF, &buf);
		if (err)
			goto err_name;

		phys = buf->phys;
		size = buf->size;
		/* Map if we have any heap VMIDs associated with this ADSP Static Process. */
		if (fl->cctx->vmcount) {
			u64 src_perms = BIT(QCOM_SCM_VMID_HLOS);

			err = qcom_scm_assign_mem(phys, (u64)size,
							&src_perms, fl->cctx->vmperms, fl->cctx->vmcount);
			if (err) {
				dev_err(smmucb->dev,
			"%s: Failed to assign memory with phys 0x%llx size 0x%llx err %d",
					__func__, phys, size, err);
				goto err_map;
			}
			scm_done = true;
		}
		fl->cctx->staticpd_status = true;
	}

	args[0].ptr = (u64)(uintptr_t)&inbuf;
	args[0].length = sizeof(inbuf);
	args[0].fd = -1;

	args[1].ptr = (u64)(uintptr_t)name;
	args[1].length = inbuf.namelen;
	args[1].fd = -1;

	pages[0].addr = phys;
	pages[0].size = size;

	args[2].ptr = (u64)(uintptr_t) pages;
	args[2].length = sizeof(*pages);
	args[2].fd = -1;

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_CREATE_STATIC, 3, 0);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
	if (err)
		goto err_invoke;

#ifdef CONFIG_DEBUG_FS
	if (fl != NULL)
		fastrpc_create_session_debugfs(fl);
#endif
	kfree(name);

	if (buf) {
		spin_lock_irqsave(&fl->cctx->lock, flags);
		list_add_tail(&buf->node, &fl->cctx->gmaps);
		spin_unlock_irqrestore(&fl->cctx->lock, flags);
	}
	return 0;
err_invoke:
	if (fl->cctx->vmcount && scm_done) {
		u64 src_perms = 0;
		struct qcom_scm_vmperm dst_perms;
		u32 i;

		for (i = 0; i < fl->cctx->vmcount; i++)
			src_perms |= BIT(fl->cctx->vmperms[i].vmid);

		dst_perms.vmid = QCOM_SCM_VMID_HLOS;
		dst_perms.perm = QCOM_SCM_PERM_RWX;
		err = qcom_scm_assign_mem(phys, (u64)size,
						&src_perms, &dst_perms, 1);
		if (err)
			dev_err(smmucb->dev,
			"%s: Failed to assign memory phys 0x%llx size 0x%llx err %d",
				__func__, phys, size, err);
	}
err_map:
	if (buf) {
		fl->cctx->staticpd_status = false;
		fastrpc_buf_free(buf, false);
	}
err_name:
	kfree(name);
	return err;
}

/*
 * Find context bank / session with root PD type
 * @arg1: channel context.
 * @arg2: session context.
 *
 * The function searches for the session reserved for root pd from
 * the list of available sessions in a channel.
 *
 * Returns 0 if there is a session reserved for root pd.
 */
static int fastrpc_get_root_session(struct fastrpc_channel_ctx *cctx,
	struct fastrpc_pool_ctx **sess)
{
	int i = 0, err = -ENOSR;
	struct fastrpc_pool_ctx *s = NULL;
	unsigned long flags = 0;

	spin_lock_irqsave(&cctx->lock, flags);
	for (i = 0; i < cctx->sesscount; i++) {
		s = &cctx->session[i];
		if (s->pd_type == ROOT_PD && s->smmucb[DEFAULT_SMMU_IDX].valid) {
			*sess = s;
			err = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
	return err;
}


/**
 * fastrpc_alloc_root_session_buf() - Allocate buffer using root PD session
 *
 * @cctx: Channel context pointer
 * @obuf: Output pointer to allocated buffer
 * @size: Size of buffer to allocate
 * @buf_type: Type of buffer to allocate
 *
 * This function allocates a buffer using the context bank/session reserved
 * for root PD. It retrieves the root session from the channel context and
 * uses it to allocate the requested buffer.
 *
 * Return: 0 on success, negative error code on failure
 */
static int fastrpc_alloc_root_session_buf(
	struct fastrpc_channel_ctx *cctx,
	struct fastrpc_buf **obuf, u64 size, u32 buf_type)
{
	struct fastrpc_buf *buf = NULL;
	struct fastrpc_pool_ctx *sess = NULL;
	struct fastrpc_smmu *smmucb = NULL;
	int err = 0;

	/* Get context bank / session reserved for rootPD */
	err = fastrpc_get_root_session(cctx, &sess);
	if (err)
		goto bail;

	smmucb = &sess->smmucb[DEFAULT_SMMU_IDX];
	err = __fastrpc_buf_alloc(NULL, smmucb, cctx->domain_id, size, &buf,
				 buf_type);
	if (err)
		goto bail;
	*obuf = buf;

bail:
	if (err) {
		dev_err(cctx->dev,
			"Error 0x%x: %s: failed to allocate buffer domain id %u size 0x%llx type %d\n",
			err, __func__, cctx->domain_id, size, buf_type);
	}
	return err;
}

/**
 * fastrpc_preload_mem_free() - Free preloaded memory buffer for a channel
 *
 * @cctx: Pointer to the fastrpc channel context
 *
 * This function frees the preloaded memory buffer associated with the given
 * channel context. It safely releases the buffer under spinlock protection
 * with interrupts disabled to prevent race conditions.
 *
 * Context: Can be called from any context. Disables interrupts internally.
 */
static void fastrpc_preload_mem_free(struct fastrpc_channel_ctx *cctx)
{
	if (!cctx->preload_buf)
		return;
	__fastrpc_buf_free(cctx->preload_buf);
	cctx->preload_buf = NULL;
}

/**
 * fastrpc_preload_mem_alloc() - Allocate preload memory buffer for DSP
 * @cctx: Pointer to the FastRPC channel context
 * @pages: Array of physical page descriptors to be populated
 * @pageslen: Pointer to the length of pages array, updated on success
 * @page_idx: Current index in the pages array
 *
 * This function allocates a preload memory buffer for the DSP if preload
 * support is enabled and the buffer doesn't already exist. The buffer is
 * allocated as a root session buffer and stored in the channel context.
 * Thread-safe allocation is ensured using spinlocks to prevent race
 * conditions. On success, the physical address and size of the buffer
 * are stored in the pages array at the specified index.
 *
 * Return: 0 on success, negative error code on failure
 */
static int fastrpc_preload_mem_alloc(struct fastrpc_channel_ctx *cctx,
	struct fastrpc_phy_page *pages, u32 *pageslen, u32 page_num)
{
	int err = 0;
	unsigned long flags = 0;
	struct fastrpc_buf *buf = NULL;


	if (!cctx->dsp_attributes[FASTRPC_PRELOAD_SUPPORT])
		return err;

	if (!cctx->preload_buf) {
		err = fastrpc_alloc_root_session_buf(cctx, &buf,
						     FASTRPC_DEFAULT_PRELOAD_BUF_SIZE,
						     ROOT_PRELOAD_BUF);
		if (err)
			goto bail;
	}

	spin_lock_irqsave(&cctx->lock, flags);
	if (!cctx->preload_buf) {
		cctx->preload_buf = buf;
		/* Set buf as NULL to indicate it is being used */
		buf = NULL;
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
	*pageslen = page_num;
	pages[page_num-1].addr = cctx->preload_buf->phys;
	pages[page_num-1].size = cctx->preload_buf->size;

bail:
	if (buf)
		__fastrpc_buf_free(buf);
	return err;
}

/*
 * Allocate buffer for growing rootheap on DSP
 * @arg1: channel context.
 * @arg2: page array to be sent with process spawn msg
 * @arg3: number of pages
 *
 * Returns 0 on success
 */
static int fastrpc_alloc_rootheap_buf(struct fastrpc_channel_ctx *cctx,
	struct fastrpc_phy_page *pages, u32 *pageslen)
{
	struct fastrpc_buf *buf = NULL;
	int err = 0;
	unsigned long flags = 0;
	const unsigned int ROOTHEAP_BUF_SIZE =
		(cctx->rootheap_buf_size != 0) ? cctx->rootheap_buf_size :
		FASTRPC_DEFAULT_ROOTHEAP_BUF_SIZE;
	const unsigned int NUM_ROOTHEAP_BUFS =
		(cctx->rootheap_buf_count != 0) ? cctx->rootheap_buf_count :
		FASTRPC_DEFAULT_ROOTHEAP_BUF_COUNT;

	/* Allocate buffer only if DSP supports growing of rootheap */
	if (!cctx->dsp_attributes[ROOTPD_RPC_HEAP_SUPPORT] ||
		cctx->rootheap_bufs.num >= NUM_ROOTHEAP_BUFS ||
		g_frpc.is_trusted_vm)
		return err;

	/* Allocate buffer from context bank / session reserved for rootPD */
	err = fastrpc_alloc_root_session_buf(cctx, &buf,
						ROOTHEAP_BUF_SIZE,
						ROOTHEAP_BUF);
	if (err)
		goto bail;

	/* Update paramaters of process-spawn with buffer info */
	*pageslen = NUM_PAGES_WITH_ROOTHEAP_BUF;
	pages[NUM_PAGES_WITH_ROOTHEAP_BUF - 1].addr = buf->phys;
	pages[NUM_PAGES_WITH_ROOTHEAP_BUF - 1].size = buf->size;

	/* Add buf to channel's rootheap buf-list and increment count */
	spin_lock_irqsave(&cctx->lock, flags);
	list_add_tail(&buf->node, &cctx->rootheap_bufs.list);
	cctx->rootheap_bufs.num++;
	spin_unlock_irqrestore(&cctx->lock, flags);
bail:
	return err;
}

/**
 * fastrpc_alloc_perf_timeline_bufs() -  Allocates timeline buffs
 * @fl: fastrpc user object.
 * @pages: Pages to be packed for DSP.
 * @pageslen: Number of pages.
 * @timeline_num_events: Total number of timeline events.
 * @user_version: Userspace version.
 *
 * Allocates timeline buffers and updates the pages to include
 * the physical addresses and sizes of these buffers.
 * Initalizes timeline object.
 *
 * Return: 0 on success, negative error code on failure.
 */
static int fastrpc_alloc_perf_timeline_bufs(struct fastrpc_user *fl,
	struct fastrpc_phy_page *pages, u32 timeline_num_events,
	u32 user_version, struct fastrpc_timeline **timeline)
{
	int err = 0;
	u32 timeline_buf_event = GET_TIMELINE_BUF_LEN(timeline_num_events);
	size_t size = GET_TIMELINE_BUF_SIZE(timeline_buf_event);
	struct fastrpc_timeline_buffer *timeline_buf_hlos_k = NULL;
	struct fastrpc_buf *timeline_buf_dsp_k = NULL,
		*timeline_buf_dsp_u = NULL;

	*timeline = kvzalloc(sizeof(struct fastrpc_timeline), GFP_KERNEL);
	if (!(*timeline))
		return -ENOMEM;

	timeline_buf_hlos_k = kvzalloc(size, GFP_KERNEL);
	if (!timeline_buf_hlos_k) {
		err = -ENOMEM;
		goto bail;
	}
	fastrpc_timeline_buffer_init(timeline_buf_hlos_k, timeline_buf_event);

	err = fastrpc_smmu_buf_alloc(fl, size, USER_BUF, &timeline_buf_dsp_k);
	if (err)
		goto bail;
	fastrpc_timeline_buffer_init(timeline_buf_dsp_k->virt, timeline_buf_event);

	err = fastrpc_smmu_buf_alloc(fl, size, USER_BUF, &timeline_buf_dsp_u);
	if (err)
		goto bail;
	fastrpc_timeline_buffer_init(timeline_buf_dsp_u->virt, timeline_buf_event);

	fastrpc_timeline_init(*timeline, user_version, timeline_buf_hlos_k,
		timeline_buf_dsp_k, timeline_buf_dsp_u);

	pages[NUM_PAGES_WITH_PERF_TIMLINE_DSP_U_SHAREDBUF - 1].addr = timeline_buf_dsp_u->phys;
	pages[NUM_PAGES_WITH_PERF_TIMLINE_DSP_U_SHAREDBUF - 1].size = timeline_buf_dsp_u->size;

	pages[NUM_PAGES_WITH_PERF_TIMLINE_DSP_K_SHAREDBUF - 1].addr = timeline_buf_dsp_k->phys;
	pages[NUM_PAGES_WITH_PERF_TIMLINE_DSP_K_SHAREDBUF - 1].size = timeline_buf_dsp_k->size;

	return 0;
bail:
	if (timeline_buf_dsp_u)
		fastrpc_buf_free(timeline_buf_dsp_u, false);
	if (timeline_buf_dsp_k)
		fastrpc_buf_free(timeline_buf_dsp_k, false);
	kvfree(timeline_buf_hlos_k);
	kvfree(*timeline);
	*timeline = NULL;

	return err;
}

static int get_unique_hlos_process_id(struct fastrpc_channel_ctx *cctx)
{
	int tgid_frpc = -1;
	int ret = -1;

	/* allocate unique id between 1 and MAX_FRPC_TGID both inclusive */
	ret = ida_alloc_range(&cctx->tgid_frpc_ida, 1,
			       MAX_FRPC_TGID, GFP_ATOMIC);
	if (ret < 0) {
		return -1;
	}
	tgid_frpc = ((cctx->domain_id) * FASTRPC_UNIQUE_ID_CONST) + ret;
	return tgid_frpc;
}

static char *get_process_basename(void)
{
    struct file *exe_file;
    const char *filename;
    char *result = NULL;

    if (!current->mm)
        return NULL;

    rcu_read_lock();
    exe_file = get_file_rcu(&current->mm->exe_file);
    rcu_read_unlock();

    if (!exe_file)
        return NULL;

    if (exe_file->f_path.dentry) {
        filename = exe_file->f_path.dentry->d_name.name;
        if(filename)
        	result = kstrdup(filename, GFP_KERNEL);
    }
    fput(exe_file);
    return result;
}

/**
 * fastrpc_pack_root_sharedpage()- Packs shared page for rootPD.
 * @fl: fastrpc user instance.
 * @pages: pages to be packed for DSP.
 * @pageslen: Number of pages.
 *
 * fastrpc_pack_root_sharedpage packs root shared page during
 * creation of a dynamic process.
 *
 * Return: 0 on success.
 */
static int fastrpc_pack_root_sharedpage(struct fastrpc_user *fl,
	struct fastrpc_phy_page *pages, u32 *pageslen)
{
	int err = 0;
	u64 addr = fl->config.root_addr;
	u32 size = fl->config.root_size;
	struct fastrpc_smmu *smmucb = &fl->sctx->smmucb[DEFAULT_SMMU_IDX];

	/* Allocate kernel buffer for rootPD shared page */
	if (addr && size) {
		err = fastrpc_buf_alloc(fl, smmucb, size, USER_BUF,
					&fl->proc_init_sharedbuf);
		if (err) {
			dev_err(smmucb->dev, "failed to allocate buffer\n");
			return err;
		}
		/* Copy contents from userspace buffer containing data for rootPD */
		if (copy_from_user(fl->proc_init_sharedbuf->virt,
				(void __user *)(uintptr_t)addr, size)) {
			err = -EFAULT;
			goto err_sharedbuf_fail;
		}
		/* Update paramaters of process-spawn with buffer info */
		*pageslen = NUM_PAGES_WITH_PROC_INIT_SHAREDBUF;
		pages[NUM_PAGES_WITH_PROC_INIT_SHAREDBUF-1].addr =
			fl->proc_init_sharedbuf->phys;
		pages[NUM_PAGES_WITH_PROC_INIT_SHAREDBUF-1].size =
			fl->proc_init_sharedbuf->size;
	}

	return 0;

err_sharedbuf_fail:
	fastrpc_buf_free(fl->proc_init_sharedbuf, false);
	fl->proc_init_sharedbuf = NULL;
	return err;
}

/*
 * fastrpc_get_and_copy_shell_file() - copy shell file into kernel
 *
 * @fl            : FastRPC user file pointer
 * @user_file_ptr : pointer to user-space pointer for file data address
 * @filelen       : pointer to length of the file data
 * @filefd        : file descriptor if file is passed as a descriptor
 *
 * Behavior:
 * - If filefd > 0, returns NULL (file provided via descriptor).
 * - If either *filelen == 0 or *user_file_ptr == 0 when no descriptor
 *   is provided, sets both to 0 and returns NULL, allowing the DSP to
 *   load the shell via the daemon. If file pointer and size is given
 *   allocate buffer copy the data from userspace to avoid invalid
 *   access.
 *
 * Returns:
 * - pointer to allocated buffer on success
 * - NULL if no buffer is needed
 * - ERR_PTR(-ENOMEM) or ERR_PTR(-EFAULT) on error
 */
static void *fastrpc_get_and_copy_shell_file(
		struct fastrpc_user *fl,
		u64 *user_file_ptr,
		u32 *filelen,
		s32 filefd)
{
	void *file = NULL;

	/* File passed as fd; no need to copy */
	if (filefd > 0)
		return NULL;

	if (!user_file_ptr || !filelen)
		return NULL;

	/*
	 * If no file provided, normalize to zero for
	 * daemon-based shell load.
	 */
	if (!(*filelen) || !(*user_file_ptr)) {
		*user_file_ptr = 0;
		*filelen = 0;
		return NULL;
	}

	/*
	 * In case file pointer and file length is valid and
	 * fd is not valid, allocate buffer and copy the data
	 * from userspace to avoid invalid access.
	 */

	file = kzalloc(*filelen, GFP_KERNEL);
	if (!file)
		return ERR_PTR(-ENOMEM);
	if (copy_from_user(file,
			(void __user *)(uintptr_t)(*user_file_ptr),
			*filelen)) {
		dev_err(fl->cctx->dev,
			"%s: copy_from_user failed for shell file of len %u\n",
			__func__, *filelen);
		kfree(file);
		return ERR_PTR(-EFAULT);
	}
	return file;
}

/**
 * Log detailed session-related information to ring buffer
 *
 * Logs session metadata, channel context, and invoke, interrupted context
 * information for a given user session to the ring buffer for debugging.
 *
 * @param[in] s_file        : Pointer to seq_file used for formatting output
 * @param[in] fl            : Pointer to fastrpc user session
 * @param[in] session_num   : Session number for identification
 */
static void fastrpc_log_session_info(struct seq_file *s_file,
	struct fastrpc_user *fl, int session_num)
{
	struct fastrpc_invoke_ctx *ictx;

	s_file->count = 0;
	print_session_info(s_file, fl);
	FASTRPC_LOG_INFO(fl->cctx->dev, fl->cctx, FASTRPC_LOG_RINGBUF,
		"session_num : %d %.*s\n", session_num, s_file->count, s_file->buf);

	if (fl->sctx) {
		s_file->count = 0;
		print_sctx_info(s_file, fl->sctx);
		FASTRPC_LOG_INFO(fl->cctx->dev, fl->cctx, FASTRPC_LOG_RINGBUF,
		"session_num : %d %.*s\n", session_num, s_file->count, s_file->buf);
	}

	spin_lock(&fl->lock);
	list_for_each_entry(ictx, &fl->pending, node) {
		s_file->count = 0;
		print_ictx_info(s_file, ictx);
		FASTRPC_LOG_INFO(fl->cctx->dev, fl->cctx, FASTRPC_LOG_RINGBUF,
		"session_num : %d %.*s\n", session_num, s_file->count, s_file->buf);
	}
	list_for_each_entry(ictx, &fl->interrupted, node) {
		s_file->count = 0;
		print_ictx_info(s_file, ictx);
		FASTRPC_LOG_INFO(fl->cctx->dev, fl->cctx, FASTRPC_LOG_RINGBUF,
		"session_num : %d %.*s\n", session_num, s_file->count, s_file->buf);
	}
	spin_unlock(&fl->lock);
}

/**
 * Collect and log information for all active sessions
 *
 * Iterates over all user sessions in the channel context and logs
 * detailed session information to the ring buffer.
 *
 * @param[in] cctx : Pointer to channel context
 *
 * @return 0 on success, error code on failure
 */
static void fastrpc_get_sessions_info(struct fastrpc_channel_ctx *cctx)
{
	struct seq_file *s_file = NULL;
	int err = 0, ret, session_num = 1;
	struct fastrpc_user *fl, *n;
	unsigned long irq_flags = 0;
	struct list_head users_list;

	INIT_LIST_HEAD(&users_list);
	s_file = kzalloc(sizeof(*s_file), GFP_KERNEL);
	if (!s_file) {
		err = -ENOMEM;
		goto bail;
	}
	s_file->buf = (char*)kzalloc(SESSION_BUF_SIZE, GFP_KERNEL);
	if (!s_file->buf) {
		err = -ENOMEM;
		goto bail;
	}
	s_file->size = SESSION_BUF_SIZE;

	print_ctx_info(s_file, cctx);
	FASTRPC_LOG_INFO(cctx->dev, cctx, FASTRPC_LOG_RINGBUF,
		"Channel information for dsp-type : %d %.*s\n",
		cctx->domain->type, s_file->count, s_file->buf);

	spin_lock_irqsave(&cctx->lock, irq_flags);
	list_for_each_entry(fl, &cctx->users, user) {
		ret = fastrpc_file_get(fl);
		if (ret) {
			dev_warn(cctx->dev, "Warning: %s: user-obj for fl (%pK) being released\n",
				__func__, fl);
			continue;
		}
		list_add_tail(&fl->rb_log_node, &users_list);
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);

	list_for_each_entry_safe(fl, n, &users_list, rb_log_node) {
		fastrpc_log_session_info(s_file, fl,
			session_num);
		list_del(&fl->rb_log_node);
		fastrpc_file_put(fl, false);
		session_num++;
	}

bail:
	if (err)
		dev_err(cctx->dev, "%s : err %d Failed to push session information into ring buffer\n",
				__func__, err);
	if (s_file) {
		kfree(s_file->buf);
		kfree(s_file);
	}
}

static int fastrpc_init_create_process(struct fastrpc_user *fl,
					char __user *argp)
{
	struct fastrpc_init_create init;
	struct fastrpc_invoke_args args[FASTRPC_CREATE_PROCESS_NARGS] = {0};
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_phy_page pages[NUM_PAGES_WITH_PERF_TIMLINE_DSP_K_SHAREDBUF] = {0};
	struct fastrpc_map *configmap = NULL;
	struct fastrpc_buf *imem = NULL;
	struct fastrpc_pool_ctx *sctx = NULL;
	struct fastrpc_timeline *timeline = NULL;
	int memlen;
	int err = 0, timeline_err = 0;
	int user_fd = fl->config.user_fd, user_size = fl->config.user_size;
	void *file = NULL;
	u32 *dsp_attributes = fl->cctx->dsp_attributes;

	/*
	 * DSP resource attribute and term variables for process resource
	 * calculation
	 */
	u32 pd = 0, compute = 0, tg = 0, mem_thread = 0, threads = 0;
	u64 compute_size = 0, thread_size = 0, proc_res_size = 0;

	struct {
		int pgid;
		u32 namelen;
		u32 filelen;
		u32 pageslen;
		u32 attrs;
		u32 siglen;
	} inbuf;

	if (copy_from_user(&init, argp, sizeof(init)))
		return -EFAULT;

	if (init.filelen > INIT_FILELEN_MAX)
		return -EINVAL;

	/* Return an error if the create process already started or completed */
	if (atomic_cmpxchg(&fl->state, DEFAULT_PROC_STATE,
				DSP_CREATE_START) != DEFAULT_PROC_STATE)
		return -EALREADY;

	file = fastrpc_get_and_copy_shell_file(fl, &init.file,
			&init.filelen, init.filefd);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		file = NULL;
		goto err_out;
	}
	/*
	 * Third-party apps don't have permission to open the fastrpc device, so
	 * it is opened on their behalf by DSP HAL. This is detected by
	 * comparing current PID with the one stored during device open.
	 */
	fl->tgid_app = current->tgid;
	if (fl->tgid_app != fl->tgid) {
		fl->untrusted_process = true;
		char *pname = get_process_basename();
		if (pname) {
			snprintf(fl->name, sizeof(fl->name), "%s-%d-%s",
					current->comm, fl->tgid_app, pname);
			kfree(pname);
		} else {
			snprintf(fl->name, sizeof(fl->name), "%s-%d", current->comm, fl->tgid_app);
		}
	}

	/* Get the uid of the current process */
	fl->uid = __kuid_val(current_euid());

	if (init.attrs & FASTRPC_MODE_UNSIGNED_MODULE)
		fl->is_unsigned_pd = true;

	/* Disregard any system unsigned PD attribute from userspace */
	init.attrs &= (~FASTRPC_MODE_SYSTEM_UNSIGNED_PD);

	if (is_session_rejected(fl, fl->is_unsigned_pd)) {
		err = -EACCES;
		goto err_out;
	}

	/* Trusted apps will be launched as system unsigned PDs */
	if (!fl->untrusted_process && fl->is_unsigned_pd)
		init.attrs |= FASTRPC_MODE_SYSTEM_UNSIGNED_PD;

	/*
	 * Use SMMU pooled session for unsigned PD,
	 * if smmucb_pool is set to true
	 */
	if (fl->is_unsigned_pd && fl->cctx->smmucb_pool)
		fl->pd_type = USER_UNSIGNEDPD_POOL;

	sctx = fastrpc_session_alloc(fl, false, fl->pd_type);
	if (!sctx) {
		dev_warn_ratelimited(fl->cctx->dev, "No session available\n");
		if (!atomic_cmpxchg(&fl->cctx->sessions_info_active, 0, 1)) {
			fastrpc_get_sessions_info(fl->cctx);
			atomic_set(&fl->cctx->sessions_info_active, 0);
		}
		err = -EBUSY;
		goto err_out;
	}
	fl->sctx = sctx;

	/* In case of privileged process update attributes */
	fastrpc_check_privileged_process(fl, &init);

	inbuf.pgid = fl->tgid_frpc;
	inbuf.namelen = strlen(fl->name) + 1;
	inbuf.filelen = init.filelen;
	inbuf.pageslen = 1;
	inbuf.attrs = init.attrs;
	inbuf.siglen = init.siglen;

	/*
	 * Default value at fastrpc_device_open is set as DEFAULT_UNUSED.
	 * If pd_type is not configured by the process in fastrpc_set_session_info,
	 * update the pd_type to USERPD, so that messages are directed to
	 * dynamic process when fastrpc_getpd_msgidx is queried.
	 * Do this only after session allocation
	 */
	if (fl->pd_type == DEFAULT_UNUSED)
		fl->pd_type = USERPD;

	if (user_fd != -1 && user_size > 0) {
		mutex_lock(&fl->map_mutex);
		err = fastrpc_map_create(fl, user_fd, 0, NULL,
				user_size, 0, 0, &configmap, true);
		mutex_unlock(&fl->map_mutex);
		if (err)
			goto err_out;
		inbuf.pageslen = NUM_PAGES_WITH_SHARED_BUF;
		pages[NUM_PAGES_WITH_SHARED_BUF - 1].addr = configmap->phys;
		pages[NUM_PAGES_WITH_SHARED_BUF - 1].size = configmap->size;
	}

	/* Process spawn should not fail if unable to alloc rootheap buffer */
	fastrpc_alloc_rootheap_buf(fl->cctx, pages, &inbuf.pageslen);

	/* Process spawn should not fail if unable to pack root buffer */
	fastrpc_pack_root_sharedpage(fl, pages, &inbuf.pageslen);

	memlen = INIT_MEMLEN_MAX;

	err = fastrpc_smmu_buf_alloc(fl, memlen, INITMEM_BUF, &imem);
	if (err)
		goto err_alloc;

	/*
	 * If dbglogbuf is supported on DSP, allocate 1MB buffer and send it to DSP
	 * Process spawn should not fail if unable to alloc debug log buffer
	 */
	if (dsp_attributes[DBGLOGBUF_SUPPORT]) {
		err = fastrpc_smmu_buf_alloc(fl, DBGLOGBUF_SIZE,
				MAP_DEBUG_BUF, &fl->dbglogbuf);
		if (err) {
			if (fl->dbglogbuf) {
				fastrpc_buf_free(fl->dbglogbuf, false);
				fl->dbglogbuf = NULL;
			}
			dev_err(fl->cctx->dev, "Error %d: %s: Failed to allocate dbglogbuf buffer size %d\n",
				err, __func__, DBGLOGBUF_SIZE);
		} else {
			pages[NUM_PAGES_WITH_MAP_DEBUG_BUF-1].addr = fl->dbglogbuf->phys;
			pages[NUM_PAGES_WITH_MAP_DEBUG_BUF-1].size = fl->dbglogbuf->size;
			inbuf.pageslen = NUM_PAGES_WITH_MAP_DEBUG_BUF;
		}
	}

	/*
	 * If FIRMWARE_MEM_PROTECTION_DOMAIN is supported on DSP,
	 * allocate memory required for process resources and send it to DSP
	 * Process spawn should fail if unable to alloc process resources buffer
	 * BUF_SIZE should be >= (#threads * qdi_stack_size) +
	 * (#hvx_contexts * hvx_ctx_size) + (#hlx_contexts * hlx_ctx_size).
	 */
	if (dsp_attributes[FIRMWARE_MEM_PROTECTION_DOMAIN] ||
		dsp_attributes[FIRMWARE_MEM_COMPUTE_RESOURCE] ||
		dsp_attributes[FIRMWARE_MEM_THREAD]) {
		/* Overflow checks for each operation */
		pd = dsp_attributes[FIRMWARE_MEM_PROTECTION_DOMAIN];
		compute = dsp_attributes[FIRMWARE_MEM_COMPUTE_RESOURCE];
		tg = dsp_attributes[HANDLE_PRIORITY_SUPPORT];
		mem_thread = dsp_attributes[FIRMWARE_MEM_THREAD];
		threads = dsp_attributes[MAX_THREAD_COUNT_PROTECTION_DOMAIN];

		/* Raise donation floor to client-requested count if higher than
		 * DSP default; set via FASTRPC_INVOKE_SESSIONINFO V2 before PROC_CREATE.
		 * fl->max_threads is 0 if V2 was never called.
		 */
		if (fl->max_threads > threads)
			threads = fl->max_threads;

		if (compute > 0 && tg > 0 && compute > (U64_MAX / tg)) {
			dev_err(fl->cctx->dev,
				"Error: %s: Overflow in compute_size: compute=%u tg=%u\n",
				__func__, compute, tg);
			err = -EINVAL;
			goto err_alloc;
		}
		compute_size = (u64)compute * (u64)tg;

		if (mem_thread > 0 && threads > 0 && mem_thread > (U64_MAX / threads)) {
			dev_err(fl->cctx->dev,
				"Error: %s: Overflow in thread_size: mem_thread=%u threads=%u\n",
				__func__, mem_thread, threads);
			err = -EINVAL;
			goto err_alloc;
		}
		thread_size = (u64)mem_thread * (u64)threads;

		if ((u64)pd > (U64_MAX - compute_size)) {
			dev_err(fl->cctx->dev,
				"Error: %s: Overflow in pd + compute_size: pd=%u compute_size=%llu\n",
				__func__, pd, compute_size);
			err = -EINVAL;
			goto err_alloc;
		}
		proc_res_size = (u64)pd + compute_size;

		if (proc_res_size > (U64_MAX - thread_size)) {
			dev_err(fl->cctx->dev,
				"Error: %s: Overflow in proc_res_size + thread_size: proc_res_size=%llu thread_size=%llu\n",
				__func__, proc_res_size, thread_size);
			err = -EINVAL;
			goto err_alloc;
		}
		proc_res_size += thread_size;

		dev_dbg(fl->cctx->dev,
			"%s: DSP RTOS donation sizes pd=%u tg=%u compute=%u mem-thread=%u threads=%u total=%llu\n",
			__func__, pd, tg, compute, mem_thread, threads, proc_res_size);

		err = fastrpc_smmu_buf_alloc(fl, proc_res_size,
				PROC_RESOURCES_BUF, &fl->proc_res_buf);
		if (err) {
			dev_err(fl->cctx->dev,
				"Error %d: %s: Failed to allocate process resources buffer size %llu\n",
				err, __func__, proc_res_size);
			goto err_alloc;
		} else {
			pages[NUM_PAGES_WITH_DSP_RTOS_MEM_DONATION - 1].addr =
				fl->proc_res_buf->phys;
			pages[NUM_PAGES_WITH_DSP_RTOS_MEM_DONATION - 1].size =
				fl->proc_res_buf->size;
			inbuf.pageslen = NUM_PAGES_WITH_DSP_RTOS_MEM_DONATION;
		}
	}

	err = fastrpc_preload_mem_alloc(fl->cctx, pages, &inbuf.pageslen, NUM_PAGES_WITH_PRELOAD_BUF);
	if(err)
		dev_err(fl->cctx->dev, "Error %d: %s: Failed to allocate preload buffer\n",
				err, __func__);

	if (fl->timeline_init_args && fl->timeline_init_args->version) {
		timeline_err = fastrpc_alloc_perf_timeline_bufs(fl, pages,
				fl->timeline_init_args->num_events,
				fl->timeline_init_args->version, &timeline);
		if (timeline_err)
			pr_err("%s: Failed to allocate timeline buffer(err 0x%x)\n",
				__func__, timeline_err);
		else
			inbuf.pageslen = NUM_PAGES_WITH_PERF_TIMLINE_DSP_K_SHAREDBUF;
	}

	fl->init_mem = imem;
	args[0].ptr = (u64)(uintptr_t)&inbuf;
	args[0].length = sizeof(inbuf);
	args[0].fd = -1;

	args[1].ptr = (u64)(uintptr_t)fl->name;
	args[1].length = inbuf.namelen;
	args[1].fd = -1;

	args[2].ptr = file ? (u64)(uintptr_t)file : init.file;
	args[2].length = inbuf.filelen;
	args[2].fd = init.filefd;

	pages[0].addr = imem->phys;
	pages[0].size = imem->size;

	args[3].ptr = (u64)(uintptr_t) pages;
	args[3].length = inbuf.pageslen * sizeof(*pages);
	args[3].fd = -1;

	args[4].ptr = (u64)(uintptr_t)&inbuf.attrs;
	args[4].length = sizeof(inbuf.attrs);
	args[4].fd = -1;

	args[5].ptr = (u64)(uintptr_t) &inbuf.siglen;
	args[5].length = sizeof(inbuf.siglen);
	args[5].fd = -1;

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_CREATE, 4, 0);
	if (init.attrs)
		ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_CREATE_ATTR, 4, 0);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
	if (err)
		goto err_invoke;

	timeline_err = fastrpc_update_timeline_version(timeline);
	if (!timeline_err)
		fl->fastrpc_timeline_obj = timeline;
	else
		kvfree(timeline);

	if (fl->cctx->domain->type == FASTRPC_NSP) {
		fastrpc_create_persistent_headers(fl);
	}

#ifdef CONFIG_DEBUG_FS
	fastrpc_create_session_debugfs(fl);
#endif
	/* remove buffer on success as no longer required */
	if (fl->proc_init_sharedbuf) {
		fastrpc_buf_free(fl->proc_init_sharedbuf, false);
		fl->proc_init_sharedbuf = NULL;
	}
	kfree(file);

	return 0;

err_invoke:
	spin_lock(&fl->lock);
	fl->init_mem = NULL;
	spin_unlock(&fl->lock);
	fastrpc_buf_free(imem, false);
	kvfree(timeline);
err_alloc:
	if (fl->proc_init_sharedbuf) {
		fastrpc_buf_free(fl->proc_init_sharedbuf, false);
		fl->proc_init_sharedbuf = NULL;
	}
	if (configmap) {
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(configmap);
		mutex_unlock(&fl->map_mutex);
	}
	if (fl->dbglogbuf) {
		fastrpc_buf_free(fl->dbglogbuf, false);
		fl->dbglogbuf = NULL;
	}
	if (fl->proc_res_buf) {
		fastrpc_buf_free(fl->proc_res_buf, false);
		fl->proc_res_buf = NULL;
	}
err_out:
	kfree(file);
	/* Reset the process state to its default in case of an error. */
	atomic_set(&fl->state, DEFAULT_PROC_STATE);
	return err;
}

static void fastrpc_context_list_free(struct fastrpc_user *fl)
{
	struct fastrpc_invoke_ctx *ctx, *n;

	list_for_each_entry_safe(ctx, n, &fl->interrupted, node) {
		spin_lock(&fl->lock);
		list_del(&ctx->node);
		spin_unlock(&fl->lock);
		fastrpc_context_put(ctx);
	}

	list_for_each_entry_safe(ctx, n, &fl->pending, node) {
		spin_lock(&fl->lock);
		list_del(&ctx->node);
		spin_unlock(&fl->lock);
		fastrpc_context_put(ctx);
	}
}

static int fastrpc_release_current_dsp_process(struct fastrpc_user *fl)
{
	struct fastrpc_invoke_args args[1];
	struct fastrpc_enhanced_invoke ioctl;
	int tgid = 0;

	tgid = fl->tgid_frpc;
	args[0].ptr = (u64)(uintptr_t) &tgid;
	args[0].length = sizeof(tgid);
	args[0].fd = -1;

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_RELEASE, 1, 0);
	ioctl.inv.args = (__u64)args;

	return fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_NONZERO_PID, &ioctl);
}

/*
 * Helper function to increment / decrement invoke count of channel
 * Caller of this function MUST spin-lock 'cctx->lock' first.
 */
static inline void fastrpc_channel_update_invoke_cnt(
		struct fastrpc_channel_ctx *cctx, bool incr)
{
	if (incr) {
		cctx->invoke_cnt++;
	} else {
		cctx->invoke_cnt--;
		/* Wake up any waiting SSR handling thread */
		if (cctx->invoke_cnt == 0)
			wake_up_interruptible(&cctx->ssr_wait_queue);
	}
}

void fastrpc_free_user(struct fastrpc_user *fl)
{
	struct fastrpc_map *map = NULL, *m = NULL;

	/* Abort all scheduler jobs owned by this user */
	fastrpc_scheduler_user_cleanup(fl);

	fastrpc_context_list_free(fl);

	if (fl->init_mem) {
		fastrpc_buf_free(fl->init_mem, false);
		fl->init_mem = NULL;
	}

	mutex_lock(&fl->map_mutex);
	// During process tear down free the map, even if refcount is non-zero
	list_for_each_entry_safe(map, m, &fl->maps, node) {
		// During process tear down, 'retained iova' maps also need to be freed
		if (map->attr == FASTRPC_MAP_ATTR_RETAIN_IOVA)
			map->attr = FASTRPC_MAP_ATTR_DEFAULT;
		__fastrpc_free_map(map);
	}
	mutex_unlock(&fl->map_mutex);

	fastrpc_buf_list_free(fl, &fl->mmaps, false);

	if (fl->pers_hdr_buf) {
		fastrpc_buf_free(fl->pers_hdr_buf, false);
		fl->pers_hdr_buf = NULL;
	}

	if (fl->dbglogbuf) {
		fastrpc_buf_free(fl->dbglogbuf, false);
		fl->dbglogbuf = NULL;
	}

	if (fl->proc_res_buf) {
		fastrpc_buf_free(fl->proc_res_buf, false);
		fl->proc_res_buf = NULL;
	}

	if (fl->hdr_bufs) {
		kfree(fl->hdr_bufs);
		fl->hdr_bufs = NULL;
	}

	fastrpc_timeline_deinit(fl->fastrpc_timeline_obj);
	kvfree(fl->fastrpc_timeline_obj);
	fl->fastrpc_timeline_obj = NULL;
	kfree(fl->timeline_init_args);
	fl->timeline_init_args = NULL;
	fastrpc_buf_list_free(fl, &fl->cached_bufs, true);

	return;
}

/*
 * Free fastrpc user object of client app/remote channel
 *
 * @arg1 : fastrpc_user (NULL for default channel user, non-NULL for user-apps)
 * @arg2 : channel context (NULL for user-apps, non-NULL for default user)
 *
 * This function deletes a fastrpc user object:
 *		- 	for a user-app when it closes the fastrpc device node
 *		- 	when a remote channel goes thru ssr and its default user needs
 *			to be removed
 *
 * Returns 0 if user object is successfully removed
 */
static int fastrpc_user_obj_free(struct fastrpc_user *user,
	struct fastrpc_channel_ctx *cctx)
{
	struct fastrpc_user *fl = NULL;
	struct fastrpc_driver *frpc_drv, *d;
	struct fastrpc_buf *buf, *b;
	struct fastrpc_mdctx_info *mdctx = NULL, *m = NULL;
	int i;
	unsigned long flags, irq_flags;
	bool locked = false, is_driver_registered = false;
	spinlock_t *glock = &g_frpc.glock;
	int err = 0;

	if (user) {
		fl = user;
		cctx = fl->cctx;
	} else {
		fl = (struct fastrpc_user *)cctx->kcomm_user.obj;
		if (!fl)
			return -EINVAL;

		/*
		 * Most of the cleanup done for user objects of regular user-apps
		 * can be skipped for the channel's default user object.
		 */
		goto skip_user_cleanup;
	}

	spin_lock_irqsave(glock, irq_flags);
	spin_lock_irqsave(&cctx->lock, flags);

	if (atomic_read(&cctx->teardown)) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		spin_unlock_irqrestore(glock, irq_flags);
		/*
		 * Wait until SSR cleanup is done to avoid parallel access of
		 * fastrpc_user object from device release thread and
		 * SSR handling thread.
		 */
		wait_for_completion(&cctx->ssr_complete);
		spin_lock_irqsave(glock, irq_flags);
		spin_lock_irqsave(&cctx->lock, flags);
	} else {
		/*
		 * Update invoke count to block the SSR handling thread from cleaning up
		 * the channel resources, while it is still being used by this thread.
		 */
		fastrpc_channel_update_invoke_cnt(cctx, true);
	}
	if (fl->device) {
		fl->device->dev_close = true;
		fl->device->fl = NULL;
	}
	atomic_set(&fl->state, DSP_EXIT_START);
	list_for_each_entry_safe(frpc_drv, d, &fl->fastrpc_drivers, hn){
		/*
		 * Registered driver can free driver object in callback.
		 * So, delete object from list first.
		 */
		list_del(&frpc_drv->hn);
		if(frpc_drv->callback) {
			spin_unlock_irqrestore(&cctx->lock, flags);
			spin_unlock_irqrestore(glock, irq_flags);
			frpc_drv->callback(fl->device, FASTRPC_PROC_DOWN);
			spin_lock_irqsave(glock, irq_flags);
			spin_lock_irqsave(&cctx->lock, flags);
		}
		is_driver_registered = true;
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
	spin_unlock_irqrestore(glock, irq_flags);

	/* Iterate thru all multidomain contexts of user and destroy each one */
	spin_lock(&fl->lock);
	list_for_each_entry_safe(mdctx, m, &fl->mdctxs, node) {
		spin_unlock(&fl->lock);
		err = fastrpc_multidomain_ctx_cleanup(fl,
			FASTRPC_MDCTX_REMOVE, mdctx->ctx);
		spin_lock(&fl->lock);
	}
	spin_unlock(&fl->lock);

	/*
	 * If no driver is registered on the device, free it here.
	 * If any active driver is still registered, device will
	 * be freed when driver is unregistered.
	 */
	if (!is_driver_registered)
		kfree(fl->device);
	if (fl->spd)
		atomic_set(&fl->spd->is_attached, 0);

	err = fastrpc_release_current_dsp_process(fl);

	/*
	 * Handle GLINK timeout during PD kill.
	 * If SSR is active (shutdown started), wait for remote subsystem
	 * to stop. Otherwise, trigger BUG_ON to prevent PD mapping
	 * removal and avoid SMMU fault.
	 */
	if (err == -ETIMEDOUT) {
		if (!cctx->startshutdown) {
			pr_err("%s failed with err %d for process %s (tgid %d, tgid_frpc %d)\n",
				__func__, err, current->comm, fl->tgid_app, fl->tgid_frpc);
			BUG_ON(1);
		} else if (!atomic_read(&cctx->teardown)) {
			pr_info("%s process %s is waiting, err %d  (tgid %d, tgid_frpc %d)\n",
			__func__, current->comm, err, fl->tgid_app, fl->tgid_frpc);
			wait_for_completion(&cctx->rpmsg_remove_start);
		}
	}

	/*
	 * Stop resource cleanup by waiting if error is -EIO,
	 * channel context teardown is not occurring, and device is about to crash.
	 */
	if (err == -EIO && !atomic_read(&cctx->teardown)
		&& fastrpc_is_device_crashing(cctx)) {
		pr_info("%s process %s is waiting, err %d  (tgid %d, tgid_frpc %d)\n",
			__func__, current->comm, err, fl->tgid_app, fl->tgid_frpc);
		/* Wait for rpmsg removal to start or a device crash */
		wait_for_completion(&cctx->rpmsg_remove_start);
	}

	atomic_set(&fl->state, DSP_EXIT_COMPLETE);
	if (IS_DYNAMIC_PD(fl->pd_type))
		fastrpc_sysfs_notify_pids(cctx->domain);

	spin_lock_irqsave(&cctx->lock, flags);
	locked = true;
	if(fl->is_dma_invoke_pend) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		wait_for_completion(&fl->dma_invoke);
		locked = false;
	}
	if(locked)
		spin_unlock_irqrestore(&cctx->lock, flags);

	spin_lock_irqsave(&cctx->lock, flags);
	list_del(&fl->user);
	spin_unlock_irqrestore(&cctx->lock, flags);

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	atomic_add(1, &fl->proc_state_notif.notif_queue_count);
	wake_up_interruptible(&fl->proc_state_notif.notif_wait_queue);
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);

	if (fl->tgid_frpc != -1)
		ida_free(&cctx->tgid_frpc_ida, fl->tgid_frpc-(cctx->domain_id*FASTRPC_UNIQUE_ID_CONST));

	fl->is_dma_invoke_pend = false;

	/*
	 * Audio remote-heap buffers won't be freed as part of "fastrpc_user" object
	 * cleanup. Instead, they will be freed after SSR dump collection.
	 * Reset "fl" pointer in the buffer objects if it is the object getting
	 * freed here.
	 */
	spin_lock_irqsave(&cctx->lock, flags);
	list_for_each_entry_safe(buf, b, &cctx->gmaps, node) {
		if (buf->fl == fl)
			buf->fl = NULL;
	}
	spin_unlock_irqrestore(&cctx->lock, flags);

	if (fl->qos_request && fl->dev_pm_qos_req) {
		for (i = 0; i < cctx->lowest_capacity_core_count; i++) {
			if (!dev_pm_qos_request_active(&fl->dev_pm_qos_req[i]))
				continue;
			dev_pm_qos_remove_request(&fl->dev_pm_qos_req[i]);
		}
	}
	kfree(fl->dev_pm_qos_req);
	fastrpc_pm_relax(fl);
	if (fl->sctx)
		fastrpc_session_free(cctx, fl->sctx);
	if (fl->secsctx)
		fastrpc_session_free(cctx, fl->secsctx);
	if (fl->extctx)
		fastrpc_session_free(cctx, fl->extctx);
	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	for (i = 0; i < (FASTRPC_DSPSIGNAL_NUM_SIGNALS /FASTRPC_DSPSIGNAL_GROUP_SIZE); i++)
		kfree(fl->signal_groups[i]);
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

#ifdef CONFIG_DEBUG_FS
	debugfs_remove(fl->debugfs_file);
#endif

	if (g_frpc.is_trusted_vm)
		fastrpc_unreserve_dma_heap(fl->tvm_dma_heap);

skip_user_cleanup:
	fastrpc_free_user(fl);

	mutex_destroy(&fl->signal_create_mutex);
	mutex_destroy(&fl->map_mutex);
	mutex_destroy(&fl->pm_qos_mutex);
	kfree(fl);

	if (user) {
		spin_lock_irqsave(&cctx->lock, flags);
		fastrpc_channel_update_invoke_cnt(cctx, false);
		spin_unlock_irqrestore(&cctx->lock, flags);

		fastrpc_channel_ctx_put(cctx);
	} else {
		/*
		 * Default user-object will be deleted only when the last
		 * reference to the channel-ctx object is removed. So there is no
		 * need to update invoke count to synchronize with ssr callback.
		 */
		cctx->kcomm_user.obj = NULL;
	}
	return 0;
}

/*
 * File kref destructor function that will be invoked when the file
 *  kref is zero
 *
 * This functions cleans up the user-object of the app.
 */
static void fastrpc_user_release(struct kref *ref)
{
	struct fastrpc_user *fl = container_of(ref, struct fastrpc_user, refcount);

	fastrpc_user_obj_free(fl, NULL);
}

/*
 * Callback function that will be invoked when user-app closes the
 * fastrpc device node.
 *
 * This function decrements the user-object refcount.
 */
static int fastrpc_device_release(struct inode *inode, struct file *file)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;

	fastrpc_file_put(fl, false);
	file->private_data = NULL;
	return 0;
}

/* Remove default user object when the channel context is freed */
int fastrpc_channel_default_user_delete(struct fastrpc_channel_ctx *cctx)
{
	return fastrpc_user_obj_free(NULL, cctx);
}

/*
 * Create fastrpc user object for a client
 *
 * @arg1 : file (NULL for default user)
 * @arg2 : channel context (NULL for user-apps, non-NULL for default user)
 *
 * This function creates a fastrpc user object:
 *		- 	for a user-app when it opens the fastrpc device node
 *		- 	when a remote channel comes up and its default user needs to
 *			be set up
 *
 * The user object for an application will be added to the channel's
 * user-list.
 *
 * Returns 0 if user object is successfully created
 */
static int fastrpc_user_obj_create(struct file *filp,
	struct fastrpc_channel_ctx *cctx) {
	struct fastrpc_device_node *fdevice;
	struct fastrpc_user *fl = NULL;
	unsigned long flags;
	struct fastrpc_tvm_dma_heap *tvm_dma_heap = NULL;
	int err;

	if (filp) {
		fdevice = miscdev_to_fdevice(filp->private_data);
		cctx = fdevice->cctx;
	}

	if (atomic_read(&cctx->teardown))
		return -EPIPE;

	fl = kzalloc(sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return -ENOMEM;

	if (filp) {
		/*
		 * Increment refcount of channel object for new user.
		 * Released in fastrpc_device_release().
		 */
		fastrpc_channel_ctx_get(cctx);

		filp->private_data = fl;
	}

	spin_lock_init(&fl->lock);
	mutex_init(&fl->map_mutex);
	spin_lock_init(&fl->dspsignals_lock);
	mutex_init(&fl->signal_create_mutex);
	mutex_init(&fl->pm_qos_mutex);
	INIT_LIST_HEAD(&fl->pending);
	INIT_LIST_HEAD(&fl->interrupted);
	INIT_LIST_HEAD(&fl->maps);
	INIT_LIST_HEAD(&fl->mmaps);
	INIT_LIST_HEAD(&fl->user);
	INIT_LIST_HEAD(&fl->active_user_ssr);
	INIT_LIST_HEAD(&fl->sched_works);
	INIT_LIST_HEAD(&fl->cached_bufs);
	INIT_LIST_HEAD(&fl->notif_queue);
	INIT_LIST_HEAD(&fl->fastrpc_drivers);
	INIT_LIST_HEAD(&fl->mdctxs);
	init_waitqueue_head(&fl->proc_state_notif.notif_wait_queue);
	spin_lock_init(&fl->proc_state_notif.nqlock);
	init_completion(&fl->dma_invoke);
	kref_init(&fl->refcount);
	INIT_WORK(&fl->put_work, fastrpc_file_put_worker);

	fl->cctx = cctx;
	fl->config.user_fd = -1;
	fl->pd_type = DEFAULT_UNUSED;
	fl->dsp_recovery = false;
	fl->logger_exit = false;
	fl->is_faulted = false;
	fl->max_threads = 0;

	if (filp) {
		fl->tgid = fl->tgid_app = current->tgid;
		fl->tgid_frpc = get_unique_hlos_process_id(cctx);
		char *pname = get_process_basename();
		if (pname) {
			snprintf(fl->name, sizeof(fl->name), "%s-%d-%s",
					current->comm, fl->tgid_app, pname);
			kfree(pname);
		} else {
			snprintf(fl->name, sizeof(fl->name), "%s-%d", current->comm, fl->tgid_app);
		}

		if (fl->tgid_frpc == -1) {
			dev_err(cctx->dev, "too many fastrpc clients, max %u allowed\n",
									MAX_FRPC_TGID);
			err = -EUSERS;
			goto error;
		}
		dev_dbg(cctx->dev, "HLOS pid %d, domain %d is mapped to unique sessions pid %d",
				fl->tgid, fl->cctx->domain_id, fl->tgid_frpc);
		fl->is_secure_dev = fdevice->secure;

		if (cctx->lowest_capacity_core_count) {
			fl->dev_pm_qos_req = kzalloc((cctx->lowest_capacity_core_count) *
					sizeof(struct dev_pm_qos_request), GFP_KERNEL);
			if (!fl->dev_pm_qos_req) {
				err = -ENOMEM;
				goto error;
			}
		}

		if (g_frpc.is_trusted_vm) {
			err = fastrpc_reserve_dma_heap(&tvm_dma_heap);
			if (err) {
				dev_err(cctx->dev, "fastrpc_reserve_dma_heap failed with error %d\n",
					err);
				goto error;
			}
		}

		fl->tvm_dma_heap = tvm_dma_heap;
		spin_lock_irqsave(&cctx->lock, flags);
		list_add_tail(&fl->user, &cctx->users);
		spin_unlock_irqrestore(&cctx->lock, flags);

	} else {
		/* No pid will be associated with the default user-object */
		fl->tgid = fl->tgid_app = -1;
		fl->tgid_frpc = -1;
		snprintf(fl->name, sizeof(fl->name), "%s", "default_user");

		/*
		 * RPC calls made with the channel's default user-object will
		 * always use the context bank reserved for rootpd.
		 */
		err = fastrpc_get_root_session(cctx, &fl->sctx);
		if (err)
			goto error;

		cctx->kcomm_user.obj = fl;
	}

	return 0;
error:
	mutex_destroy(&fl->map_mutex);
	mutex_destroy(&fl->signal_create_mutex);
	kfree(fl);
	if (filp)
		fastrpc_channel_ctx_put(cctx);

	return err;
}

/*
 * Callback function that will be invoked when user-object opens the
 * fastrpc device node
 *
 * This function creates the user-object for the app.
 */
static int fastrpc_device_open(struct inode *inode, struct file *filp)
{
	return fastrpc_user_obj_create(filp, NULL);
}

/*
 * Create the default user object for a remote channel
 *
 * The default user object will be created for a remote channel when it
 * comes up. It is required for kernel-to-rootpd rpc communication.
 *
 * Returns 0 if user object was created successfully
 */
int fastrpc_channel_default_user_create(struct fastrpc_channel_ctx *cctx)
{
	return fastrpc_user_obj_create(NULL, cctx);
}

/*
 * fastrpc_send_kernel_dispatch() - Send kernel_dispatch cmd to DSP.
 * Returns 0 or -errno.
 */
static int fastrpc_send_kernel_dispatch(
	struct fastrpc_user *fl, u32 cmd_id,
	const void *payload_in, size_t payload_in_size,
	void *payload_out, size_t payload_out_size)
{
	struct fastrpc_invoke_args args[3] = { 0 };
	struct fastrpc_enhanced_invoke ioctl = { 0 };
	struct fastrpc_kcmd_req {
		int pgid;
		u32 cmd_id;
		u32 payload_in_len;
		u32 payload_out_len;
	} inargs = { 0 };
	int err;

	inargs.pgid = fl->tgid_frpc;
	inargs.cmd_id = cmd_id;
	inargs.payload_in_len = (u32)payload_in_size;
	inargs.payload_out_len = (u32)payload_out_size;

	args[0].ptr = (u64)(uintptr_t)&inargs;
	args[0].length = sizeof(inargs);
	args[0].fd = -1;

	args[1].ptr = (u64)(uintptr_t)payload_in;
	args[1].length = payload_in_size;
	args[1].fd = -1;

	args[2].ptr = (u64)(uintptr_t)payload_out;
	args[2].length = payload_out_size;
	args[2].fd = -1;

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(
		FASTRPC_RMID_INIT_KERNEL_DISPATCH, 2, 1);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID,
				&ioctl);
	if (err)
		dev_err(fl->cctx->dev, "%s: kernel dispatch failed, cmd %u err %d\n",
			__func__, cmd_id, err);
	return err;
}


int fastrpc_send_sys_unsigned_prio_config(struct fastrpc_channel_ctx *cctx)
{
	struct fastrpc_user *fl = cctx->kcomm_user.obj;
	struct fastrpc_kcmd_prio_group_config req = {
		.sys_unsigned_tg_enable = cctx->sys_unsigned_tg_enable ? 1 : 0,
	};
	int err;

	if(!fl)
		return 0;

	err = fastrpc_send_kernel_dispatch(fl, FASTRPC_PRIO_GROUP_CONFIG,
					   &req, sizeof(req), NULL, 0);
	return err;
}


static int fastrpc_dmabuf_alloc(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_alloc_dma_buf bp;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct fastrpc_buf *buf = NULL;
	int err;

	if (copy_from_user(&bp, argp, sizeof(bp)))
		return -EFAULT;
	if (!bp.size)
		return -EFAULT;
	if (!fl->sctx)
		return -EINVAL;

	err = fastrpc_smmu_buf_alloc(fl, bp.size, USER_BUF, &buf);
	if (err)
		return err;
	exp_info.ops = &fastrpc_dma_buf_ops;
	exp_info.size = bp.size;
	exp_info.flags = O_RDWR;
	exp_info.priv = buf;
	buf->dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(buf->dmabuf)) {
		err = PTR_ERR(buf->dmabuf);
		fastrpc_buf_free(buf, false);
		return err;
	}

	bp.fd = dma_buf_fd(buf->dmabuf, O_ACCMODE);
	if (bp.fd < 0) {
		dma_buf_put(buf->dmabuf);
		return -EINVAL;
	}

	if (copy_to_user(argp, &bp, sizeof(bp))) {
		/*
		 * The usercopy failed, but we can't do much about it, as
		 * dma_buf_fd() already called fd_install() and made the
		 * file descriptor accessible for the current process. It
		 * might already be closed and dmabuf no longer valid when
		 * we reach this point. Therefore "leak" the fd and rely on
		 * the process exit path to do any required cleanup.
		 */
		return -EFAULT;
	}

	return 0;
}

static int fastrpc_send_cpuinfo_to_dsp(struct fastrpc_user *fl)
{
	int err = 0;
	u64 cpuinfo = 0;
	struct fastrpc_invoke_args args[1];
	struct fastrpc_enhanced_invoke ioctl;

	if (!fl) {
		return -EBADF;
	}

	cpuinfo = fl->cctx->cpuinfo_todsp;
	/* return success if already updated to remote processor */
	if (fl->cctx->cpuinfo_status)
		return 0;

	args[0].ptr = (u64)(uintptr_t)&cpuinfo;
	args[0].length = sizeof(cpuinfo);
	args[0].fd = -1;

	ioctl.inv.handle = FASTRPC_DSP_UTILITIES_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(1, 1, 0);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
	if (!err)
		fl->cctx->cpuinfo_status = true;

	return err;
}


/**
 * fastrpc_init_attach_common() - Common initialization for attach
 *                                 operations
 *
 * @fl: Pointer to fastrpc user context
 * @pd: Process domain type
 *
 * This function performs common initialization steps for attaching to
 * a DSP process domain. It validates security permissions, allocates
 * a session context, and updates the process domain type if needed.
 *
 * Return: 0 on success, -EACCES if security check fails, -EBUSY if
 *         no session is available
 */
static int fastrpc_init_attach_common(struct fastrpc_user *fl, int pd)
{
	struct fastrpc_pool_ctx *sctx = NULL;

	if (!fl->is_secure_dev) {
		dev_err(fl->cctx->dev,
			"Error: %s: untrusted app trying to attach to privileged DSP PD\n (pid %d)",
			__func__, fl->tgid_app);
		return -EACCES;
	}
	sctx = fastrpc_session_alloc(fl, false, fl->pd_type);
	if (!sctx) {
		dev_err(fl->cctx->dev,
			"Error: %s: No session available for pid %d, pd type %d\n",
			__func__, fl->tgid_app, fl->pd_type);
		return -EBUSY;
	}
	fl->sctx = sctx;

	/*
	 * Default value at fastrpc_device_open is set as DEFAULT_UNUSED.
	 * If pd_type is not configured by the process in
	 * fastrpc_set_session_info, update the pd_type, so that messages
	 * are directed to right process, when fastrpc_getpd_msgidx is
	 * queried. Do this only after session allocation.
	 */
	if (fl->pd_type == DEFAULT_UNUSED)
		fl->pd_type = pd;
	return 0;
}

static int fastrpc_init_attach(struct fastrpc_user *fl, int pd)
{
	struct fastrpc_invoke_args args[1];
	struct fastrpc_enhanced_invoke ioctl;
	int err, tgid = fl->tgid_frpc;

	err = fastrpc_init_attach_common(fl, pd);
	if (err)
		return err;

	if (pd == SENSORS_STATICPD) {
		if (fl->cctx->domain->type == FASTRPC_LPASS)
			fl->servloc_name = SENSORS_PDR_ADSP_SERVICE_LOCATION_CLIENT_NAME;
		else if (fl->cctx->domain->type == FASTRPC_SDSP)
			fl->servloc_name = SENSORS_PDR_SLPI_SERVICE_LOCATION_CLIENT_NAME;

		fl->spd_id = SENSORS_STATIC_ID;

		err = fastrpc_init_sensor_static_pd_status(fl);
		if (err)
			return err;
	}

	args[0].ptr = (u64)(uintptr_t) &tgid;
	args[0].length = sizeof(tgid);
	args[0].fd = -1;

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_ATTACH, 1, 0);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
	if (err)
		return err;

#ifdef CONFIG_DEBUG_FS
	if (fl != NULL)
		fastrpc_create_session_debugfs(fl);
#endif
	return 0;
}

/**
 * fastrpc_init_attach2() - Attach to a DSP process domain with shell file
 *
 * @fl: Pointer to fastrpc user context
 * @pd: Process domain type
 * @argp: User space pointer to init_attach2 arguments
 *
 * This function attaches a user process to a DSP process domain and
 * optionally loads a shell file. It performs common initialization,
 * validates input parameters, and sends the attach request to the DSP.
 *
 * Return: 0 on success, negative error code on failure
 */
static int fastrpc_init_attach2(struct fastrpc_user *fl, int pd,
				char __user *argp)
{
	struct fastrpc_invoke_args args[FASTRPC_INIT_ATTACH2_NARGS] = {0};
	struct fastrpc_enhanced_invoke ioctl = {0};
	struct fastrpc_ioctl_init_attach2 attach = {0};
	struct fastrpc_phy_page pages[ATTACH2_NUM_PAGES_WITH_PRELOAD_BUF] = {0};
	int err = 0;
	void *file = NULL;

	struct {
		int pgid;
		u32 filelen;
		u32 pageslen;
	} inbuf;

	err = fastrpc_init_attach_common(fl, pd);
	if (err)
		return err;

	if (copy_from_user(&attach, argp, sizeof(attach)))
		return -EFAULT;

	if (attach.filelen > INIT_FILELEN_MAX)
		return -EINVAL;

	/* Validate that reserved fields are set to zero */
	for (int i = 0; i < ARRAY_SIZE(attach.reserved); i++) {
		if (attach.reserved[i])
			return -EINVAL;
	}
	/* Get shell file to be passed to DSP */
	file = fastrpc_get_and_copy_shell_file(fl, &attach.file,
						&attach.filelen,
						attach.filefd);
	if (IS_ERR(file)) {
		err = PTR_ERR(file);
		file = NULL;
		goto err_out;
	}

	/* Allocate memory for preloading and pack it as page for sharing */
	err = fastrpc_preload_mem_alloc(fl->cctx, pages, &inbuf.pageslen,
					ATTACH2_NUM_PAGES_WITH_PRELOAD_BUF);
	if(err) {
		dev_err(fl->cctx->dev, "Error 0x%x: %s: Failed to allocate preload buffer\n",
				err, __func__);
		goto err_out;
	}

	/*
	 * As part of attach2, pack tgid, shell file
	 * and memory to share with DSP.
	 */
	inbuf.pgid = fl->tgid_frpc;
	inbuf.filelen = attach.filelen;

	args[0].ptr = (u64)(uintptr_t)&inbuf;
	args[0].length = sizeof(inbuf);
	args[0].fd = -1;

	args[1].ptr = file ? (u64)(uintptr_t)file : attach.file;
	args[1].length = inbuf.filelen;
	args[1].fd = attach.filefd;

	args[2].ptr = (u64)(uintptr_t) pages;
	args[2].length = inbuf.pageslen * sizeof(*pages);
	args[2].fd = -1;

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_ATTACH2,
					3, 0);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID,
				      &ioctl);
	if (err)
		goto err_out;

#ifdef CONFIG_DEBUG_FS
	if (fl != NULL)
		fastrpc_create_session_debugfs(fl);
#endif

err_out:
	kfree(file);
	return err;
}

static int fastrpc_invoke(struct fastrpc_user *fl, char __user *argp)
{
	/* Legacy invoke has no appid field; -1 sentinel bypasses appid>=0 enforcement */
	struct fastrpc_enhanced_invoke ioctl = { .appid = -1 };
	struct fastrpc_invoke inv;
	int err;

	if (copy_from_user(&inv, argp, sizeof(inv)))
		return -EFAULT;

	ioctl.inv = inv;

	err = fastrpc_internal_invoke(fl, USER_MSG, &ioctl);

	return err;
}

void fastrpc_queue_pd_status(struct fastrpc_user *fl, int domain, int status, int sessionid)
{
	struct fastrpc_notif_rsp *notif_rsp = NULL;
	unsigned long flags;

	notif_rsp = kzalloc(sizeof(*notif_rsp), GFP_ATOMIC);
	if (!notif_rsp) {
		dev_err(fl->cctx->dev, "Allocation failed for notif\n");
		return;
	}

	notif_rsp->status = status;
	notif_rsp->domain = domain;
	notif_rsp->session = sessionid;

	if (status == FASTRPC_USERPD_EXCEPTION) {
		fl->is_faulted = true;
	}

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	list_add_tail(&notif_rsp->notifn, &fl->notif_queue);
	atomic_add(1, &fl->proc_state_notif.notif_queue_count);
	wake_up_interruptible(&fl->proc_state_notif.notif_wait_queue);
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);
}

static void fastrpc_notif_find_process(int domain, struct fastrpc_channel_ctx *cctx, struct dsp_notif_rsp *notif)
{
	bool is_process_found = false;
	unsigned long irq_flags = 0;
	struct fastrpc_user *user;
	int err;

	spin_lock_irqsave(&cctx->lock, irq_flags);
	list_for_each_entry(user, &cctx->users, user) {
		if (user->tgid_frpc == notif->pid) {
			err = fastrpc_file_get(user);
			if (err) {
				dev_warn(cctx->dev, "Warning: %s: user-obj for fl (%pK) being released\n",
					__func__, user);
				break;
			}
			is_process_found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);

	if (!is_process_found)
		return;

	fastrpc_queue_pd_status(user, domain, notif->status, user->sessionid);
	fastrpc_file_put(user, true);
}


/*
 * fastrpc_handle_dsp_root_request() - Handle reverse RPC request from root PD
 * of DSP.
 * @arg1: work struct.
 *
 * This function is invoked by a kernel worker thread to service a reverse RPC
 * request made by root PD on DSP.
 *
 * The proxy object is used to make forward RPC calls to the DSP with requested
 * data or error response.
 *
 * In case of failure to create the proxy object, the reverse RPC request
 * times out on the DSP.
 */

static void fastrpc_handle_dsp_root_request(struct work_struct *work)
{
	const unsigned int ROOT_RESPONSE_ARG_LENGTH = 2, ROOT_MEM_MSG_SIZE = 4,
                       ROOT_ERROR_MSG_SIZE = 1;
	struct fastrpc_enhanced_invoke ioctl = {0};
	struct fastrpc_phy_page2 page = {0};
	struct fastrpc_buf *pbuf = NULL;
	struct kcomm_worker *kcomm_work =
			container_of(work, struct kcomm_worker, work);
	struct fastrpc_channel_ctx *cctx = kcomm_work->domain->cctx;
	struct fastrpc_root_msg rm = {0};
	struct fastrpc_smmu *smmucb = NULL;
	struct fastrpc_invoke_args args[2] = {0};
	struct fastrpc_user *default_user = cctx->kcomm_user.obj;
	u32 *served_msg_index= &cctx->kcomm_user.served_msg_index;
	int err = 0;
	unsigned long flags = 0;
	struct {
		int dsp_tid;
		int msg_type;
		int data_len;
		int data_struct_len;
	} inargs = {0};

	fastrpc_channel_ctx_get(cctx);
	spin_lock_irqsave(&cctx->lock, flags);
	if (atomic_read(&cctx->teardown)) {
		/* If subsystem already going thru SSR, then fail root req immediately */
		spin_unlock_irqrestore(&cctx->lock, flags);
		goto cleanup;
	}
	/*
	 * Update invoke count to block SSR handling thread from cleaning up
	 * the channel resources, while it is still being used by this thread.
	 */
	fastrpc_channel_update_invoke_cnt(cctx, true);

	smmucb = &default_user->sctx->smmucb[DEFAULT_SMMU_IDX];

	/*
	 * Copy over entire struct in case entry
	 * in circular buffer is overwritten
	 */
	rm = cctx->kcomm_user.root_msg[*served_msg_index];
	(*served_msg_index)++;
	if (*served_msg_index > ROOT_REQUEST_BUFFER_SIZE - 1)
		*served_msg_index = 0;
	spin_unlock_irqrestore(&cctx->lock, flags);

	if (rm.error) {
		goto error_msg;
	}

	switch (rm.type) {
		case ROOT_MEM_REQ_POOL:
		case ROOT_MEM_REQ_HEAP:
		{
			uint32_t alloc_size = *(uint32_t *)rm.data;

			/* The allocation size requested must be greater than 0 */
			if (alloc_size == 0) {
				rm.error = -EINVAL;
				goto error_msg;
			}
			rm.error = fastrpc_buf_alloc(default_user,
					smmucb, alloc_size, ROOT_MEM_BUF, &pbuf);
			if (rm.error) {
				goto error_msg;
			}

			/*
			 * Buffer will be freed only in case of an SSR
			 * when proxy object is deleted.
			 */
			spin_lock(&default_user->lock);
			list_add_tail(&pbuf->node, &default_user->mmaps);
			spin_unlock(&default_user->lock);

			if (pbuf) {
				if (rm.type == ROOT_MEM_REQ_POOL)
					page.flags = DSP_MMAP_ADD_ROOT_POOL_MEM;
				else
					page.flags = DSP_MMAP_ADD_ROOT_HEAP_MEM;

				page.addr = pbuf->phys;
				page.size = pbuf->size;
			}
			inargs.dsp_tid = rm.dsp_tid;
			inargs.msg_type = rm.type;
			inargs.data_len = ROOT_MEM_MSG_SIZE;
			inargs.data_struct_len = 1 * ROOT_MEM_MSG_SIZE;

			args[0].ptr = (u64)(uintptr_t)&inargs;
			args[0].length = sizeof(inargs);
			args[0].fd = -1;
			args[1].ptr = (u64)(uintptr_t)&page;
			args[1].length = 1 * sizeof(page);
			args[1].fd = -1;

			ioctl.inv.handle = FASTRPC_INIT_HANDLE;
			ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_KCOMM_REMOTE_CALL,
						ROOT_RESPONSE_ARG_LENGTH , 0);
			ioctl.inv.args = (__u64)args;
			break;
		}
		case ROOT_ERROR_MSG: {
error_msg:
			inargs.dsp_tid = rm.dsp_tid;
			inargs.msg_type = ROOT_ERROR_MSG;
			inargs.data_len = ROOT_ERROR_MSG_SIZE;
			inargs.data_struct_len = ROOT_ERROR_MSG_SIZE;

			args[0].ptr = (u64)(uintptr_t)&inargs;
			args[0].length = sizeof(inargs);
			args[0].fd = -1;
			args[1].ptr = (u64)(uintptr_t)&rm.error;
			args[1].length = 1 * sizeof(rm.error);
			args[1].fd = -1;
			ioctl.inv.handle = FASTRPC_INIT_HANDLE;
			ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_KCOMM_REMOTE_CALL,
						ROOT_RESPONSE_ARG_LENGTH , 0);
			ioctl.inv.args = (__u64)args;
			dev_err(smmucb->dev,
			"%s: root request failed: dsp-tid 0x%x req type %d, error 0x%llX\n",
			__func__, rm.dsp_tid, rm.type, rm.error);
			break;
		}
		default: {
			dev_err(smmucb->dev,
				"%s: root request type %d is not supported\n",
				__func__, rm.type);
			fastrpc_channel_update_invoke_cnt(cctx, false);
			goto cleanup;
		}
	}
	err = fastrpc_internal_invoke(default_user,
                                      KERNEL_MSG_WITH_ZERO_PID_ZERO_TID, &ioctl);
	if (err) {
		dev_err(smmucb->dev,
		"Error %d: %s: remote call to root failed for dsp tid 0x%x, req type %d\n",
		err, __func__, rm.dsp_tid, rm.type);
		if (pbuf) {
			struct fastrpc_buf *buf = NULL, *b = NULL;

			spin_lock(&default_user->lock);
			list_for_each_entry_safe(buf, b, &default_user->mmaps, node) {
				if (buf->phys == pbuf->phys) {
					list_del(&buf->node);
					break;
				}
			}
			spin_unlock(&default_user->lock);
			fastrpc_buf_free(pbuf, false);
		}
	}
	fastrpc_channel_update_invoke_cnt(cctx, false);
cleanup:
	fastrpc_channel_ctx_put(cctx);
	kfree(kcomm_work);
	return;
}

/*
 * fastrpc_queue_root_msg() - queue root request made by remote subsystem to
 * worker thread.
 * @arg1: channel context.
 * @arg2: root request made by remote subsystem
 * @arg3: error value in case of failure occurrence before queuing the message
 *
 * The function queues root message received from remote subsystem to a
 * worker thread to further carry out message handling.
 */

static void fastrpc_queue_root_msg(struct fastrpc_channel_ctx *cctx,
					struct root_request_msg *msg_req, int nerr)
{
	unsigned long flags = 0;
	struct kcomm_worker *kcomm_work = NULL;
	struct fastrpc_root_msg *root_msg =
		&cctx->kcomm_user.root_msg[cctx->kcomm_user.queued_msg_index];
	u32 *queued_msg_index = &cctx->kcomm_user.queued_msg_index;

	if (msg_req->data_len > sizeof(u32) * ROOT_REQUEST_MAX_SIZE)
		nerr = -EINVAL;

	/* Add root request to circular buffer */
	spin_lock_irqsave(&cctx->lock, flags);
	root_msg->dsp_tid = msg_req->dsp_tid;

	/* Send the same msg type in response to dsp */
	if (nerr == 0)
		root_msg->type = msg_req->msg_type;
	else
		root_msg->type = ROOT_ERROR_MSG;

	root_msg->data = &msg_req->data[0];
	root_msg->data_len = msg_req->data_len;
	root_msg->error = nerr;
	(*queued_msg_index)++;
	if (*queued_msg_index > ROOT_REQUEST_BUFFER_SIZE - 1)
		*queued_msg_index = 0;
	spin_unlock_irqrestore(&cctx->lock, flags);

	/*
	 * On DSP side the request has a configured timeout value.
	 * If work allocation fails then HLOS will discard the call
	 * and on the DSP side the request will timeout.
	 */
	kcomm_work = (struct kcomm_worker *)kzalloc(sizeof(*kcomm_work),
			GFP_ATOMIC);
	if (!kcomm_work)
		return;

	kcomm_work->domain = cctx->domain;

	/* Post handling of root-request to kernel worker thread */
	INIT_WORK(&kcomm_work->work, fastrpc_handle_dsp_root_request);
	schedule_work(&kcomm_work->work);
}

static int fastrpc_wait_on_notif_queue(
			struct fastrpc_internal_notif_rsp *notif_rsp,
			struct fastrpc_user *fl)
{
	int err = 0;
	unsigned long flags;
	struct fastrpc_notif_rsp *notif = NULL, *inotif, *n;

read_notif_status:
	err = wait_event_interruptible(fl->proc_state_notif.notif_wait_queue,
				atomic_read(&fl->proc_state_notif.notif_queue_count));
	if (err)
		return err;

	spin_lock_irqsave(&fl->proc_state_notif.nqlock, flags);
	list_for_each_entry_safe(inotif, n, &fl->notif_queue, notifn) {
		list_del(&inotif->notifn);
		atomic_sub(1, &fl->proc_state_notif.notif_queue_count);
		notif = inotif;
		break;
	}
	spin_unlock_irqrestore(&fl->proc_state_notif.nqlock, flags);

	if (notif) {
		notif_rsp->status = notif->status;
		notif_rsp->domain = notif->domain;
		notif_rsp->session = notif->session;
	} else {// Go back to wait if ctx is invalid
		dev_err(fl->cctx->dev, "Invalid status notification response\n");
		goto read_notif_status;
	}

	kfree(notif);
	return err;
}

/*
 * Retrieves the remote process status notification response from dsp.
 *
 * @param notif: Pointer to the fastrpc_internal_notif_rsp structure
 *         containing the notification response.
 * @param param: User addr to which notification response needs to be copied.
 * @param fl:     Pointer to the fastrpc_user object
 * @param legacy_domains: Flag indicating whether to return legacy
 *                        domains or logical domain ids.
 *
 * Returns 0 on Success.
 */
static int fastrpc_get_notif_response(
			struct fastrpc_internal_notif_rsp *notif,
			void *param, struct fastrpc_user *fl, bool legacy_domains)
{
	int err = 0;
	struct fastrpc_domain *domain = NULL;

	err = fastrpc_wait_on_notif_queue(notif, fl);
	if (err)
		return err;

	/*
	 * If user is using legacy domain ids, send the legacy id back to
	 * client in process status notification.
	 */
	if (legacy_domains) {
		domain = fastrpc_lookup_domain_in_table(notif->domain, false);
		if (domain && domain->legacy)
			notif->domain = domain->legacy_id;
	}

	if (copy_to_user((void __user *)param, notif,
			sizeof(struct fastrpc_internal_notif_rsp)))
		return -EFAULT;

	return 0;
}

/*
 * fastrpc_user_set_rpc_timeout()
 * Set user specified RPC timeout
 */
static int fastrpc_user_set_rpc_timeout(struct fastrpc_user *fl,
	struct fastrpc_internal_proc_timeout *rpc)
{
	int err = 0, ii = 0;
	uint32_t rsvd = 0;

	/* Validate that reserved fields are all zero */
	for (ii = 0; ii < FASTRPC_RPC_TIMEOUT_IOCTL_RSVD; ii++) {
		rsvd = rpc->reserved[ii];
		if (rsvd) {
			err = -EINVAL;
			dev_err(fl->cctx->dev, "Error %d: %s: rsvd[%d] %u expected to be 0",
				err, __func__, ii, rsvd);
			return err;
		}
	}
	spin_lock(&fl->lock);
	fl->timeout = rpc->timeout;
	spin_unlock(&fl->lock);
	return 0;
}

static int fastrpc_set_dsp_recovery_mode (struct fastrpc_user *fl,
	int recovery)
{
	spin_lock(&fl->lock);
	fl->dsp_recovery = recovery ? true : false;
	spin_unlock(&fl->lock);
	return 0;
}

/**
 * fastrpc_reserved_field_check - Validates that all bytes in a reserved array are zero.
 * @reserved_arr: Pointer to the memory block.
 * @len: Number of elements in the array.
 *
 * Returns true if all bytes are zero, false otherwise.
 */
static bool fastrpc_reserved_field_check(void *reserved_arr, size_t len)
{
	return !memchr_inv(reserved_arr, 0, len);
}

static int fastrpc_set_timeline_info(struct fastrpc_user *fl,
	struct fastrpc_timeline_arguments *info)
{
	struct fastrpc_timeline_arguments *t_args = NULL;

	if (info->num_events > MAX_TIMELINE_EVENT_COUNT ||
		info->num_events < TIMELINE_BUF_COUNT) {
		pr_err("%s: failed for invalid num event %u\n",
			__func__, info->num_events);
		return -EINVAL;
	}
	if (!fastrpc_reserved_field_check(info->reserved,
			sizeof(info->reserved))) {
		pr_err("%s: reserved fields are expected to be 0\n",
			__func__);
		return -EINVAL;
	}
	if (!READ_ONCE(fl->timeline_init_args)) {
		t_args = kzalloc(sizeof(struct fastrpc_timeline_arguments),
					GFP_KERNEL);
		if (!t_args)
			return -ENOMEM;
	}
	spin_lock(&fl->lock);
	if (!fl->timeline_init_args) {
		fl->timeline_init_args = t_args;
		fl->timeline_init_args->num_events = info->num_events;
		fl->timeline_init_args->version = MIN(info->version,
			TIMELINE_VERSION);
		t_args = NULL;
	}
	spin_unlock(&fl->lock);
	kfree(t_args);
	return 0;
}

static int fastrpc_get_timeline_version(struct fastrpc_user *fl,
	u32 *version)
{
	if (!fl->fastrpc_timeline_obj ||
		!fl->fastrpc_timeline_obj->initialized)
		return -ENODATA;
	*version = fl->fastrpc_timeline_obj->version;
	return 0;
}

static int fastrpc_manage_poll_mode(struct fastrpc_user *fl, u32 enable, u32 timeout)
{
	const unsigned int MAX_POLL_TIMEOUT_US = 10000;

	if ((fl->cctx->domain->type != FASTRPC_NSP &&
		fl->cctx->domain->type != FASTRPC_LPASS) ||
		(fl->pd_type != USERPD && fl->pd_type != USER_UNSIGNEDPD_POOL)) {
		dev_err(fl->cctx->dev, "poll mode only allowed for dynamic CDSP and ADSP process\n");
		return -EPERM;
	}
	if (timeout > MAX_POLL_TIMEOUT_US) {
		dev_err(fl->cctx->dev,"poll timeout %u is greater than max allowed value %u\n",
			timeout, MAX_POLL_TIMEOUT_US);
		return -EBADMSG;
	}
	spin_lock(&fl->lock);
	if (enable) {
		fl->poll_mode = true;
		fl->poll_timeout = timeout;
	} else {
		fl->poll_mode = false;
		fl->poll_timeout = 0;
	}
	spin_unlock(&fl->lock);
	dev_info(fl->cctx->dev,"updated poll mode to %d, timeout %u\n", enable, timeout);
	return 0;
}

static int fastrpc_internal_control(struct fastrpc_user *fl,
					struct fastrpc_internal_control *cp)
{
	int err = 0;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	u32 latency = 0, cpu = 0;

	if (!cp) {
		return -EINVAL;
	}

	switch (cp->req) {
	case FASTRPC_CONTROL_LATENCY:
		if (cp->lp.enable)
			latency =  cctx->qos_latency;
		else
			latency = PM_QOS_RESUME_LATENCY_DEFAULT_VALUE;
		if (latency == 0)
			return -EINVAL;
		if (!(cctx->lowest_capacity_core_count && fl->dev_pm_qos_req)) {
			dev_err(fl->cctx->dev, "Skipping PM QoS latency voting, core count: %u\n",
						cctx->lowest_capacity_core_count);
			return -EINVAL;
		}
		/*
		 * Add voting request for all possible cores corresponding to cluster
		 * id 0. If DT property 'qcom,single-core-latency-vote' is enabled
		 * then add voting request for only one core of cluster id 0.
		 */
		 mutex_lock(&fl->pm_qos_mutex);
		 for (cpu = 0; cpu < cctx->lowest_capacity_core_count; cpu++) {
			if (!fl->qos_request) {
				err = dev_pm_qos_add_request(
						get_cpu_device(cpu),
						&fl->dev_pm_qos_req[cpu],
						DEV_PM_QOS_RESUME_LATENCY,
						latency);
			} else {
				err = dev_pm_qos_update_request(
						&fl->dev_pm_qos_req[cpu],
						latency);
			}
			if (err < 0) {
				dev_err(fl->cctx->dev, "QoS with lat %u failed for CPU %d, err %d, req %d\n",
					latency, cpu, err, fl->qos_request);
				break;
			}
		}
		if (err >= 0) {
			fl->qos_request = 1;
			err = 0;
		}
		mutex_unlock(&fl->pm_qos_mutex);
		break;
	case FASTRPC_CONTROL_SMMU:
		fl->sharedcb = cp->smmu.sharedcb;
		break;
	case FASTRPC_CONTROL_WAKELOCK:
		if (!fl->is_secure_dev) {
			dev_err(fl->cctx->dev,
				"PM voting not allowed for non-secure device node");
			err = -EPERM;
			return err;
		}
		fl->wake_enable = cp->wp.enable;
		break;
	case FASTRPC_CONTROL_PM:
		if (!fl->wake_enable)
			return -EACCES;
		if (cp->pm.timeout > FASTRPC_MAX_PM_TIMEOUT_MS)
			fl->ws_timeout = FASTRPC_MAX_PM_TIMEOUT_MS;
		else
			fl->ws_timeout = cp->pm.timeout;
		mutex_lock(&cctx->wake_mutex);
		fastrpc_pm_awake(fl);
		mutex_unlock(&cctx->wake_mutex);
		break;
	case FASTRPC_CONTROL_DSPPROCESS_CLEAN:
		err = fastrpc_release_current_dsp_process(fl);
		if (!err)
			fastrpc_queue_pd_status(fl, fl->cctx->domain_id, FASTRPC_USERPD_FORCE_KILL, fl->sessionid);
		break;
	case FASTRPC_CONTROL_RPC_POLL:
		err = fastrpc_manage_poll_mode(fl, cp->lp.enable, cp->lp.latency);
		break;
	default:
		err = -EBADRQC;
		break;
	}
	return err;
}

static int fastrpc_set_session_info(
		struct fastrpc_user *fl, struct fastrpc_internal_sessinfo *sessinfo)
{
	spin_lock(&fl->lock);
	if (fl->set_session_info) {
		spin_unlock(&fl->lock);
		dev_err(fl->cctx->dev,"Set session info invoked multiple times\n");
		return -EBADR;
	}
	fl->set_session_info = true;
	spin_unlock(&fl->lock);

	if(sessinfo->pd <= DEFAULT_UNUSED ||
				sessinfo->pd >= MAX_PD_TYPE) {
		dev_err(fl->cctx->dev,"Invalid PD type %d, range is %d - %d\n",
					sessinfo->pd, DEFAULT_UNUSED + 1, MAX_PD_TYPE - 1);
		return -EBADR;
	}

	/*
	 * If PD type is not configured for context banks,
	 * ignore PD type passed by the user, leave pd_type set to DEFAULT_UNUSED(0)
	 */
	if (fl->cctx->pd_type)
		fl->pd_type = sessinfo->pd;
	// Processes attaching to Sensor Static PD, share context bank.
	if (sessinfo->pd == SENSORS_STATICPD)
		fl->sharedcb = 1;
	if (sessinfo->session_id >= fl->cctx->max_sess_per_proc) {
		dev_err(fl->cctx->dev,
		"Session ID %u cannot be beyond %u\n",
				sessinfo->session_id, fl->cctx->max_sess_per_proc);
		return -EBADR;
	}
	fl->sessionid = sessinfo->session_id;
	// Set multi_session_support, to disable old way of setting session_id
	fl->multi_session_support = true;

	return 0;
}

/*
 * fastrpc_set_session_info_v2() - V2 session info handler.
 * Validates reserved fields and max_threads bound, then delegates
 * to fastrpc_set_session_info() for common session setup.
 * Sets fl->max_threads for donation sizing in fastrpc_init_create_process().
 * fastrpc_internal_sessinfo_v2 is a superset of fastrpc_internal_sessinfo
 * with identical field layout for the first four members, so a direct
 * cast is safe.
 */
static int fastrpc_set_session_info_v2(struct fastrpc_user *fl,
		struct fastrpc_internal_sessinfo_v2 *s)
{
	int ii, err = 0;

	for (ii = 0; ii < ARRAY_SIZE(s->reserved); ii++) {
		if (s->reserved[ii]) {
			dev_err(fl->cctx->dev,
				"Error: %s: reserved[%d]=%u must be 0\n",
				__func__, ii, s->reserved[ii]);
			return -EINVAL;
		}
	}
	if (s->max_threads > FASTRPC_MAX_THREADS_PER_PD) {
		dev_err(fl->cctx->dev,
			"Error: %s: max_threads %u exceeds limit %u\n",
			__func__, s->max_threads, FASTRPC_MAX_THREADS_PER_PD);
		return -EINVAL;
	}
	err = fastrpc_set_session_info(fl, (struct fastrpc_internal_sessinfo *)s);
	if (!err && s->max_threads)
		fl->max_threads = s->max_threads;
	return err;
}

/* Get fastrpc tgid of given session on given domain */
static int fastrpc_get_frpc_tgid(uint32_t domain, uint32_t session,
	int32_t *tgid_frpc)
{
	int err = 0;
	bool found = false;
	unsigned long flags = 0;
	struct fastrpc_channel_ctx *cctx = NULL;
	struct fastrpc_user *user = NULL;

	cctx = fastrpc_get_domain_channel_ctx(domain);
	if (!cctx) {
		/* Channel is going thru ssr */
		err = -EPIPE;
		return err;
	}
	fastrpc_channel_ctx_get(cctx);
	if (atomic_read(&cctx->teardown)) {
		/* If subsystem already going thru SSR, fail immediately */
		err = -EPIPE;
		goto bail;
	}
	spin_lock_irqsave(&cctx->lock, flags);
	fastrpc_channel_update_invoke_cnt(cctx, true);
	/*
	 * Search for user objects of current process on remote channel
	 * corresponding to given domain & find object of given session
	 */
	list_for_each_entry(user, &cctx->users, user) {
		if (user->tgid_app == current->tgid && user->sessionid == session) {
			*tgid_frpc = user->tgid_frpc;
			found = true;
			break;
		}
	}
	/*
	 * If no user-object is found for given remote session in the
	 * current channel context's list, it means the channel has
	 * gone thru SSR and the user-object was present in the previous
	 * channel context's list.
	 * */
	if (!found)
		err = -EPIPE;
	fastrpc_channel_update_invoke_cnt(cctx, false);
	spin_unlock_irqrestore(&cctx->lock, flags);
bail:
	fastrpc_channel_ctx_put(cctx);
	return err;
}

/* Helper function to get frpc tgid of each session of context */
static int fastrpc_multidomain_ctx_get_tgids(struct device *dev,
	struct fastrpc_mdctx_info *mdctx)
{
	int err = 0, ii = 0;
	uint32_t logical_domain_id = 0, domain = 0 , session = 0;
	uint32_t num_domains = mdctx->num_domains;

	for (ii = 0; ii < num_domains; ii++) {
		domain = mdctx->domains[ii];
		session = mdctx->session_ids[ii];

		/* Validate domain id passed by user */
		if (fastrpc_is_valid_logical_domain_id(domain)) {
			logical_domain_id = domain;
		} else {
			if (IS_LEGACY_DOMAIN_ID(domain)) {
				/* If its a valid legacy id, get the corresponding logical id */
				err = fastrpc_convert_legacy_id_to_logical_id(domain, &logical_domain_id);
				if (err != 0) {
					dev_err(dev, "Error %d: %s: [%u of %u]: no domain found for legacy domain id %u",
						err, __func__, ii, num_domains, domain);
					break;
				}
			} else {
				/*
				 * If domain id is neither a valid logical id nor a legacy id,
				 * return error.
				 */
				err = -EINVAL;
				dev_err(dev, "Error %d: %s: [%u of %u]: %u is not a valid logical domain id",
					err, __func__, ii, num_domains, domain);
				break;
			}
		}

		if (!IS_VALID_SESSION_ID(session)) {
			err = -EINVAL;
			dev_err(dev, "Error %d: %s: [%u of %u]: session %u is invalid",
					err, __func__, ii, num_domains, session);
			break;
		}

		err = fastrpc_get_frpc_tgid(logical_domain_id, session,
					&mdctx->tgids_frpc[ii]);
		if (err) {
			dev_err(dev, "Error %d: %s: [%d of %d]: unable to get frpc tgid for domain %u, session %u",
							err, __func__, ii, num_domains, logical_domain_id, session);
			break;
		}
	}
	return err;
}

/* Helper function to initialize multidomain context object */
static int fastrpc_multidomain_ctx_obj_init(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm,
	struct fastrpc_mdctx_info **o_mdctx)
{
	int err = 0, ii = 0;
	uint32_t rsvd = 0, num_domains = ctxm->num_domains,
		max_domains = ((FASTRPC_MAX_DSP_TYPE - 1) *
						FASTRPC_MAX_SESSIONS_PER_PROCESS);
	struct device *dev = fl->cctx->dev;
	size_t size = 0;
	uint32_t *domains = NULL, *session_ids = NULL;
	uint32_t *phy_ids = NULL, *instance_ids = NULL;
	int32_t *tgids_frpc = NULL;
	struct fastrpc_mdctx_info *mdctx = NULL;
	struct fastrpc_domain *domain = NULL;
	uint32_t logical_domain_id = 0;

	/* Validate that reserved fields are all zero */
	for (ii = 0; ii < FASTRPC_MDCTX_IOCTL_RSVD; ii++) {
		rsvd = ctxm->reserved[ii];
		if (rsvd) {
			err = -EINVAL;
			dev_err(dev, "Error %d: %s: rsvd[%d] %u expected to be 0",
				err, __func__, ii, rsvd);
			goto bail;
		}
	}

	/* Validate number of domains passed by user */
	if (num_domains >= max_domains) {
		err = -EINVAL;
		dev_err(dev, "Error %d: %s: num domains %u more than max domains %u",
			err, __func__, num_domains, max_domains);
		goto bail;
	}

	mdctx = kzalloc(sizeof(*mdctx), GFP_KERNEL);
	if (!mdctx) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc mdctx obj",
			err, __func__);
		goto bail;
	}
	size = sizeof(*domains) * num_domains;

	/* Allocate local domains array to send to dsp */
	domains = kzalloc(size, GFP_KERNEL);
	if (!domains) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc domains array of size %zu",
			err, __func__, size);
		goto bail;
	}

	/* Copy list of domains passed by user */
	err = copy_from_user((void *)domains,
			(void __user *)(uintptr_t)ctxm->domain_ids, size);
	if (err) {
		dev_err(dev, "Error %d: %s: failed to copy domain ids from user (size %zu)",
			err, __func__, size);
		err = -EFAULT;
		goto bail;
	}

	/* Allocate local sessions array */
	session_ids = kzalloc(size, GFP_KERNEL);
	if (!session_ids) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc sessions array of size %zu",
			err, __func__, size);
		goto bail;
	}

	/* Copy list of session ids passed by user */
	err = copy_from_user((void *)session_ids,
			(void __user *)(uintptr_t)ctxm->session_ids, size);
	if (err) {
		dev_err(dev, "Error %d: %s: failed to copy session ids from user (size %zu)",
			err, __func__, size);
		err = -EFAULT;
		goto bail;
	}

	instance_ids = kzalloc(size, GFP_KERNEL);
	if (instance_ids == NULL) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc instance_ids array of size %zu",
			err, __func__, size);
		goto bail;
	}

	phy_ids = kzalloc(size, GFP_KERNEL);
	if (phy_ids == NULL) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc phy_ids array of size %zu",
			err, __func__, size);
		goto bail;
	}

	/* Retrieve phy_ids and instance_ids of the domains in the context */
	for(ii = 0 ; ii < num_domains; ii++) {
		if (IS_LEGACY_DOMAIN_ID(domains[ii])) {
			err = fastrpc_convert_legacy_id_to_logical_id(domains[ii],
					&logical_domain_id);
			if (err != 0) {
				dev_err(dev, "Error %d: %s: failed to get logical id for legacy domain %u",
					err, __func__, domains[ii]);
				goto bail;
			}
		} else {
			logical_domain_id = domains[ii];
		}

		domain = fastrpc_lookup_domain_in_table(logical_domain_id, false);
		if (!domain) {
			err = -EINVAL;
			pr_err("Error %d : %s : no domain found for logical id %u\n",
				err, __func__, logical_domain_id);
			goto bail;
		}
		instance_ids[ii] = domain->instance_id;
		phy_ids[ii] = domain->phy_id;
	}

	/* Allocate tgids array to send to dsp */
	size = sizeof(*tgids_frpc) * num_domains;
	tgids_frpc = kzalloc(size, GFP_KERNEL);
	if (!tgids_frpc) {
		err = -ENOMEM;
		dev_err(dev, "Error %d: %s: failed to alloc tgids array of size %zu",
			err, __func__, size);
		goto bail;
	}
	mdctx->num_domains = num_domains;
	mdctx->domains = domains;
	mdctx->session_ids = session_ids;
	mdctx->tgids_frpc = tgids_frpc;
	mdctx->phy_ids = phy_ids;
	mdctx->instance_ids = instance_ids;
	INIT_LIST_HEAD(&mdctx->node);

	err = fastrpc_multidomain_ctx_get_tgids(dev, mdctx);
	if (err)
		goto bail;

	*o_mdctx = mdctx;
bail:
	if (err) {
		kfree(tgids_frpc);
		kfree(session_ids);
		kfree(domains);
		kfree(mdctx);
		kfree(phy_ids);
		kfree(instance_ids);
	}
	return err;
}

/* Helper function to make rpc call to dsp root to reg / dereg ctx */
static int fastrpc_multidomain_ctx_dsp_send(struct fastrpc_channel_ctx *cctx,
	uint32_t req, uint64_t ctx, uint32_t id,
	struct fastrpc_mdctx_info *mdctx, uint32_t num_domains)
{
	int err = 0;
	unsigned long flags = 0;
	struct fastrpc_enhanced_invoke invoke = {0};
	struct fastrpc_invoke_args args[5] = {0};

	struct {
		u32 req;
		u32 pad;
		u64 ctx;
		u32 id;
		u32 num_domains;
		u32 num_phy_ids;
		u32 num_tgids;
		u32 rsvd_len;
	} inargs = {0};

	fastrpc_channel_ctx_get(cctx);
	if (atomic_read(&cctx->teardown)) {
		/* If subsystem already going thru SSR, fail immediately */
		err = -EPIPE;
		goto bail;
	}
	spin_lock_irqsave(&cctx->lock, flags);
	fastrpc_channel_update_invoke_cnt(cctx, true);
	spin_unlock_irqrestore(&cctx->lock, flags);

	/* Prepare args for kernel to rootpd rpc call */
	inargs.req = req;
	inargs.ctx = ctx;
	inargs.id = id;
	inargs.num_tgids = num_domains;
	inargs.num_domains = num_domains;
	inargs.num_phy_ids = num_domains;

	args[0].ptr = (u64)(uintptr_t)&inargs;
	args[0].length = sizeof(inargs);
	args[0].fd = -1;

	/* Peer-info will be shared as list of instance id's */
	args[1].ptr = (u64)(uintptr_t)mdctx->instance_ids;
	args[1].length = num_domains * sizeof(*(mdctx->instance_ids));
	args[1].fd = -1;

	args[2].ptr = (u64)(uintptr_t)mdctx->phy_ids;
	args[2].length = num_domains * sizeof(*(mdctx->phy_ids));
	args[2].fd = -1;

	args[3].ptr = (u64)(uintptr_t)mdctx->tgids_frpc;
	args[3].length = num_domains * sizeof(*(mdctx->tgids_frpc));
	args[3].fd = -1;

	invoke.inv.handle = FASTRPC_INIT_HANDLE;
	invoke.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MDCTX_MANAGE, 5, 0);
	invoke.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(cctx->kcomm_user.obj,
					KERNEL_MSG_WITH_ZERO_PID, &invoke);
	spin_lock_irqsave(&cctx->lock, flags);
	fastrpc_channel_update_invoke_cnt(cctx, false);
	spin_unlock_irqrestore(&cctx->lock, flags);
bail:
	fastrpc_channel_ctx_put(cctx);
	return err;
}

/*
 * Send multidomain context to dsp with peer information
 *
 * Context will be registered with peer-information / deregistered with
 * the resource manager on dsp.
 * Peer-info should be list of dsp instance ids (which is a HW configured
 * parameter). This is different from the dsp domain ids.
 */
static int fastrpc_multidomain_ctx_dsp_manage(struct fastrpc_user *fl,
	uint32_t req, uint64_t ctx, struct fastrpc_mdctx_info *mdctx,
	uint32_t num_domains)
{
	int err = 0, ii = 0;
	uint32_t domain = 0, logical_domain_id = 0;
	struct fastrpc_channel_ctx *cctx = NULL;
	struct device *dev = fl->cctx->dev;

	/* Make rpc call to rootpd */
	for (ii = 0; ii < num_domains; ii++) {
		domain = mdctx->domains[ii];

		/*
		 * If a context has been created on multiple sessions on same dsp,
		 * make register / deregister call only once to that dsp.
		 */
		if (ii > 0 && domain == mdctx->domains[0])
			break;

		if (IS_LEGACY_DOMAIN_ID(domain)) {
			err = fastrpc_convert_legacy_id_to_logical_id(domain, &logical_domain_id);
			if (err != 0) {
				dev_err(dev, "Error %d: %s: failed to get logical id for domain %u",
					err, __func__, domain);
				goto bail;
			}
		} else {
			logical_domain_id = domain;
		}

		cctx = fastrpc_get_domain_channel_ctx(logical_domain_id);
		if (!cctx) {
			if (req == FASTRPC_MDCTX_REMOVE) {
				/*
				 * In case of context deregister request, even if one of
				 * the domains is going thru ssr, the ctx still needs to
				 * be deregistered from the other domains.
				 */
				continue;
			} else {
				/* Channel is going thru ssr */
				err = -EPIPE;
				dev_err(dev,
					"Error %d: %s: remote channel is down for domain[%d of %d] %u",
					err, __func__, ii + 1, num_domains, domain);
				goto bail;
			}
		}
		err = fastrpc_multidomain_ctx_dsp_send(cctx, req, ctx, ii, mdctx,
						num_domains);
		if (err) {
			if (req == FASTRPC_MDCTX_REMOVE) {
				/*
				 * In case of context deregister request, even if it fails
				 * on one of the domains, the ctx still needs to be
				 * deregistered from the other domains.
				 */
				continue;
			} else {
				dev_err(dev,
					"Error 0x%x: %s: failed to register ctx on domain[%d of %d] %u",
					err, __func__, ii + 1, num_domains, domain);
				goto bail;
			}
		}
	}
bail:
	if (err && req == FASTRPC_MDCTX_SETUP) {
		/*
		 * In case of a context registration request failing on one of
		 * the dsps, the context needs to be deregistered from all dsps
		 * where the registration was successful.
		 */
		fastrpc_multidomain_ctx_dsp_manage(fl, FASTRPC_MDCTX_REMOVE,
											ctx, mdctx, ii);
	}
	return err;
}

/*
 * Setup multidomain context in kernel
 *
 * For a multidomain context created in userspace, generate a unique
 * context id in kernel.
 *
 * Also share the list of domains on which context was created to rootpd
 * on dsp.
 */
static int fastrpc_multidomain_ctx_setup(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm)
{
	int err = 0;
	uint64_t ctx = 0;
	struct mutex *gmut = &g_frpc.gmut;
	struct idr *mdctx_idr = &g_frpc.mdctx_idr;
	struct device *dev = fl->cctx->dev;
	struct fastrpc_mdctx_info *mdctx = NULL;

	err = fastrpc_multidomain_ctx_obj_init(fl, ctxm, &mdctx);
	if (err)
		return err;

	/* Generate kernel context id */
	mutex_lock(gmut);
	err = idr_alloc_cyclic(mdctx_idr, mdctx, 1,
				FASTRPC_CTX_MAX, GFP_ATOMIC);

	if (err < 0) {
		dev_err(dev, "Error %d: %s: idr alloc failed", err, __func__);
		goto bail;
	}
	ctx = (uint64_t)err;

	/* Send context with peer info (i.e. domains list) to all dsps */
	err = fastrpc_multidomain_ctx_dsp_manage(fl, ctxm->req, ctx, mdctx,
				mdctx->num_domains);
	if (err) {
		dev_err(dev, "Error 0x%x: %s: peer-info register failed",
			err, __func__);
		goto bail;
	}

	/* Copy context back to user */
	err = copy_to_user((void __user *)ctxm->ctx, &ctx, sizeof(ctx));
	if (err) {
		dev_err(dev, "Error %d: %s: failed to copy ctx 0x%llx to user",
			err, __func__, ctx);
		err = -EFAULT;
		goto bail;
	}
	mdctx->ctx = ctx;
	mdctx->fl = fl;

	/* Add node to user's multidomain context list */
	spin_lock(&fl->lock);
	list_add_tail(&mdctx->node, &fl->mdctxs);
	spin_unlock(&fl->lock);
bail:
	if (err) {
		if (ctx)
			idr_remove(mdctx_idr, ctx);

		kfree(mdctx->tgids_frpc);
		kfree(mdctx->session_ids);
		kfree(mdctx->domains);
		kfree(mdctx->phy_ids);
		kfree(mdctx->instance_ids);
		kfree(mdctx);
	}
	mutex_unlock(gmut);
	return err;
}

/* Clean-up multidomain context resources in kernel and dsp */
static int fastrpc_multidomain_ctx_cleanup(struct fastrpc_user *fl,
	uint32_t req, uint64_t ctx)
{
	int err = 0, ictx = (int)ctx;
	struct device *dev = fl->cctx->dev;
	struct mutex *gmut = &g_frpc.gmut;
	struct idr *mdctx_idr = &g_frpc.mdctx_idr;
	struct fastrpc_mdctx_info *mdctx = NULL;

	/* Release the context - if it was allocated to same client */
	mutex_lock(gmut);
	mdctx = (struct fastrpc_mdctx_info *)idr_find(mdctx_idr, ictx);
	if (!mdctx || mdctx->ctx != ctx || mdctx->fl != fl) {
		/* App potentially trying to remove ctx of another client */
		err = -EACCES;
		dev_err(dev, "Error %d: %s: attempting to remove ctx 0x%x of another app",
			err, __func__, ictx);
		goto bail;
	}

	/* Deregister context from all dsps */
	err = fastrpc_multidomain_ctx_dsp_manage(fl, req, ctx, mdctx,
				mdctx->num_domains);
	if (err)
		dev_err(dev, "Error 0x%x: %s: peer-info deregister failed for ctx 0x%x",
			err, __func__, ictx);

	idr_remove(mdctx_idr, ictx);

	/* Remove node from user's multidomain context list */
	spin_lock(&fl->lock);
	list_del(&mdctx->node);
	spin_unlock(&fl->lock);

	kfree(mdctx->tgids_frpc);
	kfree(mdctx->session_ids);
	kfree(mdctx->domains);
	kfree(mdctx->phy_ids);
	kfree(mdctx->instance_ids);
	kfree(mdctx);
bail:
	mutex_unlock(gmut);
	return err;
}

/*
 * Release a multidomain context in kernel
 *
 * Also, send msg to dsp to release the same context
 */
static int fastrpc_multidomain_ctx_remove(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm)
{
	int err = 0, ii = 0;
	uint32_t rsvd = 0;

	/* Validate that reserved fields are all zero */
	for (ii = 0; ii < FASTRPC_MDCTX_IOCTL_RSVD; ii++) {
		rsvd = ctxm->reserved[ii];
		if (rsvd) {
			err = -EINVAL;
			dev_err(fl->cctx->dev, "Error %d: %s: rsvd[%d] %u expected to be 0",
				err, __func__, ii, rsvd);
			return err;
		}
	}
	return fastrpc_multidomain_ctx_cleanup(fl, ctxm->req, ctxm->ctx);
}

/* Manage multi-domain context in kernel (register / remove) */
static int fastrpc_multidomain_ctx_manage(struct fastrpc_user *fl,
	struct fastrpc_ioctl_mdctx_manage *ctxm)
{
	int err = 0;

	switch (ctxm->req) {
	case FASTRPC_MDCTX_SETUP:
		err = fastrpc_multidomain_ctx_setup(fl, ctxm);
		break;
	case FASTRPC_MDCTX_REMOVE:
		err = fastrpc_multidomain_ctx_remove(fl, ctxm);
		break;
	default:
		err = -EBADRQC;
		break;
	}
	return err;
}

/* Make rpc call to dsp to trigger a dump of remote-process state */
static int fastrpc_remote_process_state_dump(struct fastrpc_user *fl,
	struct fastrpc_ioctl_remote_proc_state_dump *proc)
{
	int err = 0, fd = proc->fd;
	s32 tgid_frpc = -1;
	u32 domain = proc->domain, session = proc->session;
	u32 logical_domain_id = 0;
	struct device *dev = fl->cctx->dev;
	struct fastrpc_enhanced_invoke invoke = {0};
	struct fastrpc_invoke_args args[2] = {0};
	struct fastrpc_map *map = NULL;
	struct fastrpc_phy_page page = {0};
	struct {
		s32 tgid_frpc;
		u32 flags;
		s32 num_pages;
	} inargs = {0};

	/* Validate domain id passed by user */
	if (!fastrpc_is_valid_logical_domain_id(domain)) {
		if (IS_LEGACY_DOMAIN_ID(domain)) {
			/* If its a valid legacy id, get the corresponding logical id */
			err = fastrpc_convert_legacy_id_to_logical_id(domain, &logical_domain_id);
			if (err != 0) {
				err = -EBADR;
				dev_err(dev, "Error %d: %s: no domain found for legacy domain id %u",
					err, __func__, domain);
				return err;
			}
		} else {
			/*
			 * If domain id is neither a valid logical id nor a legacy id,
			 * return error.
			 */
			err = -EBADR;
			dev_err(dev, "Error %d: %s: %u is not a valid logical domain id",
					err, __func__, domain);
			return err;
		}
	} else {
		logical_domain_id = domain;
	}

	if (!IS_VALID_SESSION_ID(session)) {
		err = -EBADR;
		dev_err(dev, "Error %d: %s: session %u is invalid",
				err, __func__, session);
		return err;
	}

	/* Retrieve the fastrpc pid of the session */
	err = fastrpc_get_frpc_tgid(logical_domain_id, session, &tgid_frpc);
	if (err) {
		dev_err(dev, "Error %d: %s: failed to get frpc tgid for domain %u, session %u",
			err, __func__, logical_domain_id, session);
		return err;
	}

	/* Create smmu mapping of user's string buffer */
	mutex_lock(&fl->map_mutex);
	err = fastrpc_map_create(fl, fd, 0, NULL, proc->size, 0, 0,
			&map, true);
	mutex_unlock(&fl->map_mutex);
	if (err) {
		dev_err(dev, "Error %d: %s: smmu map failed for log-buf fd %d, size %u",
			err, __func__, fd, proc->size);
		return err;
	}

	/* Prepare args for kernel rpc call to rootpd */
	inargs.tgid_frpc = tgid_frpc;
	inargs.flags = proc->level;
	inargs.num_pages = 1;

	args[0].ptr = (u64)(uintptr_t)&inargs;
	args[0].length = sizeof(inargs);
	args[0].fd = -1;

	page.addr = map->phys;
	page.size = map->size;
	args[1].ptr = (u64)(uintptr_t)&page;
	args[1].length = sizeof(page) * inargs.num_pages;
	args[1].fd = -1;

	invoke.inv.handle = FASTRPC_INIT_HANDLE;
	invoke.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_PROCESS_DUMP, 2, 0);
	invoke.inv.args = (__u64)args;

	/* Make rpc call to rootpd to dump remote process state */
	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &invoke);

	if (map) {
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(map);
		mutex_unlock(&fl->map_mutex);
	}
	return err;
}

static int fastrpc_dspsignal_signal(struct fastrpc_user *fl,
			     struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;
	struct fastrpc_channel_ctx *cctx = NULL;
	u64 msg = 0;
	u32 signal_id = fsig->signal_id;

	dev_dbg(fl->cctx->dev, "Send signal PID %u, unique fastrpc pid %u signal %u\n",
					fl->tgid_app, fl->tgid_frpc, signal_id);
	cctx = fl->cctx;
	if (!(signal_id < FASTRPC_DSPSIGNAL_NUM_SIGNALS)) {
		dev_err(fl->cctx->dev, "Sending bad signal %u for PID %u",
				signal_id, fl->tgid);
		return -EINVAL;
	}

	msg = (((uint64_t)fl->tgid_frpc) << 32) | ((uint64_t)fsig->signal_id);
	err = fastrpc_transport_send(cctx, (void *)&msg, sizeof(msg));
	trace_fastrpc_dspsignal("signal", signal_id, 0, 0);

	return err;
}

static int fastrpc_dspsignal_wait(struct fastrpc_user *fl,
			     struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;
	uint32_t timeout_usec = fsig->timeout_usec;
	unsigned long timeout = usecs_to_jiffies(timeout_usec);
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	long ret = 0;
	unsigned long irq_flags = 0;

	dev_dbg(fl->cctx->dev, "Wait for signal %u\n", signal_id);
	if (!(signal_id <FASTRPC_DSPSIGNAL_NUM_SIGNALS)) {
		dev_err(fl->cctx->dev, "Waiting on bad signal %u\n", signal_id);
		return -EINVAL;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id %FASTRPC_DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		dev_err(fl->cctx->dev, "Unknown signal id %u\n", signal_id);
		return -ENOENT;
	}
	if (s->state != DSPSIGNAL_STATE_PENDING) {
		if ((s->state == DSPSIGNAL_STATE_CANCELED) || (s->state == DSPSIGNAL_STATE_UNUSED))
			err = -EINTR;
		if (s->state == DSPSIGNAL_STATE_SIGNALED) {
			/* Signal already received from DSP. Reset signal state and return */
			s->state = DSPSIGNAL_STATE_PENDING;
			reinit_completion(&s->comp);
		}
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		dev_dbg(fl->cctx->dev, "Signal %u in state %u, complete wait immediately",
				signal_id, s->state);
		return err;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	trace_fastrpc_dspsignal("wait", signal_id, s->state, fsig->timeout_usec);
	if (timeout_usec != FASTRPC_DSPSIGNAL_TIMEOUT_NONE)
		ret = wait_for_completion_interruptible_timeout(&s->comp, timeout);
	else
		ret = wait_for_completion_interruptible(&s->comp);
	trace_fastrpc_dspsignal("wakeup", signal_id, s->state, fsig->timeout_usec);

	if (timeout_usec != FASTRPC_DSPSIGNAL_TIMEOUT_NONE && ret == 0) {
		dev_dbg(fl->cctx->dev, "Wait for signal %u timed out %u us\n",
				signal_id, timeout_usec);
		return -ETIMEDOUT;
	} else if (ret < 0) {
		dev_err(fl->cctx->dev, "Wait for signal %u failed %d\n", signal_id, (int)ret);
		return ret;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (s->state == DSPSIGNAL_STATE_SIGNALED) {
		s->state = DSPSIGNAL_STATE_PENDING;
		dev_dbg(fl->cctx->dev, "Signal %u completed\n", signal_id);
	} else if ((s->state == DSPSIGNAL_STATE_CANCELED) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		dev_dbg(fl->cctx->dev, "Signal %u cancelled or destroyed\n", signal_id);
		err = -EINTR;
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

	return err;
}

static int fastrpc_dspsignal_create(struct fastrpc_user *fl,
			     struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *group, *sig;
	unsigned long irq_flags = 0;

	if (!(signal_id <FASTRPC_DSPSIGNAL_NUM_SIGNALS))
		return -EINVAL;

	mutex_lock(&fl->signal_create_mutex);
	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	group = fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE];
	if (group == NULL) {
		int i;
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		group = kzalloc(FASTRPC_DSPSIGNAL_GROUP_SIZE * sizeof(*group),
					     GFP_KERNEL);
		if (group == NULL) {
			dev_err(fl->cctx->dev, "Unable to allocate signal group\n");
			mutex_unlock(&fl->signal_create_mutex);
			return -ENOMEM;
		}

		for (i = 0; i < FASTRPC_DSPSIGNAL_GROUP_SIZE; i++) {
			sig = &group[i];
			init_completion(&sig->comp);
			sig->state = DSPSIGNAL_STATE_UNUSED;
		}
		spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
		fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE] = group;
	}

	sig = &group[signal_id %FASTRPC_DSPSIGNAL_GROUP_SIZE];
	if (sig->state != DSPSIGNAL_STATE_UNUSED) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		mutex_unlock(&fl->signal_create_mutex);
		dev_err(fl->cctx->dev,"Attempting to create signal %u already in use (state %u)\n",
			    signal_id, sig->state);
		return -EBUSY;
	}

	sig->state = DSPSIGNAL_STATE_PENDING;
	reinit_completion(&sig->comp);

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	mutex_unlock(&fl->signal_create_mutex);
	dev_dbg(fl->cctx->dev, "Signal %u created\n", signal_id);

	return err;
}

/* Unblock all dspsignals in pending state */
static int fastrpc_dspsignal_cancel_all(struct fastrpc_user *fl)
{
	u32 i = 0, j = 0;
	struct fastrpc_dspsignal *s = NULL;
	struct fastrpc_dspsignal *group = NULL;
	unsigned long irq_flags = 0;

	dev_dbg(fl->cctx->dev, "%s: Cancel all signals for pid %d\n",
			__func__, fl->tgid);

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	for (i = 0; i < (FASTRPC_DSPSIGNAL_NUM_SIGNALS
		/ FASTRPC_DSPSIGNAL_GROUP_SIZE); i++) {
		group = fl->signal_groups[i];
		if (!group)
			continue;

		for (j = 0; j < FASTRPC_DSPSIGNAL_GROUP_SIZE; j++) {
			s = &group[j];
			if (s->state == DSPSIGNAL_STATE_PENDING) {
				s->state = DSPSIGNAL_STATE_CANCELED;
				trace_fastrpc_dspsignal("cancel all",
					(i * FASTRPC_DSPSIGNAL_GROUP_SIZE) + j,
					s->state, 0);
				complete_all(&s->comp);
			}
		}
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);

	dev_dbg(fl->cctx->dev, "%s: All signals canceled for pid %d\n",
			__func__, fl->tgid);
	return 0;
}

static int fastrpc_dspsignal_destroy(struct fastrpc_user *fl,
			      struct fastrpc_internal_dspsignal *fsig)
{
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	unsigned long irq_flags = 0;

	dev_dbg(fl->cctx->dev, "Destroy signal %u\n", signal_id);
	if (!(signal_id <FASTRPC_DSPSIGNAL_NUM_SIGNALS))
		return -EINVAL;

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	if (fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id % FASTRPC_DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		dev_err(fl->cctx->dev,"Attempting to destroy unused signal %u\n", signal_id);
		return -ENOENT;
	}

	s->state = DSPSIGNAL_STATE_UNUSED;
	trace_fastrpc_dspsignal("destroy", signal_id, s->state, 0);
	complete_all(&s->comp);

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	dev_dbg(fl->cctx->dev, "Signal %u destroyed\n", signal_id);

	return 0;
}

static int fastrpc_dspsignal_cancel_wait(struct fastrpc_user *fl,
				  struct fastrpc_internal_dspsignal *fsig)
{
	u32 signal_id = fsig->signal_id;
	struct fastrpc_dspsignal *s = NULL;
	unsigned long irq_flags = 0;

	dev_dbg(fl->cctx->dev, "Cancel wait for signal %u\n", signal_id);
	if (!(signal_id <FASTRPC_DSPSIGNAL_NUM_SIGNALS))
		return -EINVAL;

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);

	if (fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE] != NULL) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE];

		s = &group[signal_id %FASTRPC_DSPSIGNAL_GROUP_SIZE];
	}
	if ((s == NULL) || (s->state == DSPSIGNAL_STATE_UNUSED)) {
		spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
		dev_err(fl->cctx->dev,"Attempting to cancel unused signal %u\n", signal_id);
		return -ENOENT;
	}

	if (s->state != DSPSIGNAL_STATE_CANCELED) {
		s->state = DSPSIGNAL_STATE_CANCELED;
		trace_fastrpc_dspsignal("cancel", signal_id, s->state, 0);
		complete_all(&s->comp);
	}

	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	dev_dbg(fl->cctx->dev, "Signal %u cancelled\n", signal_id);

	return 0;
}

static int fastrpc_invoke_dspsignal(struct fastrpc_user *fl, struct fastrpc_internal_dspsignal *fsig)
{
	int err = 0;

	switch(fsig->req) {
	case FASTRPC_DSPSIGNAL_SIGNAL:
		err = fastrpc_dspsignal_signal(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_WAIT :
		err = fastrpc_dspsignal_wait(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_CREATE :
		err = fastrpc_dspsignal_create(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_DESTROY :
		err = fastrpc_dspsignal_destroy(fl,fsig);
		break;
	case FASTRPC_DSPSIGNAL_CANCEL_WAIT :
		err = fastrpc_dspsignal_cancel_wait(fl,fsig);
		break;
	}
	return err;
}

/**
 * Retrieve kernel log data from ring buffer
 *
 * Copies available log entries from the ring buffer to the user-provided
 * buffer in the ioctl payload.
 *
 * @param[in]  klog   : Pointer to ioctl kernel log structure
 * @param[in]  fl : Pointer to user context
 *
 * @return 0 on success, error code on failure
 */
#if FRPC_RING_BUFFER_ENABLED
static int fastrpc_retrieve_kernel_logs(struct fastrpc_user *fl,
	struct fastrpc_ioctl_kernel_log *klog)
{
	int err = 0, copied = 0, cpu, data_len;
	struct ring_buffer_event *event;
	struct fastrpc_event_log *entry;
	const char *data;
	char __user *ubuf = (char __user*)(uintptr_t)klog->buffer;
	u32 capacity = klog->buffer_size;
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	unsigned long lost;

	if (!cctx->log.rb || !ubuf || !capacity) {
		err = -EINVAL;
		goto bail;
	}

	err = wait_event_interruptible(cctx->log.wq,
		READ_ONCE(fl->logger_exit) ||
		!ring_buffer_empty(cctx->log.rb));

	if (atomic_read(&cctx->teardown)) {
		err = -EPIPE;
		goto bail;
	}

	if (READ_ONCE(fl->logger_exit)) {
		err = -ESHUTDOWN;
		goto bail;
	}

	if (err)
		goto bail;

	for_each_possible_cpu(cpu) {
		while(1) {
			event = ring_buffer_peek(cctx->log.rb, cpu, NULL, &lost);
			if (lost)
				dev_warn(cctx->dev, "%s: %lu kernel log events lost on cpu %d\n",
					__func__, lost, cpu);

			if (!event)
				break;

			entry = ring_buffer_event_data(event);
			data_len = entry->data_len;
			data = entry->data;
			if ((copied + data_len) > capacity) {
				goto bail;
			}

			if (copy_to_user((void __user *)(ubuf + copied), data,
				data_len)) {
				err = -EFAULT;
				goto bail;
			}
			copied += data_len;

			event = ring_buffer_consume(cctx->log.rb, cpu, NULL, NULL);
			if (!event) {
				dev_err(cctx->dev, "%s: consume failed, but peek returned event",
					__func__);
				break;
			}
		}
	}
bail:
	klog->out_copied = (u32)copied;
	if (err)
		dev_err(cctx->dev, "%s failed with err 0x%x", __func__, err);
	return err;
}
#else
	static int fastrpc_retrieve_kernel_logs(struct fastrpc_user *fl,
	struct fastrpc_ioctl_kernel_log *klog)
{
	return -EOPNOTSUPP;
}
#endif

static int fastrpc_request_thread_exit(struct fastrpc_user *fl,
	struct fastrpc_thread_exit *exit_info)
{
	int err = 0;

	switch (exit_info->thread_type) {
		case FASTRPC_THREAD_LOGGER:
#if FRPC_RING_BUFFER_ENABLED
			WRITE_ONCE(fl->logger_exit, true);
			wake_up_interruptible(&fl->cctx->log.wq);
#endif
			break;
		default:
			err = -EINVAL;
	}

	return err;
}

/*
 *	fastrpc_npu_app_prio_init - Allocate the per-channel NPU priority table
 *
 *	@cctx	: Channel context
 *
 *	Return	: 0 on success, negative error code on failure
 *
 *	Called lazily on the first FASTRPC_IOCTL_NPU_PRIORITY_WORKINFO request.
 *	Only the table struct is allocated here; the entries buffer is NULL until
 *	fastrpc_npu_app_prio_set installs it on the first priority update.
 *	All subsequent access to the table (read and write) is serialised under
 *	cctx->lock, so no additional lock is embedded in the table itself.
 */
static int fastrpc_npu_app_prio_init(struct fastrpc_channel_ctx *cctx)
{
	struct npu_app_prio_table *prio_tbl = NULL;
	unsigned long flags = 0;

	if (cctx->npu_app_prio)
		return 0;

	if (atomic_read(&cctx->teardown))
		return -EPIPE;

	/*
	 * Allocate outside cctx->lock — GFP_KERNEL may sleep and cannot
	 * be used in atomic (spinlock-held) context.
	 */
	prio_tbl = kzalloc(sizeof(*prio_tbl), GFP_KERNEL);
	if (!prio_tbl) {
		dev_err(cctx->dev, "Failed to allocate NPU app prio table\n");
		return -ENOMEM;
	}

	/*
	 * Two callers may both observe cctx->npu_app_prio == NULL before
	 * either acquires the lock. The first to acquire installs the table;
	 * the second discards its allocation. Both return 0.
	 */
	spin_lock_irqsave(&cctx->lock, flags);
	if (!cctx->npu_app_prio)
		cctx->npu_app_prio = prio_tbl;
	else
		kfree(prio_tbl);
	spin_unlock_irqrestore(&cctx->lock, flags);

	return 0;
}

/*
 *	fastrpc_npu_app_prio_set - Replace the NPU application priority table
 *
 *	@cctx		: Channel context
 *	@user_configs	: Userspace pointer to array of fastrpc_npu_app_prio_config
 *	@num_configs	: Number of entries in the array
 *
 *	Return		: 0 on success, negative error code on failure
 *
 *	Allocates a kernel buffer, copies and validates the user-supplied
 *	priority configs, then atomically swaps it into the shared table under
 *	cctx->lock. The previous buffer is freed after the lock is released.
 *
 *	SSR safety: the teardown check and the swap both occur under cctx->lock,
 *	the same lock fastrpc_rpmsg_remove uses to NULL the table pointer. Either
 *	the writer completes before SSR (SSR frees the new buffer during channel
 *	cleanup) or SSR runs first (writer sees NULL and returns -ENODEV).
 */
static int fastrpc_npu_app_prio_set(struct fastrpc_channel_ctx *cctx,
				    u64 user_configs, u32 num_configs)
{
	struct fastrpc_npu_app_prio_config *new_buf = NULL, *old_buf = NULL;
	unsigned long flags = 0;
	uint32_t i = 0;
	int err = 0;

	if (!user_configs) {
		pr_err("%s: invalid args: cctx=%pK user_configs=0x%llx num_configs=%u\n",
		       __func__, cctx, user_configs, num_configs);
		return -EINVAL;
	}

	err = fastrpc_npu_app_prio_init(cctx);
	if (err)
		return err;

	/* Fast-path rejection before doing any expensive work */
	if (atomic_read(&cctx->teardown))
		return -EPIPE;

	/* kcalloc(GFP_KERNEL) and copy_from_user can both sleep;
	 * neither can be called while holding a spinlock.
	 */
	new_buf = kcalloc(num_configs, sizeof(*new_buf), GFP_KERNEL);
	if (!new_buf)
		return -ENOMEM;

	if (copy_from_user(new_buf, (void __user *)(uintptr_t)user_configs,
			   array_size(num_configs, sizeof(*new_buf)))) {
		dev_err(cctx->dev, "Failed to copy configs from user\n");
		err = -EFAULT;
		goto bail;
	}

	for (i = 0; i < num_configs; i++) {
		if (new_buf[i].priority < NPU_MIN_PRIORITY ||
		    new_buf[i].priority > NPU_MAX_PRIORITY) {
			dev_err(cctx->dev, "Invalid priority %d at index %u\n",
				new_buf[i].priority, i);
			err = -EINVAL;
			goto bail;
		}
		/*
		 * Reject any config entry that sets reserved fields to a
		 * non-zero value. Reserved fields must be zero so that a
		 * future kernel that repurposes them for new parameters does
		 * not silently misinterpret data sent by an older userspace
		 * that happened to leave garbage there. Enforcing zero here
		 * ensures the ABI can be extended safely.
		 */
		if (memchr_inv(new_buf[i].reserved, 0,
			       sizeof(new_buf[i].reserved))) {
			dev_err(cctx->dev,
				"Non-zero reserved fields in config at index %u\n", i);
			err = -EINVAL;
			goto bail;
		}
	}

	/*
	 * Atomically swap the validated buffer into the table under cctx->lock.
	 * teardown is checked under the same lock that fastrpc_rpmsg_remove
	 * uses to set it and NULL the table, so either we complete the swap
	 * before SSR or we see teardown=1 and abort.
	 * old_buf is freed after releasing the lock — kfree(NULL) is safe
	 * on the first call when entries has not yet been set.
	 */
	spin_lock_irqsave(&cctx->lock, flags);
	if (atomic_read(&cctx->teardown)) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		err = -EPIPE;
		goto bail;
	}
	if (cctx->npu_app_prio->num_entries > NPU_MAX_APP_PRIO_ENTRIES) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		dev_err(cctx->dev, "Invalid num_entries: %u (max %u)\n",
			cctx->npu_app_prio->num_entries, NPU_MAX_APP_PRIO_ENTRIES);
		err = -EINVAL;
		goto bail;
	}
	old_buf                         = cctx->npu_app_prio->entries;
	cctx->npu_app_prio->entries     = new_buf;
	cctx->npu_app_prio->num_entries = num_configs;
	spin_unlock_irqrestore(&cctx->lock, flags);

	kfree(old_buf);
	new_buf = NULL;
bail:
	kfree(new_buf);
	return err;
}

/*
 * fastrpc_npu_priority - Handle NPU scheduling IOCTL
 *
 * @fl		: User context
 * @npu_sched	: Kernel-space scheduling parameters
 *
 * Return	: 0 on success, negative error code on failure
 */
static int fastrpc_npu_priority(struct fastrpc_user *fl,
				  struct fastrpc_npu_priority *npu_sched)
{
	int err = 0;

	if (npu_sched->num_configs > NPU_MAX_APP_PRIO_ENTRIES) {
		dev_err(fl->cctx->dev, "Invalid num_configs: %u (max %u)\n",
			npu_sched->num_configs, NPU_MAX_APP_PRIO_ENTRIES);
		return -EINVAL;
	}

	switch (npu_sched->version) {
	case NPU_APP_PRIO_CONFIG_VERSION:
		err = fastrpc_npu_app_prio_set(fl->cctx,
					       npu_sched->configs,
					       npu_sched->num_configs);
		if (!err)
			fastrpc_scheduler_notify_prio_update(
				&fl->cctx->scheduler);
		break;
	default:
		dev_err(fl->cctx->dev,
			"NPU sched: unsupported config version %u\n",
			npu_sched->version);
		err = -EINVAL;
		break;
	}

	return err;
}

/*
 * fastrpc_npu_workinfo - Block until a workinfo event is available and return it.
 *
 * This is the consumer side of the per-channel NPU workinfo notification queue.
 * It is called from fastrpc_npu_priority_workinfo() when the ioctl op field is
 * FASTRPC_NPU_OP_WORKINFO.  This function dequeues one event, delivers the
 * group_id / debug_feature_id bytes to the HAL's pre-allocated output buffers,
 * and copies the final npu_work_info struct to userspace via copy_to_user().
 *
 * The per-channel FIFO queue (npu_workinfo_head/tail) and semaphore live
 * directly in fastrpc_channel_ctx and are initialised at probe time.
 *
 * Wait and wakeup:
 *   - Blocks on cctx->npu_workinfo_sem via down_interruptible() until a node
 *     is enqueued (up() called by producer) or teardown (up() called by SSR).
 *   - Returns -EPIPE on SSR/teardown so userspace can close the fd and exit.
 *   - Returns -ERESTARTSYS on signal so the kernel can restart the syscall.
 *
 * Dequeue:
 *   - Dequeues exactly one node per call under cctx->lock (spinlock).
 *   - Userspace re-issues the ioctl immediately after processing each event.
 *
 * group_id / debug_feature_id delivery:
 *   - These fields hold kernel VAs.  The HAL pre-allocates output buffers and
 *     places their addresses in kbuf->group_id / kbuf->debug_feature_id before
 *     calling the ioctl (kbuf is the kernel copy from dispatcher's copy_from_user).
 *     This function calls copy_to_user() to write the bytes into those HAL buffers,
 *     then patches the pointer fields so the delivered struct carries valid
 *     HAL-process VAs.
 *
 * @fl   : fastrpc user context for this open fd
 * @kbuf : kernel-space copy of args.workinfo (provides HAL output buffer addresses)
 * @ubuf : userspace destination — npu_work_info field inside the ioctl args struct
 *
 * Returns 0 on success, negative error code otherwise.
 */
static int fastrpc_npu_workinfo_drain_queue(struct fastrpc_user *fl,
					    struct npu_work_info __user *ubuf)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct npu_workinfo_node *node = NULL;
	/* Kernel copy of ubuf captured before the blocking wait to prevent
	 * TOCTOU on the HAL-provided output buffer addresses (group_id,
	 * debug_feature_id).
	 */
	struct npu_work_info kbuf = { 0 };
	unsigned long flags = 0;
	u32 copy_len = 0;
	int err = 0;

	if (copy_from_user(&kbuf, ubuf, sizeof(kbuf)))
		return -EFAULT;

retry_wait:
	err = down_interruptible(&cctx->npu_workinfo_sem);
	if (err)
		return -ERESTARTSYS;

	if (atomic_read(&cctx->teardown)) {
		up(&cctx->npu_workinfo_sem);
		return -EPIPE;
	}

	/* Dequeue the head node under cctx->lock and decrement the queue
	 * length counter so the producer's cap check stays accurate.
	 * Teardown is checked here under the same lock that SSR uses to set
	 * it, closing the TOCTOU window between wakeup and dequeue.
	 */
	spin_lock_irqsave(&cctx->lock, flags);
	if (atomic_read(&cctx->teardown)) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		return -EPIPE;
	}
	node = cctx->npu_workinfo_head;
	if (!node) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		goto retry_wait;
	}
	cctx->npu_workinfo_head = node->next;
	if (!cctx->npu_workinfo_head)
		cctx->npu_workinfo_tail = NULL;
	cctx->npu_workinfo_queue_len--;
	spin_unlock_irqrestore(&cctx->lock, flags);

	if (kbuf.group_id != 0 && node->info.group_id != 0 &&
	    node->info.group_id_len > 0) {
		if (node->info.group_id_len > NPU_MAX_WORKINFO_FIELD_LEN) {
			dev_warn(cctx->dev,
				 "NPU workinfo: group_id_len %u exceeds max %u, clamping\n",
				 node->info.group_id_len, NPU_MAX_WORKINFO_FIELD_LEN);
			node->info.group_id_len = NPU_MAX_WORKINFO_FIELD_LEN;
		}
		copy_len = min_t(u32, node->info.group_id_len, kbuf.group_id_len);
		if (!copy_to_user((void __user *)(uintptr_t)kbuf.group_id,
				  (void *)(uintptr_t)node->info.group_id, copy_len)) {
			kfree((void *)(uintptr_t)node->info.group_id);
			node->info.group_id     = kbuf.group_id;
			node->info.group_id_len = copy_len;
		} else {
			dev_warn(cctx->dev,
				 "NPU workinfo: copy_to_user failed for group_id\n");
			kfree((void *)(uintptr_t)node->info.group_id);
			node->info.group_id     = 0;
			node->info.group_id_len = 0;
		}
	} else {
		kfree((void *)(uintptr_t)node->info.group_id);
		node->info.group_id     = 0;
		node->info.group_id_len = 0;
	}

	if (kbuf.debug_feature_id != 0 && node->info.debug_feature_id != 0 &&
	    node->info.debug_feature_id_len > 0) {
		if (node->info.debug_feature_id_len > NPU_MAX_WORKINFO_FIELD_LEN) {
			dev_warn(cctx->dev,
				 "NPU workinfo: debug_feature_id_len %u exceeds max %u, clamping\n",
				 node->info.debug_feature_id_len, NPU_MAX_WORKINFO_FIELD_LEN);
			node->info.debug_feature_id_len = NPU_MAX_WORKINFO_FIELD_LEN;
		}
		copy_len = min_t(u32, node->info.debug_feature_id_len,
				      kbuf.debug_feature_id_len);
		if (!copy_to_user((void __user *)(uintptr_t)kbuf.debug_feature_id,
				  (void *)(uintptr_t)node->info.debug_feature_id, copy_len)) {
			kfree((void *)(uintptr_t)node->info.debug_feature_id);
			node->info.debug_feature_id     = kbuf.debug_feature_id;
			node->info.debug_feature_id_len = copy_len;
		} else {
			kfree((void *)(uintptr_t)node->info.debug_feature_id);
			node->info.debug_feature_id     = 0;
			node->info.debug_feature_id_len = 0;
		}
	} else {
		kfree((void *)(uintptr_t)node->info.debug_feature_id);
		node->info.debug_feature_id     = 0;
		node->info.debug_feature_id_len = 0;
	}

	/*
	 * ubuf points directly to
	 * the workinfo union member inside the user's ioctl args struct.
	 */
	err = copy_to_user(ubuf, &node->info, sizeof(*ubuf)) ? -EFAULT : 0;

	kfree(node);
	return err;
}

static int fastrpc_npu_workinfo(struct fastrpc_user *fl,
				struct npu_work_info __user *ubuf)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	u32 version = 0;
	int err = 0;

	if (get_user(version, &ubuf->version))
		return -EFAULT;

	switch (version) {
	case NPU_WORKINFO_VERSION:
		err = fastrpc_npu_workinfo_drain_queue(fl, ubuf);
		break;
	default:
		dev_err(cctx->dev, "NPU workinfo: unsupported version %u\n",
			version);
		err = -EINVAL;
		break;
	}
	return err;
}

int fastrpc_npu_post_workinfo(struct fastrpc_channel_ctx *cctx,
			      struct npu_work_info *info)
{
	struct npu_workinfo_node *node = NULL;
	unsigned long flags = 0;

	/*
	 * Validate event and reason at the producer so that malformed
	 * events are rejected before entering the queue.  The consumer
	 * must never see an unsatisfiable reason/event combination.
	 */
	if (info->event > WORK_ENDED) {
		dev_err(cctx->dev, "NPU workinfo: invalid event %u, dropping\n",
			info->event);
		kfree((void *)(uintptr_t)info->group_id);
		kfree((void *)(uintptr_t)info->debug_feature_id);
		return -EINVAL;
	}
	if (info->event == WORK_STARTED &&
	    info->reason != NPU_WORK_REASON_START_INITIAL) {
		dev_warn(cctx->dev,
			 "NPU workinfo: unexpected reason %u for WORK_STARTED, dropping\n",
			 info->reason);
		kfree((void *)(uintptr_t)info->group_id);
		kfree((void *)(uintptr_t)info->debug_feature_id);
		return -EINVAL;
	}
	if (info->event == WORK_ENDED &&
	    info->reason != NPU_WORK_REASON_END_COMPLETED &&
	    info->reason != NPU_WORK_REASON_END_CANCELLED) {
		dev_warn(cctx->dev,
			 "NPU workinfo: unexpected reason %u for WORK_ENDED, dropping\n",
			 info->reason);
		kfree((void *)(uintptr_t)info->group_id);
		kfree((void *)(uintptr_t)info->debug_feature_id);
		return -EINVAL;
	}

	/* Allocate a new queue node to hold a copy of the workinfo event.
	 * GFP_ATOMIC is used because this function may be called from atomic
	 * context (e.g. softirq/interrupt); GFP_KERNEL would sleep and crash.
	 */
	node = kzalloc(sizeof(*node), GFP_ATOMIC);
	if (!node) {
		dev_err(cctx->dev, "NPU workinfo: failed to alloc node\n");
		kfree((void *)(uintptr_t)info->group_id);
		kfree((void *)(uintptr_t)info->debug_feature_id);
		return -ENOMEM;
	}

	node->info = *info;
	node->next = NULL;

	/* Hold cctx->lock across both the cap check and the enqueue so that
	 * concurrent producers cannot both pass the check and exceed the limit.
	 */
	spin_lock_irqsave(&cctx->lock, flags);
	if (cctx->npu_workinfo_queue_len >= NPU_WORKINFO_QUEUE_MAX) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		dev_err(cctx->dev, "NPU workinfo: queue full (%d), dropping event\n",
			NPU_WORKINFO_QUEUE_MAX);
		kfree((void *)(uintptr_t)node->info.group_id);
		kfree((void *)(uintptr_t)node->info.debug_feature_id);
		kfree(node);
		return -ENOSPC;
	}
	/* Enqueue at the tail and update the length counter. */
	if (cctx->npu_workinfo_tail)
		cctx->npu_workinfo_tail->next = node;
	else
		cctx->npu_workinfo_head = node;
	cctx->npu_workinfo_tail = node;
	cctx->npu_workinfo_queue_len++;
	spin_unlock_irqrestore(&cctx->lock, flags);

	/* Wake the HAL consumer thread blocked in fastrpc_npu_workinfo_drain_queue(). */
	up(&cctx->npu_workinfo_sem);
	return 0;
}

/*
 * fastrpc_npu_priority_workinfo - Handle FASTRPC_IOCTL_NPU_PRIORITY_WORKINFO
 *
 * Combines NPU scheduling and workinfo into a single dedicated ioctl.
 * Dispatches to fastrpc_npu_priority or fastrpc_npu_workinfo based
 * on the op field.  copy_from_user is centralised here; copy_to_user
 * for WORKINFO is handled inside fastrpc_npu_workinfo().
 *
 * @fl   : User context
 * @argp : Userspace pointer to struct fastrpc_ioctl_npu_priority_workinfo
 *
 * Return: 0 on success, negative error code on failure
 */
static int fastrpc_npu_priority_workinfo(struct fastrpc_user *fl,
					 struct file *file, char __user *argp)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	struct fastrpc_ioctl_npu_priority_workinfo args = {};
	struct npu_work_info __user *ubuf = NULL;
	int err = 0;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	/* Reject non-zero reserved fields: a future userspace that repurposes
	 * a reserved word for a new parameter must not silently succeed on an
	 * older kernel that ignores it.
	 */
	if (memchr_inv(args.reserved, 0, sizeof(args.reserved)))
		return -EINVAL;

	/*
	 * Reject all NPU ioctl calls once the channel is tearing down (SSR or
	 * device removal).  At this point the DSP subsystem is no longer
	 * operational: PRIORITY updates would target a dead subsystem, and
	 * WORKINFO would block on a wait queue that will never be woken by a
	 * new work event.  Return -EPIPE so userspace can distinguish teardown
	 * from a permission error or malformed request.
	 */
	if (atomic_read(&cctx->teardown))
		return -EPIPE;

	switch (args.op) {
	case FASTRPC_NPU_OP_PRIORITY:
		if (memchr_inv(args.prio.reserved, 0, sizeof(args.prio.reserved)))
			return -EINVAL;
		if (args.prio.num_configs == 0)
			return -EINVAL;
		err = fastrpc_npu_priority(fl, &args.prio);
		break;
	case FASTRPC_NPU_OP_WORKINFO:
		if (memchr_inv(args.workinfo.reserved, 0, sizeof(args.workinfo.reserved)))
			return -EINVAL;
		/*
		 * ubuf points directly at the workinfo union member inside the
		 * user's ioctl args struct.  fastrpc_npu_workinfo() dequeues one
		 * event and copies it to ubuf via copy_to_user().
		 */
		ubuf = &((struct fastrpc_ioctl_npu_priority_workinfo __user *)argp)->workinfo;
		err = fastrpc_npu_workinfo(fl, ubuf);
		break;
	default:
		err = -EINVAL;
		break;
	}
	return err;
}

static int fastrpc_multimode_invoke(struct fastrpc_user *fl, char __user *argp)
{
	/* Legacy invoke has no appid field; -1 sentinel bypasses appid>=0 enforcement */
	struct fastrpc_enhanced_invoke inv2 = { .appid = -1 };
	struct fastrpc_ioctl_multimode_invoke invoke;
	struct fastrpc_internal_control cp = {0};
	struct fastrpc_internal_dspsignal *fsig = NULL;
	struct fastrpc_internal_notif_rsp notif;
	struct fastrpc_internal_config config = {0};
	struct fastrpc_internal_sessinfo sessinfo;
	struct fastrpc_internal_sessinfo_v2 sessinfo_v2 = {0};
	struct fastrpc_ioctl_mdctx_manage ctxm = {0};
	struct fastrpc_ioctl_remote_proc_state_dump proc = {0};
	struct fastrpc_internal_proc_timeout rpc = {0};
	struct fastrpc_ioctl_kernel_log klog = {0};
	struct fastrpc_thread_exit exit_info = {0};
	struct fastrpc_timeline_arguments timeline_init_args = {0};
	struct fastrpc_ioctl_remote_work work = {0};
	u32 multisession, size = 0, timeline_version = 0;
	u64 *perf_kernel;
	bool legacy_domains = true;
	int err = 0, recovery = 0;

	if (copy_from_user(&invoke, argp, sizeof(invoke)))
		return -EFAULT;
	switch (invoke.req) {
	case FASTRPC_INVOKE:
		size = sizeof(struct fastrpc_invoke);
		fallthrough;
	case FASTRPC_INVOKE_ENHANCED:
		/* nscalars is truncated here to max supported value */
		if (!size)
			size = sizeof(struct fastrpc_enhanced_invoke);
		if (copy_from_user(&inv2, (void __user *)(uintptr_t)invoke.invparam,
				   size))
			return -EFAULT;
		perf_kernel = (u64 *)(uintptr_t)inv2.perf_kernel;
		if (perf_kernel)
			fl->profile = true;
		err = fastrpc_internal_invoke(fl, USER_MSG, &inv2);
		break;
	case FASTRPC_INVOKE_CONTROL:
		if (copy_from_user(&cp, (void __user *)(uintptr_t)invoke.invparam, sizeof(cp)))
			return  -EFAULT;

		err = fastrpc_internal_control(fl, &cp);
		break;
	case FASTRPC_INVOKE_DSPSIGNAL:
		if (invoke.size > sizeof(*fsig))
			return -EINVAL;
		fsig = kzalloc(sizeof(*fsig), GFP_KERNEL);
		if (!fsig)
			return -ENOMEM;
		if (copy_from_user(fsig, (void __user *)(uintptr_t)invoke.invparam,
				invoke.size)) {
			kfree(fsig);
			return -EFAULT;
		}
		err = fastrpc_invoke_dspsignal(fl, fsig);
		kfree(fsig);
		break;
	case FASTRPC_INVOKE_NOTIF:
		if (invoke.dynamic_domains)
			legacy_domains = false;
		err = fastrpc_get_notif_response(&notif,
						(void *)invoke.invparam, fl, legacy_domains);
		break;
	case FASTRPC_INVOKE_MULTISESSION:
		if (copy_from_user(&multisession, (void __user *)(uintptr_t)invoke.invparam, sizeof(multisession)))
			return  -EFAULT;
		if(!fl->multi_session_support)
			fl->sessionid = 1;
		break;
	case FASTRPC_INVOKE_CONFIG:
		size = sizeof(struct fastrpc_internal_config);
		/* Copy with which ever is miminum size, ensures backward compatibility */
		if (invoke.size < size )
			size = invoke.size;
		if (copy_from_user(&config, (void __user *)(uintptr_t)invoke.invparam,
			size))
			return -EFAULT;
		fl->config.user_fd = config.user_fd;
		fl->config.user_size = config.user_size;
		fl->config.root_addr = config.root_addr;
		fl->config.root_size = config.root_size;
		break;
	case FASTRPC_INVOKE_SESSIONINFO:
		/* V2: invoke.size == sizeof(fastrpc_internal_sessinfo_v2) carries max_threads */
		if (invoke.size == sizeof(struct fastrpc_internal_sessinfo_v2)) {
			if (copy_from_user(&sessinfo_v2,
					(void __user *)(uintptr_t)invoke.invparam,
					sizeof(sessinfo_v2)))
				return -EFAULT;
			err = fastrpc_set_session_info_v2(fl, &sessinfo_v2);
		} else {
		/* V1: legacy path, max_threads not carried */
			if (invoke.size < sizeof(struct fastrpc_internal_sessinfo))
				return -EINVAL;
			if (copy_from_user(&sessinfo,
					(void __user *)(uintptr_t)invoke.invparam,
					sizeof(struct fastrpc_internal_sessinfo)))
				return -EFAULT;
			err = fastrpc_set_session_info(fl, &sessinfo);
		}
		break;
	case FASTRPC_INVOKE_MDCTX_MANAGE:
		if (copy_from_user(&ctxm, (void __user *)(uintptr_t)invoke.invparam,
			sizeof(ctxm)))
			return -EFAULT;
		err = fastrpc_multidomain_ctx_manage(fl, &ctxm);
		break;
	case FASTRPC_INVOKE_REMOTE_PROCESS_STATE_DUMP:
		if (copy_from_user(&proc, (void __user *)(uintptr_t)invoke.invparam,
			sizeof(proc)))
			return -EFAULT;
		err = fastrpc_remote_process_state_dump(fl, &proc);
		break;
	case FASTRPC_INVOKE_SET_RPC_TIMEOUT:
		if (copy_from_user(&rpc,
			(void __user *)(uintptr_t)invoke.invparam, sizeof(rpc)))
			return -EFAULT;
		err = fastrpc_user_set_rpc_timeout(fl, &rpc);
		break;
	case FASTRPC_INVOKE_DISABLE_DSP_RECOVERY:
		if (copy_from_user(&recovery,
			(void __user *)(uintptr_t)invoke.invparam, sizeof(recovery)))
			return -EFAULT;
		err = fastrpc_set_dsp_recovery_mode(fl, recovery);
		break;
	case FASTRPC_INVOKE_RETRIEVE_KERNEL_LOG:
		if (copy_from_user(&klog, (void __user *)(uintptr_t)invoke.invparam,
				sizeof(klog)))
			return -EFAULT;
		err = fastrpc_retrieve_kernel_logs(fl, &klog);
		if (copy_to_user((void __user *)invoke.invparam, &klog, sizeof(klog)))
			return -EFAULT;
		break;
	case FASTRPC_INVOKE_THREAD_EXIT:
		if (copy_from_user(&exit_info, (void __user *)(uintptr_t)invoke.invparam,
				sizeof(exit_info)))
			return -EFAULT;
		err = fastrpc_request_thread_exit(fl, &exit_info);
		break;
	case FASTRPC_SET_TIMELINE_INFO:
		if (copy_from_user(&timeline_init_args,
			(void __user *)(uintptr_t)invoke.invparam,
			sizeof(timeline_init_args)))
			return -EFAULT;
		err = fastrpc_set_timeline_info(fl, &timeline_init_args);
		break;
	case FASTRPC_GET_TIMELINE_VERSION:
		err = fastrpc_get_timeline_version(fl, &timeline_version);
		if (!err) {
			if (copy_to_user((void __user *)(uintptr_t)invoke.invparam,
				&timeline_version, sizeof(timeline_version)))
				return -EFAULT;
		}
		break;
	case FASTRPC_INVOKE_REMOTE_WORK:
		if (copy_from_user(&work,
				(void __user *)(uintptr_t)invoke.invparam,
				sizeof(work)))
			return -EFAULT;

		if (!work.handle ||
			(work.status != FASTRPC_REMOTE_WORK_STATUS_START &&
			 work.status != FASTRPC_REMOTE_WORK_STATUS_END))
			return -EINVAL;

		if (work.status == FASTRPC_REMOTE_WORK_STATUS_START)
			err = fastrpc_work_add(fl, &work);
		else
			err = fastrpc_work_remove(fl, &work);
		break;
	default:
		err = -ENOTTY;
		break;
	}
	return err;
}

static int fastrpc_get_info_from_dsp(struct fastrpc_user *fl, uint32_t *dsp_attr_buf,
				     uint32_t dsp_attr_buf_len)
{
	struct fastrpc_invoke_args args[2] = { 0 };
	struct fastrpc_enhanced_invoke ioctl;

	/* Capability filled in userspace */
	dsp_attr_buf[0] = 0;
	dsp_attr_buf_len -= 1;

	args[0].ptr = (u64)(uintptr_t)&dsp_attr_buf_len;
	args[0].length = sizeof(dsp_attr_buf_len);
	args[0].fd = -1;
	args[1].ptr = (u64)(uintptr_t)&dsp_attr_buf[DSP_ATTR_OFFSET];
	args[1].length = dsp_attr_buf_len * sizeof(uint32_t);
	args[1].fd = -1;

	ioctl.inv.handle = FASTRPC_DSP_UTILITIES_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(0, 1, 1);
	ioctl.inv.args = (__u64)args;

	return fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
}

static int fastrpc_get_info_from_kernel(struct fastrpc_ioctl_capability *cap,
					struct fastrpc_user *fl)
{
	struct fastrpc_channel_ctx *cctx = fl->cctx;
	uint32_t attribute_id = cap->attribute_id;
	uint32_t *dsp_attributes;
	unsigned long flags;
	uint32_t domain = cap->domain;
	struct fastrpc_user *user_obj = cctx->kcomm_user.obj;
	int err;

	if (!user_obj)
		user_obj = fl;

	/* No sctx in TVM case treated as unsupported */
	if (!user_obj->sctx) {
		return -EOPNOTSUPP;
	}

	spin_lock_irqsave(&cctx->lock, flags);
	/* check if we already have queried dsp for attributes */
	if (cctx->valid_attributes) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		goto done;
	}
	spin_unlock_irqrestore(&cctx->lock, flags);

	dsp_attributes = kzalloc(FASTRPC_MAX_DSP_ATTRIBUTES_LEN, GFP_KERNEL);
	if (!dsp_attributes)
		return -ENOMEM;

	err = fastrpc_get_info_from_dsp(user_obj, dsp_attributes, FASTRPC_MAX_DSP_ATTRIBUTES);
	if (err == DSP_UNSUPPORTED_API) {
		dev_info(cctx->dev,
			 "Warning: DSP capabilities not supported on domain: %d\n", domain);
		kfree(dsp_attributes);
		return -EOPNOTSUPP;
	} else if (err) {
		dev_dbg(cctx->dev, "Failed to get dsp information err: %d\n", err);
		kfree(dsp_attributes);
		return err;
	}

	err = fastrpc_send_sys_unsigned_prio_config(cctx);
	if(err)
		dev_warn(cctx->dev, "Failed to send sys unsigned prio config err: %d\n", err);

	spin_lock_irqsave(&cctx->lock, flags);
	memcpy(cctx->dsp_attributes, dsp_attributes, FASTRPC_MAX_DSP_ATTRIBUTES_LEN);
	cctx->valid_attributes = true;
	spin_unlock_irqrestore(&cctx->lock, flags);
	kfree(dsp_attributes);
done:
	cap->capability = cctx->dsp_attributes[attribute_id];
	return 0;
}

static inline int fastrpc_copy_timeline_events_to_user(uintptr_t *user_addr,
	uintptr_t user_end_addr, size_t timeline_buf_size,
	const struct fastrpc_timeline_event *timeline_event_list,
	size_t timeline_buf_event_cnt, __u32 *buf_write_index)
{
	if (timeline_buf_size > (user_end_addr - *user_addr))
		return -EOVERFLOW;
	if (copy_to_user((void __user *)*user_addr, timeline_event_list, timeline_buf_size))
		return -EFAULT;
	*user_addr += timeline_buf_size;
	*buf_write_index += timeline_buf_event_cnt;
	return 0;
}

static int fastrpc_get_timeline_buffer(struct fastrpc_user *fl, char __user *argp)
{
	int err = 0;
	size_t timeline_buf_size, timeline_buf_event_cnt, user_buf_size;
	uintptr_t user_addr, user_end_addr;
	struct fastrpc_ioctl_timeline_buf timeline_ioctl;
	struct fastrpc_timeline *timeline = NULL;

	err = copy_from_user(&timeline_ioctl, (void __user *)argp,
		sizeof(timeline_ioctl));
	if (err)
		return -EFAULT;

	if (!fastrpc_reserved_field_check(timeline_ioctl.reserved,
		sizeof(timeline_ioctl.reserved))) {
		pr_err("%s: reserved fields are expected to be 0\n", __func__);
		return -EINVAL;
	}

	if (!fl->fastrpc_timeline_obj || timeline_ioctl.total_events_cnt == 0 ||
		(timeline_ioctl.total_events_cnt > MAX_TIMELINE_EVENT_COUNT))
		return -EINVAL;

	user_buf_size = timeline_ioctl.total_events_cnt *
		sizeof(struct fastrpc_timeline_event);
	user_addr = (uintptr_t)timeline_ioctl.addr;
	/* userspace buffer overflow validation */
	if (user_addr > (UINTPTR_MAX - user_buf_size))
		return -EFAULT;
	/* validate user buffer address and size access */
	if (!access_ok((void __user *)user_addr, user_buf_size))
		return -EFAULT;
	user_end_addr = user_addr + user_buf_size;
	timeline = fl->fastrpc_timeline_obj;
	timeline_buf_event_cnt = timeline->timeline_buf_hlos_k->event_list_length;
	timeline_buf_size = timeline_buf_event_cnt * sizeof(struct fastrpc_timeline_event);

	err = fastrpc_copy_timeline_events_to_user(&user_addr, user_end_addr,
			timeline_buf_size, timeline->timeline_buf_hlos_k->timeline_event_list,
			timeline_buf_event_cnt, &timeline_ioctl.buf_write_index);
	if (err)
		return err;

	err = fastrpc_copy_timeline_events_to_user(&user_addr, user_end_addr,
		timeline_buf_size, timeline->timeline_buf_dsp_k->timeline_event_list,
		timeline_buf_event_cnt, &timeline_ioctl.buf_write_index);
	if (err)
		return err;

	err = fastrpc_copy_timeline_events_to_user(&user_addr, user_end_addr,
			timeline_buf_size, timeline->timeline_buf_dsp_u->timeline_event_list,
			timeline_buf_event_cnt, &timeline_ioctl.buf_write_index);
	if (err)
		return err;

	err = copy_to_user((struct fastrpc_ioctl_timeline_buf __user *)argp,
		&timeline_ioctl, sizeof(struct fastrpc_ioctl_timeline_buf));
	if (err)
		return -EFAULT;

	return 0;
}

static int fastrpc_get_dsp_info(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_ioctl_capability cap = {0};
	int err = 0;

	if (copy_from_user(&cap, argp, sizeof(cap)))
		return  -EFAULT;

	cap.capability = 0;

	/* Validate that domain passed is either a logical or legacy domain id */
	if (!IS_LEGACY_DOMAIN_ID(cap.domain) &&
		!fastrpc_is_valid_logical_domain_id(cap.domain)) {
		dev_err(fl->cctx->dev, "Error: Invalid domain id:%d, err:%d\n",
			cap.domain, err);
		return -ECHRNG;
	}

	if (cap.attribute_id == KERNEL_TSTACK_FLAG_SUPPORT) {
		/* Kernel-only attribute, no DSP query needed */
		cap.capability = 1;
		goto done;
	}

	if (cap.attribute_id >= FASTRPC_MAX_DSP_ATTRIBUTES) {
		dev_err(fl->cctx->dev, "Error: invalid attribute: %d, err: %d\n",
			cap.attribute_id, err);
		return -EOVERFLOW;
	}

	err = fastrpc_get_info_from_kernel(&cap, fl);
	if (err)
		return err;

done:
	if (copy_to_user(argp, &cap, sizeof(cap)))
		return -EFAULT;

	return 0;
}

static int fastrpc_req_munmap_dsp(struct fastrpc_user *fl, uintptr_t raddr, u64 size) {

	struct fastrpc_invoke_args args[1] = { [0] = { 0 } };
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_munmap_req_msg req_msg;
	int err = 0;

	req_msg.pgid = fl->tgid_frpc;
	req_msg.size = size;
	req_msg.vaddr = raddr;

	args[0].ptr = (u64) (uintptr_t) &req_msg;
	args[0].length = sizeof(req_msg);

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MUNMAP, 1, 0);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
	/* error to be printed by caller function */
	return err;

}

static int fastrpc_req_munmap_impl(struct fastrpc_user *fl, struct fastrpc_buf *buf)
{
	struct device *dev = fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	int err;

	err = fastrpc_req_munmap_dsp(fl, buf->raddr, buf->size);
	if (!err) {
		if (buf->type == REMOTEHEAP_BUF) {
			if (fl->cctx->vmcount) {
				u64 src_perms = 0;
				struct qcom_scm_vmperm dst_perms;
				u32 i;

				for (i = 0; i < fl->cctx->vmcount; i++)
					src_perms |= BIT(fl->cctx->vmperms[i].vmid);

				dst_perms.vmid = QCOM_SCM_VMID_HLOS;
				dst_perms.perm = QCOM_SCM_PERM_RWX;
				err = qcom_scm_assign_mem(buf->phys, (u64)buf->size,
								&src_perms, &dst_perms, 1);
				if (err) {
					dev_err(dev,
				"%s: Failed to assign memory phys 0x%llx size 0x%llx err %d",
						__func__, buf->phys, buf->size, err);
					return err;
				}
			}
		}
		dev_dbg(dev, "unmmap\tpt 0x%09lx OK\n", buf->raddr);
	} else {
		dev_err(dev, "unmmap\tpt 0x%09lx ERROR\n", buf->raddr);
	}

	return err;
}

static int fastrpc_req_munmap(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_buf *buf = NULL, *iter, *b;
	struct fastrpc_req_munmap req;
	struct fastrpc_map *map = NULL, *iterm, *m;
	struct device *dev = NULL;
	int err = -EINVAL;
	unsigned long flags;

	if (atomic_read(&fl->state) != DSP_CREATE_COMPLETE) {
		dev_err(fl->cctx->dev,
			" %s: %s: trying to unmap buf before creating remote session\n",
			__func__, current->comm);
		return -EHOSTDOWN;
	}
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	dev = fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	spin_lock(&fl->lock);
	list_for_each_entry_safe(iter, b, &fl->mmaps, node) {
		if ((iter->raddr == req.vaddrout) && (iter->size == req.size)) {
			buf = iter;
			list_del(&buf->node);
			break;
		}
	}
	spin_unlock(&fl->lock);

	if (buf) {
		err = fastrpc_req_munmap_impl(fl, buf);
		if(!err) {
			fastrpc_buf_free(buf, false);
		} else {
			spin_lock(&fl->lock);
			list_add_tail(&buf->node, &fl->mmaps);
			spin_unlock(&fl->lock);
		}
		return err;
	}

	spin_lock_irqsave(&fl->cctx->lock, flags);
	list_for_each_entry_safe(iter, b, &fl->cctx->gmaps, node) {
		if ((iter->raddr == req.vaddrout) && (iter->size == req.size)) {
			buf = iter;
			list_del(&buf->node);
			break;
		}
	}
	spin_unlock_irqrestore(&fl->cctx->lock, flags);

	if (buf) {
		err = fastrpc_req_munmap_impl(fl, buf);
		if(!err) {
			fastrpc_buf_free(buf, false);
		} else {
			spin_lock_irqsave(&fl->cctx->lock, flags);
			list_add_tail(&buf->node, &fl->cctx->gmaps);
			spin_unlock_irqrestore(&fl->cctx->lock, flags);
		}
		return err;
	}

	spin_lock(&fl->lock);
	list_for_each_entry_safe(iterm, m, &fl->maps, node) {
		if (iterm->raddr == req.vaddrout) {
			/*
			 * Check if DSP mapping is complete, then move the state to
			 * unmap in progress only if there is no other ongoing unmap.
			 */
			if (atomic_cmpxchg(&iterm->state, FD_DSP_MAP_COMPLETE,
				FD_DSP_UNMAP_IN_PROGRESS) != FD_DSP_MAP_COMPLETE)
				err = -EALREADY;
			else
				map = iterm;
			break;
		}
	}
	spin_unlock(&fl->lock);
	if (!map) {
		dev_err(dev, "buffer not in buf or map list\n");
		return err;
	}

	err = fastrpc_req_munmap_dsp(fl, map->raddr, map->size);
	if (err) {
		dev_err(dev, "unmmap\tpt fd = %d, 0x%09llx error\n",  map->fd, map->raddr);
		/* Revert the map state to map complete */
		atomic_set(&map->state, FD_DSP_MAP_COMPLETE);
	} else {
		/* Set the map state to default on successful unmapping */
		atomic_set(&map->state, FD_MAP_DEFAULT);
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(map);
		mutex_unlock(&fl->map_mutex);
	}

	return err;
}

static int fastrpc_req_mmap(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_invoke_args args[3] = { [0 ... 2] = { 0 } };
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_buf *buf = NULL;
	struct fastrpc_mmap_req_msg req_msg;
	struct fastrpc_mmap_rsp_msg rsp_msg;
	struct fastrpc_phy_page pages;
	struct fastrpc_req_mmap req;
	struct fastrpc_map *map = NULL;
	struct fastrpc_smmu *smmucb = NULL;
	struct device *dev = NULL;
	struct timespec64 start_ts, end_ts;
	int err;
	unsigned long flags;

	if (atomic_read(&fl->state) != DSP_CREATE_COMPLETE) {
		dev_err(fl->cctx->dev,
			"%s: %s: trying to map buf before creating remote session\n",
			__func__, current->comm);
		return -EHOSTDOWN;
	}
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	if (!req.size)
		return -EFAULT;

	smmucb = &fl->sctx->smmucb[DEFAULT_SMMU_IDX];
	dev = smmucb->dev;
	if ((req.flags == ADSP_MMAP_ADD_PAGES ||
		req.flags == ADSP_MMAP_REMOTE_HEAP_ADDR ||
		req.flags == ADSP_MMAP_ADD_PAGES_TSTACK) && !fl->is_unsigned_pd) {
		if (req.vaddrin) {
			dev_err(dev,
			"adding user allocated pages is only supported for unsigned PD\n");
			return -EINVAL;
		}

		if (req.flags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
			err = fastrpc_buf_alloc(fl, NULL, req.size, REMOTEHEAP_BUF, &buf);
		} else {
			err = fastrpc_smmu_buf_alloc(fl, req.size, USER_BUF, &buf);
		}

		if (err) {
			dev_err(dev, "failed to allocate buffer\n");
			return err;
		}

		/*
		 * Update dev with correct SMMU device,
		 * on which the memory is allocated.
		 */
		if (req.flags == ADSP_MMAP_ADD_PAGES || req.flags == ADSP_MMAP_ADD_PAGES_TSTACK)
			dev = buf->smmucb->dev;

		/* Mark growheap-related stack memory to be included in coredump */
		buf->flags = req.flags;

		req_msg.pgid = fl->tgid_frpc;
		req_msg.flags = req.flags;
		req_msg.vaddr = req.vaddrin;
		req_msg.num = sizeof(pages);

		args[0].ptr = (u64) (uintptr_t) &req_msg;
		args[0].length = sizeof(req_msg);

		pages.addr = buf->phys;
		pages.size = buf->size;

		args[1].ptr = (u64) (uintptr_t) &pages;
		args[1].length = sizeof(pages);

		args[2].ptr = (u64) (uintptr_t) &rsp_msg;
		args[2].length = sizeof(rsp_msg);

		ioctl.inv.handle = FASTRPC_INIT_HANDLE;
		ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MMAP, 2, 1);
		ioctl.inv.args = (__u64)args;

		err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
		if (err) {
			dev_err(dev, "mmap error (len 0x%08llx)\n", buf->size);
			goto err_invoke;
		}

		/* update the buffer to be able to deallocate the memory on the DSP */
		buf->raddr = (uintptr_t) rsp_msg.vaddr;

		/* let the client know the address to use */
		req.vaddrout = rsp_msg.vaddr;

		/* Add memory to static PD pool, protection thru hypervisor */
		if (req.flags == ADSP_MMAP_REMOTE_HEAP_ADDR && fl->cctx->vmcount) {
			u64 src_perms = BIT(QCOM_SCM_VMID_HLOS);

			ktime_get_boottime_ts64(&start_ts);
			err = qcom_scm_assign_mem(buf->phys,(u64)buf->size,
				&src_perms, fl->cctx->vmperms, fl->cctx->vmcount);
			ktime_get_boottime_ts64(&end_ts);
			buf->scm_assign_time = timespec64_sub(end_ts, start_ts);
			if (err) {
				dev_err(dev, "Failed to assign memory phys 0x%llx size 0x%llx err %d",
						buf->phys, buf->size, err);
				goto err_assign;
			}
		}
		if (req.flags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
			spin_lock_irqsave(&fl->cctx->lock, flags);
			list_add_tail(&buf->node, &fl->cctx->gmaps);
			spin_unlock_irqrestore(&fl->cctx->lock, flags);
		} else {
			spin_lock(&fl->lock);
			list_add_tail(&buf->node, &fl->mmaps);
			spin_unlock(&fl->lock);
		}
		if (copy_to_user((void __user *)argp, &req, sizeof(req)))
			/*
			 * The usercopy failed, but we can't do much about it, as this
			 * buf is already mapped in the DSP and accessible for the
			 * current process. Therefore "leak" the buf and rely on the
			 * process exit path to do any required cleanup.
			 */
			return -EFAULT;

	} else {
		if ((req.flags == ADSP_MMAP_REMOTE_HEAP_ADDR) && fl->is_unsigned_pd) {
			dev_err(dev, "remote heap is not supported for unsigned PD\n");
			return -EINVAL;
		}
		mutex_lock(&fl->map_mutex);
		err = fastrpc_map_create(fl, req.fd, req.vaddrin, NULL, req.size, 0, 0, &map, true);
		mutex_unlock(&fl->map_mutex);
		if (err) {
			dev_err(dev, "failed to map buffer, fd = %d\n", req.fd);
			return err;
		}
		/*
		 * Update the map state to in progress only if there is no ongoing or
		 * completed DSP mapping.
		 */
		if (atomic_cmpxchg(&map->state, FD_MAP_DEFAULT, FD_DSP_MAP_IN_PROGRESS)
			!= FD_MAP_DEFAULT) {
			err = -EALREADY;
			goto err_invoke;
		}

		/* Mark growheap-related stack memory to be included in coredump */
		map->flags = req.flags;

		req_msg.pgid = fl->tgid_frpc;
		req_msg.flags = req.flags;
		req_msg.vaddr = req.vaddrin;
		req_msg.num = sizeof(pages);

		args[0].ptr = (u64) (uintptr_t) &req_msg;
		args[0].length = sizeof(req_msg);

		pages.addr = map->phys;
		pages.size = map->size;

		args[1].ptr = (u64) (uintptr_t) &pages;
		args[1].length = sizeof(pages);

		args[2].ptr = (u64) (uintptr_t) &rsp_msg;
		args[2].length = sizeof(rsp_msg);

		ioctl.inv.handle = FASTRPC_INIT_HANDLE;
		ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MMAP, 2, 1);
		ioctl.inv.args = (__u64)args;

		err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
		if (err) {
			dev_err(dev, "mmap error (len 0x%08llx)\n", map->size);
			/* Revert the map state to default */
			atomic_set(&map->state, FD_MAP_DEFAULT);
			goto err_invoke;
		}

		/* update the buffer to be able to deallocate the memory on the DSP */
		map->raddr = (uintptr_t) rsp_msg.vaddr;

		/* let the client know the address to use */
		req.vaddrout = rsp_msg.vaddr;
		/* Set the map state to complete on successful mapping */
		atomic_set(&map->state, FD_DSP_MAP_COMPLETE);
		if (copy_to_user((void __user *)argp, &req, sizeof(req)))
			/*
			 * The usercopy failed, but we can't do much about it, as this
			 * map is already mapped in the DSP and accessible for the
			 * current process. Therefore "leak" the map and rely on the
			 * process exit path to do any required cleanup.
			 */
			return -EFAULT;

	}
	return 0;

err_assign:
	err = fastrpc_req_munmap_dsp(fl, buf->raddr, buf->size);
	if (err) {
		if (req.flags == ADSP_MMAP_REMOTE_HEAP_ADDR) {
			spin_lock_irqsave(&fl->cctx->lock, flags);
			list_add_tail(&buf->node, &fl->cctx->gmaps);
			spin_unlock_irqrestore(&fl->cctx->lock, flags);
		} else {
			spin_lock(&fl->lock);
			list_add_tail(&buf->node, &fl->mmaps);
			spin_unlock(&fl->lock);
		}
		buf = NULL;
	}

err_invoke:
	if (map) {
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(map);
		mutex_unlock(&fl->map_mutex);
	}
	if (buf)
		fastrpc_buf_free(buf, false);

	return err;
}

static int fastrpc_req_mem_unmap_impl(struct fastrpc_user *fl, struct fastrpc_mem_unmap *req)
{
	struct fastrpc_invoke_args args[1] = { [0] = { 0 } };
	struct fastrpc_enhanced_invoke ioctl;
	struct fastrpc_map *map = NULL, *iter, *m;
	struct fastrpc_mem_unmap_req_msg req_msg = { 0 };
	int err = -EINVAL;
	struct device *dev = fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;

	spin_lock(&fl->lock);
	list_for_each_entry_safe(iter, m, &fl->maps, node) {
		if ((req->fd < 0 || iter->fd == req->fd) && (iter->raddr == req->vaddr)) {
			/*
			 * Check if DSP mapping is complete, then move the state to
			 * unmap in progress only if there is no other ongoing unmap.
			 */
			if (atomic_cmpxchg(&iter->state, FD_DSP_MAP_COMPLETE,
				FD_DSP_UNMAP_IN_PROGRESS) != FD_DSP_MAP_COMPLETE)
				err = -EALREADY;
			else
				map = iter;
			break;
		}
	}

	spin_unlock(&fl->lock);

	if (!map) {
		dev_err(dev, "map not in list\n");
		return err;
	}

	req_msg.pgid = fl->tgid_frpc;
	req_msg.len = map->len;
	req_msg.vaddrin = map->raddr;
	req_msg.fd = map->fd;

	args[0].ptr = (u64) (uintptr_t) &req_msg;
	args[0].length = sizeof(req_msg);

	ioctl.inv.handle = FASTRPC_INIT_HANDLE;
	ioctl.inv.sc = FASTRPC_SCALARS(FASTRPC_RMID_INIT_MEM_UNMAP, 1, 0);
	ioctl.inv.args = (__u64)args;

	err = fastrpc_internal_invoke(fl, KERNEL_MSG_WITH_ZERO_PID, &ioctl);
	if (err) {
		dev_err(dev, "Unmap on DSP failed for fd:%d, addr:0x%09llx\n",  map->fd, map->raddr);
		/* Revert the map state to map complete */
		atomic_set(&map->state, FD_DSP_MAP_COMPLETE);
		return err;
	}
	/* Set the map state to default on successful unmapping */
	atomic_set(&map->state, FD_MAP_DEFAULT);
	mutex_lock(&fl->map_mutex);

	/*
	 * If the mapping was created with IOVA retention on unmap, allow
	 * clearing that flag so the unmap also releases the IOVA region.
	 */
	if (map->attr == FASTRPC_MAP_ATTR_RETAIN_IOVA &&
		req->attr == FASTRPC_MAP_ATTR_DEFAULT) {
		map->attr = req->attr;
	}

	/*
	 * Reset return virtual address of DSP, to return
	 * failure on multiple unmap requests of same FD.
	 */
	map->raddr = 0;
	fastrpc_map_put(map);
	mutex_unlock(&fl->map_mutex);
	return 0;
}

static int fastrpc_req_mem_unmap(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_mem_unmap req;

	if (atomic_read(&fl->state) != DSP_CREATE_COMPLETE) {
		dev_err(fl->cctx->dev,
			"%s: %s: trying to unmap buf before creating remote session\n",
			__func__, current->comm);
		return -EHOSTDOWN;
	}
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;

	return fastrpc_req_mem_unmap_impl(fl, &req);
}

static int fastrpc_req_mem_map(struct fastrpc_user *fl, char __user *argp)
{
	struct fastrpc_mem_map req = {0};
	struct device *dev = NULL;
	struct fastrpc_map *map = NULL;
	int err;

	if (atomic_read(&fl->state) != DSP_CREATE_COMPLETE) {
		dev_err(fl->cctx->dev,
			"%s: %s: trying to map buf before creating remote session\n",
			__func__, current->comm);
		return -EHOSTDOWN;
	}
	if (copy_from_user(&req, argp, sizeof(req)))
		return -EFAULT;
	/*
	 * Prevent mapping backward compatible DMA handles here, as they are
	 * already mapped in the remote call.
	 */
	if (req.flags == FASTRPC_MAP_LEGACY_DMA_HANDLE)
		return -EINVAL;
	dev = fl->sctx->smmucb[DEFAULT_SMMU_IDX].dev;
	/* create SMMU mapping */
	mutex_lock(&fl->map_mutex);
	err = fastrpc_map_create(fl, req.fd, req.vaddrin, NULL, req.length, req.attrs, req.flags, &map, true);
	mutex_unlock(&fl->map_mutex);
	if (err) {
		dev_err(dev, "failed to map buffer, fd = %d\n", req.fd);
		return err;
	}
	/*
	 * Update the map state to in progress only if there is no ongoing or
	 * completed DSP mapping.
	 */
	if (atomic_cmpxchg(&map->state, FD_MAP_DEFAULT, FD_DSP_MAP_IN_PROGRESS)
		!= FD_MAP_DEFAULT) {
		err = -EALREADY;
		goto err_invoke;
	}
	map->va = (void *) (uintptr_t) req.vaddrin;
	/* map to dsp, get virtual adrress for the user*/
	err = fastrpc_mem_map_to_dsp(fl, map->fd, req.offset,
					req.flags, req.vaddrin, map->phys,
					map->size, (uintptr_t *)&req.vaddrout);
	if (err) {
		dev_err(dev, "failed to map buffer on dsp, fd = %d\n", map->fd);
		/* Revert the map state to default */
		atomic_set(&map->state, FD_MAP_DEFAULT);
		goto err_invoke;
	}

	/* update the buffer to be able to deallocate the memory on the DSP */
	map->raddr = req.vaddrout;
	/* Set the map state to complete on successful mapping */
	atomic_set(&map->state, FD_DSP_MAP_COMPLETE);
	if (copy_to_user((void __user *)argp, &req, sizeof(req)))
		/*
		 * The usercopy failed, but we can't do much about it, as this
		 * map is already mapped in the DSP and accessible for the
		 * current process. Therefore "leak" the map and rely on the
		 * process exit path to do any required cleanup.
		 */
		return -EFAULT;

	return 0;
err_invoke:
	mutex_lock(&fl->map_mutex);
	fastrpc_map_put(map);
	mutex_unlock(&fl->map_mutex);

	return err;
}

static long fastrpc_device_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct fastrpc_user *fl = (struct fastrpc_user *)file->private_data;
	struct fastrpc_channel_ctx *cctx;
	char __user *argp = (char __user *)arg;
	int err;
	int process_init = 0;
	unsigned long flags = 0;

	err = fastrpc_file_get(fl);
	if (err) {
		dev_err(fl->cctx->dev, "%s : Failed to get user-obj for %s, fl (%pK)\n",
				__func__, current->comm, fl);
		return err;
	}
	cctx = fl->cctx;
	fastrpc_channel_ctx_get(cctx);
	spin_lock_irqsave(&cctx->lock, flags);
	if (atomic_read(&cctx->teardown)) {
		/* If subsystem already going thru SSR, then fail ioctl immediately */
		spin_unlock_irqrestore(&cctx->lock, flags);
		fastrpc_channel_ctx_put(cctx);
		fastrpc_file_put(fl, false);
		return -EPIPE;
	}
	/*
	 * Update invoke count to block SSR handling thread from cleaning up
	 * the channel resources, while it is still being used by this thread.
	 */
	fastrpc_channel_update_invoke_cnt(cctx, true);
	spin_unlock_irqrestore(&cctx->lock, flags);

	switch (cmd) {
	case FASTRPC_IOCTL_INVOKE:
		trace_fastrpc_msg("invoke: begin");
		fastrpc_timeline_record(2, fl->tgid_app, fl->fastrpc_timeline_obj);
		err = fastrpc_invoke(fl, argp);
		fastrpc_timeline_record(42, fl->tgid_app, fl->fastrpc_timeline_obj);
		trace_fastrpc_msg("invoke: end");
		break;
	case FASTRPC_IOCTL_MULTIMODE_INVOKE:
		fastrpc_timeline_record(2, fl->tgid_app, fl->fastrpc_timeline_obj);
		err = fastrpc_multimode_invoke(fl, argp);
		fastrpc_timeline_record(42, fl->tgid_app, fl->fastrpc_timeline_obj);
		break;
	case FASTRPC_IOCTL_INIT_ATTACH:
		err = fastrpc_init_attach(fl, ROOT_PD);
		fastrpc_send_cpuinfo_to_dsp(fl);
		process_init = 1;
		break;
	case FASTRPC_IOCTL_INIT_ATTACH_SNS:
		err = fastrpc_init_attach(fl, SENSORS_STATICPD);
		process_init = 1;
		break;
	case FASTRPC_IOCTL_INIT_ATTACH2:
		err = fastrpc_init_attach2(fl, ROOT_PD, argp);
		fastrpc_send_cpuinfo_to_dsp(fl);
		process_init = 1;
		break;
	case FASTRPC_IOCTL_INIT_CREATE_STATIC:
		err = fastrpc_init_create_static_process(fl, argp);
		process_init = 1;
		break;
	case FASTRPC_IOCTL_INIT_CREATE:
		err = fastrpc_init_create_process(fl, argp);
		process_init = 1;
		break;
	case FASTRPC_IOCTL_ALLOC_DMA_BUFF:
		err = fastrpc_dmabuf_alloc(fl, argp);
		break;
	case FASTRPC_IOCTL_MMAP:
		err = fastrpc_req_mmap(fl, argp);
		break;
	case FASTRPC_IOCTL_MUNMAP:
		err = fastrpc_req_munmap(fl, argp);
		break;
	case FASTRPC_IOCTL_MEM_MAP:
		err = fastrpc_req_mem_map(fl, argp);
		break;
	case FASTRPC_IOCTL_MEM_UNMAP:
		err = fastrpc_req_mem_unmap(fl, argp);
		break;
	case FASTRPC_IOCTL_GET_DSP_INFO:
		err = fastrpc_get_dsp_info(fl, argp);
		break;
	case FASTRPC_IOCTL_GET_TIMELINE_BUFFER:
		err = fastrpc_get_timeline_buffer(fl, argp);
		break;
	case FASTRPC_IOCTL_NPU_PRIORITY_WORKINFO: {
		struct fastrpc_ioctl_npu_priority_workinfo __user *uargs =
			(struct fastrpc_ioctl_npu_priority_workinfo __user *)argp;
		__u32 rsvd;
		__u32 reserved[16];

		/*
	 	 * TODO: Add check to ensure that NPU HAL service is the only process that is
	 	 * allowed to make this ioctl call.
	 	 * ioctl call from any other application needs to be rejected.
	 	 * For now, add this rudimentary check to block most 3rd-party apps from making
	 	 * this ioctl. This is NOT expected to block 3rd party-apps and is only a
	 	 * temporary placeholder.
	 	 */
		if (fl->tgid >= THIRD_PARTY_APP_PID) {
			err = -EPERM;
			break;
		}
		if (get_user(rsvd, &uargs->rsvd) ||
		    copy_from_user(reserved, uargs->reserved, sizeof(reserved))) {
			err = -EFAULT;
			break;
		}
		if (memchr_inv(&rsvd, 0, sizeof(rsvd)) ||
		    memchr_inv(reserved, 0, sizeof(reserved))) {
			err = -EINVAL;
			break;
		}
		/*
 		 * NPU workinfo case added as a placeholder.
 		 * This will be implemented in a separate change.
 		 * The ioctl is a combined ioctl for priority sending and workinfo notification
 		 * retrieval to ensure strict ordering of priority updates and work events.
 		 */
		err = fastrpc_npu_priority_workinfo(fl, file, argp);
		break;
	}
	default:
		err = -ENOTTY;
		break;
	}

	if (process_init && !err) {
		err = fastrpc_device_create(fl);
		if (!err && IS_DYNAMIC_PD(fl->pd_type)) {
			fastrpc_sysfs_notify_pids(cctx->domain);
		}
	}
	spin_lock_irqsave(&cctx->lock, flags);
	fastrpc_channel_update_invoke_cnt(cctx, false);
	spin_unlock_irqrestore(&cctx->lock, flags);
	fastrpc_channel_ctx_put(fl->cctx);
	fastrpc_file_put(fl, false);
	return err;
}

int fastrpc_init_privileged_gids(struct device *dev, char *prop_name,
						struct gid_list *gidlist)
{
	int err = 0;
	u32 len = 0, i;
	u32 *gids = NULL;

	if (!of_find_property(dev->of_node, prop_name, &len))
		return 0;
	if (len == 0)
		return 0;

	len /= sizeof(u32);
	gids = kcalloc(len, sizeof(u32), GFP_KERNEL);
	if (!gids)
		return -ENOMEM;

	for (i = 0; i < len; i++) {
		err = of_property_read_u32_index(dev->of_node, prop_name,
								i, &gids[i]);
		if (err) {
			dev_err(dev, "%s: failed to read GID %u\n",
					__func__, i);
			goto read_error;
		}
		dev_info(dev, "adsprpc: %s: privileged GID: %u\n", __func__, gids[i]);
	}
	sort(gids, len, sizeof(*gids), uint_cmp_func, NULL);
	gidlist->gids = gids;
	gidlist->gidcount = len;

	return 0;
read_error:
	kfree(gids);
	return err;
}

union fastrpc_dev_param {
	struct fastrpc_dev_map_dma *map;
	struct fastrpc_dev_unmap_dma *unmap;
	struct fastrpc_dev_get_hlos_pid *hpid;
};
   /*
	* fastrpc_dev_map_dma() - Function to map buffers mapped on DSP.
	* @arg1: client instance of fastrpc_device struct
	* @arg2: invoke param
	*
	* fastrpc_dev_map_dma is used to map buffers mapped on DSP
	*
	*
	* Return: 0 on success.
	*
	*/
static long fastrpc_dev_map_dma(struct fastrpc_device *dev,
			unsigned long invoke_param)
{
	int err = 0;
	bool is_cnt_updated = false;
	union fastrpc_dev_param p;
	struct fastrpc_user *fl = NULL;
	struct fastrpc_map *map = NULL;
	uintptr_t raddr = 0;
	unsigned long irq_flags = 0;
	struct fastrpc_channel_ctx * cctx = NULL;
	spinlock_t *glock = &g_frpc.glock;

	p.map = (struct fastrpc_dev_map_dma *)invoke_param;

	spin_lock_irqsave(glock, irq_flags);
	if (!dev || dev->dev_close) {
		err = -ESRCH;
		pr_err("%s : bad dev or device is already closed", __func__);
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}

	fl = dev->fl;
	if (!fl) {
		err = -EBADF;
		pr_err("%s : bad fl", __func__);
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}
	err = fastrpc_file_get(fl);
	if (err) {
		pr_err("%s : Failed to get user-obj for fl (%pK)\n",
				__func__, fl);
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}
	cctx = fl->cctx;
	fastrpc_channel_ctx_get(cctx);
	fl->is_dma_invoke_pend = true;
	spin_unlock_irqrestore(glock, irq_flags);

	spin_lock_irqsave(&cctx->lock, irq_flags);
	if (atomic_read(&cctx->teardown)) {
		spin_unlock_irqrestore(&cctx->lock, irq_flags);
		err = -EPIPE;
		goto error;
	} else {
		/*
		 * Update invoke count to block SSR handling thread
		 * from cleaning up the channel resources, while it
		 * is stillbeing used by this thread.
		 */
		fastrpc_channel_update_invoke_cnt(cctx, true);
		is_cnt_updated = true;
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);

	/* Map DMA buffer on SMMU device*/
	mutex_lock(&fl->map_mutex);
	err = fastrpc_map_create(fl, -1, 0, p.map->buf,
				p.map->size, p.map->attrs,
				ADSP_MMAP_DMA_BUFFER, &map, true);
	mutex_unlock(&fl->map_mutex);
	if (err)
		goto error;
	/*
	 * Update the map state to in progress only if there is no ongoing or
	 * completed DSP mapping.
	 */
	if (atomic_cmpxchg(&map->state, FD_MAP_DEFAULT, FD_DSP_MAP_IN_PROGRESS)
		!= FD_MAP_DEFAULT) {
		err = -EALREADY;
		goto error;
	}
	/* Map DMA buffer on DSP*/

	err = fastrpc_mem_map_to_dsp(fl, -1, 0, map->flags, 0, map->phys, map->size, &raddr);
	if (err) {
		pr_err("%s : failed to map buffer on DSP ", __func__);
		/* Revert the map state to map default */
		atomic_set(&map->state, FD_MAP_DEFAULT);
		goto error;
	}
	map->raddr = raddr;
	p.map->v_dsp_addr = raddr;
	/* Set the map state to complete on successful mapping */
	atomic_set(&map->state, FD_DSP_MAP_COMPLETE);
error:
	if (err && map) {
		mutex_lock(&fl->map_mutex);
		fastrpc_map_put(map);
		mutex_unlock(&fl->map_mutex);
	}

	spin_lock_irqsave(&cctx->lock, irq_flags);
	if (fl) {
		if (atomic_read(&fl->state) >= DSP_EXIT_START && fl->is_dma_invoke_pend) {
			/*
			 * If process exit has already started and is waiting for this invoke
			 * to complete, then unblock it.
			 */
			complete(&fl->dma_invoke);
		}
		fl->is_dma_invoke_pend = false;
	}
	if (is_cnt_updated) {
		fastrpc_channel_update_invoke_cnt(cctx, false);
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);
	fastrpc_channel_ctx_put(cctx);
	fastrpc_file_put(fl, false);
	return err;
}
   /*
	* fastrpc_dev_unmap_dma() - Function to unmap buffers mapped on DSP.
	* @arg1: client instance of fastrpc_device struct
	* @arg2: invoke param
	*
	* fastrpc_dev_unmap_dma is used to unmap buffers mapped on DSP
	*
	*
	* Return: 0 on success.
	*
	*/
static long fastrpc_dev_unmap_dma(struct fastrpc_device *dev,
			unsigned long invoke_param)
{
	int err = 0;
	bool is_cnt_updated = false;
	union fastrpc_dev_param p;
	struct fastrpc_user *fl = NULL;
	struct fastrpc_map *map = NULL;
	unsigned long irq_flags = 0;
	struct fastrpc_channel_ctx * cctx = NULL;
	spinlock_t *glock = &g_frpc.glock;

	p.unmap = (struct fastrpc_dev_unmap_dma *)invoke_param;

	spin_lock_irqsave(glock, irq_flags);
	if (!dev || dev->dev_close) {
		pr_err("%s : bad dev or device is already closed", __func__);
		err = -ESRCH;
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}
	fl = dev->fl;
	if (!fl) {
		err = -EBADF;
		pr_err("%s : bad fl ", __func__);
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}
	err = fastrpc_file_get(fl);
	if (err) {
		pr_err("%s : Failed to get user-obj for fl (%pK)\n",
				__func__, fl);
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}
	cctx = fl->cctx;
	fastrpc_channel_ctx_get(cctx);
	fl->is_dma_invoke_pend = true;
	spin_unlock_irqrestore(glock, irq_flags);

	spin_lock_irqsave(&cctx->lock, irq_flags);
	if (atomic_read(&cctx->teardown)) {
		spin_unlock_irqrestore(&cctx->lock, irq_flags);
		err = -EPIPE;
		goto error;
	} else {
		/*
		 * Update invoke count to block SSR handling thread
		 * from cleaning up the channel resources, while it
		 * is stillbeing used by this thread.
		 */
		fastrpc_channel_update_invoke_cnt(cctx, true);
		is_cnt_updated = true;
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);
	mutex_lock(&fl->map_mutex);
	err = fastrpc_map_lookup(fl, -1, 0, 0, p.unmap->buf,
				ADSP_MMAP_DMA_BUFFER, &map,
				false, FASTRPC_MAP_ATTR_DEFAULT);
	 /*
	  * Check if DSP mapping is complete, then move the state to
	  * unmap in progress only if there is no other ongoing unmap.
	  */
	if (!err && atomic_cmpxchg(&map->state, FD_DSP_MAP_COMPLETE,
		FD_DSP_UNMAP_IN_PROGRESS) != FD_DSP_MAP_COMPLETE)
		err = -EALREADY;
	mutex_unlock(&fl->map_mutex);
	if (err)
		goto error;
	/* Un-map DMA buffer on DSP*/
	err = fastrpc_req_munmap_dsp(fl, map->raddr, map->size);
	if (err) {
		pr_err("Unmap on DSP failed for buf phy:0x%llx, raddr:0x%llx, size:0x%llx\n",
			map->phys, map->raddr, map->size);
		/* Revert the map state to map complete */
		atomic_set(&map->state, FD_DSP_MAP_COMPLETE);
		goto error;
	}
	/* Set the map state to default on successful unmapping */
	atomic_set(&map->state, FD_MAP_DEFAULT);
	mutex_lock(&fl->map_mutex);
	fastrpc_map_put(map);
	mutex_unlock(&fl->map_mutex);

error:
	spin_lock_irqsave(&cctx->lock, irq_flags);
	if (fl) {
		if (atomic_read(&fl->state) >= DSP_EXIT_START && fl->is_dma_invoke_pend) {
			/*
			 * If process exit has already started and is waiting for this invoke
			 * to complete, then unblock it.
			 */
			complete(&fl->dma_invoke);
		}
		fl->is_dma_invoke_pend = false;
	}
	if (is_cnt_updated) {
		fastrpc_channel_update_invoke_cnt(cctx, false);
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);
	fastrpc_channel_ctx_put(cctx);
	fastrpc_file_put(fl, false);
	return err;
}
   /*
	* fastrpc_dev_get_hlos_pid() - Function to get hlos pid.
	* @arg1: client instance of fastrpc_device struct.
	* @arg2: invoke param.
	*
	* fastrpc_dev_get_hlos_pid is used to get hlos id
	*
	* Return: void.
	*
	*/
static long fastrpc_dev_get_hlos_pid(struct fastrpc_device *dev,
			unsigned long invoke_param)
{
	int err = 0;
	union fastrpc_dev_param p;
	struct fastrpc_user *fl = NULL;
	unsigned long irq_flags = 0;
	struct fastrpc_channel_ctx * cctx = NULL;
	spinlock_t *glock = &g_frpc.glock;

	spin_lock_irqsave(glock, irq_flags);
	if (!dev  || dev->dev_close) {
		pr_err("%s : bad dev or device is already closed", __func__);
		err = -ESRCH;
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}

	fl = dev->fl;
	if (!fl) {
		err = -EBADF;
		pr_err("%s : bad fl ", __func__);
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}
	err = fastrpc_file_get(fl);
	if (err) {
		pr_err("%s: Failed to get user-obj for fl (%pK)\n",
				__func__, fl);
		spin_unlock_irqrestore(glock, irq_flags);
		return err;
	}
	cctx = fl->cctx;
	fastrpc_channel_ctx_get(cctx);

	p.hpid = (struct fastrpc_dev_get_hlos_pid *)invoke_param;
	p.hpid->hlos_pid = fl->tgid;
	spin_unlock_irqrestore(glock, irq_flags);
	fastrpc_channel_ctx_put(cctx);
	fastrpc_file_put(fl, false);
	return err;
}
   /*
	* fastrpc_driver_invoke() - Invocation function for client drivers.
	* @arg1: client instance of fastrpc_device struct
	* @arg2: invoke number
	* @arg3: invoke param
	*
	* fastrpc_driver_invoke is exposed to the client drivers to make invoke
	* calls. Clients can map and unmap buffers on dsp using invoke calls.
	* function can be called with an instance of the fastrpc_device instance,
	* invocation number and corresponding invoke params.
	*
	*
	* Return: 0 on success.
	*
	*/
long fastrpc_driver_invoke(struct fastrpc_device *dev, unsigned int invoke_num,
			unsigned long invoke_param)
{
	int err = 0;

	switch (invoke_num) {
	case FASTRPC_DEV_MAP_DMA:
		err = fastrpc_dev_map_dma(dev, invoke_param);
		break;
	case FASTRPC_DEV_UNMAP_DMA:
		err = fastrpc_dev_unmap_dma(dev, invoke_param);
		break;
	case FASTRPC_DEV_GET_HLOS_PID:
		err = fastrpc_dev_get_hlos_pid(dev, invoke_param);
		break;
	default:
		err = -ENOTTY;
		break;
	}

	return err;
}
EXPORT_SYMBOL_GPL(fastrpc_driver_invoke);

   /*
	* fastrpc_device_create() - Create an instance of fastrpc_device.
	* @arg1: fastrpc_user instance corresponding to the process.
	*
	* fastrpc_device_create will create an instance of struct fastrpc_device
	* for each process
	*
	*
	* Return: 0 on success, error code on failure.
	*
	*/
static int fastrpc_device_create(struct fastrpc_user *fl)
{
	int err = 0;
	struct fastrpc_device *frpc_dev = NULL;

	frpc_dev = kzalloc(sizeof(*frpc_dev), GFP_KERNEL);
	if (!frpc_dev) {
		err = -ENOMEM;
		return err;
	}

	frpc_dev->fl = fl;
	frpc_dev->handle = fl->tgid_frpc;
	fl->device = frpc_dev;
	atomic_set(&fl->state, DSP_CREATE_COMPLETE);
	return err;
}

   /*
	* fastrpc_driver_unregister() - Function to unregister client drivers.
	* @arg1: client instance of fastrpc_driver struct
	*
	* fastrpc_driver_unregister is used to unregister the client drivers
	* from fastrpc driver.
	*
	* Context: Acquires channel context spin-lock and glock
	*
	* Return: void.
	*
	*/
void fastrpc_driver_unregister(struct fastrpc_driver *frpc_driver){

	struct fastrpc_device *frpc_dev = NULL;
	unsigned long irq_flags = 0, flags = 0;
	struct fastrpc_channel_ctx * cctx = NULL;
	struct fastrpc_user *fl = NULL;
	spinlock_t *glock = &g_frpc.glock;

	if (!frpc_driver) {
		pr_err("%s : invalid driver passed", __func__);
		return;
	}

	spin_lock_irqsave(glock, irq_flags);
	frpc_dev = (struct fastrpc_device *)frpc_driver->device;
	if (!frpc_dev) {
		spin_unlock_irqrestore(glock, irq_flags);
		pr_err("passed invalid driver, fastrpc device not present");
		return;
	}

	// If device is already closed, free the device
	if (frpc_dev->dev_close) {
		frpc_driver->device = NULL;
		spin_unlock_irqrestore(glock, irq_flags);
		kfree(frpc_dev);
		pr_info("Un-registering fastrpc driver with handle 0x%x\n",
			frpc_driver->handle);
		return;
	}

	fl = frpc_dev->fl;
	if (!fl) {
		spin_unlock_irqrestore(glock, irq_flags);
		pr_err("passed invalid driver, invalid process");
		return;
	}
	cctx = frpc_dev->fl->cctx;
	fastrpc_channel_ctx_get(cctx);

	spin_lock_irqsave(&cctx->lock, flags);
	list_del_init(&frpc_driver->hn);
	spin_unlock_irqrestore(&cctx->lock, flags);
	spin_unlock_irqrestore(glock, irq_flags);

	fastrpc_channel_ctx_put(cctx);

	pr_info("Un-registering fastrpc driver with handle 0x%x\n",
			frpc_driver->handle);
}
EXPORT_SYMBOL_GPL(fastrpc_driver_unregister);

   /*
	* fastrpc_driver_register() - Function to register client drivers.
	* @arg1: client instance of fastrpc_driver struct.
	*
	* fastrpc_driver_register is used to register client drivers with
	* fastrpc driver. Clients will pass instance of fastrpc_driver struct.
	* The instance will contain unique id corresponding to a process. Function
	* will iterate through channel context to find a match. If match is found,
	* probe function provided in the input struct will be called. During probe
	* we will share fastrpc_device instance as a handle which can be used by the
	* client driver while making invoke calls.
	*
	* Context: Acquires channel context spin-lock to iterate through
	*          contexts.
	* Return: 0 on success. Corresponding error value on failure.
	*
	*/

int fastrpc_driver_register(struct fastrpc_driver *frpc_driver)
{
	int err = 0, i = 0;
	unsigned long irq_flags = 0;
	struct fastrpc_user *user = NULL;
	struct fastrpc_channel_ctx *cctx = NULL;
	struct fastrpc_domain *domain = NULL;

	if(frpc_driver == NULL) {
		pr_err("%s : invalid registraion request", __func__);
		return -EINVAL;
	}

	/* Set to NULL to avoid stale values */
	frpc_driver->device = NULL;

	/*
	 * Iterate through all channel contexts to find the process
	 * requested by the client driver.
	 */
	hash_for_each(g_frpc.fastrpc_domains_table, i, domain, node) {
		cctx = domain->cctx;
		if (!cctx)
			continue;

		spin_lock_irqsave(&cctx->lock, irq_flags);
		list_for_each_entry(user, &cctx->users, user) {
			if (user->tgid_frpc == frpc_driver->handle) {
				err = fastrpc_file_get(user);
				if (err) {
					dev_warn(cctx->dev, "Warning: %s: user-obj for fl (%pK) being released\n",
						__func__, user);
					break;
				}
				goto process_found;
			}
		}
		spin_unlock_irqrestore(&cctx->lock, irq_flags);
	}
	pr_err("%s: no client found for handle 0x%x",
		__func__, frpc_driver->handle);
	return -ESRCH;

process_found:
	if(atomic_read(&user->state) >= DSP_EXIT_START) {
		spin_unlock_irqrestore(&cctx->lock, irq_flags);
		pr_err("%s : process already exited", __func__);
		fastrpc_file_put(user, false);
		return -ESRCH;
	}

	frpc_driver->device = (struct device *)user->device;
	list_add_tail(&frpc_driver->hn, &user->fastrpc_drivers);
	spin_unlock_irqrestore(&cctx->lock, irq_flags);
	/* Execute the probe fn. of the client driver if matching process found */
	frpc_driver->probe(user->device);
	pr_info("fastrpc driver registered with handle 0x%x\n", frpc_driver->handle);
	fastrpc_file_put(user, false);
	return err;
}
EXPORT_SYMBOL_GPL(fastrpc_driver_register);
void fastrpc_notify_users(struct fastrpc_user *user)
{
	struct fastrpc_invoke_ctx *ctx;
	struct fastrpc_user *fl;

	spin_lock(&user->lock);
	list_for_each_entry(ctx, &user->pending, node) {
		fl = ctx->fl;
		/*
		 * After audio or ois PDR, skip notifying the pending kill call,
		 * as the DSP guestOS may still be processing and might result
		 * improper access issues. But in case of SSR cleanup pending
                 * kill calls as well.
		 */
		if (atomic_read(&fl->state) >= DSP_EXIT_START &&
                        !IS_SSR(fl) && IS_PDR(fl) &&
			fl->pd_type != SENSORS_STATICPD &&
			ctx->msg.handle == FASTRPC_INIT_HANDLE)
			continue;
		ctx->retval = -EPIPE;
		ctx->is_work_done = true;
		trace_fastrpc_context_complete(ctx->fl->cctx->domain_id, (uint64_t)ctx,
			ctx->retval, ctx->pid, ctx->pid, ctx->sc);
		complete(&ctx->work);
	}
	list_for_each_entry(ctx, &user->interrupted, node) {
		ctx->retval = -EPIPE;
		ctx->is_work_done = true;
		trace_fastrpc_context_complete(ctx->fl->cctx->domain_id, (uint64_t)ctx,
			ctx->retval, ctx->pid, ctx->pid, ctx->sc);
		complete(&ctx->work);
	}

	fastrpc_dspsignal_cancel_all(user);
#if FRPC_RING_BUFFER_ENABLED
	if (user->pd_type == ROOT_PD &&
		user->cctx->domain->type == FASTRPC_NSP) {
		WRITE_ONCE(user->logger_exit, true);
		wake_up_interruptible(&user->cctx->log.wq);
	}
#endif
	spin_unlock(&user->lock);
}


/*
 * fastrpc_notify_pdr_drivers() - Function to notify userspace on
 * static PD down
 * @arg1: channel context
 * @arg2: name of the process to send the notification
 */
static void fastrpc_notify_pdr_drivers(struct fastrpc_channel_ctx *cctx,
	int pid)
{
	struct fastrpc_user *fl;
	unsigned long flags;
	int err = 0;

	spin_lock_irqsave(&cctx->lock, flags);
	list_for_each_entry(fl, &cctx->users, user) {
		err = fastrpc_file_get(fl);
		if (err) {
			dev_warn(cctx->dev, "Warning: %s: user-obj for fl (%pK) being released\n",
				__func__, fl);
			continue;
		}
		if (fl->spd_id == pid)
			fastrpc_notify_users(fl);
		fastrpc_file_put(fl, false);
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
}

/*
 * fastrpc_populate_static_pd_session() - Populate the static
 * pd structure on PD up
 * @arg1: channel context
 * @arg2: pid of the process to populate
 */
static int fastrpc_populate_static_pd_session(struct fastrpc_channel_ctx *cctx,
	int pid)
{
	int i = 0, err = 0;
	unsigned long flags = 0;
	struct fastrpc_static_pd *spd = NULL;

	spin_lock_irqsave(&cctx->lock, flags);
	// Re-use the session, if found to retain the pdr count
	for (i = 0; i < FASTRPC_MAX_SPD; i++) {
		if (cctx->spd[i].spd_id == pid)
			break;
	}
	if (i < FASTRPC_MAX_SPD)
		goto spd_session_found;
	// Use un-used session to populate static PD
	for (i = 0; i < FASTRPC_MAX_SPD; i++) {
		if (cctx->spd[i].used)
			continue;

		break;
	}
	if (i >= FASTRPC_MAX_SPD) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		return -EUSERS;
	}

spd_session_found:
	spd = &cctx->spd[i];
	spd->spd_id = pid;
	spd->cctx = cctx;
	atomic_set(&spd->ispdup, 1);
	spd->used = true;

	/* Ignore PDR callback */
	atomic_set(&spd->spd_status_notif, 1);
	spin_unlock_irqrestore(&cctx->lock, flags);
	return err;
}

/*
 * fastrpc_reset_staticpd_session() - Reset static PD variables on PD down
 * @arg1: static pd structure
 */
static void fastrpc_reset_staticpd_session(struct fastrpc_static_pd *spd)
{
	struct fastrpc_channel_ctx *cctx = spd->cctx;

	spd->pdrcount++;
	atomic_set(&spd->ispdup, 0);
	atomic_set(&spd->is_attached, 0);

	/*
	 * Audio PD status tracked using variable staticpd_status.
	 * Used for enabling remoteheap only for Audio PD.
	 */
	if (spd->spd_id == AUDIO_STATIC_ID)
		cctx->staticpd_status = false;
}

/*
 * get_static_id_from_pd_name() - Get pid of the static PD given the name
 * @arg1: static pd name
 */
static int get_static_id_from_pd_name(char *name) {

	if(!strncmp(name, AUDIOPD, strlen(AUDIOPD)))
		return AUDIO_STATIC_ID;
	else if(!strncmp(name, SENSORSPD, strlen(SENSORSPD)))
		return SENSORS_STATIC_ID;
	else if(!strncmp(name, OISPD, strlen(OISPD)))
		return OIS_STATIC_ID;
	else
		return INVALID_STATIC_ID;
}

/*
 * fastrpc_depopulate_static_pd_session() - De-populate the static pd structure on PD down
 * @arg1: channel context
 * @arg2: notif struct sent on PD down
 */
static int fastrpc_depopulate_static_pd_session(struct fastrpc_channel_ctx *cctx,
	struct dsp_notif_rsp *notif)
{
	int i, err = 0;
	unsigned long flags;

	spin_lock_irqsave(&cctx->lock, flags);
	for (i = 0; i < FASTRPC_MAX_SPD ; i++) {
		if( cctx->spd[i].spd_id == notif->pid)
			break;
	}
	if (i >= FASTRPC_MAX_SPD) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		return -EUSERS;
	}
	fastrpc_reset_staticpd_session(&cctx->spd[i]);
	spin_unlock_irqrestore(&cctx->lock, flags);
	return err;
}

/*
 * fastrpc_pdr_notif() - Function which handles static PD notification from DSP
 * @arg1: channel context
 * @arg2: notif struct sent on PD down
 */
static void fastrpc_pdr_notif(struct fastrpc_channel_ctx *cctx,
	struct dsp_notif_rsp *notif)
{

	if (!cctx || !notif)
		return;

	switch (notif->status) {
	case FASTRPC_USERPD_EXIT:
		pr_info("%s: Static PD with pid %d is down for PDR on domain %d\n",
			__func__, notif->pid, cctx->domain->id);
		fastrpc_depopulate_static_pd_session(cctx, notif);
		fastrpc_notify_pdr_drivers(cctx, notif->pid);
		break;
	case FASTRPC_USERPD_UP:
		pr_info("%s: Static PD with pid %d is up for PDR on domain %d\n",
			__func__, notif->pid, cctx->domain->id);
		fastrpc_populate_static_pd_session(cctx, notif->pid);
		break;
	default:
		pr_info("%s: Invalid status %d\n", __func__, notif->status);
		break;
	}
	return;
}

static void fastrpc_pdr_cb(int state, char *service_path, void *priv)
{
	struct fastrpc_static_pd *spd = (struct fastrpc_static_pd *)priv;
	struct fastrpc_channel_ctx *cctx;
	unsigned long flags;

	if (!spd)
		return;

	cctx = spd->cctx;

	/*
	 * Ignore PDR callback if internal static pd notif
	 * mechanism is supported
	 */
	if (atomic_read(&spd->spd_status_notif))
		return;

	switch (state) {
	case SERVREG_SERVICE_STATE_DOWN:
		pr_info("fastrpc: %s: %d (%s) is down for PDR on %s\n",
			__func__, spd->spd_id,
			spd->servloc_name,
			cctx->domain->name);
		spin_lock_irqsave(&cctx->lock, flags);
		fastrpc_reset_staticpd_session(spd);

		spin_unlock_irqrestore(&cctx->lock, flags);
		if (!strcmp(spd->servloc_name,
				AUDIO_PDR_SERVICE_LOCATION_CLIENT_NAME))
			cctx->staticpd_status = false;

		fastrpc_notify_pdr_drivers(cctx, spd->spd_id);
		break;
	case SERVREG_SERVICE_STATE_UP:
		pr_info("fastrpc: %s: %d (%s) is up for PDR on %s\n",
			__func__, spd->spd_id,
			spd->servloc_name,
			cctx->domain->name);
		atomic_set(&spd->ispdup, 1);
		break;
	default:
		break;
	}
	return;
}

static inline void populate_dump_metadata(struct fastrpc_dump_info *entry,
		u64 offset, u64 size, enum fastrpc_dump_type type, u64 addr)
{
	entry->offset = offset;
	entry->size = size;
	entry->type = type;
	entry->phys = addr;
}

static int fastrpc_get_mem_content(struct dma_buf *dbuf, void *dst, size_t size)
{
	struct iosys_map map = {0};
	int ret = 0;

	if (!dbuf || !dst || !size || (size > dbuf->size))
		return -EINVAL;

	ret = dma_buf_begin_cpu_access(dbuf, DMA_FROM_DEVICE);
	if (ret)
		return ret;

	ret = dma_buf_vmap(dbuf, &map);
	if (ret)
		goto bail;

	if (!map.vaddr) {
		ret = -EFAULT;
		goto bail;
	}

	memcpy(dst, map.vaddr, size);

bail:
	if(map.vaddr)
		dma_buf_vunmap(dbuf, &map);

	dma_buf_end_cpu_access(dbuf, DMA_FROM_DEVICE);
	return ret;
}

void frpc_coredump(struct fastrpc_channel_ctx *cctx,
	struct list_head *active_users_list)
{
	int iter = 0, err = 0;
	bool scm_done = false;
	struct device *dev = cctx->dev;
	char *pos = NULL, *dump = NULL;
	u64 total_size = 0, offset = 0;
	struct fastrpc_user *user, *n;
	struct seq_file *s_file = NULL;
	struct seq_buf gmsgbuf;
	struct fastrpc_dump_info *dinfo;
	struct fastrpc_buf *buf, *b;
	struct fastrpc_map *map;
	unsigned long flags;
	struct list_head lgmaps_list;

	INIT_LIST_HEAD(&lgmaps_list);

	spin_lock_irqsave(&cctx->lock, flags);
	list_for_each_entry_safe(buf, b, &cctx->gmaps, node) {
		total_size += buf->size;
		list_del(&buf->node);
		list_add_tail(&buf->node, &lgmaps_list);
	}
	spin_unlock_irqrestore(&cctx->lock, flags);

	list_for_each_entry_safe(user, n, active_users_list, active_user_ssr) {
		total_size += DBG_FS_SIZE;

		if (!user->is_faulted)
			continue;

		if (user->init_mem)
			total_size += user->init_mem->size;

		if (user->dbglogbuf && user->dbglogbuf->virt)
			total_size += user->dbglogbuf->size;

		list_for_each_entry(buf, &user->mmaps, node) {
			if (buf->flags == ADSP_MMAP_ADD_PAGES_TSTACK)
				total_size += buf->size;
		}

		list_for_each_entry(map, &user->maps, node) {
			if (map->flags == ADSP_MMAP_ADD_PAGES_TSTACK)
				total_size += map->size;
		}
	}

	total_size += RPMSG_LOG_SIZE;
	total_size += NUM_DUMPED * sizeof(struct fastrpc_dump_info);
	dump = vmalloc(total_size);
	if (!dump) {
		err = -ENOMEM;
		goto bail;
	}
	pos = dump;
	dinfo = (struct fastrpc_dump_info *)dump;
	pos += NUM_DUMPED * sizeof(struct fastrpc_dump_info);
	offset += NUM_DUMPED * sizeof(struct fastrpc_dump_info);

	list_for_each_entry_safe(buf, b, &lgmaps_list, node) {
		err = fastrpc_remote_heap_unassign(cctx, buf);
		list_del(&buf->node);
		if (err)
			continue;
		if ((dump + total_size) - pos >= buf->size) {
			memcpy(pos, buf->virt, buf->size);
			populate_dump_metadata(&dinfo[iter], offset, buf->size,
				CMA, buf->phys);
			pos += buf->size;
			iter += 1;
			offset += buf->size;
		} else {
			pr_err("%s: Insufficient space for gmaps buf, aborting coredump\n", __func__);
			err = -EFAULT;
			goto bail;
		}
		__fastrpc_buf_free(buf);
	}
	scm_done = true;

	if ((dump + total_size) - pos >= RPMSG_LOG_SIZE) {
		seq_buf_init(&gmsgbuf, pos, RPMSG_LOG_SIZE);
		print_rpmsg_glink_logs(&gmsgbuf, &cctx->gmsg_log);
		if (seq_buf_has_overflowed(&gmsgbuf))
			pr_warn("%s: RPMSG log buffer overflow, logs truncated\n", __func__);
		populate_dump_metadata(&dinfo[iter], offset, RPMSG_LOG_SIZE, DEBUGFS, 0);
		pos    += RPMSG_LOG_SIZE;
		iter   += 1;
		offset += RPMSG_LOG_SIZE;
	} else {
		pr_err("%s: Insufficient space for RPMSG logs, aborting coredump\n", __func__);
		err = -EFAULT;
		goto bail;
	}

	s_file = kzalloc(sizeof(*s_file), GFP_KERNEL);
	if (!s_file) {
		err = -ENOMEM;
		goto bail;
	}
	s_file->size = DBG_FS_SIZE;

	list_for_each_entry_safe(user, n, active_users_list, active_user_ssr) {
		s_file->private = user;
		if ((dump + total_size) - pos >= DBG_FS_SIZE)
			s_file->buf = pos;
		else {
			pr_err("%s: Insufficient space for debugfs, aborting coredump\n", __func__);
			err = -EFAULT;
			goto bail;
		}
		fastrpc_debugfs_show(s_file, NULL);
		if (s_file->buf) {
			memcpy(pos, s_file->buf, DBG_FS_SIZE);
			populate_dump_metadata(&dinfo[iter], offset, DBG_FS_SIZE, DEBUGFS, 0);
			pos += DBG_FS_SIZE;
			iter += 1;
			offset += DBG_FS_SIZE;
		}

		if (!user->is_faulted)
			continue;

		if (user->init_mem) {
			if ((dump + total_size) - pos >= user->init_mem->size) {
				memcpy(pos, user->init_mem->virt, user->init_mem->size);
				populate_dump_metadata(&dinfo[iter], offset, user->init_mem->size,
					INIT_MEM, user->init_mem->phys);
				pos += user->init_mem->size;
				iter += 1;
				offset += user->init_mem->size;
			} else {
				pr_err("%s: Insufficient space for init_mem, aborting coredump\n", __func__);
				err = -EFAULT;
				goto bail;
			}
		}
		if (user->dbglogbuf && user->dbglogbuf->virt && user->dbglogbuf->size > 0) {
			if ((dump + total_size) - pos >= user->dbglogbuf->size) {
				memcpy(pos, user->dbglogbuf->virt, user->dbglogbuf->size);
				populate_dump_metadata(&dinfo[iter], offset, user->dbglogbuf->size,
					DEBUGFS, user->dbglogbuf->phys);
				pos += user->dbglogbuf->size;
				iter += 1;
				offset += user->dbglogbuf->size;
			} else {
				pr_err("%s: Insufficient space for dbglogbuf, aborting coredump\n", __func__);
				err = -EFAULT;
				goto bail;
			}
		}

		list_for_each_entry(buf, &user->mmaps, node) {
			if (buf->flags == ADSP_MMAP_ADD_PAGES_TSTACK) {
				if ((dump + total_size) - pos >= buf->size) {
					memcpy(pos, buf->virt, buf->size);
					populate_dump_metadata(&dinfo[iter], offset, buf->size,
						TSTACK_MEM, buf->phys);
					pos += buf->size;
					iter++;
					offset += buf->size;
				} else {
					err = -EFAULT;
					goto bail;
				}
			}
		}

		list_for_each_entry(map, &user->maps, node) {
			if (map->flags == ADSP_MMAP_ADD_PAGES_TSTACK) {
				if ((dump + total_size) - pos >= map->size) {
					err = fastrpc_get_mem_content(map->buf, pos, map->size);
					if (err)
						goto bail;

					populate_dump_metadata(&dinfo[iter], offset, map->size,
						TSTACK_MEM, map->phys);
					pos += map->size;
					iter++;
					offset += map->size;
				} else {
					err = -EFAULT;
					goto bail;
				}
			}
		}
	}

	dev_coredumpv(dev, dump, total_size, GFP_KERNEL);
bail:
	if (!scm_done) {
		list_for_each_entry_safe(buf, b, &lgmaps_list, node) {
			err = fastrpc_remote_heap_unassign(cctx, buf);
			list_del(&buf->node);
			if (err)
				continue;
			__fastrpc_buf_free(buf);
		}
	}
	if (err)
		pr_err("%s : Failed to dump user data for iter %d err %d\n",
					__func__, iter, err);
	kfree(s_file);
}

static const struct file_operations fastrpc_fops = {
	.open = fastrpc_device_open,
	.release = fastrpc_device_release,
	.unlocked_ioctl = fastrpc_device_ioctl,
	.compat_ioctl = fastrpc_device_ioctl,
};

static int fastrpc_cb_probe(struct platform_device *pdev)
{
	struct fastrpc_channel_ctx *cctx;
	struct fastrpc_pool_ctx *sess = NULL;
	struct device *dev = &pdev->dev;
	int i, sessions = 0;
	unsigned long flags;
	u32 pd_type = DEFAULT_UNUSED, smmuidx = DEFAULT_SMMU_IDX;
	int rc, err = 0;
	struct fastrpc_buf *buf = NULL;
	struct iommu_domain *domain = NULL;
	struct gen_pool *gen_pool = NULL;
	int frpc_gen_addr_pool[2] = {0};
	u32 smmu_alloc_range32[2] = {0};
	u64 smmu_alloc_range64[2] = {0};
	struct sg_table sgt;
	struct fastrpc_smmu *smmucb = NULL;
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_root = g_frpc.debugfs_root;
	struct dentry *debugfs_global_file = NULL;
#endif

	cctx = get_current_channel_ctx(dev);

	if (IS_ERR_OR_NULL(cctx))
		return -EINVAL;

	of_property_read_u32(dev->of_node, "qcom,nsessions", &sessions);

	if (of_get_property(dev->of_node, "pd-type", NULL) != NULL) {
		err = of_property_read_u32(dev->of_node, "pd-type",
				&pd_type);
		if (err)
			goto bail;
		// Set pd_type, if the process type is configured for context banks
		cctx->pd_type = true;
	}

	spin_lock_irqsave(&cctx->lock, flags);
	if (cctx->sesscount >= FASTRPC_MAX_SESSIONS) {
		dev_err(&pdev->dev, "too many sessions\n");
		spin_unlock_irqrestore(&cctx->lock, flags);
		return -ENOSPC;
	}

	/* Find any existing session for pooling CBs with same PD type */
	for (i = 0; i < cctx->sesscount; i++) {
		/* Only USER_UNSIGNEDPD_POOL or EXT_MAP_PD_TYPE type are pooled */
		if (pd_type != USER_UNSIGNEDPD_POOL && pd_type != EXT_MAP_PD_TYPE)
			break;

		if (cctx->session[i].pd_type == pd_type) {
			sess = &cctx->session[i];
			/* Set smmucb_pool to true, if SMMU CB pooling is enabled */
			cctx->smmucb_pool = true;
			break;
		}
	}

	/* If no existing session was found, prepare new session */
	if (!sess)
		sess = &cctx->session[cctx->sesscount++];

	/* Update session info during probe of first CB only */
	if (sess->smmucount == 0) {
		sess->usecount = 0;
		sess->pd_type = pd_type;
	}
	/* Read secure flag for each context bank, even if part of CB pool */
	sess->secure = of_property_read_bool(dev->of_node,
						"qcom,secure-context-bank");

	/* Populate SMMU CB info at next available free SMMU index */
	smmuidx = sess->smmucount++;
	smmucb = &sess->smmucb[smmuidx];
	smmucb->valid = true;
	smmucb->dev = dev;
	smmucb->sess = sess;
	smmucb->pa_bits = DSP_DEFAULT_BUS_WIDTH;
	mutex_init(&smmucb->map_mutex);

	/*
	 * If upstream bus size is specified for context bank in dtsi, then
	 * use that value to configure the range of addresses allowed for
	 * smmu mappings from this device.
	 * If the property is not set, then use the default bus size.
	 */
	of_property_read_u32(dev->of_node, "ubs", &smmucb->pa_bits);

	/* Configure where sid will be prepended to pa */
	smmucb->sid_pos = (cctx->iova_format ? SID_POS_IN_IOVA : smmucb->pa_bits);

	if (of_property_read_u32(dev->of_node, "reg", &smmucb->sid))
		dev_info(dev, "FastRPC Session ID not specified in DT\n");

	/*
	 * Set SMMU context bank, min and max allocation range.
	 * Read alloc-size-range property according to defintion array
	 * type in devicetree.
	 */
	if (!of_property_read_u64_array(dev->of_node, "alloc-size-range",
				smmu_alloc_range64,
				sizeof(smmu_alloc_range64)/sizeof(smmu_alloc_range64[0]))) {
		smmucb->minallocsize = smmu_alloc_range64[0];
		smmucb->maxallocsize = smmu_alloc_range64[1];
	} else if (!of_property_read_u32_array(dev->of_node, "alloc-size-range",
				smmu_alloc_range32,
				sizeof(smmu_alloc_range32)/sizeof(smmu_alloc_range32[0]))) {
		smmucb->minallocsize = (u64)smmu_alloc_range32[0];
		smmucb->maxallocsize = (u64)smmu_alloc_range32[1];
	}

	smmucb->totalbytes = (1ULL << smmucb->pa_bits) - 1;

	/* Set SMMU device private data with fastrpc SMMU CB pointer */
	dev_set_drvdata(dev, smmucb);

	/* Context bank can be shared by multiple apps. Create duplicate sessions */
	if (sessions > 0) {
		struct fastrpc_pool_ctx *dup_sess = NULL;

		sess->sharedcb = true;
		for (i = 1; i < sessions; i++) {
			if (cctx->sesscount >= FASTRPC_MAX_SESSIONS)
				break;
			dup_sess = &cctx->session[cctx->sesscount++];
			memcpy(dup_sess, sess, sizeof(*dup_sess));
			mutex_init(&dup_sess->smmucb[DEFAULT_SMMU_IDX].map_mutex);
		}
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
	if (of_get_property(dev->of_node, "qrtr-gen-pool", NULL) != NULL) {

		err = of_property_read_u32_array(dev->of_node, "frpc-gen-addr-pool",
							frpc_gen_addr_pool, 2);
		if (err) {
			dev_err(&pdev->dev, "Error: parsing frpc-gen-addr-pool arguments failed for %s with err %d\n",
					dev_name(dev), err);
			goto bail;
		}
		smmucb->genpool_iova = frpc_gen_addr_pool[0];
		smmucb->genpool_size = frpc_gen_addr_pool[1];

		buf = kzalloc(sizeof(*buf), GFP_KERNEL);
		if (IS_ERR_OR_NULL(buf)) {
			err = -ENOMEM;
			dev_err(&pdev->dev, "allocation failed for size 0x%zx\n", sizeof(*buf));
			goto bail;
		}
		INIT_LIST_HEAD(&buf->attachments);
		INIT_LIST_HEAD(&buf->node);
		mutex_init(&buf->lock);
		buf->virt = NULL;
		buf->phys = 0;
		buf->size = frpc_gen_addr_pool[1];
		buf->dev = smmucb->dev;
		buf->raddr = 0;


		/* Allocate memory for adding to genpool */
		buf->virt = dma_alloc_coherent(buf->dev, buf->size,
					(dma_addr_t *)&buf->phys, GFP_KERNEL);

		if (IS_ERR_OR_NULL(buf->virt)) {
			dev_err(&pdev->dev, "dma_alloc failed for size 0x%llx, returned %pK\n",
				buf->size, buf->virt);
			err = -ENOBUFS;
			goto dma_alloc_bail;
		}

		err = dma_get_sgtable(smmucb->dev, &sgt, buf->virt,
				buf->phys, buf->size);
		if (err) {
			dev_err(&pdev->dev, "dma_get_sgtable_attrs failed with err %d", err);
				goto iommu_map_bail;
		}
		domain = iommu_get_domain_for_dev(smmucb->dev);
		if (!domain) {
			dev_err(&pdev->dev, "iommu_get_domain_for_dev failed ");
			goto iommu_map_bail;
		}

		/* Map the allocated memory with fixed IOVA and is shared to remote subsystem */
#if (KERNEL_VERSION(6, 3, 0) <= LINUX_VERSION_CODE)
		err = iommu_map_sg(domain, frpc_gen_addr_pool[0], sgt.sgl,
				sgt.nents, IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE, GFP_KERNEL);
#else
		err = iommu_map_sg(domain, frpc_gen_addr_pool[0], sgt.sgl,
				sgt.nents, IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);

#endif
		if (err < 0) {
			dev_err(&pdev->dev, "iommu_map_sg failed with err %d", err);
			goto iommu_map_bail;
		}

		/* Create genpool using SMMU device */
		gen_pool = devm_gen_pool_create(smmucb->dev, 0, NUMA_NO_NODE, NULL);
		if (IS_ERR(gen_pool)) {
			err = PTR_ERR(gen_pool);
			dev_err(&pdev->dev, "devm_gen_pool_create failed with err %d", err);
			goto genpool_create_bail;
		}
		/* Add allocated memory to genpool */
		err = gen_pool_add_virt(gen_pool, (unsigned long)buf->virt,
				buf->phys, buf->size, NUMA_NO_NODE);
		if (err) {
				dev_err(&pdev->dev, "gen_pool_add_virt failed with err %d", err);
			goto genpool_add_bail;
		}
		smmucb->frpc_genpool = gen_pool;
		smmucb->frpc_genpool_buf = buf;
		dev_err(&pdev->dev, "fastrpc_cb_probe qrtr-gen-pool end\n");
	}

	/* Mask determines range of addresses returned by smmu driver */
	rc = dma_set_mask(dev, DMA_BIT_MASK(smmucb->pa_bits));
	if (rc) {
		dev_err(dev, "32-bit DMA enable failed\n");
		return rc;
	}
	/* Set larger segment size to allow smmu to map > 4GB */
	dma_set_max_seg_size(dev, DMA_BIT_MASK(32));

	/*
	 * Set the DMA mask for the SMMU parent device to 64-bit,
	 * allowing the device to access the full range of DDR memory.
	 * This is necessary for devices that need to perform DMA operations,
	 * on high memory addresses beyond the 32-bit limit.
	 */
	rc = dma_set_mask(dev->parent, DMA_BIT_MASK(64));
	if (rc) {
		dev_err(dev, "64-bit parent SMMU dev DMA enable failed\n");
		return rc;
	}

#ifdef CONFIG_DEBUG_FS
	if (debugfs_root && !g_frpc.debugfs_global_file) {
		debugfs_global_file = debugfs_create_file("global", 0644,
			debugfs_root, NULL, &fastrpc_debugfs_fops);
		if (IS_ERR_OR_NULL(debugfs_global_file)) {
			pr_warn("Error: %s: %s: failed to create debugfs global file\n",
				current->comm, __func__);
			debugfs_global_file = NULL;
		}
		g_frpc.debugfs_global_file = debugfs_global_file;
	}
#endif

bail:
	if (!err)
		dev_info(dev, "Successfully added %s", dev->kobj.name);
	return err;
genpool_add_bail:
	gen_pool_destroy(gen_pool);
genpool_create_bail:
	iommu_unmap(domain, smmucb->genpool_iova, smmucb->genpool_size);
iommu_map_bail:
	dma_free_coherent(smmucb->dev, buf->size, buf->virt,
			IOVA_TO_PHYSADDR(buf->phys, smmucb->sid_pos));
dma_alloc_bail:
	kfree(buf);
	return err;
}

/* Function to free fastrpc genpool buffer */
static void fastrpc_genpool_free(struct fastrpc_smmu *smmucb)
{
	struct fastrpc_buf *buf = NULL;
	struct iommu_domain *domain = NULL;

	if (!smmucb)
		return;
	buf = smmucb->frpc_genpool_buf;
	if (smmucb->frpc_genpool) {
		gen_pool_destroy(smmucb->frpc_genpool);
		smmucb->frpc_genpool = NULL;
	}
	if (buf && smmucb->dev) {
		domain = iommu_get_domain_for_dev(smmucb->dev);
		if (domain) {
			iommu_unmap(domain, smmucb->genpool_iova,
						smmucb->genpool_size);
		}
		if (buf->phys)
			dma_free_coherent(buf->dev, buf->size, buf->virt,
				IOVA_TO_PHYSADDR(buf->phys, smmucb->sid_pos));
		kfree(buf);
		smmucb->frpc_genpool_buf = NULL;
	}
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
static void fastrpc_cb_remove(struct platform_device *pdev)
#else
static int fastrpc_cb_remove(struct platform_device *pdev)
#endif
{
	struct fastrpc_channel_ctx *cctx = dev_get_drvdata(pdev->dev.parent);
	struct fastrpc_smmu *smmucb = dev_get_drvdata(&pdev->dev),
							*ismmucb = NULL;
	struct fastrpc_pool_ctx *sess = smmucb->sess;
	unsigned long flags;
	int i = 0, j = 0;

	if (sess->pd_type == ROOT_PD) {
		fastrpc_rootheap_buf_list_free(cctx);
		fastrpc_preload_mem_free(cctx);
	}

	spin_lock_irqsave(&cctx->lock, flags);
	for (i = 0; i < FASTRPC_MAX_SESSIONS; i++) {
		for (j = 0; j < cctx->session[i].smmucount; j++) {
			ismmucb = &cctx->session[i].smmucb[j];
			if (ismmucb->sid != smmucb->sid)
				continue;
			spin_unlock_irqrestore(&cctx->lock, flags);
			mutex_lock(&ismmucb->map_mutex);
			if (ismmucb->frpc_genpool)
				fastrpc_genpool_free(ismmucb);
			ismmucb->dev = NULL;
			mutex_unlock(&ismmucb->map_mutex);
			spin_lock_irqsave(&cctx->lock, flags);
			ismmucb->valid = false;
			cctx->sesscount--;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, flags);
	dev_info(&pdev->dev, "Successfully removed %s", pdev->dev.kobj.name);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
        return 0;
#endif
}

static const struct of_device_id fastrpc_match_table[] = {
	{ .compatible = "qcom,fastrpc-compute-cb", },
	{}
};

static struct platform_driver fastrpc_cb_driver = {
	.probe = fastrpc_cb_probe,
	.remove = fastrpc_cb_remove,
	.driver = {
		.name = "qcom,fastrpc-cb",
		.of_match_table = fastrpc_match_table,
		.suppress_bind_attrs = true,
	},
};

int fastrpc_device_register(struct device *dev, struct fastrpc_channel_ctx *cctx,
				   bool is_secured, bool legacy, const char *domain)
{
	struct fastrpc_device_node *fdev;
	int err;

	fdev = devm_kzalloc(dev, sizeof(*fdev), GFP_KERNEL);
	if (!fdev)
		return -ENOMEM;

	fdev->secure = is_secured;
	fdev->cctx = cctx;
	cctx->dev = dev;
	fdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	fdev->miscdev.fops = &fastrpc_fops;
	if (legacy)
		fdev->miscdev.name = devm_kasprintf(dev, GFP_KERNEL, "fastrpc-%s%s",
						domain, is_secured ? "-secure" : "");
	else
		fdev->miscdev.name = devm_kasprintf(dev, GFP_KERNEL, "fastrpc-%s",
							domain);
	if (!fdev->miscdev.name)
		return -ENOMEM;

	err = misc_register(&fdev->miscdev);
	if (!err) {
		/*
		 * Device nodes are created based on following criteria:
		 *   - For all channels, create a single device node with the
		 *     new domain name
		 *   - For channels that are marked as the legacy dsp of that type,
		 *     (for backward compatibility), also create the secure (and
		 *     non-secure, if applicable) device nodes using the legacy name
		 *     of the channel (eg: using CDSP name for the first NSP)
		 */
		if (legacy) {
			if (is_secured)
					cctx->legacy_secure_fdevice = fdev;
			else
					cctx->legacy_fdevice = fdev;
		} else {
			cctx->fdevice = fdev;
		}
	}
	return err;
}

void fastrpc_lowest_capacity_corecount(struct device *dev, struct fastrpc_channel_ctx *cctx)
{
	u32 cpu = 0;

	cpu =  cpumask_first(cpu_possible_mask);
	for_each_cpu(cpu, cpu_possible_mask) {
		if (topology_cluster_id(cpu) == 0)
			cctx->lowest_capacity_core_count++;
	}
	dev_info(dev, "Lowest capacity core count: %u\n",
					cctx->lowest_capacity_core_count);
}

int fastrpc_setup_service_locator(struct fastrpc_channel_ctx *cctx, char *client_name,
					char *service_name, char *service_path, int spd_session)
{
	int err = 0;
	struct pdr_handle *handle = NULL;
	struct pdr_service *service = NULL;

	/* Register the service locator's callback function */
	handle = pdr_handle_alloc(fastrpc_pdr_cb, &cctx->spd[spd_session]);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto bail;
	}
	cctx->spd[spd_session].pdrhandle = handle;
	cctx->spd[spd_session].servloc_name = service_path;
	cctx->spd[spd_session].spd_id = get_static_id_from_pd_name(client_name);
	cctx->spd[spd_session].cctx = cctx;
	service = pdr_add_lookup(handle, service_name, service_path);
	if (IS_ERR(service)) {
		err = PTR_ERR(service);
		goto bail;
	}
	dev_info(cctx->dev, "%s: pdr_add_lookup enabled for %s (%s, %s)\n",
		__func__, service_name, client_name, service_path);

bail:
	if (err)
		dev_err(cctx->dev, "%s: failed for %s (%s, %s)with err %d\n",
				__func__, service_name, client_name, service_path, err);
	return err;
}

void fastrpc_register_wakeup_source(struct device *dev,
	const char *client_name, struct wakeup_source **device_wake_source)
{
	struct wakeup_source *wake_source = NULL;

	wake_source = wakeup_source_register(NULL, client_name);
	if (IS_ERR_OR_NULL(wake_source)) {
		dev_err(dev, "wakeup_source_register failed for dev %s, client %s with err %ld\n",
		dev_name(dev), client_name, PTR_ERR(wake_source));
		return;
	}

	*device_wake_source = wake_source;
}

static void fastrpc_notify_user_ctx(struct fastrpc_invoke_ctx *ctx, int retval,
		u32 rsp_flags, u32 early_wake_time)
{
	u32 tgid_app = ctx->fl->tgid_app;

	if (ctx->cctx) {
		if (!atomic_read(&ctx->cctx->teardown))
			fastrpc_pm_awake(ctx->fl);
		trace_fastrpc_context_complete(ctx->cctx->domain_id, (uint64_t)ctx,
			retval, ctx->ctxid, ctx->pid, ctx->sc);
	}
	ctx->retval = retval;
	ctx->rsp_flags = (enum fastrpc_response_flags)rsp_flags;
	switch (rsp_flags) {
	case NORMAL_RESPONSE:
	case COMPLETE_SIGNAL:
		/* normal and complete response with return value */
		ctx->is_work_done = true;
		trace_fastrpc_msg("wakeup_task: begin");
		fastrpc_timeline_record(38, tgid_app, ctx->fl->fastrpc_timeline_obj);
		complete(&ctx->work);
		trace_fastrpc_msg("wakeup_task: end");
		break;
	case USER_EARLY_SIGNAL:
		fastrpc_timeline_record(52, tgid_app, ctx->fl->fastrpc_timeline_obj);
		/* user hint of approximate time of completion */
		ctx->early_wake_time = early_wake_time;
		fallthrough;
	case EARLY_RESPONSE:
		/* rpc framework early response with return value */
		trace_fastrpc_msg("wakeup_task: begin");
		fastrpc_timeline_record(22, tgid_app, ctx->fl->fastrpc_timeline_obj);
		complete(&ctx->work);
		trace_fastrpc_msg("wakeup_task: end");
		break;
	default:
		break;
	}
}

static void fastrpc_handle_signal_rpmsg(uint64_t msg, struct fastrpc_channel_ctx *cctx)
{
	u32 pid = msg >> 32;
	u32 signal_id = msg & 0xffffffff;
	struct fastrpc_user *fl ;
	unsigned long irq_flags = 0;
	bool process_found = false;
	int err;

	if (signal_id >=FASTRPC_DSPSIGNAL_NUM_SIGNALS)
		return;

	spin_lock_irqsave(&cctx->lock, irq_flags);
	list_for_each_entry(fl, &cctx->users, user) {
		if (fl->tgid_frpc == pid) {
			err = fastrpc_file_get(fl);
			if (err) {
				dev_warn(cctx->dev, "Warning: %s: user-obj for fl (%pK) being released\n",
					__func__, fl);
				break;
			}
			process_found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&cctx->lock, irq_flags);

	if (!process_found) {
		pr_warn("Warning: %s: no active processes found for pid %u, signal id %u",
			__func__, pid, signal_id);
		return;
	}

	spin_lock_irqsave(&fl->dspsignals_lock, irq_flags);
	if (fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE]) {
		struct fastrpc_dspsignal *group =
			fl->signal_groups[signal_id /FASTRPC_DSPSIGNAL_GROUP_SIZE];
		struct fastrpc_dspsignal *sig =
			&group[signal_id %FASTRPC_DSPSIGNAL_GROUP_SIZE];
		if ((sig->state == DSPSIGNAL_STATE_PENDING) ||
			(sig->state == DSPSIGNAL_STATE_SIGNALED)) {
			trace_fastrpc_dspsignal("complete", signal_id, sig->state, 0);
			complete(&sig->comp);
			sig->state = DSPSIGNAL_STATE_SIGNALED;
		} else if (sig->state == DSPSIGNAL_STATE_UNUSED) {
			pr_err("Received unknown signal %u for PID %u\n",
					signal_id, pid);
		}
	} else {
		pr_err("Received unknown signal %u for PID %u\n",
				signal_id, pid);
	}
	spin_unlock_irqrestore(&fl->dspsignals_lock, irq_flags);
	fastrpc_file_put(fl, true);
}

/*
 * fastrpc_handle_ipcmsg_rsp - Handle fastrpc_ipcmsg message response from DSP.
 * @cctx: DSP Channel context.
 * @rsp: IPC message response.
 *
 * This function handles fastrpc_ipcmsg format from DSP base on the
 * type of request.
 *
 * Return: 0 on success, error code on failure.
 */
static int fastrpc_handle_ipcmsg_rsp(struct fastrpc_channel_ctx *cctx,
	struct fastrpc_ipcmsg *rsp)
{
	u32 type = rsp->type, payload_size = rsp->size, version = 0;
	struct transport_err_rsp err_rsp = rsp->payload.err_rsp;
	int err = 0;

	switch (type) {
		case TX_IPCMSG_VER_ERROR :
			if (payload_size != sizeof(err_rsp)) {
				err = -EINVAL;
				dev_err(cctx->dev,
					"Error %d: %s: Incorrect payload size %u for rspv4 type %d Expected size %zu",
					err, __func__, payload_size, type,
					sizeof(err_rsp));
			} else {
				err = -EINVAL;
				version = err_rsp.maxVersionSupported;
				dev_err(cctx->dev,
					"Error %d: %s: IPC message version mismatch, Max DSP RX supported version %u Kernel TX version %u",
					err, __func__, version,
					KERNEL_MAX_IPC_TX_VER);
				trace_fastrpc_transport_responsev4_err(
					cctx->domain_id, err_rsp.userCtx, err_rsp.retVal,
					payload_size, sizeof(err_rsp), version);
			}
			break;
		default:
			err = -EINVAL;
			dev_err(cctx->dev,
					"Error %d: %s: Unsupported DSP TX message type %u, Kernel max RX ver type %u",
					err, __func__, type,
					KERNEL_MAX_IPC_RX_VER);
			break;
	}
	return err;
}

/*
 * fastrpc_handle_legacy_rsp - Handle legacy response from DSP.
 * @cctx: DSP Channel context.
 * @data: Response data.
 * @is_v1: Flag indicating if the response is in version 1 format.
 * @is_glink_wakeup: Flag indicating if the response is a glink wakeup.
 *
 * This function handles legacy response format from DSP
 * based on the type of request.
 *
 * Return: 0 on success, error code on failure.
 */
static int fastrpc_handle_legacy_rsp(struct fastrpc_channel_ctx *cctx,
	void *data, bool is_v1, bool is_glink_wakeup)
{
	struct fastrpc_invoke_rsp rsp = {0};
	struct fastrpc_invoke_rspv2 rspv2 = {0};
	struct fastrpc_invoke_ctx *ctx = NULL;
	u32 rsp_flags = 0, early_wake_time = 0, version = 0;
	unsigned long flags = 0, idr = 0;
	u64 ctxid = 0;

	if (is_v1)
		rsp = *((struct fastrpc_invoke_rsp *)(data));
	else {
		rspv2 = *((struct fastrpc_invoke_rspv2 *)(data));
		rsp.ctx = rspv2.ctx;
		rsp.retval = rspv2.retval;
		early_wake_time = rspv2.early_wake_time;
		rsp_flags = rspv2.flags;
		version = rspv2.version;
	}

	fastrpc_update_rxmsg_buf(cctx, rsp.ctx, rsp.retval,
		rsp_flags, early_wake_time, version, get_timestamp_in_ns());
	trace_fastrpc_transport_response(cctx->domain_id, rsp.ctx,
			rsp.retval, rsp_flags, early_wake_time);

	idr = FASTRPC_GET_IDR_FROM_CTXID(rsp.ctx);
	ctxid = FASTRPC_GET_CTXID_FROM_RSP_CTX(rsp.ctx);

	spin_lock_irqsave(&cctx->lock, flags);
	ctx = idr_find(&cctx->ctx_idr, idr);

	if (!ctx) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		return 0;
	}

	if (ctx->ctxid != ctxid) {
		spin_unlock_irqrestore(&cctx->lock, flags);
		dev_info(cctx->dev,
			"Warning: rsp ctxid 0x%llx mismatch with local ctxid 0x%llx (full rsp ctx 0x%llx)",
				ctxid, ctx->ctxid, rsp.ctx);
		return 0;
	}

	if (rspv2.version != FASTRPC_RSP_VERSION2) {
			dev_err(cctx->dev, "Incorrect response version %d\n",
				rspv2.version);
			spin_unlock_irqrestore(&cctx->lock, flags);
			return -EINVAL;
	}
	fastrpc_timeline_record(21, ctx->fl->tgid_app,
		ctx->fl->fastrpc_timeline_obj);
	fastrpc_notify_user_ctx(ctx, rsp.retval, rsp_flags, early_wake_time);

	if (is_glink_wakeup && ctx->fl)
		dev_info(cctx->dev,
				"glink wakeup by(%s): domain(%d) pd_type(%d) "
				"pid(%d: %d: %d) tid(%d) handle(0x%x) sc(0x%x)\n",
					ctx->fl->name,
					cctx->domain_id,
					ctx->fl->pd_type,
					ctx->fl->tgid,
					ctx->fl->tgid_app,
					ctx->fl->tgid_frpc,
					ctx->pid,
					ctx->handle,
					ctx->sc);

	spin_unlock_irqrestore(&cctx->lock, flags);
	return 0;
}
/*
 * fastrpc_handle_rpc_response - Handle RPC response from DSP.
 * @cctx			: DSP Channel context.
 * @data			: Response data.
 * @len 			: Response data length.
 * @is_glink_wakeup : glink wakeup
 *
 * This function handles RPC response from DSP and notifies the user context
 * about the response.
 *
 * Return: 0 on success, error code on failure.
 */
int fastrpc_handle_rpc_response(struct fastrpc_channel_ctx *cctx,
	union rsp *data, int len, bool is_glink_wakeup)
{
	struct dsp_notif_rsp *notif = NULL;
	struct root_request_msg *root_req = NULL;
	int err = 0;

	switch (len) {
		/* rspv1 msg*/
		case SIZE_RSPV1 :
			err = fastrpc_handle_legacy_rsp(cctx,
				(void *)(&data->rsp), true, is_glink_wakeup);
			break;

		/* Both rspv2 and rspv3(notif response) have same size */
		case SIZE_RSPV2 :
		{
			struct fastrpc_invoke_rspv2 *rspv2 = &data->rsp2;
			if (rspv2->ctx == FASTRPC_NOTIF_CTX_RESERVED) {
				notif = &data->rsp3;
				if (notif->type == STATUS_RESPONSE &&
					len >= sizeof(*notif))
					fastrpc_notif_find_process(
						cctx->domain_id, cctx, notif);
				else
					err = -ENOENT;
			} else if (rspv2->ctx == FASTRPC_STATICPD_RSP_CTX) {
				notif = &data->rsp3;
				fastrpc_pdr_notif(cctx, notif);
			} else {
				err = fastrpc_handle_legacy_rsp(cctx,
					(void *)(&data->rsp), false, is_glink_wakeup);
			}
			break;
		}

		/* DSP signal response */
		case SIZE_DSPSIGNAL:
			trace_fastrpc_transport_response(cctx->domain_id,
				*((uint64_t *)data), 0, 0, 0);
			fastrpc_handle_signal_rpmsg(*((uint64_t *)data), cctx);
			break;

		/* Root request message */
		case SIZE_ROOT_REQ_MSG:
			root_req = &data->rsp4;
			if (root_req->ctx == FASTRPC_ROOT_CTX_RESERVED) {
				if (root_req->req_type == ROOT_REQ_SIGNAL &&
						len >= sizeof(*root_req)) {
					fastrpc_queue_root_msg(cctx, root_req, 0);
				} else {
					err = -EINVAL;
					dev_err(cctx->dev, "Error: %s: Invalid root request type or size",
						__func__);
				}
			}
			break;

		/* fastrpc_ipcmsg response */
		default:
			err = fastrpc_handle_ipcmsg_rsp(cctx, &data->rsp5);
			break;
	}

	if (err != 0) {
		dev_err(cctx->dev,
			"Error %d: %s: Failed to handle response packet size %d",
			err, __func__, len);
	}

	return err;
}

/*
 * Retrieves legacy information for a given fastrpc_domain.
 *
 * This function maps the domain's type to its corresponding legacy name
 * and ID, based on the following table:
 *
 *   Domain Type       | Legacy Name              | Legacy ID
 *   ------------------|--------------------------|---------------
 *   SDSP              | domains[SDSP_DOMAIN_ID]  | SDSP_DOMAIN_ID
 *   LPASS             | domains[ADSP_DOMAIN_ID]  | ADSP_DOMAIN_ID
 *   NSP(instance 0)   | domains[CDSP_DOMAIN_ID]  | CDSP_DOMAIN_ID
 *   NSP(instance 1)   | domains[CDSP1_DOMAIN_ID] | CDSP1_DOMAIN_ID
 *
 * @param domain Pointer to the fastrpc_domain structure to retrieve
 * legacy info
 *
 * @return 0 on success, or a negative error code on failure
 *
 * Error codes:
 *   -EINVAL: Invalid domain type
 */
static int fastrpc_retrieve_legacy_info(struct fastrpc_domain *domain)
{
	int err = 0;

	switch (domain->type) {
	case FASTRPC_SDSP:
		domain->legacy_name = (char *)legacy_domains[SDSP_DOMAIN_ID];
		domain->legacy_id = SDSP_DOMAIN_ID;
		break;
	case FASTRPC_LPASS:
		domain->legacy_name = (char *)legacy_domains[ADSP_DOMAIN_ID];
		domain->legacy_id = ADSP_DOMAIN_ID;
		break;
	case FASTRPC_NSP:
		if (domain->instance_id == 0) {
			domain->legacy_name = (char *)legacy_domains[CDSP_DOMAIN_ID];
			domain->legacy_id = CDSP_DOMAIN_ID;
		} else if (domain->instance_id == 1) {
			domain->legacy_name = (char *)legacy_domains[CDSP1_DOMAIN_ID];
			domain->legacy_id = CDSP1_DOMAIN_ID;
		}
		break;
	default:
		err = -EINVAL;
		break;
	}
	return err;
}

void fastrpc_log_internal(struct device *dev,
	struct fastrpc_channel_ctx *cctx, int dest_mask,
	enum fastrpc_log_level level, const char *fmt, ...)
{
#if FRPC_RING_BUFFER_ENABLED
	if ((dest_mask & FASTRPC_LOG_RINGBUF) && cctx && cctx->log.rb) {
		int msg_len;
		va_list arg;

		/* Measure formatted length (excluding NULL terminator). */
		va_start(arg, fmt);
		msg_len = vsnprintf(NULL, 0, fmt, arg);
		va_end(arg);

		if (msg_len >= 0) {
			/* rb_data_len : level + string + NULL terminator */
			const int rb_data_len = 1 + msg_len + 1;
			const size_t event_len   =
				sizeof(struct fastrpc_event_log) + rb_data_len;
			struct ring_buffer_event *event =
				ring_buffer_lock_reserve(cctx->log.rb, event_len);

			if (event) {
				struct fastrpc_event_log *entry = ring_buffer_event_data(event);
				entry->data_len = rb_data_len;
				entry->data[0]  = (char)level;

				va_list arg_rb;
				va_start(arg_rb, fmt);
				vsnprintf(&entry->data[1], rb_data_len - 1, fmt, arg_rb);
				va_end(arg_rb);

				ring_buffer_unlock_commit(cctx->log.rb);
				wake_up_interruptible(&cctx->log.wq);
			}
		}
	}
#endif /* >= 6.18 */

	if (dest_mask & FASTRPC_LOG_DMESG) {
		va_list arg_dmesg;
		struct va_format vaf;

		va_start(arg_dmesg, fmt);
		vaf.fmt = fmt;
		vaf.va  = &arg_dmesg;

		switch (level) {
		case FASTRPC_LOG_LEVEL_INFO:
			dev ? dev_info(dev, "%pV\n", &vaf) : pr_info("%pV\n", &vaf);
			break;
		case FASTRPC_LOG_LEVEL_WARN:
			dev ? dev_warn(dev, "%pV\n", &vaf) : pr_warn("%pV\n", &vaf);
			break;
		case FASTRPC_LOG_LEVEL_ERROR:
		default:
			dev ? dev_err(dev, "%pV\n", &vaf) : pr_err("%pV\n", &vaf);
			break;
		}

		va_end(arg_dmesg);
	}
}

/*
 * Add entry for domain in hash-table or update status of existing entry.
 *
 * @param domain  Pointer to the fastrpc domain structure to be added.
 * @param type    Type of the domain.
 * @param label   Label of the domain.
 * @param instance_id  Instance ID of the domain.
 *
 * @return 0 on success, negative error code on failure.
 */
static int fastrpc_add_domain_to_table(struct fastrpc_domain **domain,
	u32 type, const char* label, u32 instance_id)
{
	struct fastrpc_domain *entry = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	u32 phy_id = 0;
	int err = 0;

	phy_id = GENERATE_DSP_PHYSICAL_ID(type, instance_id);

	/* Validate if there is an exisitng entry for phy_id */
	entry = fastrpc_lookup_domain_in_table(phy_id, true);
	if (!entry) {
		/*
		 * If the domain is not found in the table, create a new
		 * entry and populate all the attributes
		 * phy_id, instance_id, type, logical_id, name
		 */
		entry = kzalloc(sizeof(*entry), GFP_KERNEL);
		if (!entry) {
			err = -ENOMEM;
			goto bail;
		}
		entry->phy_id = phy_id;
		entry->instance_id = instance_id;
		entry->type = type;

		/* Channel name will be generated as <dsp-type-name><physical-id> */
		err = snprintf(entry->name, sizeof(entry->name), "%s%d", label, phy_id);
		if (err < 0 || err >= sizeof(entry->name)) {
			err = -EFAULT;
			pr_err("Error %d: %s failed to generate name for label %s phy_id %u",
				err, __func__, label, phy_id);
			goto bail;
		}

		if (instance_id == 0 || (type == FASTRPC_NSP && instance_id == 1))  {
			/*
			 * For LPASS, SDSP types only the dsp with instance_id 0 is
			 *                 assigned as legacy adsp, slpi domains
			 * For NSP types, DSP with instance id '0' and '1' are marked as legacy
			 *                to handle legacy cdsp and cdsp1 domains
			*/
			entry->legacy = true;
			err = fastrpc_retrieve_legacy_info(entry);
			if (err) {
				pr_err("Error %d: %s failed to retrieve legacy info for %s",
					err, __func__, entry->name);
				goto bail;
			}
		}

		err = fastrpc_sysfs_domain_create(entry);
		if (err) {
			pr_err("Error %d: %s: failed to create sysfs node for %s",
				err, __func__, entry->name);
			goto bail;
		}

		mutex_lock(hmut);
		g_frpc.dsp_counter[type]++;
		entry->id = GENERATE_LOGICAL_DOMAIN_ID(type, g_frpc.dsp_counter[type]);
		hash_add(g_frpc.fastrpc_domains_table, &entry->node, phy_id);
		mutex_unlock(hmut);
	} else {
		if (entry->status != DSP_STATUS_DOWN) {
			/*
			* If entry for channel is already present in hash-table, it means
			* the channel has gone through ssr. In that case, its status has to be
			* DOWN. If not, system is in bad-state.
			 */
			err = EINVAL;
			pr_err("Error %d: %s: %s (phy id %u) already in table with bad status %d",
					err, __func__, entry->name, entry->phy_id, entry->status);
			return err;
		}
	}
	*domain = entry;
bail:
	if (err)
		kfree(entry);

	return err;
}

/*
 * fastrpc_lookup_domain_in_table() -
 * Looks up a domain in the in the fastrpc domains hash-table using either
 * physical id or logical domain id based on the flag.
 *
 * @param key          : physical id / logical domain id to lookup in table
 * @param use_phy_id   : Flag to indicate whether to lookup using phy id
 *                       or logical id.
 *
 * @return Pointer to the matching domain structure, or NULL if not found.
 */
static struct fastrpc_domain *fastrpc_lookup_domain_in_table(
	u32 key, bool use_phy_id)
{
	struct fastrpc_domain *domain = NULL, *match = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	int i = 0;

	mutex_lock(hmut);
	hash_for_each(g_frpc.fastrpc_domains_table, i, domain, node) {
		/*
		 * Based on flag, lookup domain based on 32-bit physical id,
		 * logical id
		 */
		if (use_phy_id) {
			if (domain->phy_id == key) {
				match = domain;
				break;
			}
		} else {
			if (domain->id == key) {
				match = domain;
				break;
			}
		}
	}
	mutex_unlock(hmut);
	return match;
}

/*
 * Deletes all entries from the fastrpc domains hash-table.
 */
static void fastrpc_delete_domains_table(void)
{
	struct fastrpc_domain *domain = NULL;
	struct hlist_node *tmp = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	int i = 0;

	mutex_lock(hmut);
	hash_for_each_safe(g_frpc.fastrpc_domains_table, i, tmp, domain, node) {
		fastrpc_sysfs_domain_remove(domain);
		hash_del(&domain->node);
		kfree(domain);
	}
	mutex_unlock(hmut);
}

/*
 * Convert legacy domain ID to logical domain ID
 *
 * This function takes a legacy ID as input and returns the corresponding
 * logical ID.
 *
 * @param id: Legacy ID to convert
 * @param logical_id :   Pointer to logical id
 *
 * @return 0 on success
 *         EINAL if logical id is not found.
 */
static int fastrpc_convert_legacy_id_to_logical_id(u32 legacy_id,
	u32 *logical_id)
{
	struct fastrpc_domain *domain = NULL;
	struct mutex *hmut = &g_frpc.hmut;
	int i = 0, err = -EINVAL;

	mutex_lock(hmut);
	hash_for_each(g_frpc.fastrpc_domains_table, i, domain, node) {
		if (domain->legacy_id == legacy_id) {
			*logical_id = domain->id;
			err = 0;
			break;
		}
	}
	mutex_unlock(hmut);
	return err;
}

/*
 * Populate fastrpc_domain from device tree node.
 *
 * @param rdev   Device structure to extract info from.
 * @param domain Pointer to fastrpc_domain pointer to be populated.
 *
 * @return 0 on success, negative error code on failure.
 */
int fastrpc_populate_domain_from_dt(struct device *rdev,
				struct fastrpc_domain **domain)
{
	const char *label = NULL;
	u32 type = 0, instance_id = U32_MAX;
	int err = 0;
	bool valid_label = false;

	/* Retrieve the label of DSP from DT */
	err = of_property_read_string(rdev->of_node, "label", &label);
	if (err < 0) {
		dev_err(rdev, "Error %d: %s: FastRPC DSP label not specified in DT\n",
			err, __func__);
		return err;
	}
	/* Validate the label retrieved from DT */
	for (int i = 1; i < FASTRPC_MAX_DSP_TYPE; i++) {
		if (strcmp(label, fastrpc_dsp_labels[i]) == 0) {
			valid_label = true;
			break;
		}
	}

	/*
	 * Fail the device probe if it has invalid label. This driver assumes that
	 * the DTSI file is always updated to contain the new DT properties.
	 */
	if (!valid_label) {
		err = -EINVAL;
		dev_err(rdev, "Error %d: %s: DSP label %s specified in DT is invalid\n",
			err, __func__, label);
		return err;
	}

	/*
	 * Retrieve and validate the type of DSP from DT
	 *
	 * Fail the call if either dsp-type is not present in DT,
	 * or invalid DSP type is specified in DT
	 */
	err = of_property_read_u32(rdev->of_node, "dsp-type", &type);
	if (err < 0) {
		dev_err(rdev, "Error %d : %s: dsp-type not specified for %s",
			err, __func__, label);
		return -EINVAL;
	} else if (type >= FASTRPC_MAX_DSP_TYPE || type == 0) {
		err = -EINVAL;
		dev_err(rdev, "Error %d: %s: DSP type %u specified in DT is invalid\n",
			err, __func__, type);
		return err;
	}

	/* Retrieve the instance id of the DSP, fail the call if not specified */
	err = of_property_read_u32(rdev->of_node, "instance-id", &instance_id);
	if (err < 0) {
		dev_err(rdev, "Error %d: %s: instance-id not specified for %s\n",
			err, __func__, label);
		return -EINVAL;
	}

	/* Add the info to the domain table */
	err = fastrpc_add_domain_to_table(domain, type, label, instance_id);
	if (err < 0) {
		dev_err(rdev, "Error %d: %s: failed to add domain %s to table (type %u, instance id %u)",
			err, __func__, label, type, instance_id);
		return err;
	}
	return err;
}

int fastrpc_get_domain_pid_info(struct fastrpc_domain *domain, char **out_buf,
				int *len_written)
{
	struct fastrpc_channel_ctx *cctx = NULL;
	struct fastrpc_user *fl = NULL;
	unsigned long flags;
	int *pids = NULL;
	int pid_count = 0, total_len = 0, err = 0, i = 0;
	char *pid_buf = NULL;

	*out_buf = NULL;
	*len_written = 0;

	pids = kcalloc(FASTRPC_MAX_SESS_PER_DOMAIN,
			sizeof(int), GFP_KERNEL);
	if (!pids)
		return -ENOMEM;

	cctx = domain->cctx;
	if (!cctx) {
		err = -EINVAL;
		goto bail;
	}

	spin_lock_irqsave(&cctx->lock, flags);
	list_for_each_entry(fl, &cctx->users, user) {
		/* Only include pids of apps with dynamic type remote sessions on DSP */
		if (!IS_DYNAMIC_PD(fl->pd_type) ||
			atomic_read(&fl->state) != DSP_CREATE_COMPLETE)
			continue;

		if (pid_count >= FASTRPC_MAX_SESS_PER_DOMAIN) {
			/* Too many PIDs */
			err = -ENOSPC;
			goto bail_unlock;
		}

		pids[pid_count++] = fl->tgid_app;

		/* Add length of PID + 1 for comma (if not first) */
		total_len += snprintf(NULL, 0, "%d", fl->tgid_app) + 1;
	}

	/* Exit if no pids with active sessions */
	if (pid_count == 0)
		goto bail_unlock;

	/* Add +1 to total length for null terminator */
	total_len++;

	pid_buf = kzalloc(total_len, GFP_ATOMIC);
	if (!pid_buf) {
		err = -ENOMEM;
		goto bail_unlock;
	}

	for (i = 0; i < pid_count; i++) {
		if (i > 0) {
			*len_written += snprintf(pid_buf + *len_written,
						total_len - *len_written, ",");
		}
		*len_written += snprintf(pid_buf + *len_written,
						total_len - *len_written, "%d", pids[i]);
	}

	/* Add newline character */
	snprintf(pid_buf + *len_written, total_len - *len_written, "\n");
	*len_written += 1;

	*out_buf = pid_buf;

bail_unlock:
	spin_unlock_irqrestore(&cctx->lock, flags);
bail:
	kfree(pids);
	return err;
}

static int fastrpc_init(void)
{
	int ret, i;
#ifdef CONFIG_DEBUG_FS
	struct dentry *debugfs_root = NULL;
#endif
	spin_lock_init(&g_frpc.glock);
	mutex_init(&g_frpc.gmut);
	mutex_init(&g_frpc.hmut);
	idr_init(&g_frpc.mdctx_idr);
	hash_init(g_frpc.fastrpc_domains_table);
	fastrpc_sysfs_register_kset();
	for (i = 0; i < FASTRPC_MAX_DSP_TYPE; i++) {
		g_frpc.dsp_counter[i] = -1;
	}
	ret = platform_driver_register(&fastrpc_cb_driver);
	if (ret < 0) {
		pr_err("fastrpc: failed to register cb driver\n");
		goto platform_register_bail;
	}

	ret = fastrpc_transport_init();
	if (ret < 0) {
		pr_err("fastrpc: failed to register rpmsg driver\n");
		goto transport_init_bail;
	}

#if IS_ENABLED(CONFIG_QCOM_FASTRPC_TRUSTED)
	g_frpc.is_trusted_vm = true;
#else
	g_frpc.is_trusted_vm = false;
#endif
	g_frpc.debug_mode_enable = false;

#ifdef CONFIG_DEBUG_FS
	debugfs_root = debugfs_create_dir("fastrpc", NULL);
	if (IS_ERR_OR_NULL(debugfs_root)) {
		pr_warn("Error: %s: %s: failed to create debugfs root dir\n",
			current->comm, __func__);
		debugfs_remove_recursive(debugfs_root);
		debugfs_root = NULL;
	}
	g_frpc.debugfs_root = debugfs_root;
	if (debugfs_root)
		debugfs_create_bool("debug_mode", 0644, debugfs_root,
			&g_frpc.debug_mode_enable);
#endif
	return 0;

transport_init_bail:
	platform_driver_unregister(&fastrpc_cb_driver);
platform_register_bail:
	fastrpc_sysfs_deregister_kset();
	idr_destroy(&g_frpc.mdctx_idr);
	mutex_destroy(&g_frpc.hmut);
	mutex_destroy(&g_frpc.gmut);
	return ret;
}
module_init(fastrpc_init);

static void fastrpc_exit(void)
{
	platform_driver_unregister(&fastrpc_cb_driver);
	idr_destroy(&g_frpc.mdctx_idr);
	mutex_destroy(&g_frpc.gmut);
	fastrpc_delete_domains_table();
	fastrpc_sysfs_deregister_kset();
	mutex_destroy(&g_frpc.hmut);
	fastrpc_transport_deinit();
#ifdef CONFIG_DEBUG_FS
	debugfs_remove_recursive(g_frpc.debugfs_root);
#endif
}
module_exit(fastrpc_exit);

MODULE_LICENSE("GPL v2");
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
MODULE_IMPORT_NS("DMA_BUF");
#else
MODULE_IMPORT_NS(DMA_BUF);
#endif
