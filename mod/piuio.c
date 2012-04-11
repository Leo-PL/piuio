/*
 * PIUIO interface driver
 *
 * Copyright (C) 2012 Devin J. Pohly (djpohly+linux@gmail.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 * This code is based on the USB skeleton driver by Greg Kroah-Hartman.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/errno.h>


/* Module and driver info */
MODULE_AUTHOR("Devin J. Pohly");
MODULE_DESCRIPTION("PIUIO input/output driver");
MODULE_LICENSE("GPL");

/* Vendor/product ID table */
static const struct usb_device_id piuio_ids[] = {
	{ USB_DEVICE(0x0547, 0x1002) },
	{},
};
MODULE_DEVICE_TABLE(usb, piuio_ids);

/* Module parameters */
static int timeout_ms = 10;
module_param(timeout_ms, int, 0644);
MODULE_PARM_DESC(timeout_ms, "Timeout for PIUIO USB messages in ms"
		" (default 10)");


/* Protocol-specific parameters */
#define PIUIO_MSG_REQ 0xae
#define PIUIO_MSG_VAL 0
#define PIUIO_MSG_IDX 0

/* Size of input and output packets */
#define PIUIO_INPUT_SZ 8
#define PIUIO_OUTPUT_SZ 8
/* XXX: The piuio_read code currently expects this to be 4.  Until we know more
 * about how the device works, it will have to stay that way. */
#define PIUIO_NUM_INPUTS 4


/* Represents the current state of an interface */
struct piuio_state {
	/* USB device and interface */
	struct usb_device *dev;	
	struct usb_interface *intf;
	/* Concurrency control */
	struct mutex lock;
	struct kref kref;
	/* Send outputs immediately (1) or batch with next input (0)? */
	int out_imm;
	/* Current state of outputs */
	char outputs[PIUIO_OUTPUT_SZ];
};

static struct usb_driver piuio_driver;


/* Auxiliary function to clean up interface state */
static void piuio_free(struct kref *kref)
{
	struct piuio_state *st = container_of(kref, struct piuio_state, kref);

	usb_put_dev(st->dev);
	kfree(st);
}

/* Handles open() for /dev/piuioN */
static int piuio_open(struct inode *inode, struct file *filp)
{
	struct usb_interface *intf;
	struct piuio_state *st;
	int rv;

	/* Get the corresponding interface and state */
	intf = usb_find_interface(&piuio_driver, iminor(inode));
	if (!intf)
		return -ENODEV;
	st = usb_get_intfdata(intf);
	if (!st)
		return -ENODEV;

	/* Pick up a reference to the interface */
	kref_get(&st->kref);

	/* Ensure the device isn't suspended while in use */
	rv = usb_autopm_get_interface(intf);
	if (rv) {
		kref_put(&st->kref, piuio_free);
		return rv;
	}

	/* Attach our state to the file */
	filp->private_data = st;
	return 0;
}

/* Cleans up after the last close() on a descriptor for /dev/piuioN */
static int piuio_release(struct inode *inode, struct file *filp)
{
	struct piuio_state *st;

	st = filp->private_data;
	if (st == NULL)
		return -ENODEV;

	if (st->intf)
		usb_autopm_put_interface(st->intf);

	/* Drop reference */
	kref_put(&st->kref, piuio_free);
	return 0;
}

/* Reading a packet from /dev/piuioN return the state of all the sensors */
static ssize_t piuio_read(struct file *filp, char __user *ubuf, size_t sz,
		loff_t *pofs)
{
	struct piuio_state *st;
	char buf[PIUIO_INPUT_SZ * PIUIO_NUM_INPUTS];
	int i;
	int rv = 0;

	if (sz != sizeof(buf))
		return -EINVAL;

	st = filp->private_data;

	mutex_lock(&st->lock);

	/* Error if the device has been closed */
	if (!st->intf) {
		rv = -ENODEV;
		goto out;
	}

	for (i = 0; i < PIUIO_NUM_INPUTS; i++) {
		/* First select which set of inputs to get */
		st->outputs[0] = (st->outputs[0] & ~3) | i;
		st->outputs[2] = (st->outputs[2] & ~3) | i;
		rv = usb_control_msg(st->dev, usb_sndctrlpipe(st->dev, 0),
				PIUIO_MSG_REQ,
				USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_DEVICE,
				PIUIO_MSG_VAL, PIUIO_MSG_IDX,
				&st->outputs, sizeof(st->outputs), timeout_ms);
		if (rv < 0)
			break;

		/* Then request the status of the inputs */
		rv = usb_control_msg(st->dev, usb_rcvctrlpipe(st->dev, 0),
				PIUIO_MSG_REQ,
				USB_DIR_IN|USB_TYPE_VENDOR|USB_RECIP_DEVICE,
				PIUIO_MSG_VAL, PIUIO_MSG_IDX,
				&buf[i * PIUIO_INPUT_SZ], PIUIO_INPUT_SZ,
				timeout_ms);
		if (rv < 0)
			break;
	}

out:
	mutex_unlock(&st->lock);
	if (rv < 0)
		return rv;
	if (copy_to_user(ubuf, buf, sizeof(buf)))
		return -EFAULT;
	return sizeof(buf);
}

/* Writing a packet to /dev/piuioN controls the lights and other outputs */
static ssize_t piuio_write(struct file *filp, const char __user *ubuf,
		size_t sz, loff_t *pofs)
{
	struct piuio_state *st;
	int rv = 0;

	if (sz != sizeof(st->outputs))
		return -EINVAL;

	st = filp->private_data;

	mutex_lock(&st->lock);

	/* Error if the device has been closed */
	if (!st->intf) {
		rv = -ENODEV;
		goto out;
	}

	/* Save the desired outputs */
	if (copy_from_user(st->outputs, ubuf, sz)) {
		rv = -EFAULT;
		goto out;
	}

	/* Batching with the next input request?  If so, return now. */
	if (!st->out_imm)
		goto out;

	/* Otherwise, update the lights right away */
	rv = usb_control_msg(st->dev, usb_sndctrlpipe(st->dev, 0),
			PIUIO_MSG_REQ,
			USB_DIR_OUT|USB_TYPE_VENDOR|USB_RECIP_DEVICE,
			PIUIO_MSG_VAL, PIUIO_MSG_IDX,
			&st->outputs, sizeof(st->outputs), timeout_ms);
out:
	mutex_unlock(&st->lock);
	return rv;
}

/* File operations for /dev/piuioN */
static const struct file_operations piuio_fops = {
	.owner =	THIS_MODULE,
	.read =		piuio_read,
	.write =	piuio_write,
	.open =		piuio_open,
	.release =	piuio_release,
	.llseek =	noop_llseek,
};

/* Class driver, for creating device files */
static struct usb_class_driver piuio_class = {
	.name =		"piuio%d",
	.fops =		&piuio_fops,
};


/* Set up a device being connected to this driver */
static int piuio_probe(struct usb_interface *intf,
		const struct usb_device_id *id)
{
	struct piuio_state *st;
	int rv;

	/* Set up state structure */
	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st) {
		err("Failed to allocate state");
		return -ENOMEM;
	}
	kref_init(&st->kref);
	mutex_init(&st->lock);

	st->dev = usb_get_dev(interface_to_usbdev(intf));
	st->intf = intf;
	/* XXX For now, send light changes immediately */
	st->out_imm = 1;

	/* Store a pointer so we can get at the state later */
	usb_set_intfdata(intf, st);

	/* Register the device */
	rv = usb_register_dev(intf, &piuio_class);
	if (rv) {
		err("Failed to register device");
		usb_set_intfdata(intf, NULL);
		kref_put(&st->kref, piuio_free);
	}
	return rv;
}

/* Clean up when a device is disconnected */
static void piuio_disconnect(struct usb_interface *intf)
{
	struct piuio_state *st = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	usb_deregister_dev(intf, &piuio_class);

	mutex_lock(&st->lock);
	st->intf = NULL;
	mutex_unlock(&st->lock);

	kref_put(&st->kref, piuio_free);
}

/* Device driver handlers */
static struct usb_driver piuio_driver = {
	.name =		"piuio",
	.probe =	piuio_probe,
	.disconnect =	piuio_disconnect,
	.id_table =	piuio_ids,
	.supports_autosuspend = 1,
};

/* From 3.3 on, we can replace everthing below with:
 * module_usb_driver(piuio_driver);
 */

/* Register the driver */
static int __init piuio_init(void)
{
	return usb_register(&piuio_driver);
}

/* Remove the driver */
static void __exit piuio_exit(void)
{
	usb_deregister(&piuio_driver);
}

module_init(piuio_init);
module_exit(piuio_exit);