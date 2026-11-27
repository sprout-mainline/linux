// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Sony Mobile Communications Inc.
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
 */

#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/rpmsg.h>
#include <linux/soc/qcom/qrtr.h>

#include "qrtr.h"

struct qrtr_smd_dev {
	struct qrtr_endpoint ep;
	struct rpmsg_endpoint *channel;
	struct device *dev;
};

struct qrtr_new_server {
	struct qrtr_smd_dev *parent;
	unsigned int node;
	unsigned int port;
	u16 service;
	u16 instance;

	struct work_struct work;
};

struct qrtr_del_server {
	struct qrtr_smd_dev *parent;
	unsigned int port;

	struct work_struct work;
};

static int qcom_smd_qrtr_device_match(struct device *dev, const struct device_driver *drv)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);
	struct qrtr_driver *qdrv = to_qrtr_driver(drv);
	const struct qrtr_device_id *id = qdrv->id_table;

	if (!id)
		return 0;

	while (id->service != 0) {
		if (id->service == qdev->service && id->instance == qdev->instance)
			return 1;
		id++;
	}

	return 0;
}

static int qcom_smd_qrtr_uevent(const struct device *dev, struct kobj_uevent_env *env)
{
	const struct qrtr_device *qdev = to_qrtr_device(dev);

	return add_uevent_var(env, "MODALIAS=%s%x:%x", QRTR_MODULE_PREFIX, qdev->service,
			      qdev->instance);
}

static int qcom_smd_qrtr_device_probe(struct device *dev)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);
	struct qrtr_driver *qdrv = to_qrtr_driver(dev->driver);

	return qdrv->probe(qdev);
}

static void qcom_smd_qrtr_device_remove(struct device *dev)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);
	struct qrtr_driver *qdrv = to_qrtr_driver(dev->driver);

	if (qdrv->remove)
		qdrv->remove(qdev);
}

const struct bus_type qrtr_bus = {
	.name		= "qrtr",
	.match		= qcom_smd_qrtr_device_match,
	.uevent		= qcom_smd_qrtr_uevent,
	.probe		= qcom_smd_qrtr_device_probe,
	.remove		= qcom_smd_qrtr_device_remove,
};
EXPORT_SYMBOL_GPL(qrtr_bus);

int __qrtr_driver_register(struct qrtr_driver *drv, struct module *owner)
{
	drv->driver.bus = &qrtr_bus;
	drv->driver.owner = owner;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL_GPL(__qrtr_driver_register);

void qrtr_driver_unregister(struct qrtr_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL_GPL(qrtr_driver_unregister);

static void qcom_smd_qrtr_dev_release(struct device *dev)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);

	kfree(qdev);
}

static int qcom_smd_qrtr_match_device_by_port(struct device *dev, const void *data)
{
	struct qrtr_device *qdev = to_qrtr_device(dev);
	unsigned int port = *(unsigned int *)data;

	return qdev->port == port;
}

static void qcom_smd_qrtr_add_device_worker(struct work_struct *work)
{
	struct qrtr_new_server *new_server = container_of(work, struct qrtr_new_server, work);
	struct qrtr_smd_dev *qsdev = new_server->parent;
	struct qrtr_device *qdev;
	int ret;

	qdev = kzalloc(sizeof(*qdev), GFP_KERNEL);
	if (!qdev)
		return;

	qdev->node = new_server->node;
	qdev->port = new_server->port;
	qdev->service = new_server->service;
	qdev->instance = new_server->instance;

	devm_kfree(qsdev->dev, new_server);

	dev_set_name(&qdev->dev, "%d-%d", qdev->node, qdev->port);

	qdev->dev.bus = &qrtr_bus;
	qdev->dev.parent = qsdev->dev;
	qdev->dev.release = qcom_smd_qrtr_dev_release;
	qdev->dev.driver = NULL;

	ret = device_register(&qdev->dev);
	if (ret) {
		dev_err(qsdev->dev, "Failed to register QRTR device: %pe\n", ERR_PTR(ret));
		put_device(&qdev->dev);
	}
}

static void qcom_smd_qrtr_del_device_worker(struct work_struct *work)
{
	struct qrtr_del_server *del_server = container_of(work, struct qrtr_del_server, work);
	struct qrtr_smd_dev *qsdev = del_server->parent;
	struct device *dev = device_find_child(qsdev->dev, &del_server->port,
					       qcom_smd_qrtr_match_device_by_port);

	devm_kfree(qsdev->dev, del_server);

	if (dev)
		device_unregister(dev);
}

static int qcom_smd_qrtr_add_device(struct qrtr_endpoint *parent, unsigned int node,
				    unsigned int port, u16 service, u16 instance)
{
	struct qrtr_smd_dev *qsdev = container_of(parent, struct qrtr_smd_dev, ep);
	struct qrtr_new_server *new_server;

	new_server = devm_kzalloc(qsdev->dev, sizeof(struct qrtr_new_server), GFP_KERNEL);
	if (!new_server)
		return -ENOMEM;

	new_server->parent = qsdev;
	new_server->node = node;
	new_server->port = port;
	new_server->service = service;
	new_server->instance = instance;

	INIT_WORK(&new_server->work, qcom_smd_qrtr_add_device_worker);
	schedule_work(&new_server->work);

	return 0;
}

static int qcom_smd_qrtr_del_device(struct qrtr_endpoint *parent, unsigned int port)
{
	struct qrtr_smd_dev *qsdev = container_of(parent, struct qrtr_smd_dev, ep);
	struct qrtr_del_server *del_server;

	del_server = devm_kzalloc(qsdev->dev, sizeof(struct qrtr_del_server), GFP_KERNEL);
	if (!del_server)
		return -ENOMEM;

	del_server->parent = qsdev;
	del_server->port = port;

	INIT_WORK(&del_server->work, qcom_smd_qrtr_del_device_worker);
	schedule_work(&del_server->work);

	return 0;
}

static int qcom_smd_qrtr_device_unregister(struct device *dev, void *data)
{
	device_unregister(dev);

	return 0;
}

/* The rpmsg core opens the channel before it calls our probe, so the remote
 * may send packets before the QRTR endpoint is registered. SMD retries a
 * packet whose callback fails, but GLINK drops it, and losing the remote's
 * HELLO this way leaves the link without any service announcements. Packets
 * that arrive while probe is running are therefore held here and posted
 * once the endpoint is registered.
 */
struct qrtr_smd_early_rx {
	struct list_head node;
	struct rpmsg_device *rpdev;
	struct sk_buff_head queue;
};

static LIST_HEAD(qrtr_smd_early_rx_list);
static DEFINE_SPINLOCK(qrtr_smd_early_rx_lock);

static struct qrtr_smd_early_rx *qcom_smd_qrtr_early_rx(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_early_rx *early;

	lockdep_assert_held(&qrtr_smd_early_rx_lock);

	list_for_each_entry(early, &qrtr_smd_early_rx_list, node)
		if (early->rpdev == rpdev)
			return early;

	return NULL;
}

static int qcom_smd_qrtr_post(struct qrtr_smd_dev *qsdev, void *data, int len)
{
	int rc;

	rc = qrtr_endpoint_post(&qsdev->ep, data, len);
	if (rc == -EINVAL) {
		dev_err(qsdev->dev, "invalid ipcrouter packet\n");
		/* return 0 to let smd drop the packet */
		rc = 0;
	}

	return rc;
}

static int qcom_smd_qrtr_hold(struct rpmsg_device *rpdev, void *data, int len)
{
	struct qrtr_smd_early_rx *early;
	struct qrtr_smd_dev *qsdev;
	struct sk_buff *skb;
	unsigned long flags;
	int rc = 0;

	spin_lock_irqsave(&qrtr_smd_early_rx_lock, flags);

	/* probe may have finished in the meantime */
	qsdev = dev_get_drvdata(&rpdev->dev);
	if (qsdev) {
		rc = qcom_smd_qrtr_post(qsdev, data, len);
		goto out;
	}

	/* Still bound means we are past remove: drop, as before */
	if (device_is_bound(&rpdev->dev)) {
		rc = -EAGAIN;
		goto out;
	}

	early = qcom_smd_qrtr_early_rx(rpdev);
	if (!early) {
		early = kzalloc_obj(*early, GFP_ATOMIC);
		if (!early) {
			rc = -ENOMEM;
			goto out;
		}
		early->rpdev = rpdev;
		skb_queue_head_init(&early->queue);
		list_add_tail(&early->node, &qrtr_smd_early_rx_list);
	}

	skb = alloc_skb(len, GFP_ATOMIC);
	if (!skb) {
		rc = -ENOMEM;
		goto out;
	}
	skb_put_data(skb, data, len);
	__skb_queue_tail(&early->queue, skb);

out:
	spin_unlock_irqrestore(&qrtr_smd_early_rx_lock, flags);

	return rc;
}

/* Post the packets held during probe, then publish @qsdev so the callback
 * posts directly. Both happen under the lock, so held packets are posted
 * before any packet that arrives later. With @qsdev NULL (probe failed),
 * free them instead.
 */
static unsigned int qcom_smd_qrtr_flush_early_rx(struct rpmsg_device *rpdev,
						 struct qrtr_smd_dev *qsdev)
{
	struct qrtr_smd_early_rx *early;
	unsigned int count = 0;
	struct sk_buff *skb;
	unsigned long flags;

	spin_lock_irqsave(&qrtr_smd_early_rx_lock, flags);

	early = qcom_smd_qrtr_early_rx(rpdev);
	if (early) {
		list_del(&early->node);
		while ((skb = __skb_dequeue(&early->queue))) {
			if (qsdev)
				qcom_smd_qrtr_post(qsdev, skb->data, skb->len);
			kfree_skb(skb);
			count++;
		}
		kfree(early);
	}

	if (qsdev)
		dev_set_drvdata(&rpdev->dev, qsdev);

	spin_unlock_irqrestore(&qrtr_smd_early_rx_lock, flags);

	return count;
}

/* from smd to qrtr */
static int qcom_smd_qrtr_callback(struct rpmsg_device *rpdev,
				  void *data, int len, void *priv, u32 addr)
{
	struct qrtr_smd_dev *qsdev = dev_get_drvdata(&rpdev->dev);

	if (!qsdev)
		return qcom_smd_qrtr_hold(rpdev, data, len);

	return qcom_smd_qrtr_post(qsdev, data, len);
}

/* from qrtr to smd */
static int qcom_smd_qrtr_send(struct qrtr_endpoint *ep, struct sk_buff *skb)
{
	struct qrtr_smd_dev *qsdev = container_of(ep, struct qrtr_smd_dev, ep);
	int rc;

	rc = skb_linearize(skb);
	if (rc)
		goto out;

	rc = rpmsg_send(qsdev->channel, skb->data, skb->len);

out:
	if (rc)
		kfree_skb(skb);
	else
		consume_skb(skb);
	return rc;
}

static int qcom_smd_qrtr_probe(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qsdev;
	unsigned int held;
	int rc;

	qsdev = devm_kzalloc(&rpdev->dev, sizeof(*qsdev), GFP_KERNEL);
	if (!qsdev) {
		qcom_smd_qrtr_flush_early_rx(rpdev, NULL);
		return -ENOMEM;
	}

	qsdev->channel = rpdev->ept;
	qsdev->dev = &rpdev->dev;
	qsdev->ep.xmit = qcom_smd_qrtr_send;
	qsdev->ep.add_device = qcom_smd_qrtr_add_device;
	qsdev->ep.del_device = qcom_smd_qrtr_del_device;

	rc = qrtr_endpoint_register(&qsdev->ep, QRTR_EP_NID_AUTO);
	if (rc) {
		qcom_smd_qrtr_flush_early_rx(rpdev, NULL);
		return rc;
	}

	held = qcom_smd_qrtr_flush_early_rx(rpdev, qsdev);

	dev_dbg(&rpdev->dev, "Qualcomm SMD QRTR driver probed, held packets: %u\n",
		held);

	return 0;
}

static void qcom_smd_qrtr_remove(struct rpmsg_device *rpdev)
{
	struct qrtr_smd_dev *qsdev = dev_get_drvdata(&rpdev->dev);

	device_for_each_child(qsdev->dev, NULL, qcom_smd_qrtr_device_unregister);

	qrtr_endpoint_unregister(&qsdev->ep);

	dev_set_drvdata(&rpdev->dev, NULL);
}

static const struct rpmsg_device_id qcom_smd_qrtr_smd_match[] = {
	{ "IPCRTR" },
	{}
};

static struct rpmsg_driver qcom_smd_qrtr_driver = {
	.probe = qcom_smd_qrtr_probe,
	.remove = qcom_smd_qrtr_remove,
	.callback = qcom_smd_qrtr_callback,
	.id_table = qcom_smd_qrtr_smd_match,
	.drv = {
		.name = "qcom_smd_qrtr",
	},
};

static int __init qcom_smd_qrtr_init(void)
{
	int ret;

	ret = bus_register(&qrtr_bus);
	if (!ret)
		ret = register_rpmsg_driver(&qcom_smd_qrtr_driver);
	else
		bus_unregister(&qrtr_bus);

	return ret;
}

static void __exit qcom_smd_qrtr_exit(void)
{
	struct qrtr_smd_early_rx *early, *tmp;

	bus_unregister(&qrtr_bus);
	unregister_rpmsg_driver(&qcom_smd_qrtr_driver);

	/* Only a failed probe can leave held packets behind */
	list_for_each_entry_safe(early, tmp, &qrtr_smd_early_rx_list, node) {
		skb_queue_purge(&early->queue);
		kfree(early);
	}
}

subsys_initcall(qcom_smd_qrtr_init);
module_exit(qcom_smd_qrtr_exit);

MODULE_ALIAS("rpmsg:IPCRTR");
MODULE_DESCRIPTION("Qualcomm IPC-Router SMD interface driver");
MODULE_LICENSE("GPL v2");
