/******************************************************************************
 * arch/xen/drivers/netif/backend/interface.c
 * 
 * Network-device interface management.
 * 
 * Copyright (c) 2004-2005, Keir Fraser
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
#include <linux/ethtool.h>
#include <linux/rtnetlink.h>
#include <linux/delay.h>

/*
 * Module parameter 'queue_length':
 * 
 * Enables queuing in the network stack when a client has run out of receive
 * descriptors. Although this feature can improve receive bandwidth by avoiding
 * packet loss, it can also result in packets sitting in the 'tx_queue' for
 * unbounded time. This is bad if those packets hold onto foreign resources.
 * For example, consider a packet that holds onto resources belonging to the
 * guest for which it is queued (e.g., packet received on vif1.0, destined for
 * vif1.1 which is not activated in the guest): in this situation the guest
 * will never be destroyed, unless vif1.1 is taken down. To avoid this, we
 * run a timer (tx_queue_timeout) to drain the queue when the interface is
 * blocked.
 */
static unsigned long netbk_queue_length = 32;
module_param_named(queue_length, netbk_queue_length, ulong, 0644);

static void __netif_up(netif_t *netif)
{
	unsigned int group = 0;
	unsigned int min_domains = xen_netbk[0].group_domain_nr;
	unsigned int i;

	/* Find the list which contains least number of domains. */
	for (i = 1; i < netbk_nr_groups; i++) {
		if (xen_netbk[i].group_domain_nr < min_domains) {
			group = i;
			min_domains = xen_netbk[i].group_domain_nr;
		}
	}

	spin_lock(&xen_netbk[group].group_domain_list_lock);
	list_add_tail(&netif->group_list,
		      &xen_netbk[group].group_domain_list);
	xen_netbk[group].group_domain_nr++;
	spin_unlock(&xen_netbk[group].group_domain_list_lock);
	netif->group = group;

	enable_irq(netif->irq);
	netif_schedule_work(netif);
}

static void __netif_down(netif_t *netif)
{
	struct xen_netbk *netbk = xen_netbk + netif->group;

	disable_irq(netif->irq);
	netif_deschedule_work(netif);

	netif->group = UINT_MAX;
	spin_lock(&netbk->group_domain_list_lock);
	netbk->group_domain_nr--;
	list_del(&netif->group_list);
	spin_unlock(&netbk->group_domain_list_lock);
}

static int net_open(struct net_device *dev)
{
	netif_t *netif = netdev_priv(dev);
	if (netback_carrier_ok(netif)) {
		__netif_up(netif);
		netif_start_queue(dev);
	}
	return 0;
}

static int net_close(struct net_device *dev)
{
	netif_t *netif = netdev_priv(dev);
	if (netback_carrier_ok(netif))
		__netif_down(netif);
	netif_stop_queue(dev);
	return 0;
}

static int netbk_change_mtu(struct net_device *dev, int mtu)
{
	int max = netbk_can_sg(dev) ? 65535 - ETH_HLEN : ETH_DATA_LEN;

	if (mtu > max)
		return -EINVAL;
	dev->mtu = mtu;
	return 0;
}

static int netbk_set_sg(struct net_device *dev, u32 data)
{
	if (data) {
		netif_t *netif = netdev_priv(dev);

		if (!(netif->features & NETIF_F_SG))
			return -ENOSYS;
	}

	return ethtool_op_set_sg(dev, data);
}

static int netbk_set_tso(struct net_device *dev, u32 data)
{
	if (data) {
		netif_t *netif = netdev_priv(dev);

		if (!(netif->features & NETIF_F_TSO))
			return -ENOSYS;
	}

	return ethtool_op_set_tso(dev, data);
}

static void netbk_get_drvinfo(struct net_device *dev,
			      struct ethtool_drvinfo *info)
{
	strcpy(info->driver, "netbk");
	strcpy(info->bus_info, dev_name(dev->dev.parent));
}

static const struct netif_stat {
	char name[ETH_GSTRING_LEN];
	u16 offset;
} netbk_stats[] = {
	{ "copied_skbs", offsetof(netif_t, nr_copied_skbs) },
};

static int netbk_get_stats_count(struct net_device *dev)
{
	return ARRAY_SIZE(netbk_stats);
}

static void netbk_get_ethtool_stats(struct net_device *dev,
				   struct ethtool_stats *stats, u64 * data)
{
	void *netif = netdev_priv(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(netbk_stats); i++)
		data[i] = *(int *)(netif + netbk_stats[i].offset);
}

static void netbk_get_strings(struct net_device *dev, u32 stringset, u8 * data)
{
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ARRAY_SIZE(netbk_stats); i++)
			memcpy(data + i * ETH_GSTRING_LEN,
			       netbk_stats[i].name, ETH_GSTRING_LEN);
		break;
	}
}

static const struct ethtool_ops network_ethtool_ops =
{
	.get_drvinfo = netbk_get_drvinfo,

	.get_tx_csum = ethtool_op_get_tx_csum,
	.set_tx_csum = ethtool_op_set_tx_csum,
	.get_sg = ethtool_op_get_sg,
	.set_sg = netbk_set_sg,
	.get_tso = ethtool_op_get_tso,
	.set_tso = netbk_set_tso,
	.get_link = ethtool_op_get_link,

	.get_stats_count = netbk_get_stats_count,
	.get_ethtool_stats = netbk_get_ethtool_stats,
	.get_strings = netbk_get_strings,
};

static const struct net_device_ops netif_be_netdev_ops = {
	.ndo_open               = net_open,
	.ndo_stop               = net_close,
	.ndo_start_xmit         = netif_be_start_xmit,
	.ndo_change_mtu	        = netbk_change_mtu,
	.ndo_get_stats          = netif_be_get_stats,
};

netif_t *netif_alloc(struct device *parent, domid_t domid, unsigned int handle)
{
	int err = 0;
	struct net_device *dev;
	netif_t *netif;
	char name[IFNAMSIZ] = {};

	snprintf(name, IFNAMSIZ - 1, "vif%u.%u", domid, handle);
	dev = alloc_netdev(sizeof(netif_t), name, ether_setup);
	if (dev == NULL) {
		DPRINTK("Could not create netif: out of memory\n");
		return ERR_PTR(-ENOMEM);
	}

	SET_NETDEV_DEV(dev, parent);

	netif = netdev_priv(dev);
	memset(netif, 0, sizeof(*netif));
	netif->domid  = domid;
	netif->group = UINT_MAX;
	netif->handle = handle;
	atomic_set(&netif->refcnt, 1);
	init_waitqueue_head(&netif->waiting_to_free);
	netif->dev = dev;

	netback_carrier_off(netif);

	netif->credit_bytes = netif->remaining_credit = ~0UL;
	netif->credit_usec  = 0UL;
	init_timer(&netif->credit_timeout);
	/* Initialize 'expires' now: it's used to track the credit window. */
	netif->credit_timeout.expires = jiffies;

	init_timer(&netif->tx_queue_timeout);

	dev->netdev_ops	     = &netif_be_netdev_ops;
	dev->features        = NETIF_F_IP_CSUM;

	SET_ETHTOOL_OPS(dev, &network_ethtool_ops);

	dev->tx_queue_len = netbk_queue_length;

	/*
	 * Initialise a dummy MAC address. We choose the numerically
	 * largest non-broadcast address to prevent the address getting
	 * stolen by an Ethernet bridge for STP purposes.
	 * (FE:FF:FF:FF:FF:FF)
	 */ 
	memset(dev->dev_addr, 0xFF, ETH_ALEN);
	dev->dev_addr[0] &= ~0x01;

	rtnl_lock();
	err = register_netdevice(dev);
	rtnl_unlock();
	if (err) {
		DPRINTK("Could not register new net device %s: err=%d\n",
			dev->name, err);
		free_netdev(dev);
		return ERR_PTR(err);
	}

	DPRINTK("Successfully created netif\n");
	return netif;
}

static int map_frontend_pages(
	netif_t *netif, grant_ref_t tx_ring_ref, grant_ref_t rx_ring_ref)
{
	struct gnttab_map_grant_ref op;

	gnttab_set_map_op(&op, (unsigned long)netif->tx_comms_area->addr,
			  GNTMAP_host_map, tx_ring_ref, netif->domid);
    do {
		if (HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1))
			BUG();
        msleep(10);
    } while(op.status == GNTST_eagain);

	if (op.status) { 
		DPRINTK(" Gnttab failure mapping tx_ring_ref!\n");
		return op.status;
	}

	netif->tx_shmem_ref    = tx_ring_ref;
	netif->tx_shmem_handle = op.handle;

	gnttab_set_map_op(&op, (unsigned long)netif->rx_comms_area->addr,
			  GNTMAP_host_map, rx_ring_ref, netif->domid);
    do {
	    if (HYPERVISOR_grant_table_op(GNTTABOP_map_grant_ref, &op, 1))
		    BUG();
        msleep(10);
    } while(op.status == GNTST_eagain);

	if (op.status) {
		struct gnttab_unmap_grant_ref unop;

		gnttab_set_unmap_op(&unop,
				    (unsigned long)netif->tx_comms_area->addr,
				    GNTMAP_host_map, netif->tx_shmem_handle);
		VOID(HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref,
					       &unop, 1));
		DPRINTK(" Gnttab failure mapping rx_ring_ref!\n");
		return op.status;
	}

	netif->rx_shmem_ref    = rx_ring_ref;
	netif->rx_shmem_handle = op.handle;

	return 0;
}

static void unmap_frontend_pages(netif_t *netif)
{
	struct gnttab_unmap_grant_ref op;

	gnttab_set_unmap_op(&op, (unsigned long)netif->tx_comms_area->addr,
			    GNTMAP_host_map, netif->tx_shmem_handle);

	if (HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1))
		BUG();

	gnttab_set_unmap_op(&op, (unsigned long)netif->rx_comms_area->addr,
			    GNTMAP_host_map, netif->rx_shmem_handle);

	if (HYPERVISOR_grant_table_op(GNTTABOP_unmap_grant_ref, &op, 1))
		BUG();
}

int netif_map(netif_t *netif, unsigned long tx_ring_ref,
	      unsigned long rx_ring_ref, unsigned int evtchn)
{
	int err = -ENOMEM;
	netif_tx_sring_t *txs;
	netif_rx_sring_t *rxs;

	/* Already connected through? */
	if (netif->irq)
		return 0;

	netif->tx_comms_area = alloc_vm_area(PAGE_SIZE);
	if (netif->tx_comms_area == NULL)
		return -ENOMEM;
	netif->rx_comms_area = alloc_vm_area(PAGE_SIZE);
	if (netif->rx_comms_area == NULL)
		goto err_rx;

	err = map_frontend_pages(netif, tx_ring_ref, rx_ring_ref);
	if (err)
		goto err_map;

	err = bind_interdomain_evtchn_to_irqhandler(
		netif->domid, evtchn, netif_be_int, 0,
		netif->dev->name, netif);
	if (err < 0)
		goto err_hypervisor;
	BUG_ON(err < DYNIRQ_BASE || err >= DYNIRQ_BASE + NR_DYNIRQS);
	netif->irq = err;
	disable_irq(netif->irq);

	txs = (netif_tx_sring_t *)netif->tx_comms_area->addr;
	BACK_RING_INIT(&netif->tx, txs, PAGE_SIZE);

	rxs = (netif_rx_sring_t *)
		((char *)netif->rx_comms_area->addr);
	BACK_RING_INIT(&netif->rx, rxs, PAGE_SIZE);

	netif->rx_req_cons_peek = 0;

	netif_get(netif);

	rtnl_lock();
	netback_carrier_on(netif);
	if (netif_running(netif->dev))
		__netif_up(netif);
	rtnl_unlock();

	return 0;
err_hypervisor:
	unmap_frontend_pages(netif);
err_map:
	free_vm_area(netif->rx_comms_area);
err_rx:
	free_vm_area(netif->tx_comms_area);
	return err;
}

void netif_disconnect(netif_t *netif)
{
	if (netback_carrier_ok(netif)) {
		rtnl_lock();
		netback_carrier_off(netif);
		netif_carrier_off(netif->dev); /* discard queued packets */
		if (netif_running(netif->dev))
			__netif_down(netif);
		rtnl_unlock();
		netif_put(netif);
	}

	atomic_dec(&netif->refcnt);
	wait_event(netif->waiting_to_free, atomic_read(&netif->refcnt) == 0);

	del_timer_sync(&netif->credit_timeout);
	del_timer_sync(&netif->tx_queue_timeout);

	if (netif->irq)
		unbind_from_irqhandler(netif->irq, netif);
	
	unregister_netdev(netif->dev);

	if (netif->tx.sring) {
		unmap_frontend_pages(netif);
		free_vm_area(netif->tx_comms_area);
		free_vm_area(netif->rx_comms_area);
	}

	free_netdev(netif->dev);
}
