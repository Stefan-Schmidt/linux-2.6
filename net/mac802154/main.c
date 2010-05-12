/*
 * Copyright (C) 2007, 2008, 2009 Siemens AG
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/nl802154.h>
#include <net/route.h>

#include <net/af_ieee802154.h>
#include <net/mac802154.h>
#include <net/wpan-phy.h>

#include "mac802154.h"

int ieee802154_slave_open(struct net_device *dev)
{
	struct ieee802154_sub_if_data *priv = netdev_priv(dev);
	int res = 0;

	if (priv->hw->open_count++ == 0) {
		res = priv->hw->ops->start(&priv->hw->hw);
		WARN_ON(res);
		if (res)
			goto err;
	}

	netif_start_queue(dev);
	return 0;
err:
	priv->hw->open_count--;

	return res;
}

int ieee802154_slave_close(struct net_device *dev)
{
	struct ieee802154_sub_if_data *priv = netdev_priv(dev);

	dev->priv_flags &= ~IFF_IEEE802154_COORD;

	netif_stop_queue(dev);

	if ((--priv->hw->open_count) == 0)
		priv->hw->ops->stop(&priv->hw->hw);

	return 0;
}


static int ieee802154_netdev_register(struct wpan_phy *phy,
					struct net_device *dev)
{
	struct ieee802154_sub_if_data *priv;
	struct ieee802154_priv *ipriv;
	int err;

	ipriv = wpan_phy_priv(phy);

	priv = netdev_priv(dev);
	priv->dev = dev;
	priv->hw = ipriv;

	dev->needed_headroom = ipriv->hw.extra_tx_headroom;

	SET_NETDEV_DEV(dev, &ipriv->phy->dev);

	err = register_netdev(dev);
	if (err < 0)
		return err;

	rtnl_lock();
	mutex_lock(&ipriv->slaves_mtx);
	list_add_tail_rcu(&priv->list, &ipriv->slaves);
	mutex_unlock(&ipriv->slaves_mtx);
	rtnl_unlock();

	return 0;
}

static void ieee802154_del_iface(struct wpan_phy *phy,
		struct net_device *dev)
{
	struct ieee802154_sub_if_data *sdata;
	ASSERT_RTNL();

	BUG_ON(dev->type != ARPHRD_IEEE802154);

	sdata = netdev_priv(dev);

	BUG_ON(sdata->hw->phy != phy);

	mutex_lock(&sdata->hw->slaves_mtx);
	list_del_rcu(&sdata->list);
	mutex_unlock(&sdata->hw->slaves_mtx);

	synchronize_rcu();
	unregister_netdevice(sdata->dev);
}

static struct net_device *ieee802154_add_iface(struct wpan_phy *phy,
		const char *name, int type)
{
	struct net_device *dev;
	int err = -ENOMEM;

	switch (type) {
	case IEEE802154_DEV_WPAN:
		dev = alloc_netdev(sizeof(struct ieee802154_sub_if_data),
				name, ieee802154_wpan_setup);
		break;
	case IEEE802154_DEV_MONITOR:
		dev = alloc_netdev(sizeof(struct ieee802154_sub_if_data),
				name, ieee802154_monitor_setup);
		break;
	default:
		dev = NULL;
		err = -EINVAL;
		break;
	}
	if (!dev)
		goto err;


	err = ieee802154_netdev_register(phy, dev);

	if (err)
		goto err_free;

	dev_hold(dev); /* we return a device w/ incremented refcount */
	return dev;

err_free:
	free_netdev(dev);
err:
	return ERR_PTR(err);
}


struct ieee802154_dev *ieee802154_alloc_device(size_t priv_size,
		struct ieee802154_ops *ops)
{
	struct wpan_phy *phy;
	struct ieee802154_priv *priv;

	phy = wpan_phy_alloc(ALIGN(sizeof(*priv), NETDEV_ALIGN) + priv_size);
	if (!phy) {
		printk(KERN_ERR
			"Failure to initialize master IEEE802154 device\n");
		return NULL;
	}

	priv = wpan_phy_priv(phy);
	priv->hw.phy = priv->phy = phy;

	priv->hw.priv = (char *)priv + ALIGN(sizeof(*priv), NETDEV_ALIGN);

	BUG_ON(!ops);
	BUG_ON(!ops->xmit);
	BUG_ON(!ops->ed);
	BUG_ON(!ops->start);
	BUG_ON(!ops->stop);

	priv->ops = ops;

	INIT_LIST_HEAD(&priv->slaves);
	mutex_init(&priv->slaves_mtx);

	return &priv->hw;
}
EXPORT_SYMBOL(ieee802154_alloc_device);

void ieee802154_free_device(struct ieee802154_dev *hw)
{
	struct ieee802154_priv *priv = ieee802154_to_priv(hw);

	BUG_ON(!list_empty(&priv->slaves));

	wpan_phy_free(priv->phy);
}
EXPORT_SYMBOL(ieee802154_free_device);

int ieee802154_register_device(struct ieee802154_dev *dev)
{
	struct ieee802154_priv *priv = ieee802154_to_priv(dev);
	int rc;

	priv->dev_workqueue =
		create_singlethread_workqueue(wpan_phy_name(priv->phy));
	if (!priv->dev_workqueue) {
		rc = -ENOMEM;
		goto out;
	}

	wpan_phy_set_dev(priv->phy, priv->hw.parent);

	priv->phy->add_iface = ieee802154_add_iface;
	priv->phy->del_iface = ieee802154_del_iface;

	rc = wpan_phy_register(priv->phy);
	if (rc < 0)
		goto out_wq;

	return 0;

out_wq:
	destroy_workqueue(priv->dev_workqueue);
out:
	return rc;
}
EXPORT_SYMBOL(ieee802154_register_device);

void ieee802154_unregister_device(struct ieee802154_dev *dev)
{
	struct ieee802154_priv *priv = ieee802154_to_priv(dev);
	struct ieee802154_sub_if_data *sdata, *next;


	flush_workqueue(priv->dev_workqueue);
	destroy_workqueue(priv->dev_workqueue);

	rtnl_lock();

	list_for_each_entry_safe(sdata, next, &priv->slaves, list) {
		mutex_lock(&sdata->hw->slaves_mtx);
		list_del(&sdata->list);
		mutex_unlock(&sdata->hw->slaves_mtx);

		unregister_netdevice(sdata->dev);
	}

	rtnl_unlock();

	wpan_phy_unregister(priv->phy);
}
EXPORT_SYMBOL(ieee802154_unregister_device);

MODULE_DESCRIPTION("IEEE 802.15.4 implementation");
MODULE_LICENSE("GPL v2");

