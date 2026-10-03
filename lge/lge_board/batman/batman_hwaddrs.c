/* Publish file-backed radio addresses after userspace finishes saving them. */
#include <linux/err.h>
#include <linux/etherdevice.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <net/net_namespace.h>
#include <batman_hwaddrs.h>

#define WIFI_ADDR_FILE "/data/misc/wifi/config"
#define BT_ADDR_FILE "/data/misc/bd_addr"

static DEFINE_MUTEX(readiness_lock);
/* Wi-Fi retains the last address read at file publication across power cycles. */
static unsigned char wifi_address[ETH_ALEN];

static int parse_address(const char *text, unsigned char *addr, bool wifi)
{
	int i, hi, lo;

	if (strlen(text) != 17)
		return -EINVAL;
	for (i = 0; i < ETH_ALEN; ++i) {
		hi = hex_to_bin(text[i * 3]);
		lo = hex_to_bin(text[i * 3 + 1]);
		if (hi < 0 || lo < 0 || (i < 5 && text[i * 3 + 2] != ':'))
			return -EINVAL;
		addr[i] = (hi << 4) | lo;
	}
	if (is_zero_ether_addr(addr) || is_broadcast_ether_addr(addr) ||
	    (wifi && is_multicast_ether_addr(addr)))
		return -EINVAL;
	return 0;
}

static int read_address_file(bool wifi, unsigned char *addr)
{
	struct file *file;
	char *buf, *cursor, *token;
	int length, ret = -EINVAL;
	bool found = false;

	file = filp_open(wifi ? WIFI_ADDR_FILE : BT_ADDR_FILE,
			 O_RDONLY | O_NOFOLLOW, 0);
	if (IS_ERR(file))
		return PTR_ERR(file);
	if (!S_ISREG(file->f_path.dentry->d_inode->i_mode)) {
		filp_close(file, NULL);
		return -EINVAL;
	}
	buf = kmalloc(PAGE_SIZE + 1, GFP_KERNEL);
	if (!buf) {
		filp_close(file, NULL);
		return -ENOMEM;
	}
	length = kernel_read(file, 0, buf, PAGE_SIZE);
	filp_close(file, NULL);
	if (length < 0) {
		ret = length;
		goto out;
	}
	/* Reject truncation and embedded NULs rather than accepting a prefix. */
	if (!length || length == PAGE_SIZE || memchr(buf, '\0', length))
		goto out;
	buf[length] = '\0';
	if (!wifi) {
		ret = parse_address(strim(buf), addr, false);
		goto out;
	}

	/* Preserve support for other settings in the existing Wi-Fi config. */
	cursor = buf;
	while ((token = strsep(&cursor, " \t\r\n")) != NULL) {
		if (strncmp(token, "cur_etheraddr=", 14))
			continue;
		if (found) {
			ret = -EINVAL;
			break;
		}
		found = true;
		ret = parse_address(token + 14, addr, true);
		if (ret)
			break;
	}
out:
	kfree(buf);
	return ret;
}

/* Only publication changes the selected address. Reopen a running interface
 * through DHD so both firmware and netdev use it; leave an idle radio off.
 */
static int publish_wifi_address(const unsigned char *addr)
{
	struct net_device *dev;
	bool running;
	int ret = 0;

	rtnl_lock();
	mutex_lock(&readiness_lock);
	memcpy(wifi_address, addr, ETH_ALEN);
	mutex_unlock(&readiness_lock);
	dev = __dev_get_by_name(&init_net, "wlan0");
	if (dev && dev->addr_len == ETH_ALEN &&
	    memcmp(dev->dev_addr, addr, ETH_ALEN)) {
		running = netif_running(dev);
		if (running)
			dev_close(dev);
		memcpy(dev->dev_addr, addr, ETH_ALEN);
		dev->addr_assign_type = NET_ADDR_PERM;
		call_netdevice_notifiers(NETDEV_CHANGEADDR, dev);
		if (running)
			ret = dev_open(dev);
	}
	rtnl_unlock();
	return ret;
}

/* Accept only a completion signal; address bytes must come from the saved file. */
static ssize_t ready_store(const char *buf, size_t count, bool wifi)
{
	unsigned char addr[ETH_ALEN];
	int ret;

	if (!((count == 1 && buf[0] == '1') ||
	      (count == 2 && buf[0] == '1' && buf[1] == '\n')))
		return -EINVAL;
	ret = read_address_file(wifi, addr);
	if (ret)
		return ret;
	if (wifi)
		ret = publish_wifi_address(addr);
	if (ret)
		return ret;
	return count;
}

static ssize_t wifi_ready_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	return ready_store(buf, count, true);
}

static ssize_t bt_ready_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	return ready_store(buf, count, false);
}

static ssize_t wifi_addr_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned char addr[ETH_ALEN];
	int ret = batman_wifi_get_mac_addr(addr);

	return ret ? ret : scnprintf(buf, PAGE_SIZE, "%pM\n", addr);
}

static ssize_t bt_addr_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	unsigned char addr[ETH_ALEN];
	int ret;

	/* A missing/invalid file lets Bluedroid use its existing fallback. */
	ret = read_address_file(false, addr);
	return ret ? ret : scnprintf(buf, PAGE_SIZE, "%pM\n", addr);
}

static struct kobj_attribute wifi_attribute =
	__ATTR(wifi_addr, 0444, wifi_addr_show, NULL);
static struct kobj_attribute bt_attribute =
	__ATTR(bt_addr, 0444, bt_addr_show, NULL);
static struct kobj_attribute wifi_ready_attribute =
	__ATTR(wifi_ready, 0200, NULL, wifi_ready_store);
static struct kobj_attribute bt_ready_attribute =
	__ATTR(bt_ready, 0200, NULL, bt_ready_store);
static struct attribute *address_attributes[] = {
	&wifi_attribute.attr,
	&bt_attribute.attr,
	&wifi_ready_attribute.attr,
	&bt_ready_attribute.attr,
	NULL,
};
static const struct attribute_group address_group = {
	.attrs = address_attributes,
};

int batman_wifi_get_mac_addr(unsigned char *addr)
{
	int ret = 0;

	if (!addr)
		return -EINVAL;
	mutex_lock(&readiness_lock);
	if (is_zero_ether_addr(wifi_address))
		ret = -ENOENT;
	else
		memcpy(addr, wifi_address, ETH_ALEN);
	mutex_unlock(&readiness_lock);
	return ret;
}

static int __init batman_hwaddrs_init(void)
{
	struct kobject *kobj;
	int ret;

	kobj = kobject_create_and_add("batman_hwaddrs", kernel_kobj);
	if (!kobj)
		return -ENOMEM;
	ret = sysfs_create_group(kobj, &address_group);
	if (ret)
		kobject_put(kobj);
	return ret;
}
device_initcall(batman_hwaddrs_init);
