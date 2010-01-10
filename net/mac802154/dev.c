/*
 * Copyright 2007, 2008, 2009 Siemens AG
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
 * Written by:
 * Dmitry Eremin-Solenikov <dbaryshkov@gmail.com>
 * Sergey Lapin <slapin@ossfans.org>
 * Maxim Gorbachyov <maxim.gorbachev@siemens.com>
 */

#include <linux/net.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/if_arp.h>
#include <linux/rculist.h>
#include <linux/random.h>
#include <linux/crc-ccitt.h>

#include <net/rtnetlink.h>
#include <net/af_ieee802154.h>
#include <net/mac802154.h>
#include <net/ieee802154_netdev.h>
#include <net/ieee802154.h>
#include <net/wpan-phy.h>

#include "mac802154.h"
#include "beacon.h"
#include "beacon_hash.h"
#include "mib.h"

struct xmit_work {
	struct sk_buff *skb;
	struct work_struct work;
	struct ieee802154_priv *priv;
	u8 page;
	u8 chan;
};

static void ieee802154_xmit_worker(struct work_struct *work)
{
	struct xmit_work *xw = container_of(work, struct xmit_work, work);
	int res;

	BUG_ON(xw->chan == (u8)-1);

	mutex_lock(&xw->priv->phy->pib_lock);
	if (xw->priv->phy->current_channel != xw->chan) {
		res = xw->priv->ops->set_channel(&xw->priv->hw,
				xw->chan);
		if (res) {
			pr_debug("set_channel failed\n");
			goto out;
		}
	}

	res = xw->priv->ops->xmit(&xw->priv->hw, xw->skb);

out:
	mutex_unlock(&xw->priv->phy->pib_lock);

	/* FIXME: result processing and/or requeue!!! */
	dev_kfree_skb(xw->skb);

	kfree(xw);
}


static netdev_tx_t ieee802154_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ieee802154_sub_if_data *priv;
	struct xmit_work *work;

	priv = netdev_priv(dev);

	if (priv->chan == (u8)-1) /* not init */
		return NETDEV_TX_OK;

	BUG_ON(priv->page >= 32);
	BUG_ON(priv->chan >= 27);

	if (WARN_ON(!(priv->hw->phy->channels_supported[priv->page] &
					(1 << priv->chan))))
		return NETDEV_TX_OK;

	if (!(priv->hw->hw.flags & IEEE802154_HW_OMIT_CKSUM)) {
		u16 crc = crc_ccitt(0, skb->data, skb->len);
		u8 *data = skb_put(skb, 2);
		data[0] = crc & 0xff;
		data[1] = crc >> 8;
	}

	skb->iif = dev->ifindex;
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;


	if (skb_cow_head(skb, priv->hw->hw.extra_tx_headroom)) {
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}

	work = kzalloc(sizeof(struct xmit_work), GFP_ATOMIC);
	if (!work)
		return NETDEV_TX_BUSY;

	INIT_WORK(&work->work, ieee802154_xmit_worker);
	work->skb = skb;
	work->priv = priv->hw;

	spin_lock_bh(&priv->mib_lock);
	work->chan = priv->chan;
	work->page = priv->page;
	spin_unlock_bh(&priv->mib_lock);


	queue_work(priv->hw->dev_workqueue, &work->work);

	return NETDEV_TX_OK;
}

static int ieee802154_slave_open(struct net_device *dev)
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

static int ieee802154_slave_close(struct net_device *dev)
{
	struct ieee802154_sub_if_data *priv = netdev_priv(dev);

	dev->priv_flags &= ~IFF_IEEE802154_COORD;

	netif_stop_queue(dev);

	if ((--priv->hw->open_count) == 0)
		priv->hw->ops->stop(&priv->hw->hw);

	return 0;
}


static int ieee802154_slave_ioctl(struct net_device *dev, struct ifreq *ifr,
		int cmd)
{
	struct ieee802154_sub_if_data *priv = netdev_priv(dev);
	struct sockaddr_ieee802154 *sa =
		(struct sockaddr_ieee802154 *)&ifr->ifr_addr;
	int err = -ENOIOCTLCMD;

	spin_lock_bh(&priv->mib_lock);

	switch (cmd) {
	case SIOCGIFADDR:
		if (priv->pan_id == IEEE802154_PANID_BROADCAST ||
		    priv->short_addr == IEEE802154_ADDR_BROADCAST) {
			err = -EADDRNOTAVAIL;
			break;
		}

		sa->family = AF_IEEE802154;
		sa->addr.addr_type = IEEE802154_ADDR_SHORT;
		sa->addr.pan_id = priv->pan_id;
		sa->addr.short_addr = priv->short_addr;

		err = 0;
		break;
	case SIOCSIFADDR:
		dev_warn(&dev->dev,
			"Using DEBUGing ioctl SIOCSIFADDR isn't recommened!\n");
		if (sa->family != AF_IEEE802154 ||
		    sa->addr.addr_type != IEEE802154_ADDR_SHORT ||
		    sa->addr.pan_id == IEEE802154_PANID_BROADCAST ||
		    sa->addr.short_addr == IEEE802154_ADDR_BROADCAST ||
		    sa->addr.short_addr == IEEE802154_ADDR_UNDEF) {
			err = -EINVAL;
			break;
		}

		priv->pan_id = sa->addr.pan_id;
		priv->short_addr = sa->addr.short_addr;
		err = 0;
		break;
	}
	spin_unlock_bh(&priv->mib_lock);
	return err;
}

static int ieee802154_slave_mac_addr(struct net_device *dev, void *p)
{
	struct sockaddr *addr = p;

	if (netif_running(dev))
		return -EBUSY;
	/* FIXME: validate addr */
	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	return 0;
}

static void ieee802154_haddr_copy_swap(u8 *dest, const u8 *src)
{
	int i;
	for (i = 0; i < IEEE802154_ADDR_LEN; i++)
		dest[IEEE802154_ADDR_LEN - i - 1] = src[i];
}

static int ieee802154_header_create(struct sk_buff *skb,
			   struct net_device *dev,
			   unsigned short type, const void *_daddr,
			   const void *_saddr, unsigned len)
{
	u8 head[24] = {};
	int pos = 0;

	u16 fc;
	const struct ieee802154_addr *saddr = _saddr;
	const struct ieee802154_addr *daddr = _daddr;
	struct ieee802154_addr dev_addr;
	struct ieee802154_sub_if_data *priv = netdev_priv(dev);

	fc = mac_cb_type(skb);
	if (mac_cb_is_ackreq(skb))
		fc |= IEEE802154_FC_ACK_REQ;

	pos = 2;

	head[pos++] = mac_cb(skb)->seq; /* DSN/BSN */

	if (!daddr)
		return -EINVAL;

	if (!saddr) {
		spin_lock_bh(&priv->mib_lock);
		if (priv->short_addr == IEEE802154_ADDR_BROADCAST ||
		    priv->short_addr == IEEE802154_ADDR_UNDEF ||
		    priv->pan_id == IEEE802154_PANID_BROADCAST) {
			dev_addr.addr_type = IEEE802154_ADDR_LONG;
			memcpy(dev_addr.hwaddr, dev->dev_addr,
					IEEE802154_ADDR_LEN);
		} else {
			dev_addr.addr_type = IEEE802154_ADDR_SHORT;
			dev_addr.short_addr = priv->short_addr;
		}

		dev_addr.pan_id = priv->pan_id;
		saddr = &dev_addr;

		spin_unlock_bh(&priv->mib_lock);
	}

	if (daddr->addr_type != IEEE802154_ADDR_NONE) {
		fc |= (daddr->addr_type << IEEE802154_FC_DAMODE_SHIFT);

		head[pos++] = daddr->pan_id & 0xff;
		head[pos++] = daddr->pan_id >> 8;

		if (daddr->addr_type == IEEE802154_ADDR_SHORT) {
			head[pos++] = daddr->short_addr & 0xff;
			head[pos++] = daddr->short_addr >> 8;
		} else {
			ieee802154_haddr_copy_swap(head + pos, daddr->hwaddr);
			pos += IEEE802154_ADDR_LEN;
		}
	}

	if (saddr->addr_type != IEEE802154_ADDR_NONE) {
		fc |= (saddr->addr_type << IEEE802154_FC_SAMODE_SHIFT);

		if ((saddr->pan_id == daddr->pan_id) &&
		    (saddr->pan_id != IEEE802154_PANID_BROADCAST))
			/* PANID compression/ intra PAN */
			fc |= IEEE802154_FC_INTRA_PAN;
		else {
			head[pos++] = saddr->pan_id & 0xff;
			head[pos++] = saddr->pan_id >> 8;
		}

		if (saddr->addr_type == IEEE802154_ADDR_SHORT) {
			head[pos++] = saddr->short_addr & 0xff;
			head[pos++] = saddr->short_addr >> 8;
		} else {
			ieee802154_haddr_copy_swap(head + pos, saddr->hwaddr);
			pos += IEEE802154_ADDR_LEN;
		}
	}

	head[0] = fc;
	head[1] = fc >> 8;

	memcpy(skb_push(skb, pos), head, pos);

	return pos;
}

static int ieee802154_header_parse(const struct sk_buff *skb,
		unsigned char *haddr)
{
	const u8 *hdr = skb_mac_header(skb), *tail = skb_tail_pointer(skb);
	struct ieee802154_addr *addr = (struct ieee802154_addr *)haddr;
	u16 fc;
	int da_type;

	if (hdr + 3 > tail)
		goto malformed;

	fc = hdr[0] | (hdr[1] << 8);

	hdr += 3;

	da_type = IEEE802154_FC_DAMODE(fc);
	addr->addr_type = IEEE802154_FC_SAMODE(fc);

	switch (da_type) {
	case IEEE802154_ADDR_NONE:
		if (fc & IEEE802154_FC_INTRA_PAN)
			goto malformed;
		break;

	case IEEE802154_ADDR_LONG:
		if (hdr + 2 > tail)
			goto malformed;
		if (fc & IEEE802154_FC_INTRA_PAN) {
			addr->pan_id = hdr[0] | (hdr[1] << 8);
			hdr += 2;
		}

		if (hdr + IEEE802154_ADDR_LEN > tail)
			goto malformed;
		hdr += IEEE802154_ADDR_LEN;
		break;

	case IEEE802154_ADDR_SHORT:
		if (hdr + 2 > tail)
			goto malformed;
		if (fc & IEEE802154_FC_INTRA_PAN) {
			addr->pan_id = hdr[0] | (hdr[1] << 8);
			hdr += 2;
		}

		if (hdr + 2 > tail)
			goto malformed;
		hdr += 2;
		break;

	default:
		goto malformed;

	}

	switch (addr->addr_type) {
	case IEEE802154_ADDR_NONE:
		break;

	case IEEE802154_ADDR_LONG:
		if (hdr + 2 > tail)
			goto malformed;
		if (!(fc & IEEE802154_FC_INTRA_PAN)) {
			addr->pan_id = hdr[0] | (hdr[1] << 8);
			hdr += 2;
		}

		if (hdr + IEEE802154_ADDR_LEN > tail)
			goto malformed;
		ieee802154_haddr_copy_swap(addr->hwaddr, hdr);
		hdr += IEEE802154_ADDR_LEN;
		break;

	case IEEE802154_ADDR_SHORT:
		if (hdr + 2 > tail)
			goto malformed;
		if (!(fc & IEEE802154_FC_INTRA_PAN)) {
			addr->pan_id = hdr[0] | (hdr[1] << 8);
			hdr += 2;
		}

		if (hdr + 2 > tail)
			goto malformed;
		addr->short_addr = hdr[0] | (hdr[1] << 8);
		hdr += 2;
		break;

	default:
		goto malformed;

	}

	return sizeof(struct ieee802154_addr);

malformed:
	pr_debug("malformed packet\n");
	return 0;
}

static struct header_ops ieee802154_header_ops = {
	.create		= ieee802154_header_create,
	.parse		= ieee802154_header_parse,
};

static const struct net_device_ops ieee802154_slave_ops = {
	.ndo_open		= ieee802154_slave_open,
	.ndo_stop		= ieee802154_slave_close,
	.ndo_start_xmit		= ieee802154_net_xmit,
	.ndo_do_ioctl		= ieee802154_slave_ioctl,
	.ndo_set_mac_address	= ieee802154_slave_mac_addr,
};

static void ieee802154_netdev_setup(struct net_device *dev)
{
	dev->addr_len		= IEEE802154_ADDR_LEN;
	memset(dev->broadcast, 0xff, IEEE802154_ADDR_LEN);
	dev->features		= NETIF_F_NO_CSUM;
	dev->hard_header_len	= 2 + 1 + 20 + 14;
	dev->header_ops		= &ieee802154_header_ops;
	dev->needed_tailroom	= 2; /* FCS */
	dev->mtu		= 127;
	dev->tx_queue_len	= 10;
	dev->type		= ARPHRD_IEEE802154;
	dev->flags		= IFF_NOARP | IFF_BROADCAST;
	dev->watchdog_timeo	= 0;

	dev->destructor		= free_netdev;
	dev->netdev_ops		= &ieee802154_slave_ops;
	dev->ml_priv		= &mac802154_mlme;
}

/*
 * This is for hw unregistration only, as it doesn't do RCU locking
 */
void ieee802154_drop_slaves(struct ieee802154_dev *hw)
{
	struct ieee802154_priv *priv = ieee802154_to_priv(hw);
	struct ieee802154_sub_if_data *sdata, *next;

	ASSERT_RTNL();

	list_for_each_entry_safe(sdata, next, &priv->slaves, list) {
		mutex_lock(&sdata->hw->slaves_mtx);
		list_del(&sdata->list);
		mutex_unlock(&sdata->hw->slaves_mtx);

		unregister_netdevice(sdata->dev);
	}
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

	priv->chan = -1; /* not initialized */
	priv->page = 0; /* for compat */

	spin_lock_init(&priv->mib_lock);

	get_random_bytes(&priv->bsn, 1);
	get_random_bytes(&priv->dsn, 1);

	priv->pan_id = IEEE802154_PANID_BROADCAST;
	priv->short_addr = IEEE802154_ADDR_BROADCAST;

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

void ieee802154_del_iface(struct wpan_phy *phy,
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

struct net_device *ieee802154_add_iface(struct wpan_phy *phy,
		const char *name)
{
	struct net_device *dev;
	int err = -ENOMEM;

	dev = alloc_netdev(sizeof(struct ieee802154_sub_if_data),
			name, ieee802154_netdev_setup);
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

static int ieee802154_process_beacon(struct net_device *dev,
		struct sk_buff *skb)
{
	int flags;
	int ret;
	ret = parse_beacon_frame(skb, NULL, &flags, NULL);

	/* Here we have cb->sa = coordinator address, and PAN address */

	if (ret < 0) {
		ret = NET_RX_DROP;
		goto fail;
	}
	dev_dbg(&dev->dev, "got beacon from pan %04x\n",
			mac_cb(skb)->sa.pan_id);
	ieee802154_beacon_hash_add(&mac_cb(skb)->sa);
	ieee802154_beacon_hash_dump();
	ret = NET_RX_SUCCESS;
fail:
	kfree_skb(skb);
	return ret;
}

static int ieee802154_process_ack(struct net_device *dev, struct sk_buff *skb)
{
	pr_debug("got ACK for SEQ=%d\n", mac_cb(skb)->seq);

	kfree_skb(skb);
	return NET_RX_SUCCESS;
}

static int ieee802154_process_data(struct net_device *dev, struct sk_buff *skb)
{
	if (in_interrupt())
		return netif_rx(skb);
	else
		return netif_rx_ni(skb);
}

static int ieee802154_subif_frame(struct ieee802154_sub_if_data *sdata,
		struct sk_buff *skb)
{
	pr_debug("%s Getting packet via slave interface %s\n",
				__func__, sdata->dev->name);

	spin_lock_bh(&sdata->mib_lock);

	switch (mac_cb(skb)->da.addr_type) {
	case IEEE802154_ADDR_NONE:
		if (mac_cb(skb)->sa.addr_type != IEEE802154_ADDR_NONE)
			/* FIXME: check if we are PAN coordinator :) */
			skb->pkt_type = PACKET_OTHERHOST;
		else
			/* ACK comes with both addresses empty */
			skb->pkt_type = PACKET_HOST;
		break;
	case IEEE802154_ADDR_LONG:
		if (mac_cb(skb)->da.pan_id != sdata->pan_id &&
		    mac_cb(skb)->da.pan_id != IEEE802154_PANID_BROADCAST)
			skb->pkt_type = PACKET_OTHERHOST;
		else if (!memcmp(mac_cb(skb)->da.hwaddr, sdata->dev->dev_addr,
					IEEE802154_ADDR_LEN))
			skb->pkt_type = PACKET_HOST;
		else
			skb->pkt_type = PACKET_OTHERHOST;
		break;
	case IEEE802154_ADDR_SHORT:
		if (mac_cb(skb)->da.pan_id != sdata->pan_id &&
		    mac_cb(skb)->da.pan_id != IEEE802154_PANID_BROADCAST)
			skb->pkt_type = PACKET_OTHERHOST;
		else if (mac_cb(skb)->da.short_addr == sdata->short_addr)
			skb->pkt_type = PACKET_HOST;
		else if (mac_cb(skb)->da.short_addr ==
					IEEE802154_ADDR_BROADCAST)
			skb->pkt_type = PACKET_BROADCAST;
		else
			skb->pkt_type = PACKET_OTHERHOST;
		break;
	}

	spin_unlock_bh(&sdata->mib_lock);

	skb->dev = sdata->dev;

	if (skb->pkt_type == PACKET_HOST && mac_cb_is_ackreq(skb) &&
			!(sdata->hw->hw.flags & IEEE802154_HW_AACK))
		dev_warn(&sdata->dev->dev,
			"ACK requested, however AACK not supported.\n");

	switch (mac_cb_type(skb)) {
	case IEEE802154_FC_TYPE_BEACON:
		return ieee802154_process_beacon(sdata->dev, skb);
	case IEEE802154_FC_TYPE_ACK:
		return ieee802154_process_ack(sdata->dev, skb);
	case IEEE802154_FC_TYPE_MAC_CMD:
		return ieee802154_process_cmd(sdata->dev, skb);
	case IEEE802154_FC_TYPE_DATA:
		return ieee802154_process_data(sdata->dev, skb);
	default:
		pr_warning("ieee802154: Bad frame received (type = %d)\n",
				mac_cb_type(skb));
		kfree_skb(skb);
		return NET_RX_DROP;
	}
}

static u8 fetch_skb_u8(struct sk_buff *skb)
{
	u8 ret;

	BUG_ON(skb->len < 1);

	ret = skb->data[0];
	skb_pull(skb, 1);

	return ret;
}

static u16 fetch_skb_u16(struct sk_buff *skb)
{
	u16 ret;

	BUG_ON(skb->len < 2);

	ret = skb->data[0] + (skb->data[1] * 256);
	skb_pull(skb, 2);
	return ret;
}

static void fetch_skb_u64(struct sk_buff *skb, void *data)
{
	BUG_ON(skb->len < IEEE802154_ADDR_LEN);

	ieee802154_haddr_copy_swap(data, skb->data);
	skb_pull(skb, IEEE802154_ADDR_LEN);
}

#define IEEE802154_FETCH_U8(skb, var)		\
	do {					\
		if (skb->len < 1)		\
			goto exit_error;	\
		var = fetch_skb_u8(skb);	\
	} while (0)

#define IEEE802154_FETCH_U16(skb, var)		\
	do {					\
		if (skb->len < 2)		\
			goto exit_error;	\
		var = fetch_skb_u16(skb);	\
	} while (0)

#define IEEE802154_FETCH_U64(skb, var)			\
	do {						\
		if (skb->len < IEEE802154_ADDR_LEN)	\
			goto exit_error;		\
		fetch_skb_u64(skb, &var);		\
	} while (0)

static int parse_frame_start(struct sk_buff *skb)
{
	u8 *head = skb->data;
	u16 fc;

	if (skb->len < 3) {
		pr_debug("frame size %d bytes is too short\n", skb->len);
		return -EINVAL;
	}

	IEEE802154_FETCH_U16(skb, fc);
	IEEE802154_FETCH_U8(skb, mac_cb(skb)->seq);

	pr_debug("%s: %04x dsn%02x\n", __func__, fc, head[2]);

	mac_cb(skb)->flags = IEEE802154_FC_TYPE(fc);

	if (fc & IEEE802154_FC_ACK_REQ) {
		pr_debug("%s(): ACKNOWLEDGE required\n", __func__);
		mac_cb(skb)->flags |= MAC_CB_FLAG_ACKREQ;
	}

	if (fc & IEEE802154_FC_SECEN)
		mac_cb(skb)->flags |= MAC_CB_FLAG_SECEN;

	if (fc & IEEE802154_FC_INTRA_PAN)
		mac_cb(skb)->flags |= MAC_CB_FLAG_INTRAPAN;

	/* TODO */
	if (mac_cb_is_secen(skb)) {
		pr_info("security support is not implemented\n");
		return -EINVAL;
	}

	mac_cb(skb)->sa.addr_type = IEEE802154_FC_SAMODE(fc);
	if (mac_cb(skb)->sa.addr_type == IEEE802154_ADDR_NONE)
		pr_debug("%s(): src addr_type is NONE\n", __func__);

	mac_cb(skb)->da.addr_type = IEEE802154_FC_DAMODE(fc);
	if (mac_cb(skb)->da.addr_type == IEEE802154_ADDR_NONE)
		pr_debug("%s(): dst addr_type is NONE\n", __func__);

	if (IEEE802154_FC_TYPE(fc) == IEEE802154_FC_TYPE_ACK) {
		/* ACK can only have NONE-type addresses */
		if (mac_cb(skb)->sa.addr_type != IEEE802154_ADDR_NONE ||
		    mac_cb(skb)->da.addr_type != IEEE802154_ADDR_NONE)
			return -EINVAL;
	}

	if (mac_cb(skb)->da.addr_type != IEEE802154_ADDR_NONE) {
		IEEE802154_FETCH_U16(skb, mac_cb(skb)->da.pan_id);

		if (mac_cb_is_intrapan(skb)) { /* ! panid compress */
			pr_debug("%s(): src IEEE802154_FC_INTRA_PAN\n",
					__func__);
			mac_cb(skb)->sa.pan_id = mac_cb(skb)->da.pan_id;
			pr_debug("%s(): src PAN address %04x\n",
					__func__, mac_cb(skb)->sa.pan_id);
		}

		pr_debug("%s(): dst PAN address %04x\n",
				__func__, mac_cb(skb)->da.pan_id);

		if (mac_cb(skb)->da.addr_type == IEEE802154_ADDR_SHORT) {
			IEEE802154_FETCH_U16(skb, mac_cb(skb)->da.short_addr);
			pr_debug("%s(): dst SHORT address %04x\n",
					__func__, mac_cb(skb)->da.short_addr);

		} else {
			IEEE802154_FETCH_U64(skb, mac_cb(skb)->da.hwaddr);
			pr_debug("%s(): dst hardware addr\n", __func__);
		}
	}

	if (mac_cb(skb)->sa.addr_type != IEEE802154_ADDR_NONE) {
		pr_debug("%s(): got src non-NONE address\n", __func__);
		if (!(mac_cb_is_intrapan(skb))) { /* ! panid compress */
			IEEE802154_FETCH_U16(skb, mac_cb(skb)->sa.pan_id);
			pr_debug("%s(): src IEEE802154_FC_INTRA_PAN\n",
					__func__);
		}

		if (mac_cb(skb)->sa.addr_type == IEEE802154_ADDR_SHORT) {
			IEEE802154_FETCH_U16(skb, mac_cb(skb)->sa.short_addr);
			pr_debug("%s(): src IEEE802154_ADDR_SHORT\n",
					__func__);
		} else {
			IEEE802154_FETCH_U64(skb, mac_cb(skb)->sa.hwaddr);
			pr_debug("%s(): src hardware addr\n", __func__);
		}
	}

	return 0;

exit_error:
	return -EINVAL;
}

void ieee802154_subif_rx(struct ieee802154_dev *hw, struct sk_buff *skb)
{
	struct ieee802154_priv *priv = ieee802154_to_priv(hw);
	struct ieee802154_sub_if_data *sdata, *prev = NULL;
	int ret;

	BUILD_BUG_ON(sizeof(struct ieee802154_mac_cb) > sizeof(skb->cb));
	pr_debug("%s()\n", __func__);

	if (!(priv->hw.flags & IEEE802154_HW_OMIT_CKSUM)) {
		u16 crc;

		if (skb->len < 2) {
			pr_debug("%s(): Got invalid frame\n", __func__);
			goto out;
		}
		crc = crc_ccitt(0, skb->data, skb->len);
		if (crc) {
			pr_debug("%s(): CRC mismatch\n", __func__);
			goto out;
		}
		skb_trim(skb, skb->len - 2); /* CRC */
	}

	ret = parse_frame_start(skb); /* 3 bytes pulled after this */
	if (ret) {
		pr_debug("%s(): Got invalid frame\n", __func__);
		goto out;
	}

	pr_debug("%s() frame %d\n", __func__, mac_cb_type(skb));

	rcu_read_lock();
	list_for_each_entry_rcu(sdata, &priv->slaves, list)
	{
		if (prev) {
			struct sk_buff *skb2 = skb_clone(skb, GFP_ATOMIC);
			if (skb2)
				ieee802154_subif_frame(prev, skb2);
		}

		prev = sdata;
	}

	if (prev) {
		ieee802154_subif_frame(prev, skb);
		skb = NULL;
	}

	rcu_read_unlock();

out:
	dev_kfree_skb(skb);
	return;
}

