// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2016-2019, The Linux Foundation. All rights reserved.
 */

#define DEBUG
#define pr_fmt(fmt) "qbt:%s: " fmt, __func__

#include <linux/input.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/pm.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/of_gpio.h>
#include <linux/kfifo.h>
#include <linux/poll.h>
#include <uapi/linux/qbt_handler.h>
#include <linux/input/touch_event_notify.h>

#define QBT_DEV "qbt"
#define MAX_FW_EVENTS 128
#define MINOR_NUM_FD 0
#define MINOR_NUM_IPC 1
#define QBT_INPUT_DEV_NAME "qbt_key_input"
#define QBT_INPUT_DEV_VERSION 0x0100

struct finger_detect_gpio {
	int gpio;
	int active_low;
	int irq;
	struct work_struct work;
	int last_gpio_state;
	int event_reported;
	bool irq_enabled;
};

struct ipc_event {
	enum qbt_fw_event ev;
};

struct fd_event {
	struct timeval timestamp;
	int X;
	int Y;
	int id;
	int state;
	bool touch_valid;
};

struct fd_userspace_buf {
	uint32_t num_events;
	struct fd_event fd_events[MAX_FW_EVENTS];
};

struct fw_ipc_info {
	int gpio;
	int irq;
	bool irq_enabled;
	struct work_struct work;
};

struct qbt_drvdata {
	struct class	*qbt_class;
	struct cdev	qbt_fd_cdev;
	struct cdev	qbt_ipc_cdev;
	struct input_dev	*in_dev;
	struct device	*dev;
	char		*qbt_node;
	atomic_t	fd_available;
	atomic_t	ipc_available;
	struct mutex	mutex;
	struct mutex	fd_events_mutex;
	struct mutex	ipc_events_mutex;
	struct fw_ipc_info	fw_ipc;
	struct finger_detect_gpio fd_gpio;
	DECLARE_KFIFO(fd_events, struct fd_event, MAX_FW_EVENTS);
	DECLARE_KFIFO(ipc_events, struct ipc_event, MAX_FW_EVENTS);
	wait_queue_head_t read_wait_queue_fd;
	wait_queue_head_t read_wait_queue_ipc;
	bool is_wuhb_connected;
	struct qbt_touch_config touch_config;
	struct fd_userspace_buf scrath_buf;
	atomic_t wakelock_acquired;
};

static struct qbt_drvdata *drvdata_g;

static void qbt_add_touch_event(struct touch_event *evt)
{
	struct qbt_drvdata *drvdata = drvdata_g;
	struct fd_event event;

	memset(&event, 0, sizeof(event));
	memcpy(&event.timestamp, &evt->time, sizeof(struct timeval));
	event.X = evt->x;
	event.Y = evt->y;
	event.id = evt->fid;
	event.touch_valid = true;
	switch (evt->type) {
	case 'D':
		event.state = QBT_EVENT_FINGER_DOWN;
		break;
	case 'U':
		event.state = QBT_EVENT_FINGER_UP;
		break;
	case 'M':
		event.state = QBT_EVENT_FINGER_MOVE;
		break;
	default:
		pr_err("Invalid touch event type\n");
	}
	pr_debug("Adding event id: %d state: %d x: %d y: %d\n",
			event.id, event.state, event.X, event.Y);
	pr_debug("timestamp: %ld.%06ld\n", event.timestamp.tv_sec,
			event.timestamp.tv_usec);
	if (!kfifo_put(&drvdata->fd_events, event))
		pr_err("FD events fifo: error adding item\n");
}

static void qbt_radius_filter(struct touch_event *evt)
{
	struct qbt_drvdata *drvdata = drvdata_g;
	struct fd_event event;
	int fifo_len = 0, last_x = 0, last_y = 0,
			last_state = QBT_EVENT_FINGER_UP,
			delta_x = 0, delta_y = 0, i = 0;

	fifo_len = kfifo_len(&drvdata->fd_events);
	for (i = 0; i < fifo_len; i++) {
		if (!kfifo_get(&drvdata->fd_events, &event))
			pr_err("FD events fifo: error removing item\n");
		else {
			if (event.id == evt->fid) {
				last_state = event.state;
				last_x = event.X;
				last_y = event.Y;
			}
			kfifo_put(&drvdata->fd_events, event);
		}
	}
	if (last_state == QBT_EVENT_FINGER_DOWN ||
			last_state == QBT_EVENT_FINGER_MOVE) {
		delta_x = abs(last_x - evt->x);
		delta_y = abs(last_y - evt->y);
		if (delta_x > drvdata->touch_config.rad_x ||
				delta_y > drvdata->touch_config.rad_y)
			qbt_add_touch_event(evt);
	} else
		qbt_add_touch_event(evt);
}

static void qbt_filter_touch_event(struct touch_event *evt)
{
	struct qbt_drvdata *drvdata = drvdata_g;

	pr_debug("Received event id: %d type: %c x: %d y: %d\n",
			evt->fid, evt->type, evt->x, evt->y);
	pr_debug("timestamp: %ld.%06ld\n", evt->time.tv_sec,
			evt->time.tv_usec);

	mutex_lock(&drvdata->fd_events_mutex);
	switch (evt->type) {
	case 'D':
	case 'U':
		qbt_add_touch_event(evt);
		break;
	case 'M':
		if (drvdata->touch_config.rad_filter_enable)
			qbt_radius_filter(evt);
		else
			qbt_add_touch_event(evt);
		break;
	default:
		pr_err("Invalid touch event type\n");
	}
	mutex_unlock(&drvdata->fd_events_mutex);
	wake_up_interruptible(&drvdata->read_wait_queue_fd);
}
static int qfp_touch_event_notify(struct notifier_block *self,
			unsigned long action, void *data)
{
	int i = 0;
	struct touch_event *event = (struct touch_event *)data;

	while (action > 0 && i < sizeof(action)) {
		if (__test_and_clear_bit(i, &action))
			qbt_filter_touch_event(event);
		i++;
		event++;
	}
	return NOTIFY_OK;
}

struct notifier_block _input_event_notifier = {
	.notifier_call = qfp_touch_event_notify,
};

/**
 * qbt_open() - Function called when user space opens device.
 * Successful if driver not currently open.
 * @inode:	ptr to inode object
 * @file:	ptr to file object
 *
 * Return: 0 on success. Error code on failure.
 */
static int qbt_open(struct inode *inode, struct file *file)
{
	struct qbt_drvdata *drvdata = NULL;
	int rc = 0;
	int minor_no = -1;

	if (!inode || !inode->i_cdev || !file) {
		pr_err("NULL pointer passed\n");
		return -EINVAL;
	}
	minor_no = iminor(inode);
	if (minor_no == MINOR_NUM_FD) {
		drvdata = container_of(inode->i_cdev,
				struct qbt_drvdata, qbt_fd_cdev);
	} else if (minor_no == MINOR_NUM_IPC) {
		drvdata = container_of(inode->i_cdev,
				struct qbt_drvdata, qbt_ipc_cdev);
	} else {
		pr_err("Invalid minor number\n");
		return -EINVAL;
	}

	file->private_data = drvdata;

	pr_debug("entry minor_no=%d fd_available=%d\n",
			minor_no, drvdata->fd_available);

	/* disallowing concurrent opens */
	if (minor_no == MINOR_NUM_FD &&
			!atomic_dec_and_test(&drvdata->fd_available)) {
		atomic_inc(&drvdata->fd_available);
		rc = -EBUSY;
	} else if (minor_no == MINOR_NUM_IPC &&
			!atomic_dec_and_test(&drvdata->ipc_available)) {
		atomic_inc(&drvdata->ipc_available);
		rc = -EBUSY;
	}

	pr_debug("exit : %d  fd_available=%d\n",
			rc, drvdata->fd_available);
	return rc;
}

/**
 * qbt_release() - Function called when user space closes device.

 * @inode:	ptr to inode object
 * @file:	ptr to file object
 *
 * Return: 0 on success. Error code on failure.
 */
static int qbt_release(struct inode *inode, struct file *file)
{
	struct qbt_drvdata *drvdata;
	int minor_no = -1;

	if (!file || !file->private_data || !inode) {
		pr_err("NULL pointer passed\n");
		return -EINVAL;
	}
	drvdata = file->private_data;
	minor_no = iminor(inode);
	pr_debug("entry minor_no=%d fd_available=%d\n",
			minor_no, drvdata->fd_available);
	if (minor_no == MINOR_NUM_FD) {
		atomic_inc(&drvdata->fd_available);
	} else if (minor_no == MINOR_NUM_IPC) {
		atomic_inc(&drvdata->ipc_available);
	} else {
		pr_err("Invalid minor number\n");
		return -EINVAL;
	}
	if (atomic_read(&drvdata->wakelock_acquired) != 0) {
		pr_debug("Releasing wakelock\n");
		pm_relax(drvdata->dev);
		atomic_set(&drvdata->wakelock_acquired, 0);
	}
	pr_debug("exit : fd_available=%d\n", drvdata->fd_available);
	return 0;
}

/**
 * qbt_ioctl() - Function called when user space calls ioctl.
 * @file:	struct file - not used
 * @cmd:	cmd identifier such as QBT_IS_WUHB_CONNECTED
 * @arg:	ptr to relevant structe: either qbt_app or
 *              qbt_send_tz_cmd depending on which cmd is passed
 *
 * Return: 0 on success. Error code on failure.
 */
static long qbt_ioctl(
		struct file *file, unsigned int cmd, unsigned long arg)
{
	int rc = 0;
	void __user *priv_arg = (void __user *)arg;
	struct qbt_drvdata *drvdata;

	if (!file || !file->private_data) {
		pr_err("NULL pointer passed\n");
		return -EINVAL;
	}

	drvdata = file->private_data;

	if (IS_ERR(priv_arg)) {
		dev_err(drvdata->dev, "%s: invalid user space pointer %lu\n",
			__func__, arg);
		return -EINVAL;
	}

	mutex_lock(&drvdata->mutex);

	pr_debug("cmd received %d\n", cmd);

	switch (cmd) {
	case QBT_ENABLE_IPC:
	{
		if (!drvdata->fw_ipc.irq_enabled) {
			enable_irq(drvdata->fw_ipc.irq);
			drvdata->fw_ipc.irq_enabled = true;
			pr_debug("%s: QBT_ENABLE_IPC\n", __func__);
		}
		break;
	}
	case QBT_DISABLE_IPC:
	{
		if (drvdata->fw_ipc.irq_enabled) {
			disable_irq(drvdata->fw_ipc.irq);
			drvdata->fw_ipc.irq_enabled = false;
			pr_debug("%s: QBT_DISABLE_IPC\n", __func__);
		}
		break;
	}
	case QBT_ENABLE_FD:
	{
		if (drvdata->is_wuhb_connected &&
				!drvdata->fd_gpio.irq_enabled) {
			enable_irq(drvdata->fd_gpio.irq);
			drvdata->fd_gpio.irq_enabled = true;
			pr_debug("%s: QBT_ENABLE_FD\n", __func__);
		}
		break;
	}
	case QBT_DISABLE_FD:
	{
		if (drvdata->is_wuhb_connected &&
				drvdata->fd_gpio.irq_enabled) {
			disable_irq(drvdata->fd_gpio.irq);
			drvdata->fd_gpio.irq_enabled = false;
			pr_debug("%s: QBT_DISABLE_FD\n", __func__);
		}
		break;
	}
	case QBT_IS_WUHB_CONNECTED:
	{
		struct qbt_wuhb_connected_status wuhb_connected_status;

		wuhb_connected_status.is_wuhb_connected =
				drvdata->is_wuhb_connected;
		rc = copy_to_user((void __user *)priv_arg,
				&wuhb_connected_status,
				sizeof(wuhb_connected_status));

		if (rc != 0) {
			pr_err("Failed to copy wuhb connected status: %d\n",
					rc);
			rc = -EFAULT;
			goto end;
		}

		break;
	}
	case QBT_SEND_KEY_EVENT:
	{
		struct qbt_key_event key_event;

		if (copy_from_user(&key_event, priv_arg,
			sizeof(key_event))
				!= 0) {
			rc = -EFAULT;
			pr_err("failed copy from user space %d\n", rc);
			goto end;
		}

		input_event(drvdata->in_dev, EV_KEY,
				key_event.key, key_event.value);
		input_sync(drvdata->in_dev);
		break;
	}
	case QBT_CONFIGURE_TOUCH_FD:
	{
		if (copy_from_user(&drvdata->touch_config, priv_arg,
			sizeof(drvdata->touch_config))
				!= 0) {
			rc = -EFAULT;
			pr_err("failed copy from user space %d\n", rc);
			goto end;
		}
		pr_debug("Touch FD Radius Filter enable: %d\n",
			drvdata->touch_config.rad_filter_enable);
		pr_debug("rad_x: %d rad_y: %d\n",
			drvdata->touch_config.rad_x,
			drvdata->touch_config.rad_y);
		break;
	}
	case QBT_ACQUIRE_WAKELOCK:
	{
		if (atomic_read(&drvdata->wakelock_acquired) == 0) {
			pr_debug("Acquiring wakelock\n");
			pm_stay_awake(drvdata->dev);
		}
		atomic_inc(&drvdata->wakelock_acquired);
		break;
	}
	case QBT_RELEASE_WAKELOCK:
	{
		if (atomic_read(&drvdata->wakelock_acquired) == 0)
			break;
		if (atomic_dec_and_test(&drvdata->wakelock_acquired)) {
			pr_debug("Releasing wakelock\n");
			pm_relax(drvdata->dev);
		}
		break;
	}
	default:
		pr_err("invalid cmd %d\n", cmd);
		rc = -ENOIOCTLCMD;
		goto end;
	}

end:
	mutex_unlock(&drvdata->mutex);
	return rc;
}

static int get_events_fifo_len_locked(
		struct qbt_drvdata *drvdata, int minor_no)
{
	int len = 0;

	if (minor_no == MINOR_NUM_FD) {
		mutex_lock(&drvdata->fd_events_mutex);
		len = kfifo_len(&drvdata->fd_events);
		mutex_unlock(&drvdata->fd_events_mutex);
	} else if (minor_no == MINOR_NUM_IPC) {
		mutex_lock(&drvdata->ipc_events_mutex);
		len = kfifo_len(&drvdata->ipc_events);
		mutex_unlock(&drvdata->ipc_events_mutex);
	}

	return len;
}

static ssize_t qbt_read(struct file *filp, char __user *ubuf,
		size_t cnt, loff_t *ppos)
{
	struct ipc_event fw_event;
	struct fd_event *fd_evt;
	struct qbt_drvdata *drvdata;
	struct fd_userspace_buf *scratch_buf;
	wait_queue_head_t *read_wait_queue = NULL;
	int i = 0;
	int minor_no = -1;
	int fifo_len = 0;
	ssize_t num_bytes = 0;

	pr_debug("entry with numBytes = %zd, minor_no = %d\n", cnt, minor_no);

	if (!filp || !filp->private_data) {
		pr_err("NULL pointer passed\n");
		return -EINVAL;
	}
	drvdata = filp->private_data;

	minor_no = iminor(filp->f_path.dentry->d_inode);
	scratch_buf = &drvdata->scrath_buf;
	memset(scratch_buf, 0, sizeof(*scratch_buf));

	if (minor_no == MINOR_NUM_FD) {
		if (cnt < sizeof(*scratch_buf)) {
			pr_err("Num bytes to read is too small\n");
			return -EINVAL;
		}
		read_wait_queue = &drvdata->read_wait_queue_fd;
	} else if (minor_no == MINOR_NUM_IPC) {
		if (cnt < sizeof(fw_event.ev)) {
			pr_err("Num bytes to read is too small\n");
			return -EINVAL;
		}
		read_wait_queue = &drvdata->read_wait_queue_ipc;
	} else {
		pr_err("Invalid minor number\n");
		return -EINVAL;
	}

	fifo_len = get_events_fifo_len_locked(drvdata, minor_no);
	while (fifo_len == 0) {
		if (filp->f_flags & O_NONBLOCK) {
			pr_debug("fw_events fifo: empty, returning\n");
			return -EAGAIN;
		}
		pr_debug("fw_events fifo: empty, waiting\n");
		if (wait_event_interruptible(*read_wait_queue,
				(get_events_fifo_len_locked(
				drvdata, minor_no) > 0)))
			return -ERESTARTSYS;
		fifo_len = get_events_fifo_len_locked(drvdata, minor_no);
	}

	if (minor_no == MINOR_NUM_FD) {
		mutex_lock(&drvdata->fd_events_mutex);

		scratch_buf->num_events = kfifo_len(&drvdata->fd_events);

		for (i = 0; i < scratch_buf->num_events; i++) {
			fd_evt = &scratch_buf->fd_events[i];
			if (!kfifo_get(&drvdata->fd_events, fd_evt)) {
				pr_err("FD event fifo: err popping item\n");
				scratch_buf->num_events = i;
				break;
			}
			pr_debug("Reading event id: %d state: %d\n",
					fd_evt->id, fd_evt->state);
			pr_debug("x: %d y: %d timestamp: %ld.%06ld\n",
					fd_evt->X, fd_evt->Y,
					fd_evt->timestamp.tv_sec,
					fd_evt->timestamp.tv_usec);
		}
		pr_debug("%d FD events read at time %lu uS\n",
				scratch_buf->num_events,
				(unsigned long)ktime_to_us(ktime_get()));
		num_bytes = copy_to_user(ubuf, scratch_buf,
				sizeof(*scratch_buf));
		mutex_unlock(&drvdata->fd_events_mutex);
	} else if (minor_no == MINOR_NUM_IPC) {
		mutex_lock(&drvdata->ipc_events_mutex);
		if (!kfifo_get(&drvdata->ipc_events, &fw_event))
			pr_err("IPC events fifo: error removing item\n");
		pr_debug("IPC event %d at minor no %d read at time %lu uS\n",
				(int)fw_event.ev, minor_no,
				(unsigned long)ktime_to_us(ktime_get()));
		num_bytes = copy_to_user(ubuf, &fw_event.ev,
				sizeof(fw_event.ev));
		mutex_unlock(&drvdata->ipc_events_mutex);
	} else {
		pr_err("Invalid minor number\n");
	}
	if (num_bytes != 0)
		pr_warn("Could not copy %d bytes\n");
	return num_bytes;
}

static unsigned int qbt_poll(struct file *filp,
	struct poll_table_struct *wait)
{
	struct qbt_drvdata *drvdata;
	unsigned int mask = 0;
	int minor_no = -1;

	if (!filp || !filp->private_data) {
		pr_err("NULL pointer passed\n");
		return -EINVAL;
	}
	drvdata = filp->private_data;

	minor_no = iminor(filp->f_path.dentry->d_inode);
	if (minor_no == MINOR_NUM_FD) {
		poll_wait(filp, &drvdata->read_wait_queue_fd, wait);
		if (kfifo_len(&drvdata->fd_events) > 0)
			mask |= (POLLIN | POLLRDNORM);
	} else if (minor_no == MINOR_NUM_IPC) {
		poll_wait(filp, &drvdata->read_wait_queue_ipc, wait);
		if (kfifo_len(&drvdata->ipc_events) > 0)
			mask |= (POLLIN | POLLRDNORM);
	} else {
		pr_err("Invalid minor number\n");
		return -EINVAL;
	}

	return mask;
}

static const struct file_operations qbt_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = qbt_ioctl,
	.open = qbt_open,
	.release = qbt_release,
	.read = qbt_read,
	.poll = qbt_poll
};

static int qbt_dev_register(struct qbt_drvdata *drvdata)
{
	dev_t dev_no, major_no;
	int ret = 0;
	size_t node_size;
	char *node_name = QBT_DEV;
	struct device *dev = drvdata->dev;
	struct device *device;

	node_size = strlen(node_name) + 1;

	drvdata->qbt_node = devm_kzalloc(dev, node_size, GFP_KERNEL);
	if (!drvdata->qbt_node) {
		ret = -ENOMEM;
		goto err_alloc;
	}

	strlcpy(drvdata->qbt_node, node_name, node_size);

	ret = alloc_chrdev_region(&dev_no, 0, 2, drvdata->qbt_node);
	if (ret) {
		pr_err("alloc_chrdev_region failed %d\n", ret);
		goto err_alloc;
	}
	major_no = MAJOR(dev_no);

	cdev_init(&drvdata->qbt_fd_cdev, &qbt_fops);

	drvdata->qbt_fd_cdev.owner = THIS_MODULE;
	ret = cdev_add(&drvdata->qbt_fd_cdev,
			MKDEV(major_no, MINOR_NUM_FD), 1);
	if (ret) {
		pr_err("cdev_add failed for fd %d\n", ret);
		goto err_cdev_add;
	}
	cdev_init(&drvdata->qbt_ipc_cdev, &qbt_fops);

	drvdata->qbt_ipc_cdev.owner = THIS_MODULE;
	ret = cdev_add(&drvdata->qbt_ipc_cdev,
			MKDEV(major_no, MINOR_NUM_IPC), 1);
	if (ret) {
		pr_err("cdev_add failed for ipc %d\n", ret);
		goto err_cdev_add;
	}

	drvdata->qbt_class = class_create(THIS_MODULE,
					   drvdata->qbt_node);
	if (IS_ERR(drvdata->qbt_class)) {
		ret = PTR_ERR(drvdata->qbt_class);
		pr_err("class_create failed %d\n", ret);
		goto err_class_create;
	}

	device = device_create(drvdata->qbt_class, NULL,
			       drvdata->qbt_fd_cdev.dev, drvdata,
			       "%s_fd", drvdata->qbt_node);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		pr_err("fd device_create failed %d\n", ret);
		goto err_dev_create;
	}

	device = device_create(drvdata->qbt_class, NULL,
				drvdata->qbt_ipc_cdev.dev, drvdata,
				"%s_ipc", drvdata->qbt_node);
	if (IS_ERR(device)) {
		ret = PTR_ERR(device);
		pr_err("ipc device_create failed %d\n", ret);
		goto err_dev_create;
	}

	return 0;
err_dev_create:
	class_destroy(drvdata->qbt_class);
err_class_create:
	cdev_del(&drvdata->qbt_fd_cdev);
	cdev_del(&drvdata->qbt_ipc_cdev);
err_cdev_add:
	unregister_chrdev_region(drvdata->qbt_fd_cdev.dev, 1);
	unregister_chrdev_region(drvdata->qbt_ipc_cdev.dev, 1);
err_alloc:
	return ret;
}

/**
 * qbt1000_create_input_device() - Function allocates an input
 * device, configures it for key events and registers it
 *
 * @drvdata:	ptr to driver data
 *
 * Return: 0 on success. Error code on failure.
 */
static int qbt_create_input_device(struct qbt_drvdata *drvdata)
{
	int rc = 0;

	drvdata->in_dev = input_allocate_device();
	if (drvdata->in_dev == NULL) {
		dev_err(drvdata->dev, "%s: input_allocate_device() failed\n",
			__func__);
		rc = -ENOMEM;
		goto end;
	}

	drvdata->in_dev->name = QBT_INPUT_DEV_NAME;
	drvdata->in_dev->phys = NULL;
	drvdata->in_dev->id.bustype = BUS_HOST;
	drvdata->in_dev->id.vendor  = 0x0001;
	drvdata->in_dev->id.product = 0x0001;
	drvdata->in_dev->id.version = QBT_INPUT_DEV_VERSION;

	drvdata->in_dev->evbit[0] = BIT_MASK(EV_KEY) |  BIT_MASK(EV_ABS);
	drvdata->in_dev->keybit[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH);

	drvdata->in_dev->keybit[BIT_WORD(KEY_HOMEPAGE)] |=
		BIT_MASK(KEY_HOMEPAGE);
	drvdata->in_dev->keybit[BIT_WORD(KEY_VOLUMEDOWN)] |=
		BIT_MASK(KEY_VOLUMEDOWN);
	drvdata->in_dev->keybit[BIT_WORD(KEY_POWER)] |=
		BIT_MASK(KEY_POWER);

	input_set_abs_params(drvdata->in_dev, ABS_X,
			     0,
			     1000,
			     0, 0);
	input_set_abs_params(drvdata->in_dev, ABS_Y,
			     0,
			     1000,
			     0, 0);

	rc = input_register_device(drvdata->in_dev);
	if (rc) {
		dev_err(drvdata->dev, "%s: input_reg_dev() failed %d\n",
			__func__, rc);
		goto end;
	}

end:
	if (rc)
		input_free_device(drvdata->in_dev);
	return rc;
}

static void qbt_fd_report_event(struct qbt_drvdata *drvdata, int state)
{
	struct fd_event event;

	memset(&event, 0, sizeof(event));

	if (!drvdata->is_wuhb_connected) {
		pr_err("Skipping as WUHB_INT is disconnected\n");
		return;
	}

	if (drvdata->fd_gpio.event_reported
			&& state == drvdata->fd_gpio.last_gpio_state)
		return;

	pr_debug("gpio %d: report state %d current_time %lu uS\n",
		drvdata->fd_gpio.gpio, state,
		(unsigned long)ktime_to_us(ktime_get()));

	drvdata->fd_gpio.event_reported = 1;
	drvdata->fd_gpio.last_gpio_state = state;

	event.state = state;
	event.touch_valid = false;
	do_gettimeofday(&event.timestamp);

	mutex_lock(&drvdata->fd_events_mutex);

	if (!kfifo_put(&drvdata->fd_events, event)) {
		pr_err("FD events fifo: error adding item\n");
	} else {
		pr_debug("FD event %d queued at time %lu uS\n", event.id,
				(unsigned long)ktime_to_us(ktime_get()));
	}
	mutex_unlock(&drvdata->fd_events_mutex);
	wake_up_interruptible(&drvdata->read_wait_queue_fd);
}

static void qbt_gpio_work_func(struct work_struct *work)
{
	int state;
	struct qbt_drvdata *drvdata;

	if (!work) {
		pr_err("NULL pointer passed\n");
		return;
	}

	drvdata = container_of(work, struct qbt_drvdata, fd_gpio.work);

	state = (__gpio_get_value(drvdata->fd_gpio.gpio) ?
			QBT_EVENT_FINGER_DOWN : QBT_EVENT_FINGER_UP)
			^ drvdata->fd_gpio.active_low;

	qbt_fd_report_event(drvdata, state);
	pm_relax(drvdata->dev);
}

static irqreturn_t qbt_gpio_isr(int irq, void *dev_id)
{
	struct qbt_drvdata *drvdata = dev_id;

	if (!drvdata) {
		pr_err("NULL pointer passed\n");
		return IRQ_HANDLED;
	}

	if (irq != drvdata->fd_gpio.irq) {
		pr_warn("invalid irq %d (expected %d)\n",
			irq, drvdata->fd_gpio.irq);
		return IRQ_HANDLED;
	}

	pr_debug("FD event received at time %lu uS\n",
			(unsigned long)ktime_to_us(ktime_get()));

	pm_stay_awake(drvdata->dev);
	schedule_work(&drvdata->fd_gpio.work);

	return IRQ_HANDLED;
}

static void qbt_irq_report_event(struct work_struct *work)
{
	struct qbt_drvdata *drvdata;
	struct ipc_event fw_ev_des;

	if (!work) {
		pr_err("NULL pointer passed\n");
		return;
	}
	drvdata = container_of(work, struct qbt_drvdata, fw_ipc.work);

	fw_ev_des.ev = FW_EVENT_IPC;
	mutex_lock(&drvdata->ipc_events_mutex);
	if (!kfifo_put(&drvdata->ipc_events, fw_ev_des)) {
		pr_err("ipc events: fifo full, drop event %d\n",
				(int) fw_ev_des.ev);
	} else {
		pr_debug("IPC event %d queued at time %lu uS\n", fw_ev_des.ev,
				(unsigned long)ktime_to_us(ktime_get()));
	}
	mutex_unlock(&drvdata->ipc_events_mutex);
	wake_up_interruptible(&drvdata->read_wait_queue_ipc);
	pm_relax(drvdata->dev);
}

/**
 * qbt_ipc_irq_handler() - function processes IPC
 * interrupts on its own thread
 * @irq:	the interrupt that occurred
 * @dev_id: pointer to the qbt_drvdata
 *
 * Return: IRQ_HANDLED when complete
 */
static irqreturn_t qbt_ipc_irq_handler(int irq, void *dev_id)
{
	struct qbt_drvdata *drvdata = (struct qbt_drvdata *)dev_id;

	if (!drvdata) {
		pr_err("NULL pointer passed\n");
		return IRQ_HANDLED;
	}

	if (irq != drvdata->fw_ipc.irq) {
		pr_warn("invalid irq %d (expected %d)\n",
			irq, drvdata->fw_ipc.irq);
		return IRQ_HANDLED;
	}

	pr_debug("IPC event received at time %lu uS\n",
			(unsigned long)ktime_to_us(ktime_get()));

	pm_stay_awake(drvdata->dev);
	schedule_work(&drvdata->fw_ipc.work);

	return IRQ_HANDLED;
}

static int setup_fd_gpio_irq(struct platform_device *pdev,
		struct qbt_drvdata *drvdata)
{
	int rc = 0;
	int irq;
	const char *desc = "qbt_finger_detect";

	if (!drvdata->is_wuhb_connected) {
		pr_err("Skipping as WUHB_INT is disconnected\n");
		goto end;
	}

	rc = devm_gpio_request_one(&pdev->dev, drvdata->fd_gpio.gpio,
		GPIOF_IN, desc);
	if (rc < 0) {
		pr_err("failed to request gpio %d, error %d\n",
			drvdata->fd_gpio.gpio, rc);
		goto end;
	}


	irq = gpio_to_irq(drvdata->fd_gpio.gpio);
	if (irq < 0) {
		rc = irq;
		pr_err("unable to get irq number for gpio %d, error %d\n",
			drvdata->fd_gpio.gpio, rc);
		goto end;
	}


	drvdata->fd_gpio.irq = irq;
	INIT_WORK(&drvdata->fd_gpio.work, qbt_gpio_work_func);

	rc = devm_request_any_context_irq(&pdev->dev, drvdata->fd_gpio.irq,
		qbt_gpio_isr, IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
		desc, drvdata);

	if (rc < 0) {
		pr_err("unable to claim irq %d; error %d\n",
			drvdata->fd_gpio.irq, rc);
		goto end;
	}

end:
	pr_debug("rc %d\n", rc);
	return rc;
}

static int setup_ipc_irq(struct platform_device *pdev,
	struct qbt_drvdata *drvdata)
{
	int rc = 0;
	const char *desc = "qbt_ipc";

	drvdata->fw_ipc.irq = gpio_to_irq(drvdata->fw_ipc.gpio);
	INIT_WORK(&drvdata->fw_ipc.work, qbt_irq_report_event);
	pr_debug("irq %d gpio %d\n",
			drvdata->fw_ipc.irq, drvdata->fw_ipc.gpio);

	if (drvdata->fw_ipc.irq < 0) {
		rc = drvdata->fw_ipc.irq;
		pr_err("no irq for gpio %d, error=%d\n",
		  drvdata->fw_ipc.gpio, rc);
		goto end;
	}

	rc = devm_gpio_request_one(&pdev->dev, drvdata->fw_ipc.gpio,
			GPIOF_IN, desc);

	if (rc < 0) {
		pr_err("failed to request gpio %d, error %d\n",
			drvdata->fw_ipc.gpio, rc);
		goto end;
	}

	rc = devm_request_threaded_irq(&pdev->dev,
		drvdata->fw_ipc.irq,
		NULL,
		qbt_ipc_irq_handler,
		IRQF_ONESHOT | IRQF_TRIGGER_FALLING,
		desc,
		drvdata);

	if (rc < 0) {
		pr_err("failed to register for ipc irq %d, rc = %d\n",
			drvdata->fw_ipc.irq, rc);
		goto end;
	}

end:
	return rc;
}

/**
 * qbt_read_device_tree() - Function reads device tree
 * properties into driver data
 * @pdev:	ptr to platform device object
 * @drvdata:	ptr to driver data
 *
 * Return: 0 on success. Error code on failure.
 */
static int qbt_read_device_tree(struct platform_device *pdev,
	struct qbt_drvdata *drvdata)
{
	int rc = 0;
	int gpio;
	enum of_gpio_flags flags;

	/* read IPC gpio */
	drvdata->fw_ipc.gpio = of_get_named_gpio(pdev->dev.of_node,
		"qcom,ipc-gpio", 0);
	if (drvdata->fw_ipc.gpio < 0) {
		rc = drvdata->fw_ipc.gpio;
		pr_err("ipc gpio not found, error=%d\n", rc);
		goto end;
	}

	gpio = of_get_named_gpio_flags(pdev->dev.of_node,
				"qcom,finger-detect-gpio", 0, &flags);
	if (gpio < 0) {
		pr_err("failed to get gpio flags\n");
		drvdata->is_wuhb_connected = 0;
		goto end;
	}

	drvdata->is_wuhb_connected = 1;
	drvdata->fd_gpio.gpio = gpio;
	drvdata->fd_gpio.active_low = flags & OF_GPIO_ACTIVE_LOW;

end:
	return rc;
}

/**
 * qbt_probe() - Function loads hardware config from device tree
 * @pdev:	ptr to platform device object
 *
 * Return: 0 on success. Error code on failure.
 */
static int qbt_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qbt_drvdata *drvdata;
	int rc = 0;

	pr_debug("entry\n");
	drvdata = devm_kzalloc(dev, sizeof(*drvdata), GFP_KERNEL);
	if (!drvdata)
		return -ENOMEM;

	drvdata->dev = &pdev->dev;
	platform_set_drvdata(pdev, drvdata);

	rc = qbt_read_device_tree(pdev, drvdata);
	if (rc < 0)
		goto end;

	atomic_set(&drvdata->fd_available, 1);
	atomic_set(&drvdata->ipc_available, 1);
	atomic_set(&drvdata->wakelock_acquired, 0);

	mutex_init(&drvdata->mutex);
	mutex_init(&drvdata->fd_events_mutex);
	mutex_init(&drvdata->ipc_events_mutex);

	rc = qbt_dev_register(drvdata);
	if (rc < 0)
		goto end;
	rc = qbt_create_input_device(drvdata);
	if (rc < 0)
		goto end;
	INIT_KFIFO(drvdata->fd_events);
	INIT_KFIFO(drvdata->ipc_events);
	init_waitqueue_head(&drvdata->read_wait_queue_fd);
	init_waitqueue_head(&drvdata->read_wait_queue_ipc);

	rc = setup_fd_gpio_irq(pdev, drvdata);
	if (rc < 0)
		goto end;
	drvdata->fd_gpio.irq_enabled = false;
	disable_irq(drvdata->fd_gpio.irq);

	rc = setup_ipc_irq(pdev, drvdata);
	if (rc < 0)
		goto end;
	drvdata->fw_ipc.irq_enabled = false;
	disable_irq(drvdata->fw_ipc.irq);

	rc = device_init_wakeup(&pdev->dev, 1);
	if (rc < 0)
		goto end;

	rc = touch_event_register_notifier(&_input_event_notifier);
	if (rc < 0)
		pr_err("Touch Event Registration failed: %d\n", rc);
	drvdata_g = drvdata;

end:
	pr_debug("exit : %d\n", rc);
	return rc;
}

static int qbt_remove(struct platform_device *pdev)
{
	struct qbt_drvdata *drvdata = platform_get_drvdata(pdev);

	mutex_destroy(&drvdata->mutex);
	mutex_destroy(&drvdata->fd_events_mutex);
	mutex_destroy(&drvdata->ipc_events_mutex);

	device_destroy(drvdata->qbt_class, drvdata->qbt_fd_cdev.dev);
	device_destroy(drvdata->qbt_class, drvdata->qbt_ipc_cdev.dev);

	class_destroy(drvdata->qbt_class);
	cdev_del(&drvdata->qbt_fd_cdev);
	cdev_del(&drvdata->qbt_ipc_cdev);
	unregister_chrdev_region(drvdata->qbt_fd_cdev.dev, 1);
	unregister_chrdev_region(drvdata->qbt_ipc_cdev.dev, 1);

	device_init_wakeup(&pdev->dev, 0);
	touch_event_unregister_notifier(&_input_event_notifier);
	drvdata_g = NULL;

	return 0;
}

static int qbt_suspend(struct platform_device *pdev, pm_message_t state)
{
	int rc = 0;
	struct qbt_drvdata *drvdata = platform_get_drvdata(pdev);

	/*
	 * Returning an error code if driver currently making a TZ call.
	 * Note: The purpose of this driver is to ensure that the clocks are on
	 * while making a TZ call. Hence the clock check to determine if the
	 * driver will allow suspend to occur.
	 */
	if (!mutex_trylock(&drvdata->mutex))
		return -EBUSY;

	else {
		if (drvdata->is_wuhb_connected)
			enable_irq_wake(drvdata->fd_gpio.irq);

		enable_irq_wake(drvdata->fw_ipc.irq);
	}

	mutex_unlock(&drvdata->mutex);

	return rc;
}

static int qbt_resume(struct platform_device *pdev)
{
	struct qbt_drvdata *drvdata = platform_get_drvdata(pdev);

	if (drvdata->is_wuhb_connected)
		disable_irq_wake(drvdata->fd_gpio.irq);

	disable_irq_wake(drvdata->fw_ipc.irq);

	return 0;
}

static const struct of_device_id qbt_match[] = {
	{ .compatible = "qcom,qbt-handler" },
	{}
};

static struct platform_driver qbt_plat_driver = {
	.probe = qbt_probe,
	.remove = qbt_remove,
	.suspend = qbt_suspend,
	.resume = qbt_resume,
	.driver = {
		.name = "qbt_handler",
		.of_match_table = qbt_match,
	},
};

module_platform_driver(qbt_plat_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Qualcomm Technologies, Inc. QBT HANDLER");
