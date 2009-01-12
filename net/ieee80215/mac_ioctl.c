#include <linux/kernel.h>
#include <linux/socket.h>
#include <linux/errno.h>
#include <linux/uaccess.h>

#include <net/ieee80215/af_ieee80215.h>
int ioctl_network_discovery(struct ieee80215_user_data *data)
{
	struct ieee80215_user_data kdata;
	if(copy_from_user(&kdata, data, sizeof(struct ieee80215_user_data))) {
		printk(KERN_ERR "copy_to_user() failed in %s", __FUNCTION__);
	}
/*
int zb_nwk_nlme_discovery(zb_nwk_t *nwk, u32 channels, u8 duration)
{
	if (duration > 0x0e) {
		zb_err("Duration is out of range\n");
		_apsme(nwk)->nlme_discovery_confirm(_apsme(nwk), 0, NULL, INVALID_PARAMETER);
		return 0;
	}
	nwk->mlme_scan_confirm = zb_nwk_scan_confirm;
	nwk->mac->mlme_scan_req(nwk->mac, IEEE80215_SCAN_ACTIVE, channels, duration);
	return 0;
}
	case ZB_SIOC_NETWORK_DISCOVERY: {
		zb_sioc_network_discovery_t data;
		if (copy_from_user(&data, req.ifr_data, sizeof(data))) {
			zb_err("copy_from_user() failed\n");
			return -EFAULT;
		}
		aps->nwk->nlme_nwk_discovery(aps->nwk, data.channels, data.duration);
		return 0;
	}
*/
	return -ENOIOCTLCMD;
}
int ioctl_network_formation(struct ieee80215_user_data *data)
{
	struct ieee80215_user_data kdata;
	if(copy_from_user(&kdata, data, sizeof(struct ieee80215_user_data))) {
		printk(KERN_ERR "copy_to_user() failed in %s", __FUNCTION__);
	}
/*
	case ZB_SIOC_NETWORK_FORMATION: {
		zb_sioc_network_formation_t data;
		if (copy_from_user(&data, req.ifr_data, sizeof(data))) {
			zb_err("copy_from_user() failed\n");
			return -EFAULT;
		}
		aps->nwk->nlme_nwk_formation(aps->nwk,
			data.channels,
			data.duration,
			data.bo,
			data.so,
			data.panid,
			data.ble);
		return 0;
	}
*/
	return -ENOIOCTLCMD;
}
int ioctl_permit_joining(struct ieee80215_user_data *data)
{
	struct ieee80215_user_data kdata;
	if(copy_from_user(&kdata, data, sizeof(struct ieee80215_user_data))) {
		printk(KERN_ERR "copy_to_user() failed in %s", __FUNCTION__);
	}
/*
	case ZB_SIOC_PERMIT_JOINING: {
		u8 duration;
		if (copy_from_user(&duration, req.ifr_data, sizeof(duration))) {
			zb_err("copy_from_user() failed\n");
			return -EFAULT;
		}
		aps->nwk->nlme_permit_join(aps->nwk, duration);
		return 0;
	}
*/
	return -ENOIOCTLCMD;
}
int ioctl_start_router(struct ieee80215_user_data *data)
{
	struct ieee80215_user_data kdata;
	if(copy_from_user(&kdata, data, sizeof(struct ieee80215_user_data))) {
		printk(KERN_ERR "copy_to_user() failed in %s", __FUNCTION__);
	}
/*
	case ZB_SIOC_START_ROUTER: {
		zb_sioc_start_router_t data;
		if (copy_from_user(&data, req.ifr_data, sizeof(data))) {
			zb_err("copy_from_user() failed\n");
			return -EFAULT;
		}
		aps->nwk->nlme_start_router(aps->nwk,
			data.bo,
			data.so,
			data.ble);
		return 0;
	}
*/
	return -ENOIOCTLCMD;
}
int ioctl_mac_join(struct ieee80215_user_data *data)
{
	struct ieee80215_user_data kdata;
	if(copy_from_user(&kdata, data, sizeof(struct ieee80215_user_data))) {
		printk(KERN_ERR "copy_to_user() failed in %s", __FUNCTION__);
	}
/*
	case ZB_SIOC_JOIN: {
		zb_sioc_join_t data;
		if (copy_from_user(&data, req.ifr_data, sizeof(data))) {
			zb_err("copy_from_user() failed\n");
			return -EFAULT;
		}
		aps->nwk->nlme_join(aps->nwk,
			data.panid,
			data.as_router,
			data.rejoin,
			data.channels,
			data.duration,
			data.power,
			data.rxon,
			data.mac_security);
		return 0;
	}
*/
	return -ENOIOCTLCMD;
}
int ioctl_mac_cmd(struct ieee80215_user_data *data)
{
	struct ieee80215_user_data kdata;
	if(copy_from_user(&kdata, data, sizeof(struct ieee80215_user_data))) {
		printk(KERN_ERR "copy_to_user() failed in %s", __FUNCTION__);
	}
/*
*/
	return -ENOIOCTLCMD;
}

