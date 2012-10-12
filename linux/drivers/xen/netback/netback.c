/******************************************************************************
 * drivers/xen/netback/netback.c
 * 
 * Back-end of the driver for virtual network devices. This portion of the
 * driver exports a 'unified' network-device interface that can be accessed
 * by any operating system that implements a compatible front end. A 
 * reference front-end implementation can be found in:
 *  drivers/xen/netfront/netfront.c
 * 
 * Copyright (c) 2002-2005, K A Fraser
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this source file (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "common.h"
#include <linux/kthread.h>
#include <linux/vmalloc.h>
#include <xen/balloon.h>
#include <xen/interface/memory.h>

/*define NETBE_DEBUG_INTERRUPT*/

struct xen_netbk *xen_netbk;
unsigned int netbk_nr_groups;
static bool use_kthreads = true;
static bool __initdata bind_threads;

#define GET_GROUP_INDEX(netif) ((netif)->group)

static void netif_idx_release(struct xen_netbk *, u16 pending_idx);
static void make_tx_response(netif_t *netif, 
			     netif_tx_request_t *txp,
			     s8       st);
static netif_rx_response_t *make_rx_response(netif_t *netif, 
					     u16      id, 
					     s8       st,
					     u16      offset,
					     u16      size,
					     u16      flags);

static void net_tx_action(unsigned long group);
static void net_rx_action(unsigned long group);

static inline unsigned long idx_to_pfn(struct xen_netbk *netbk, unsigned int idx)
{
	return page_to_pfn(netbk->mmap_pages[idx]);
}

static inline unsigned long idx_to_kaddr(struct xen_netbk *netbk, unsigned int idx)
{
	return (unsigned long)pfn_to_kaddr(idx_to_pfn(netbk, idx));
}

/* extra field used in struct page */
static inline void netif_set_page_ext(struct page *pg, struct page_ext *ext)
{
	pg->mapping = (void *)ext;
}

#define PKT_PROT_LEN 64

#define MASK_PEND_IDX(_i) ((_i)&(MAX_PENDING_REQS-1))

static inline pending_ring_idx_t nr_pending_reqs(const struct xen_netbk *netbk)
{
	return MAX_PENDING_REQS -
		netbk->pending_prod + netbk->pending_cons;
}

/* Setting this allows the safe use of this driver without netloop. */
static int MODPARM_copy_skb = 1;
module_param_named(copy_skb, MODPARM_copy_skb, bool, 0);
MODULE_PARM_DESC(copy_skb, "Copy data received from netfront without netloop");
static int MODPARM_permute_returns = 0;
module_param_named(permute_returns, MODPARM_permute_returns, bool, S_IRUSR|S_IWUSR);
MODULE_PARM_DESC(permute_returns, "Randomly permute the order in which TX responses are sent to the frontend");
module_param_named(groups, netbk_nr_groups, uint, 0);
MODULE_PARM_DESC(groups, "Specify the number of tasklet pairs/threads to use");
module_param_named(tasklets, use_kthreads, invbool, 0);
MODULE_PARM_DESC(tasklets, "Use tasklets instead of kernel threads");
module_param_named(bind, bind_threads, bool, 0);
MODULE_PARM_DESC(bind, "Bind kernel threads to (v)CPUs");

int netbk_copy_skb_mode;

static inline unsigned long alloc_mfn(struct xen_netbk *netbk)
{
	BUG_ON(netbk->alloc_index == 0);
	return netbk->mfn_list[--netbk->alloc_index];
}

static int check_mfn(struct xen_netbk *netbk, unsigned int nr)
{
	struct xen_memory_reservation reservation = {
		.extent_order = 0,
		.domid        = DOMID_SELF
	};
	int rc;

	if (likely(netbk->alloc_index >= nr))
		return 0;

	set_xen_guest_handle(reservation.extent_start,
			     netbk->mfn_list + netbk->alloc_index);
	reservation.nr_extents = MAX_MFN_ALLOC - netbk->alloc_index;
	rc = HYPERVISOR_memory_op(XENMEM_increase_reservation, &reservation);
	if (likely(rc > 0))
		netbk->alloc_index += rc;

	return netbk->alloc_index >= nr ? 0 : -ENOMEM;
}

static inline void maybe_schedule_tx_action(unsigned int group)
{
	struct xen_netbk *netbk = &xen_netbk[group];

	smp_mb();
	if ((nr_pending_reqs(netbk) < (MAX_PENDING_REQS/2)) &&
	    !list_empty(&netbk->net_schedule_list)) {
		if (use_kthreads)
			wake_up(&netbk->netbk_action_wq);
		else
			tasklet_schedule(&netbk->net_tx_tasklet);
	}
}

static struct sk_buff *netbk_copy_skb(struct sk_buff *skb)
{
	struct skb_shared_info *ninfo;
	struct sk_buff *nskb;
	unsigned long offset;
	int ret;
	int len;
	int headlen;

	BUG_ON(skb_shinfo(skb)->frag_list != NULL);

	nskb = alloc_skb(SKB_MAX_HEAD(0), GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!nskb))
		goto err;

	skb_reserve(nskb, 16 + NET_IP_ALIGN);
	headlen = skb_end_pointer(nskb) - nskb->data;
	if (headlen > skb_headlen(skb))
		headlen = skb_headlen(skb);
	ret = skb_copy_bits(skb, 0, __skb_put(nskb, headlen), headlen);
	BUG_ON(ret);

	ninfo = skb_shinfo(nskb);
	ninfo->gso_size = skb_shinfo(skb)->gso_size;
	ninfo->gso_type = skb_shinfo(skb)->gso_type;

	offset = headlen;
	len = skb->len - headlen;

	nskb->len = skb->len;
	nskb->data_len = len;
	nskb->truesize += len;

	while (len) {
		struct page *page;
		int copy;
		int zero;

		if (unlikely(ninfo->nr_frags >= MAX_SKB_FRAGS)) {
			dump_stack();
			goto err_free;
		}

		copy = len >= PAGE_SIZE ? PAGE_SIZE : len;
		zero = len >= PAGE_SIZE ? 0 : __GFP_ZERO;

		page = alloc_page(GFP_ATOMIC | __GFP_NOWARN | zero);
		if (unlikely(!page))
			goto err_free;

		ret = skb_copy_bits(skb, offset, page_address(page), copy);
		BUG_ON(ret);

		ninfo->frags[ninfo->nr_frags].page = page;
		ninfo->frags[ninfo->nr_frags].page_offset = 0;
		ninfo->frags[ninfo->nr_frags].size = copy;
		ninfo->nr_frags++;

		offset += copy;
		len -= copy;
	}

#ifdef NET_SKBUFF_DATA_USES_OFFSET
	offset = 0;
#else
	offset = nskb->data - skb->data;
#endif

	nskb->transport_header = skb->transport_header + offset;
	nskb->network_header   = skb->network_header   + offset;
	nskb->mac_header       = skb->mac_header       + offset;

	return nskb;

 err_free:
	kfree_skb(nskb);
 err:
	return NULL;
}

static inline int netbk_max_required_rx_slots(netif_t *netif)
{
	if (netif->features & (NETIF_F_SG|NETIF_F_TSO))
		return MAX_SKB_FRAGS + 2; /* header + extra_info + frags */
	return 1; /* all in one */
}

static inline int netbk_queue_full(netif_t *netif)
{
	RING_IDX peek   = netif->rx_req_cons_peek;
	RING_IDX needed = netbk_max_required_rx_slots(netif);

	return ((netif->rx.sring->req_prod - peek) < needed) ||
	       ((netif->rx.rsp_prod_pvt + NET_RX_RING_SIZE - peek) < needed);
}

static void tx_queue_callback(unsigned long data)
{
	netif_t *netif = (netif_t *)data;
	if (netif_schedulable(netif))
		netif_wake_queue(netif->dev);
}

int netif_be_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	netif_t *netif = netdev_priv(dev);
	struct xen_netbk *netbk;

	BUG_ON(skb->dev != dev);

	/* Drop the packet if the target domain has no receive buffers. */
	if (unlikely(!netif_schedulable(netif) || netbk_queue_full(netif)))
		goto drop;

	/*
	 * Copy the packet here if it's destined for a flipping interface
	 * but isn't flippable (e.g. extra references to data).
	 * XXX For now we also copy skbuffs whose head crosses a page
	 * boundary, because netbk_gop_skb can't handle them.
	 */
	if (!netif->copying_receiver ||
	    ((skb_headlen(skb) + offset_in_page(skb->data)) >= PAGE_SIZE)) {
		struct sk_buff *nskb = netbk_copy_skb(skb);
		if ( unlikely(nskb == NULL) )
			goto drop;
		/* Copy only the header fields we use in this driver. */
		nskb->dev = skb->dev;
		nskb->ip_summed = skb->ip_summed;
		nskb->proto_data_valid = skb->proto_data_valid;
		dev_kfree_skb(skb);
		skb = nskb;
	}

	netif->rx_req_cons_peek += skb_shinfo(skb)->nr_frags + 1 +
				   !!skb_shinfo(skb)->gso_size;
	netif_get(netif);

	if (netbk_can_queue(dev) && netbk_queue_full(netif)) {
		netif->rx.sring->req_event = netif->rx_req_cons_peek +
			netbk_max_required_rx_slots(netif);
		mb(); /* request notification /then/ check & stop the queue */
		if (netbk_queue_full(netif)) {
			netif_stop_queue(dev);
			/*
			 * Schedule 500ms timeout to restart the queue, thus
			 * ensuring that an inactive queue will be drained.
			 * Packets will be immediately be dropped until more
			 * receive buffers become available (see
			 * netbk_queue_full() check above).
			 */
			netif->tx_queue_timeout.data = (unsigned long)netif;
			netif->tx_queue_timeout.function = tx_queue_callback;
			mod_timer(&netif->tx_queue_timeout, jiffies + HZ/2);
		}
	}

	netbk = &xen_netbk[GET_GROUP_INDEX(netif)];
	skb_queue_tail(&netbk->rx_queue, skb);
	if (use_kthreads)
		wake_up(&netbk->netbk_action_wq);
	else
		tasklet_schedule(&netbk->net_rx_tasklet);

	return NETDEV_TX_OK;

 drop:
	netif->stats.tx_dropped++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

#if 0
static void xen_network_done_notify(void)
{
	static struct net_device *eth0_dev = NULL;
	if (unlikely(eth0_dev == NULL))
		eth0_dev = __dev_get_by_name(&init_net, "eth0");
	napi_schedule(???);
}
/* 
 * Add following to poll() function in NAPI driver (Tigon3 is example):
 *  if ( xen_network_done() )
 *      tg3_enable_ints(tp);
 */
int xen_network_done(void)
{
	return skb_queue_empty(&rx_queue);
}
#endif

struct netrx_pending_operations {
	unsigned trans_prod, trans_cons;
	unsigned mmu_prod, mmu_mcl;
	unsigned mcl_prod, mcl_cons;
	unsigned copy_prod, copy_cons;
	unsigned meta_prod, meta_cons;
	mmu_update_t *mmu;
	gnttab_transfer_t *trans;
	gnttab_copy_t *copy;
	multicall_entry_t *mcl;
	struct netbk_rx_meta *meta;
};

/* Set up the grant operations for this fragment.  If it's a flipping
   interface, we also set up the unmap request from here. */
static u16 netbk_gop_frag(netif_t *netif, struct netbk_rx_meta *meta,
			  int i, struct netrx_pending_operations *npo,
			  struct page *page, unsigned long size,
			  unsigned long offset)
{
	mmu_update_t *mmu;
	gnttab_transfer_t *gop;
	gnttab_copy_t *copy_gop;
	multicall_entry_t *mcl;
	netif_rx_request_t *req;
	unsigned long old_mfn, new_mfn;
	struct xen_netbk *netbk = &xen_netbk[GET_GROUP_INDEX(netif)];

	old_mfn = virt_to_mfn(page_address(page));

	req = RING_GET_REQUEST(&netif->rx, netif->rx.req_cons + i);
	if (netif->copying_receiver) {
		struct page_ext *ext;

		/* The fragment needs to be copied rather than
		   flipped. */
		meta->copy = 1;
		copy_gop = npo->copy + npo->copy_prod++;
		copy_gop->flags = GNTCOPY_dest_gref;
		if (PageForeign(page) &&
		    (ext = (void *)page->mapping) != NULL &&
		    ext->idx < MAX_PENDING_REQS &&
		    ext->group < netbk_nr_groups) {
			struct pending_tx_info *src_pend;

			netbk = &xen_netbk[ext->group];
			BUG_ON(ext < netbk->page_extinfo ||
			       ext >= netbk->page_extinfo +
				      ARRAY_SIZE(netbk->page_extinfo));
			BUG_ON(netbk->mmap_pages[ext->idx] != page);
			src_pend = &netbk->pending_tx_info[ext->idx];
			BUG_ON(ext->group != GET_GROUP_INDEX(src_pend->netif));
			copy_gop->source.domid = src_pend->netif->domid;
			copy_gop->source.u.ref = src_pend->req.gref;
			copy_gop->flags |= GNTCOPY_source_gref;
		} else {
			copy_gop->source.domid = DOMID_SELF;
			copy_gop->source.u.gmfn = old_mfn;
		}
		copy_gop->source.offset = offset;
		copy_gop->dest.domid = netif->domid;
		copy_gop->dest.offset = 0;
		copy_gop->dest.u.ref = req->gref;
		copy_gop->len = size;
	} else {
		meta->copy = 0;
		if (!xen_feature(XENFEAT_auto_translated_physmap)) {
			new_mfn = alloc_mfn(netbk);

			/*
			 * Set the new P2M table entry before
			 * reassigning the old data page. Heed the
			 * comment in pgtable-2level.h:pte_page(). :-)
			 */
			set_phys_to_machine(page_to_pfn(page), new_mfn);

			mcl = npo->mcl + npo->mcl_prod++;
			MULTI_update_va_mapping(mcl,
					     (unsigned long)page_address(page),
					     pfn_pte_ma(new_mfn, PAGE_KERNEL),
					     0);

			mmu = npo->mmu + npo->mmu_prod++;
			mmu->ptr = ((maddr_t)new_mfn << PAGE_SHIFT) |
				MMU_MACHPHYS_UPDATE;
			mmu->val = page_to_pfn(page);
		}

		gop = npo->trans + npo->trans_prod++;
		gop->mfn = old_mfn;
		gop->domid = netif->domid;
		gop->ref = req->gref;
	}
	return req->id;
}

static void netbk_gop_skb(struct sk_buff *skb,
			  struct netrx_pending_operations *npo)
{
	netif_t *netif = netdev_priv(skb->dev);
	int nr_frags = skb_shinfo(skb)->nr_frags;
	int i;
	int extra;
	struct netbk_rx_meta *head_meta, *meta;

	head_meta = npo->meta + npo->meta_prod++;
	head_meta->frag.page_offset = skb_shinfo(skb)->gso_type;
	head_meta->frag.size = skb_shinfo(skb)->gso_size;
	extra = !!head_meta->frag.size + 1;

	for (i = 0; i < nr_frags; i++) {
		meta = npo->meta + npo->meta_prod++;
		meta->frag = skb_shinfo(skb)->frags[i];
		meta->id = netbk_gop_frag(netif, meta, i + extra, npo,
					  meta->frag.page,
					  meta->frag.size,
					  meta->frag.page_offset);
	}

	/*
	 * This must occur at the end to ensure that we don't trash skb_shinfo
	 * until we're done. We know that the head doesn't cross a page
	 * boundary because such packets get copied in netif_be_start_xmit.
	 */
	head_meta->id = netbk_gop_frag(netif, head_meta, 0, npo,
				       virt_to_page(skb->data),
				       skb_headlen(skb),
				       offset_in_page(skb->data));

	netif->rx.req_cons += nr_frags + extra;
}

static inline void netbk_free_pages(int nr_frags, struct netbk_rx_meta *meta)
{
	int i;

	for (i = 0; i < nr_frags; i++)
		put_page(meta[i].frag.page);
}

/* This is a twin to netbk_gop_skb.  Assume that netbk_gop_skb was
   used to set up the operations on the top of
   netrx_pending_operations, which have since been done.  Check that
   they didn't give any errors and advance over them. */
static int netbk_check_gop(int nr_frags, domid_t domid,
			   struct netrx_pending_operations *npo, int *eagain)
{
	multicall_entry_t *mcl;
	gnttab_transfer_t *gop;
	gnttab_copy_t     *copy_op;
	int status = NETIF_RSP_OKAY;
	int i;

    *eagain = 0;

	for (i = 0; i <= nr_frags; i++) {
		if (npo->meta[npo->meta_cons + i].copy) {
			copy_op = npo->copy + npo->copy_cons++;
			if (copy_op->status != GNTST_okay) {
				DPRINTK("Bad status %d from copy to DOM%d.\n",
					copy_op->status, domid);
				status = NETIF_RSP_ERROR;
                if(copy_op->status == GNTST_eagain)
                    *eagain = 1;
			}
		} else {
			if (!xen_feature(XENFEAT_auto_translated_physmap)) {
				mcl = npo->mcl + npo->mcl_cons++;
				/* The update_va_mapping() must not fail. */
				BUG_ON(mcl->result != 0);
			}

			gop = npo->trans + npo->trans_cons++;
			/* Check the reassignment error code. */
			if (gop->status != 0) {
				DPRINTK("Bad status %d from grant transfer to DOM%u\n",
					gop->status, domid);
				/*
				 * Page no longer belongs to us unless
				 * GNTST_bad_page, but that should be
				 * a fatal error anyway.
				 */
				BUG_ON(gop->status == GNTST_bad_page);
                if(gop->status == GNTST_eagain)
                    *eagain = 1;
				status = NETIF_RSP_ERROR;
			}
		}
	}

	return status;
}

static void netbk_add_frag_responses(netif_t *netif, int status,
				     struct netbk_rx_meta *meta, int nr_frags)
{
	int i;
	unsigned long offset;

	for (i = 0; i < nr_frags; i++) {
		int id = meta[i].id;
		int flags = (i == nr_frags - 1) ? 0 : NETRXF_more_data;

		if (meta[i].copy)
			offset = 0;
		else
			offset = meta[i].frag.page_offset;
		make_rx_response(netif, id, status, offset,
				 meta[i].frag.size, flags);
	}
}

static void net_rx_action(unsigned long group)
{
	netif_t *netif = NULL;
	s8 status;
	u16 id, irq, flags;
	netif_rx_response_t *resp;
	multicall_entry_t *mcl;
	struct sk_buff_head rxq;
	struct sk_buff *skb;
	int notify_nr = 0;
	int ret, eagain;
	int nr_frags;
	int count;
	unsigned long offset;
	struct xen_netbk *netbk = &xen_netbk[group];

	struct netrx_pending_operations npo = {
		.mmu   = netbk->rx_mmu,
		.trans = netbk->grant_trans_op,
		.copy  = netbk->grant_copy_op,
		.mcl   = netbk->rx_mcl,
		.meta  = netbk->meta,
	};

	skb_queue_head_init(&rxq);

	count = 0;

	while ((skb = skb_dequeue(&netbk->rx_queue)) != NULL) {
		nr_frags = skb_shinfo(skb)->nr_frags;
		*(int *)skb->cb = nr_frags;

		if (!xen_feature(XENFEAT_auto_translated_physmap) &&
		    !((netif_t *)netdev_priv(skb->dev))->copying_receiver &&
		    check_mfn(netbk, nr_frags + 1)) {
			/* Memory squeeze? Back off for an arbitrary while. */
			if ( net_ratelimit() )
				WPRINTK("Memory squeeze in netback "
					"driver.\n");
			mod_timer(&netbk->net_timer, jiffies + HZ);
			skb_queue_head(&netbk->rx_queue, skb);
			break;
		}

		netbk_gop_skb(skb, &npo);

		count += nr_frags + 1;

		__skb_queue_tail(&rxq, skb);

		/* Filled the batch queue? */
		if (count + MAX_SKB_FRAGS >= NET_RX_RING_SIZE)
			break;
	}

	BUG_ON(npo.meta_prod > ARRAY_SIZE(netbk->meta));

	npo.mmu_mcl = npo.mcl_prod;
	if (npo.mcl_prod) {
		BUG_ON(xen_feature(XENFEAT_auto_translated_physmap));
		BUG_ON(npo.mmu_prod > ARRAY_SIZE(netbk->rx_mmu));
		mcl = npo.mcl + npo.mcl_prod++;

		BUG_ON(mcl[-1].op != __HYPERVISOR_update_va_mapping);
		mcl[-1].args[MULTI_UVMFLAGS_INDEX] = UVMF_TLB_FLUSH|UVMF_ALL;

		mcl->op = __HYPERVISOR_mmu_update;
		mcl->args[0] = (unsigned long)netbk->rx_mmu;
		mcl->args[1] = npo.mmu_prod;
		mcl->args[2] = 0;
		mcl->args[3] = DOMID_SELF;
	}

	if (npo.trans_prod) {
		BUG_ON(npo.trans_prod > ARRAY_SIZE(netbk->grant_trans_op));
		mcl = npo.mcl + npo.mcl_prod++;
		mcl->op = __HYPERVISOR_grant_table_op;
		mcl->args[0] = GNTTABOP_transfer;
		mcl->args[1] = (unsigned long)netbk->grant_trans_op;
		mcl->args[2] = npo.trans_prod;
	}

	if (npo.copy_prod) {
		BUG_ON(npo.copy_prod > ARRAY_SIZE(netbk->grant_copy_op));
		mcl = npo.mcl + npo.mcl_prod++;
		mcl->op = __HYPERVISOR_grant_table_op;
		mcl->args[0] = GNTTABOP_copy;
		mcl->args[1] = (unsigned long)netbk->grant_copy_op;
		mcl->args[2] = npo.copy_prod;
	}

	/* Nothing to do? */
	if (!npo.mcl_prod)
		return;

	BUG_ON(npo.mcl_prod > ARRAY_SIZE(netbk->rx_mcl));

	ret = HYPERVISOR_multicall(npo.mcl, npo.mcl_prod);
	BUG_ON(ret != 0);
	/* The mmu_machphys_update() must not fail. */
	BUG_ON(npo.mmu_mcl && npo.mcl[npo.mmu_mcl].result != 0);

	while ((skb = __skb_dequeue(&rxq)) != NULL) {
		nr_frags = *(int *)skb->cb;

		netif = netdev_priv(skb->dev);

		status = netbk_check_gop(nr_frags, netif->domid, &npo, &eagain);

		/* We can't rely on skb_release_data to release the
		   pages used by fragments for us, since it tries to
		   touch the pages in the fraglist.  If we're in
		   flipping mode, that doesn't work.  In copying mode,
		   we still have access to all of the pages, and so
		   it's safe to let release_data deal with it. */
		/* (Freeing the fragments is safe since we copy
		   non-linear skbs destined for flipping interfaces) */
		if (!netif->copying_receiver) {
            /* 
             * Cannot handle failed grant transfers at the moment (because
             * mmu_updates likely completed)
             */
            BUG_ON(eagain);
			atomic_set(&(skb_shinfo(skb)->dataref), 1);
			skb_shinfo(skb)->frag_list = NULL;
			skb_shinfo(skb)->nr_frags = 0;
			netbk_free_pages(nr_frags, netbk->meta + npo.meta_cons + 1);
		}

        if(!eagain)
        {
		    netif->stats.tx_bytes += skb->len;
		    netif->stats.tx_packets++;
        }

		id = netbk->meta[npo.meta_cons].id;
		flags = nr_frags ? NETRXF_more_data : 0;

		if (skb->ip_summed == CHECKSUM_PARTIAL) /* local packet? */
			flags |= NETRXF_csum_blank | NETRXF_data_validated;
		else if (skb->proto_data_valid) /* remote but checksummed? */
			flags |= NETRXF_data_validated;

		if (netbk->meta[npo.meta_cons].copy)
			offset = 0;
		else
			offset = offset_in_page(skb->data);
		resp = make_rx_response(netif, id, status, offset,
					skb_headlen(skb), flags);

		if (netbk->meta[npo.meta_cons].frag.size) {
			struct netif_extra_info *gso =
				(struct netif_extra_info *)
				RING_GET_RESPONSE(&netif->rx,
						  netif->rx.rsp_prod_pvt++);

			resp->flags |= NETRXF_extra_info;

			gso->u.gso.size = netbk->meta[npo.meta_cons].frag.size;
			gso->u.gso.type = XEN_NETIF_GSO_TYPE_TCPV4;
			gso->u.gso.pad = 0;
			gso->u.gso.features = 0;

			gso->type = XEN_NETIF_EXTRA_TYPE_GSO;
			gso->flags = 0;
		}

		netbk_add_frag_responses(netif, status,
					 netbk->meta + npo.meta_cons + 1,
					 nr_frags);

		RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&netif->rx, ret);
		irq = netif->irq - DYNIRQ_BASE;
		if (ret && !__test_and_set_bit(irq, netbk->rx_notify))
			netbk->notify_list[notify_nr++] = irq;

		if (netif_queue_stopped(netif->dev) &&
		    netif_schedulable(netif) &&
		    !netbk_queue_full(netif))
			netif_wake_queue(netif->dev);

        if(!eagain || netbk_queue_full(netif))
        {
		    netif_put(netif);
		    dev_kfree_skb(skb);
		    netif->stats.tx_dropped += !!eagain;
        } 
        else
        {
	        netif->rx_req_cons_peek += skb_shinfo(skb)->nr_frags + 1 +
				   !!skb_shinfo(skb)->gso_size;
            skb_queue_head(&netbk->rx_queue, skb);
        }

		npo.meta_cons += nr_frags + 1;
	}

	if (notify_nr == 1) {
		irq = *netbk->notify_list;
		__clear_bit(irq, netbk->rx_notify);
		notify_remote_via_irq(irq + DYNIRQ_BASE);
	} else {
		for (count = ret = 0; ret < notify_nr; ++ret) {
			irq = netbk->notify_list[ret];
			__clear_bit(irq, netbk->rx_notify);
			if (!multi_notify_remote_via_irq(netbk->rx_mcl + count,
							 irq + DYNIRQ_BASE))
				++count;
		}
		if (HYPERVISOR_multicall(netbk->rx_mcl, count))
			BUG();
	}

	/* More work to do? */
	if (!skb_queue_empty(&netbk->rx_queue) &&
	    !timer_pending(&netbk->net_timer)) {
		if (use_kthreads)
			wake_up(&netbk->netbk_action_wq);
		else
			tasklet_schedule(&netbk->net_rx_tasklet);
	}
#if 0
	else
		xen_network_done_notify();
#endif
}

static void net_alarm(unsigned long group)
{
	if (use_kthreads)
		wake_up(&xen_netbk[group].netbk_action_wq);
	else
		tasklet_schedule(&xen_netbk[group].net_rx_tasklet);
}

static void netbk_tx_pending_timeout(unsigned long group)
{
	if (use_kthreads)
		wake_up(&xen_netbk[group].netbk_action_wq);
	else
		tasklet_schedule(&xen_netbk[group].net_tx_tasklet);
}

struct net_device_stats *netif_be_get_stats(struct net_device *dev)
{
	netif_t *netif = netdev_priv(dev);
	return &netif->stats;
}

static int __on_net_schedule_list(netif_t *netif)
{
	return netif->list.next != NULL;
}

static void remove_from_net_schedule_list(netif_t *netif)
{
	struct xen_netbk *netbk = &xen_netbk[GET_GROUP_INDEX(netif)];

	spin_lock_irq(&netbk->net_schedule_list_lock);
	if (likely(__on_net_schedule_list(netif))) {
		list_del(&netif->list);
		netif->list.next = NULL;
		netif_put(netif);
	}
	spin_unlock_irq(&netbk->net_schedule_list_lock);
}

static void add_to_net_schedule_list_tail(netif_t *netif)
{
	struct xen_netbk *netbk = &xen_netbk[GET_GROUP_INDEX(netif)];

	if (__on_net_schedule_list(netif))
		return;

	spin_lock_irq(&netbk->net_schedule_list_lock);
	if (!__on_net_schedule_list(netif) &&
	    likely(netif_schedulable(netif))) {
		list_add_tail(&netif->list, &netbk->net_schedule_list);
		netif_get(netif);
	}
	spin_unlock_irq(&netbk->net_schedule_list_lock);
}

/*
 * Note on CONFIG_XEN_NETDEV_PIPELINED_TRANSMITTER:
 * If this driver is pipelining transmit requests then we can be very
 * aggressive in avoiding new-packet notifications -- frontend only needs to
 * send a notification if there are no outstanding unreceived responses.
 * If we may be buffer transmit buffers for any reason then we must be rather
 * more conservative and treat this as the final check for pending work.
 */
void netif_schedule_work(netif_t *netif)
{
	int more_to_do;

#ifdef CONFIG_XEN_NETDEV_PIPELINED_TRANSMITTER
	more_to_do = RING_HAS_UNCONSUMED_REQUESTS(&netif->tx);
#else
	RING_FINAL_CHECK_FOR_REQUESTS(&netif->tx, more_to_do);
#endif

	if (more_to_do) {
		add_to_net_schedule_list_tail(netif);
		maybe_schedule_tx_action(GET_GROUP_INDEX(netif));
	}
}

void netif_deschedule_work(netif_t *netif)
{
	remove_from_net_schedule_list(netif);
}


static void tx_add_credit(netif_t *netif)
{
	unsigned long max_burst, max_credit;

	/*
	 * Allow a burst big enough to transmit a jumbo packet of up to 128kB.
	 * Otherwise the interface can seize up due to insufficient credit.
	 */
	max_burst = RING_GET_REQUEST(&netif->tx, netif->tx.req_cons)->size;
	max_burst = min(max_burst, 131072UL);
	max_burst = max(max_burst, netif->credit_bytes);

	/* Take care that adding a new chunk of credit doesn't wrap to zero. */
	max_credit = netif->remaining_credit + netif->credit_bytes;
	if (max_credit < netif->remaining_credit)
		max_credit = ULONG_MAX; /* wrapped: clamp to ULONG_MAX */

	netif->remaining_credit = min(max_credit, max_burst);
}

static void tx_credit_callback(unsigned long data)
{
	netif_t *netif = (netif_t *)data;
	tx_add_credit(netif);
	netif_schedule_work(netif);
}

static inline int copy_pending_req(struct xen_netbk *netbk,
				   pending_ring_idx_t pending_idx)
{
	return gnttab_copy_grant_page(netbk->grant_tx_handle[pending_idx],
				      &netbk->mmap_pages[pending_idx]);
}

static void permute_dealloc_ring(u16 *dealloc_ring, pending_ring_idx_t dc,
				 pending_ring_idx_t dp)
{
	static unsigned random_src = 0x12345678;
	unsigned dst_offset;
	pending_ring_idx_t dest;
	u16 tmp;

	while (dc != dp) {
		dst_offset = (random_src / 256) % (dp - dc);
		dest = dc + dst_offset;
		tmp = dealloc_ring[MASK_PEND_IDX(dest)];
		dealloc_ring[MASK_PEND_IDX(dest)] =
			dealloc_ring[MASK_PEND_IDX(dc)];
		dealloc_ring[MASK_PEND_IDX(dc)] = tmp;
		dc++;
		random_src *= 68389;
	}
}

inline static void net_tx_action_dealloc(struct xen_netbk *netbk)
{
	struct netbk_tx_pending_inuse *inuse, *n;
	gnttab_unmap_grant_ref_t *gop;
	u16 pending_idx;
	pending_ring_idx_t dc, dp;
	netif_t *netif;
	int ret;
	LIST_HEAD(list);

	dc = netbk->dealloc_cons;
	gop = netbk->tx_unmap_ops;

	/*
	 * Free up any grants we have finished using
	 */
	do {
		dp = netbk->dealloc_prod;

		/* Ensure we see all indices enqueued by netif_idx_release(). */
		smp_rmb();

		if (MODPARM_permute_returns)
			permute_dealloc_ring(netbk->dealloc_ring, dc, dp);

		while (dc != dp) {
			unsigned long pfn;
			struct netbk_tx_pending_inuse *pending_inuse =
					netbk->pending_inuse;

			pending_idx = netbk->dealloc_ring[MASK_PEND_IDX(dc++)];
			list_move_tail(&pending_inuse[pending_idx].list, &list);

			pfn = idx_to_pfn(netbk, pending_idx);
			/* Already unmapped? */
			if (!phys_to_machine_mapping_valid(pfn))
				continue;

			gnttab_set_unmap_op(gop, idx_to_kaddr(netbk, pending_idx),
					    GNTMAP_host_map,
					    netbk->grant_tx_handle[pending_idx]);
			gop++;
		}

		if (netbk_copy_skb_mode != NETBK_DELAYED_COPY_SKB ||
		    list_empty(&netbk->pending_inuse_head))
			break;

		/* Copy any entries that have been pending for too long. */
		list_for_each_entry_safe(inuse, n, &netbk->pending_inuse_head, list) {
			struct pending_tx_info *pending_tx_info
				= netbk->pending_tx_info;

			if (time_after(inuse->alloc_time + HZ / 2, jiffies))
				break;

			pending_idx = inuse - netbk->pending_inuse;

			pending_tx_info[pending_idx].netif->nr_copied_skbs++;

			switch (copy_pending_req(netbk, pending_idx)) {
			case 0:
				list_move_tail(&inuse->list, &list);
				continue;
			case -EBUSY:
				list_del_init(&inuse->list);
				continue;
			case -ENOENT:
				continue;
			}

			break;
		}
	} while (dp != netbk->dealloc_prod);

	netbk->dealloc_cons = dc;

	ret = HYPERVISOR_grant_table_op(
		GNTTABOP_unmap_grant_ref, netbk->tx_unmap_ops,
		gop - netbk->tx_unmap_ops);
	BUG_ON(ret);

	list_for_each_entry_safe(inuse, n, &list, list) {
		struct pending_tx_info *pending_tx_info =
			netbk->pending_tx_info;

		pending_idx = inuse - netbk->pending_inuse;
		netif = pending_tx_info[pending_idx].netif;

		make_tx_response(netif, &pending_tx_info[pending_idx].req, 
				 NETIF_RSP_OKAY);

		/* Ready for next use. */
		gnttab_reset_grant_page(netbk->mmap_pages[pending_idx]);

		netbk->pending_ring[MASK_PEND_IDX(netbk->pending_prod++)] =
			pending_idx;

		netif_put(netif);

		list_del_init(&inuse->list);
	}
}

static void netbk_tx_err(netif_t *netif, netif_tx_request_t *txp, RING_IDX end)
{
	RING_IDX cons = netif->tx.req_cons;

	do {
		make_tx_response(netif, txp, NETIF_RSP_ERROR);
		if (cons >= end)
			break;
		txp = RING_GET_REQUEST(&netif->tx, cons++);
	} while (1);
	netif->tx.req_cons = cons;
	netif_schedule_work(netif);
	netif_put(netif);
}

static int netbk_count_requests(netif_t *netif, netif_tx_request_t *first,
				netif_tx_request_t *txp, int work_to_do)
{
	RING_IDX cons = netif->tx.req_cons;
	int frags = 0;

	if (!(first->flags & NETTXF_more_data))
		return 0;

	do {
		if (frags >= work_to_do) {
			DPRINTK("Need more frags\n");
			return -frags;
		}

		if (unlikely(frags >= MAX_SKB_FRAGS)) {
			DPRINTK("Too many frags\n");
			return -frags;
		}

		memcpy(txp, RING_GET_REQUEST(&netif->tx, cons + frags),
		       sizeof(*txp));
		if (txp->size > first->size) {
			DPRINTK("Frags galore\n");
			return -frags;
		}

		first->size -= txp->size;
		frags++;

		if (unlikely((txp->offset + txp->size) > PAGE_SIZE)) {
			DPRINTK("txp->offset: %x, size: %u\n",
				txp->offset, txp->size);
			return -frags;
		}
	} while ((txp++)->flags & NETTXF_more_data);

	return frags;
}

static gnttab_map_grant_ref_t *netbk_get_requests(netif_t *netif,
						  struct sk_buff *skb,
						  netif_tx_request_t *txp,
						  gnttab_map_grant_ref_t *mop)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	skb_frag_t *frags = shinfo->frags;
	unsigned long pending_idx = *((u16 *)skb->data);
	int i, start;

	/* Skip first skb fragment if it is on same page as header fragment. */
	start = ((unsigned long)shinfo->frags[0].page == pending_idx);

	for (i = start; i < shinfo->nr_frags; i++, txp++) {
		struct xen_netbk *netbk = &xen_netbk[GET_GROUP_INDEX(netif)];
		pending_ring_idx_t index = MASK_PEND_IDX(netbk->pending_cons++);
		struct pending_tx_info *pending_tx_info =
			netbk->pending_tx_info;

		pending_idx = netbk->pending_ring[index];

		gnttab_set_map_op(mop++, idx_to_kaddr(netbk, pending_idx),
				  GNTMAP_host_map | GNTMAP_readonly,
				  txp->gref, netif->domid);

		memcpy(&pending_tx_info[pending_idx].req, txp, sizeof(*txp));
		netif_get(netif);
		pending_tx_info[pending_idx].netif = netif;
		frags[i].page = (void *)pending_idx;
	}

	return mop;
}

static int netbk_tx_check_mop(struct xen_netbk *netbk, struct sk_buff *skb,
			      gnttab_map_grant_ref_t **mopp)
{
	gnttab_map_grant_ref_t *mop = *mopp;
	int pending_idx = *((u16 *)skb->data);
	struct pending_tx_info *pending_tx_info = netbk->pending_tx_info;
	netif_t *netif = pending_tx_info[pending_idx].netif;
	netif_tx_request_t *txp;
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int nr_frags = shinfo->nr_frags;
	int i, err, start;

	/* Check status of header. */
	err = mop->status;
	if (unlikely(err)) {
		pending_ring_idx_t index = MASK_PEND_IDX(netbk->pending_prod++);

		txp = &pending_tx_info[pending_idx].req;
		make_tx_response(netif, txp, NETIF_RSP_ERROR);
		netbk->pending_ring[index] = pending_idx;
		netif_put(netif);
	} else {
		set_phys_to_machine(idx_to_pfn(netbk, pending_idx),
			FOREIGN_FRAME(mop->dev_bus_addr >> PAGE_SHIFT));
		netbk->grant_tx_handle[pending_idx] = mop->handle;
	}

	/* Skip first skb fragment if it is on same page as header fragment. */
	start = ((unsigned long)shinfo->frags[0].page == pending_idx);

	for (i = start; i < nr_frags; i++) {
		int j, newerr;
		pending_ring_idx_t index;

		pending_idx = (unsigned long)shinfo->frags[i].page;

		/* Check error status: if okay then remember grant handle. */
		newerr = (++mop)->status;
		if (likely(!newerr)) {
			set_phys_to_machine(idx_to_pfn(netbk, pending_idx),
				FOREIGN_FRAME(mop->dev_bus_addr>>PAGE_SHIFT));
			netbk->grant_tx_handle[pending_idx] = mop->handle;
			/* Had a previous error? Invalidate this fragment. */
			if (unlikely(err))
				netif_idx_release(netbk, pending_idx);
			continue;
		}

		/* Error on this fragment: respond to client with an error. */
		txp = &pending_tx_info[pending_idx].req;
		make_tx_response(netif, txp, NETIF_RSP_ERROR);
		index = MASK_PEND_IDX(netbk->pending_prod++);
		netbk->pending_ring[index] = pending_idx;
		netif_put(netif);

		/* Not the first error? Preceding frags already invalidated. */
		if (err)
			continue;

		/* First error: invalidate header and preceding fragments. */
		pending_idx = *((u16 *)skb->data);
		netif_idx_release(netbk, pending_idx);
		for (j = start; j < i; j++) {
			pending_idx = (unsigned long)shinfo->frags[i].page;
			netif_idx_release(netbk, pending_idx);
		}

		/* Remember the error: invalidate all subsequent fragments. */
		err = newerr;
	}

	*mopp = mop + 1;
	return err;
}

static void netbk_fill_frags(struct xen_netbk *netbk, struct sk_buff *skb)
{
	struct skb_shared_info *shinfo = skb_shinfo(skb);
	int nr_frags = shinfo->nr_frags;
	int i;

	for (i = 0; i < nr_frags; i++) {
		skb_frag_t *frag = shinfo->frags + i;
		netif_tx_request_t *txp;
		unsigned long pending_idx;

		pending_idx = (unsigned long)frag->page;

		netbk->pending_inuse[pending_idx].alloc_time = jiffies;
		list_add_tail(&netbk->pending_inuse[pending_idx].list,
			      &netbk->pending_inuse_head);

		txp = &netbk->pending_tx_info[pending_idx].req;
		frag->page = netbk->mmap_pages[pending_idx];
		frag->size = txp->size;
		frag->page_offset = txp->offset;

		skb->len += txp->size;
		skb->data_len += txp->size;
		skb->truesize += txp->size;
	}
}

int netbk_get_extras(netif_t *netif, struct netif_extra_info *extras,
		     int work_to_do)
{
	struct netif_extra_info extra;
	RING_IDX cons = netif->tx.req_cons;

	do {
		if (unlikely(work_to_do-- <= 0)) {
			DPRINTK("Missing extra info\n");
			return -EBADR;
		}

		memcpy(&extra, RING_GET_REQUEST(&netif->tx, cons),
		       sizeof(extra));
		if (unlikely(!extra.type ||
			     extra.type >= XEN_NETIF_EXTRA_TYPE_MAX)) {
			netif->tx.req_cons = ++cons;
			DPRINTK("Invalid extra type: %d\n", extra.type);
			return -EINVAL;
		}

		memcpy(&extras[extra.type - 1], &extra, sizeof(extra));
		netif->tx.req_cons = ++cons;
	} while (extra.flags & XEN_NETIF_EXTRA_FLAG_MORE);

	return work_to_do;
}

static int netbk_set_skb_gso(struct sk_buff *skb, struct netif_extra_info *gso)
{
	if (!gso->u.gso.size) {
		DPRINTK("GSO size must not be zero.\n");
		return -EINVAL;
	}

	/* Currently only TCPv4 S.O. is supported. */
	if (gso->u.gso.type != XEN_NETIF_GSO_TYPE_TCPV4) {
		DPRINTK("Bad GSO type %d.\n", gso->u.gso.type);
		return -EINVAL;
	}

	skb_shinfo(skb)->gso_size = gso->u.gso.size;
	skb_shinfo(skb)->gso_type = SKB_GSO_TCPV4;

	/* Header must be checked, and gso_segs computed. */
	skb_shinfo(skb)->gso_type |= SKB_GSO_DODGY;
	skb_shinfo(skb)->gso_segs = 0;

	return 0;
}

/* Called after netfront has transmitted */
static void net_tx_action(unsigned long group)
{
	struct xen_netbk *netbk = &xen_netbk[group];
	struct sk_buff *skb;
	netif_t *netif;
	netif_tx_request_t txreq;
	netif_tx_request_t txfrags[MAX_SKB_FRAGS];
	struct netif_extra_info extras[XEN_NETIF_EXTRA_TYPE_MAX - 1];
	u16 pending_idx;
	RING_IDX i;
	gnttab_map_grant_ref_t *mop;
	unsigned int data_len;
	int ret, work_to_do;

	net_tx_action_dealloc(netbk);

	mop = netbk->tx_map_ops;
	BUILD_BUG_ON(MAX_SKB_FRAGS >= MAX_PENDING_REQS);
	while (((nr_pending_reqs(netbk) + MAX_SKB_FRAGS) < MAX_PENDING_REQS) &&
		!list_empty(&netbk->net_schedule_list)) {
		/* Get a netif from the list with work to do. */
		netif = list_first_entry(&netbk->net_schedule_list,
					 netif_t, list);
		netif_get(netif);
		remove_from_net_schedule_list(netif);

		RING_FINAL_CHECK_FOR_REQUESTS(&netif->tx, work_to_do);
		if (!work_to_do) {
			netif_put(netif);
			continue;
		}

		i = netif->tx.req_cons;
		rmb(); /* Ensure that we see the request before we copy it. */
		memcpy(&txreq, RING_GET_REQUEST(&netif->tx, i), sizeof(txreq));

		/* Credit-based scheduling. */
		if (txreq.size > netif->remaining_credit) {
			unsigned long now = jiffies;
			unsigned long next_credit = 
				netif->credit_timeout.expires +
				msecs_to_jiffies(netif->credit_usec / 1000);

			/* Timer could already be pending in rare cases. */
			if (timer_pending(&netif->credit_timeout)) {
				netif_put(netif);
				continue;
			}

			/* Passed the point where we can replenish credit? */
			if (time_after_eq(now, next_credit)) {
				netif->credit_timeout.expires = now;
				tx_add_credit(netif);
			}

			/* Still too big to send right now? Set a callback. */
			if (txreq.size > netif->remaining_credit) {
				netif->credit_timeout.data     =
					(unsigned long)netif;
				netif->credit_timeout.function =
					tx_credit_callback;
				mod_timer(&netif->credit_timeout, next_credit);
				netif_put(netif);
				continue;
			}
		}
		netif->remaining_credit -= txreq.size;

		work_to_do--;
		netif->tx.req_cons = ++i;

		memset(extras, 0, sizeof(extras));
		if (txreq.flags & NETTXF_extra_info) {
			work_to_do = netbk_get_extras(netif, extras,
						      work_to_do);
			i = netif->tx.req_cons;
			if (unlikely(work_to_do < 0)) {
				netbk_tx_err(netif, &txreq, i);
				continue;
			}
		}

		ret = netbk_count_requests(netif, &txreq, txfrags, work_to_do);
		if (unlikely(ret < 0)) {
			netbk_tx_err(netif, &txreq, i - ret);
			continue;
		}
		i += ret;

		if (unlikely(txreq.size < ETH_HLEN)) {
			DPRINTK("Bad packet size: %d\n", txreq.size);
			netbk_tx_err(netif, &txreq, i);
			continue;
		}

		/* No crossing a page as the payload mustn't fragment. */
		if (unlikely((txreq.offset + txreq.size) > PAGE_SIZE)) {
			DPRINTK("txreq.offset: %x, size: %u, end: %lu\n", 
				txreq.offset, txreq.size, 
				(txreq.offset &~PAGE_MASK) + txreq.size);
			netbk_tx_err(netif, &txreq, i);
			continue;
		}

		pending_idx = netbk->pending_ring[MASK_PEND_IDX(netbk->pending_cons)];

		data_len = (txreq.size > PKT_PROT_LEN &&
			    ret < MAX_SKB_FRAGS) ?
			PKT_PROT_LEN : txreq.size;

		skb = alloc_skb(data_len + 16 + NET_IP_ALIGN,
				GFP_ATOMIC | __GFP_NOWARN);
		if (unlikely(skb == NULL)) {
			DPRINTK("Can't allocate a skb in start_xmit.\n");
			netbk_tx_err(netif, &txreq, i);
			break;
		}

		/* Packets passed to netif_rx() must have some headroom. */
		skb_reserve(skb, 16 + NET_IP_ALIGN);

		if (extras[XEN_NETIF_EXTRA_TYPE_GSO - 1].type) {
			struct netif_extra_info *gso;
			gso = &extras[XEN_NETIF_EXTRA_TYPE_GSO - 1];

			if (netbk_set_skb_gso(skb, gso)) {
				kfree_skb(skb);
				netbk_tx_err(netif, &txreq, i);
				continue;
			}
		}

		gnttab_set_map_op(mop, idx_to_kaddr(netbk, pending_idx),
				  GNTMAP_host_map | GNTMAP_readonly,
				  txreq.gref, netif->domid);
		mop++;

		memcpy(&netbk->pending_tx_info[pending_idx].req,
		       &txreq, sizeof(txreq));
		netbk->pending_tx_info[pending_idx].netif = netif;
		*((u16 *)skb->data) = pending_idx;

		__skb_put(skb, data_len);

		skb_shinfo(skb)->nr_frags = ret;
		if (data_len < txreq.size) {
			skb_shinfo(skb)->nr_frags++;
			skb_shinfo(skb)->frags[0].page =
				(void *)(unsigned long)pending_idx;
		} else {
			/* Discriminate from any valid pending_idx value. */
			skb_shinfo(skb)->frags[0].page = (void *)~0UL;
		}

		__skb_queue_tail(&netbk->tx_queue, skb);

		netbk->pending_cons++;

		mop = netbk_get_requests(netif, skb, txfrags, mop);

		netif->tx.req_cons = i;
		netif_schedule_work(netif);

		if ((mop - netbk->tx_map_ops) >= ARRAY_SIZE(netbk->tx_map_ops))
			break;
	}

	if (mop == netbk->tx_map_ops)
		goto out;

    /* NOTE: some maps may fail with GNTST_eagain, which could be successfully
     * retried in the backend after a delay. However, we can also fail the tx
     * req and let the frontend resend the relevant packet again. This is fine
     * because it is unlikely that a network buffer will be paged out or shared,
     * and therefore it is unlikely to fail with GNTST_eagain. */
	ret = HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref,
					netbk->tx_map_ops,
					mop - netbk->tx_map_ops);
	BUG_ON(ret);

	mop = netbk->tx_map_ops;
	while ((skb = __skb_dequeue(&netbk->tx_queue)) != NULL) {
		netif_tx_request_t *txp;

		pending_idx = *((u16 *)skb->data);
		netif = netbk->pending_tx_info[pending_idx].netif;
		txp = &netbk->pending_tx_info[pending_idx].req;

		/* Check the remap error code. */
		if (unlikely(netbk_tx_check_mop(netbk, skb, &mop))) {
			DPRINTK("netback grant failed.\n");
			skb_shinfo(skb)->nr_frags = 0;
			kfree_skb(skb);
			continue;
		}

		data_len = skb->len;
		memcpy(skb->data,
		       (void *)(idx_to_kaddr(netbk, pending_idx)|txp->offset),
		       data_len);
		if (data_len < txp->size) {
			/* Append the packet payload as a fragment. */
			txp->offset += data_len;
			txp->size -= data_len;
		} else {
			/* Schedule a response immediately. */
			netif_idx_release(netbk, pending_idx);
		}

		/*
		 * Old frontends do not assert data_validated but we
		 * can infer it from csum_blank so test both flags.
		 */
		if (txp->flags & (NETTXF_data_validated|NETTXF_csum_blank)) {
			skb->ip_summed = CHECKSUM_UNNECESSARY;
			skb->proto_data_valid = 1;
		} else {
			skb->ip_summed = CHECKSUM_NONE;
			skb->proto_data_valid = 0;
		}
		skb->proto_csum_blank = !!(txp->flags & NETTXF_csum_blank);

		netbk_fill_frags(netbk, skb);

		skb->dev      = netif->dev;
		skb->protocol = eth_type_trans(skb, skb->dev);

		netif->stats.rx_bytes += skb->len;
		netif->stats.rx_packets++;

		if (unlikely(netbk_copy_skb_mode == NETBK_ALWAYS_COPY_SKB) &&
		    unlikely(skb_linearize(skb))) {
			DPRINTK("Can't linearize skb in net_tx_action.\n");
			kfree_skb(skb);
			continue;
		}

		if (use_kthreads)
			netif_rx_ni(skb);
		else
			netif_rx(skb);
		netif->dev->last_rx = jiffies;
	}

 out:
	if (netbk_copy_skb_mode == NETBK_DELAYED_COPY_SKB &&
	    !list_empty(&netbk->pending_inuse_head)) {
		struct netbk_tx_pending_inuse *oldest;

		oldest = list_entry(netbk->pending_inuse_head.next,
				    struct netbk_tx_pending_inuse, list);
		mod_timer(&netbk->tx_pending_timer, oldest->alloc_time + HZ);
	}
}

static void netif_idx_release(struct xen_netbk *netbk, u16 pending_idx)
{
	unsigned long flags;

	spin_lock_irqsave(&netbk->release_lock, flags);
	netbk->dealloc_ring[MASK_PEND_IDX(netbk->dealloc_prod)] = pending_idx;
	/* Sync with net_tx_action_dealloc: insert idx /then/ incr producer. */
	smp_wmb();
	netbk->dealloc_prod++;
	spin_unlock_irqrestore(&netbk->release_lock, flags);

	if (use_kthreads)
		wake_up(&netbk->netbk_action_wq);
	else
		tasklet_schedule(&netbk->net_tx_tasklet);
}

static void netif_page_release(struct page *page, unsigned int order)
{
	struct page_ext *ext = (void *)page->mapping;
	unsigned int idx = ext->idx;
	unsigned int group = ext->group;
	struct xen_netbk *netbk = &xen_netbk[group];

	BUG_ON(order);
	BUG_ON(group >= netbk_nr_groups || idx >= MAX_PENDING_REQS);
	BUG_ON(netbk->mmap_pages[idx] != page);
	netif_idx_release(netbk, idx);
}

irqreturn_t netif_be_int(int irq, void *dev_id)
{
	netif_t *netif = dev_id;
	unsigned int group = GET_GROUP_INDEX(netif);

	if (unlikely(group >= netbk_nr_groups)) {
		/*
		 * Short of having a way to bind the IRQ in disabled mode
		 * (IRQ_NOAUTOEN), we have to ignore the first invocation(s)
		 * (before we got assigned to a group).
		 */
		BUG_ON(group != UINT_MAX);
		return IRQ_HANDLED;
	}

	add_to_net_schedule_list_tail(netif);
	maybe_schedule_tx_action(group);

	if (netif_schedulable(netif) && !netbk_queue_full(netif))
		netif_wake_queue(netif->dev);

	return IRQ_HANDLED;
}

static void make_tx_response(netif_t *netif, 
			     netif_tx_request_t *txp,
			     s8       st)
{
	RING_IDX i = netif->tx.rsp_prod_pvt;
	netif_tx_response_t *resp;
	int notify;

	resp = RING_GET_RESPONSE(&netif->tx, i);
	resp->id     = txp->id;
	resp->status = st;

	if (txp->flags & NETTXF_extra_info)
		RING_GET_RESPONSE(&netif->tx, ++i)->status = NETIF_RSP_NULL;

	netif->tx.rsp_prod_pvt = ++i;
	RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&netif->tx, notify);
	if (notify)
		notify_remote_via_irq(netif->irq);

#ifdef CONFIG_XEN_NETDEV_PIPELINED_TRANSMITTER
	if (i == netif->tx.req_cons) {
		int more_to_do;
		RING_FINAL_CHECK_FOR_REQUESTS(&netif->tx, more_to_do);
		if (more_to_do)
			add_to_net_schedule_list_tail(netif);
	}
#endif
}

static netif_rx_response_t *make_rx_response(netif_t *netif, 
					     u16      id, 
					     s8       st,
					     u16      offset,
					     u16      size,
					     u16      flags)
{
	RING_IDX i = netif->rx.rsp_prod_pvt;
	netif_rx_response_t *resp;

	resp = RING_GET_RESPONSE(&netif->rx, i);
	resp->offset     = offset;
	resp->flags      = flags;
	resp->id         = id;
	resp->status     = (s16)size;
	if (st < 0)
		resp->status = (s16)st;

	netif->rx.rsp_prod_pvt = ++i;

	return resp;
}

#ifdef NETBE_DEBUG_INTERRUPT
static irqreturn_t netif_be_dbg(int irq, void *dev_id)
{
	struct list_head *ent;
	netif_t *netif;
	unsigned int i = 0, group;

	printk(KERN_ALERT "netif_schedule_list:\n");

	for (group = 0; group < netbk_nr_groups; ++group) {
		struct xen_netbk *netbk = &xen_netbk[group];

		spin_lock_irq(&netbk->net_schedule_list_lock);

		list_for_each(ent, &netbk->net_schedule_list) {
			netif = list_entry(ent, netif_t, list);
			printk(KERN_ALERT " %d: private(rx_req_cons=%08x "
			       "rx_resp_prod=%08x\n",
			       i, netif->rx.req_cons, netif->rx.rsp_prod_pvt);
			printk(KERN_ALERT "   tx_req_cons=%08x tx_resp_prod=%08x)\n",
			       netif->tx.req_cons, netif->tx.rsp_prod_pvt);
			printk(KERN_ALERT "   shared(rx_req_prod=%08x "
			       "rx_resp_prod=%08x\n",
			       netif->rx.sring->req_prod, netif->rx.sring->rsp_prod);
			printk(KERN_ALERT "   rx_event=%08x tx_req_prod=%08x\n",
			       netif->rx.sring->rsp_event, netif->tx.sring->req_prod);
			printk(KERN_ALERT "   tx_resp_prod=%08x, tx_event=%08x)\n",
			       netif->tx.sring->rsp_prod, netif->tx.sring->rsp_event);
			i++;
		}

		spin_unlock_irq(&netbk->netbk->net_schedule_list_lock);
	}

	printk(KERN_ALERT " ** End of netif_schedule_list **\n");

	return IRQ_HANDLED;
}

static struct irqaction netif_be_dbg_action = {
	.handler = netif_be_dbg,
	.flags   = IRQF_SHARED,
	.name    = "net-be-dbg"
};
#endif

static inline int rx_work_todo(struct xen_netbk *netbk)
{
	return !skb_queue_empty(&netbk->rx_queue);
}

static inline int tx_work_todo(struct xen_netbk *netbk)
{
	if (netbk->dealloc_cons != netbk->dealloc_prod)
		return 1;

	if (nr_pending_reqs(netbk) + MAX_SKB_FRAGS < MAX_PENDING_REQS &&
	    !list_empty(&netbk->net_schedule_list))
		return 1;

	return 0;
}

static int netbk_action_thread(void *index)
{
	unsigned long group = (unsigned long)index;
	struct xen_netbk *netbk = &xen_netbk[group];

	while (1) {
		wait_event_interruptible(netbk->netbk_action_wq,
					 rx_work_todo(netbk) ||
					 tx_work_todo(netbk));
		cond_resched();

		if (rx_work_todo(netbk))
			net_rx_action(group);

		if (tx_work_todo(netbk))
			net_tx_action(group);
	}

	return 0;
}


static int __init netback_init(void)
{
	unsigned int i, group;
	int rc;
	struct page *page;

	if (!is_running_on_xen())
		return -ENODEV;

	if (!netbk_nr_groups)
		netbk_nr_groups = (num_online_cpus() + 1) / 2;

	/* We can increase reservation by this much in net_rx_action(). */
	balloon_update_driver_allowance(netbk_nr_groups * NET_RX_RING_SIZE);

	xen_netbk = __vmalloc(netbk_nr_groups * sizeof(*xen_netbk),
			    GFP_KERNEL|__GFP_HIGHMEM|__GFP_ZERO, PAGE_KERNEL);
	if (!xen_netbk) {
		printk(KERN_ALERT "%s: out of memory\n", __func__);
		return -ENOMEM;
	}

	for (group = 0; group < netbk_nr_groups; group++) {
		struct xen_netbk *netbk = &xen_netbk[group];

		if (use_kthreads) {
			init_waitqueue_head(&netbk->netbk_action_wq);
			netbk->task = kthread_create(netbk_action_thread,
						     (void *)(long)group,
						     "netback/%u", group);

			if (!IS_ERR(netbk->task)) {
				if (bind_threads)
					kthread_bind(netbk->task, group);
				wake_up_process(netbk->task);
			} else {
				printk(KERN_ALERT
				       "kthread_create() fails at netback\n");
				rc = PTR_ERR(netbk->task);
				goto failed_init;
			}
		} else {
			tasklet_init(&netbk->net_tx_tasklet, net_tx_action, group);
			tasklet_init(&netbk->net_rx_tasklet, net_rx_action, group);
		}

		skb_queue_head_init(&netbk->rx_queue);
		skb_queue_head_init(&netbk->tx_queue);

		netbk->mmap_pages =
			alloc_empty_pages_and_pagevec(MAX_PENDING_REQS);
		if (netbk->mmap_pages == NULL) {
			printk(KERN_ALERT "%s: out of memory\n", __func__);
			rc = -ENOMEM;
			goto failed_init;
		}

		init_timer(&netbk->net_timer);
		netbk->net_timer.data = group;
		netbk->net_timer.function = net_alarm;

		init_timer(&netbk->tx_pending_timer);
		netbk->tx_pending_timer.data = group;
		netbk->tx_pending_timer.function =
			netbk_tx_pending_timeout;

		netbk->pending_prod = MAX_PENDING_REQS;

		INIT_LIST_HEAD(&netbk->pending_inuse_head);
		INIT_LIST_HEAD(&netbk->net_schedule_list);
		INIT_LIST_HEAD(&netbk->group_domain_list);

		spin_lock_init(&netbk->net_schedule_list_lock);
		spin_lock_init(&netbk->release_lock);
		spin_lock_init(&netbk->group_domain_list_lock);

		for (i = 0; i < MAX_PENDING_REQS; i++) {
			page = netbk->mmap_pages[i];
			SetPageForeign(page, netif_page_release);
			netbk->page_extinfo[i].group = group;
			netbk->page_extinfo[i].idx = i;
			netif_set_page_ext(page,
					   &netbk->page_extinfo[i]);
			netbk->pending_ring[i] = i;
			INIT_LIST_HEAD(&netbk->pending_inuse[i].list);
		}
	}

	netbk_copy_skb_mode = NETBK_DONT_COPY_SKB;
	if (MODPARM_copy_skb) {
		if (HYPERVISOR_grant_table_op(GNTTABOP_unmap_and_replace,
					      NULL, 0))
			netbk_copy_skb_mode = NETBK_ALWAYS_COPY_SKB;
		else
			netbk_copy_skb_mode = NETBK_DELAYED_COPY_SKB;
	}

	netif_accel_init();

	netif_xenbus_init();

#ifdef NETBE_DEBUG_INTERRUPT
	(void)bind_virq_to_irqaction(VIRQ_DEBUG,
				     0,
				     &netif_be_dbg_action);
#endif

	return 0;

failed_init:
	while (group-- > 0) {
		struct xen_netbk *netbk = &xen_netbk[group];

		if (use_kthreads && netbk->task && !IS_ERR(netbk->task))
			kthread_stop(netbk->task);
		if (netbk->mmap_pages)
			free_empty_pages_and_pagevec(netbk->mmap_pages,
						     MAX_PENDING_REQS);
		del_timer(&netbk->tx_pending_timer);
		del_timer(&netbk->net_timer);
	}
	vfree(xen_netbk);

	return rc;
}

module_init(netback_init);

MODULE_LICENSE("Dual BSD/GPL");
