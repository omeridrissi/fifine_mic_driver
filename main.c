/* SPDX-License-Identifier: GPL-3.0 */
/*
 * USB Fifine Mic driver
 *
 * Copyright (C) 2026 Omer El Idrissi (omer.e.idrissi@gmail.com)
 *
 * This driver is based on drivers/usb/usb-skeleton.c
 */
/*
 * USB Skeleton driver - 2.2
 *
 * Copyright (C) 2001-2004 Greg Kroah-Hartman (greg@kroah.com)
 *
 * This driver is based on the 2.6.3 version of drivers/usb/usb-skeleton.c
 * but has been rewritten to be easier to read and use.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include "core.h"

/* Define these values to match your devices */
#define FIFINE_VENDOR_ID	0x0c76
#define FIFINE_PRODUCT_ID	0x161f

/* table of devices that work with this driver */
static const struct usb_device_id fifine_table[] = {
	{ USB_DEVICE(FIFINE_VENDOR_ID, FIFINE_PRODUCT_ID) },
	{ }					/* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, fifine_table);

static struct usb_driver fifine_driver;

static void fifine_delete(struct kref *kref)
{
	struct usb_fifine *dev = to_fifine_dev(kref);


	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	kfree(dev->b_buf.buf);
	kfree(dev);
}

static int fifine_open(struct inode *inode, struct file *file)
{
	struct usb_fifine *dev;
	struct usb_interface *interface;
	struct circ_reader *circ_reader;
	unsigned long flags;
	int subminor;
	int i;
	int retval;

	subminor = iminor(inode);

	interface = usb_find_interface(&fifine_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;

	for (i = 0; i < NUM_URBS; ++i) {
		usb_anchor_urb(dev->urb_list[i], &dev->submitted);
	}

	spin_lock_irqsave(&dev->b_buf.lock, flags);
	/* if first opening file, submit urb */
	if (dev->open_count == 0) {
		for (i = 0; i < NUM_URBS; ++i) {
			retval = usb_submit_urb(dev->urb_list[i], GFP_ATOMIC);
			if (retval) {
				dev_err(&interface->dev, 
					"Could not submit URB with error code %d\n", 
					retval);
				mutex_unlock(&dev->io_mutex);
				goto exit;
			}
		}

		dev->urb_running = true;
	}
	
	dev->open_count++;
	spin_unlock_irqrestore(&dev->b_buf.lock, flags);

	circ_reader = kzalloc(sizeof(*circ_reader), GFP_KERNEL);
	circ_reader->dev = dev;
	circ_reader->tail = 0;
	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data = circ_reader;

exit:
	return retval;
}

static int fifine_release(struct inode *inode, struct file *file)
{
	struct usb_fifine *dev;
	struct circ_reader *circ_reader;
	int i;

	circ_reader = file->private_data;
	if (circ_reader == NULL)
		return -ENODEV;

	dev = circ_reader->dev;

	/* allow the device to be autosuspended */
	usb_autopm_put_interface(dev->interface);

	for (i = 0; i < NUM_URBS; ++i) {
		usb_kill_urb(dev->urb_list[i]);
	}
	dev->open_count--;

	kfree(circ_reader);

	/* decrement the count on our device */
	kref_put(&dev->kref, fifine_delete);
	return 0;
}

static ssize_t fifine_read(struct file *file, char *user_buf, size_t count,
			 loff_t *offset)
{
	struct circ_reader *circ_reader;
	struct usb_fifine *dev;
	struct blocking_buf *b_buf;
	size_t count_to_read;
	size_t avail;
	size_t count_to_end;
	size_t head;
	size_t retval;

	circ_reader = file->private_data;
	dev = circ_reader->dev;
	b_buf = &dev->b_buf;
	
	if (wait_event_interruptible(b_buf->read_wq,
			(CIRC_CNT(READ_ONCE(b_buf->head), circ_reader->tail, b_buf->size) > CHUNK_SIZE)))
		return -ERESTARTSYS;
	
	head = READ_ONCE(b_buf->head);
	smp_rmb();

	avail = CIRC_CNT(head, circ_reader->tail, b_buf->size);
	count_to_read = min(count, min(avail, (size_t)CHUNK_SIZE));


	// If we're too far behind the writer
	if (CIRC_CNT(head, circ_reader->tail, b_buf->size) > (b_buf->size - CHUNK_SIZE)) {
		// Snap the tail to a safe distance behind the head
		// (e.g. one chunk size's worth)
		circ_reader->tail = (head - CHUNK_SIZE) & (b_buf->size - 1);
	}

	count_to_end = CIRC_CNT_TO_END(head,
					circ_reader->tail,
					b_buf->size);
	if (count_to_end < count_to_read) {
		retval = copy_to_user(user_buf, b_buf->buf+circ_reader->tail, count_to_end);
		if (retval)
			return -EFAULT;
		retval = copy_to_user(user_buf+count_to_end, b_buf->buf, count_to_read-count_to_end);
		if (retval)
			return -EFAULT;
	} else {
		retval = copy_to_user(user_buf, b_buf->buf+circ_reader->tail, count_to_read);
		if (retval)
			return -EFAULT;
	}

	circ_reader->tail = (circ_reader->tail+count_to_read) & (b_buf->size - 1);

	return count_to_read;
}

static const struct file_operations fifine_fops = {
	.owner =	THIS_MODULE,
	.read =		fifine_read,
	.open =		fifine_open,
	.release =	fifine_release,
	.llseek =	noop_llseek,
};

/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver fifine_class = {
	.name =		"fifine%d",
	.fops =		&fifine_fops,
	.minor_base =	usb_fifine_MINOR_BASE,
};

static int fifine_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_fifine *dev;
	struct usb_endpoint_descriptor *isoc_in;
	int i;
	int retval;

	pr_info("fifine: RUNNING PROBE\n");
	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->b_buf.lock);
	init_waitqueue_head(&dev->b_buf.read_wq);
	init_usb_anchor(&dev->submitted);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));

	/* Unmute this bih first thing */
	__u8 mute_val = 0; // 0 for unmute?
	retval = usb_control_msg(dev->udev, 
				 usb_sndctrlpipe(dev->udev, 0),
				 0x01,
				 0x21, // SET_CUR
				 0x0100,
				 (49 << 8) | 0x00,
				 &mute_val, 1, 1000);
	if (retval < 0) {
		dev_err(&interface->dev,
			"Could not unmute this bih (retval %d)\n", retval);
		goto error;
	}

	/* then set the volume */
	__u16 vol = 0x0000; // 0dB or unity gain
	retval = usb_control_msg(dev->udev,
				 usb_sndctrlpipe(dev->udev, 0),
				 0x01, 
				 0x21,
				 0x0200,
				 (49 << 8) | 0x00,
				 &vol, 2, 1000);
	if (retval < 0) {
		dev_err(&interface->dev,
			"Could not set volume on this bih (retval %d)\n", retval);
		goto error;
	}	

	retval = usb_set_interface(dev->udev, 2, 1);
	if (retval) {
		dev_err(&interface->dev,
			"Could not set cur_altsetting, usb_control_msg returned %d\n", retval);
		goto error;
	}

	dev->interface = usb_get_intf(interface);

	/* set up the endpoint information */
	retval = fifine_find_endpoint(interface->cur_altsetting,
			&isoc_in);
	if (retval) {
		dev_err(&interface->dev,
			"Could not find isochronous IN endpoint, ret code %d\n", retval);
		goto error;
	}

	dev->isoc_in = isoc_in;
	dev->isoc_in_size = usb_endpoint_maxp(isoc_in);
	dev->isoc_in_endpointAddr = isoc_in->bEndpointAddress;
	
	//dev->isoc_in_buf = usb_alloc_coherent(dev->udev, 
	//			      dev->isoc_in_size*NUM_ISO_PACKETS,
	//			      GFP_KERNEL,
	//			      &dev->dma_handle);
	//if (!dev->isoc_in_buf) {
	//	retval = -ENOMEM;
	//	goto error;
	//}

		
	dev->b_buf.size = roundup_pow_of_two
				(dev->isoc_in_size*CHUNK_COUNT);
	pr_info("fifine: allocated ring buffer of size b_buf.size=%zu\n",
		dev->b_buf.size);

	dev->b_buf.buf = kzalloc(dev->b_buf.size, GFP_KERNEL);
	if (!dev->b_buf.buf) {
		retval = -ENOMEM;
		goto error;
	}

	for (i = 0; i < NUM_URBS; ++i) {
		dev->urb_list[i] = usb_alloc_urb(NUM_ISO_PACKETS, GFP_KERNEL);
		if (!dev->urb_list[i]) {
			retval = -ENOMEM;
			goto error;
		}
	}
	
	fifine_fill_iso_urbs(dev);

	/* save our data pointer in this interface device */
	usb_set_intfdata(interface, dev);

	/* we can register the device now, as it is ready */
	retval = usb_register_dev(interface, &fifine_class);
	if (retval) {
		/* something prevented us from registering this driver */
		dev_err(&interface->dev,
			"Not able to get a minor for this device.\n");
		usb_set_intfdata(interface, NULL);
		goto error;
	}

	pr_info("Connected to endpoint address 0x%02x\n", isoc_in->bEndpointAddress);

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "USB fifine device now attached to USBfifine-%d",
		 interface->minor);
	return 0;

error:
	/* this frees allocated memory */
	kref_put(&dev->kref, fifine_delete);

	return retval;
}

static void fifine_disconnect(struct usb_interface *interface)
{
	struct usb_fifine *dev;
	int minor = interface->minor;
	int i;

	dev = usb_get_intfdata(interface);

	/* give back our minor */
	usb_deregister_dev(interface, &fifine_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->disconnected = 1;
	for (i = 0; i < NUM_URBS; ++i) {
		usb_free_urb(dev->urb_list[i]);
	}
	fifine_free_isoc_bufs(dev);
	mutex_unlock(&dev->io_mutex);

	/* decrement our usage count */
	kref_put(&dev->kref, fifine_delete);

	dev_info(&interface->dev, "USB fifine #%d now disconnected", minor);
}

static struct usb_driver fifine_driver = {
	.name =		"fifine",
	.probe =	fifine_probe,
	.disconnect =	fifine_disconnect,
	.id_table =	fifine_table,
	.supports_autosuspend = 1,
};

module_usb_driver(fifine_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Omer El Idrissi");
MODULE_DESCRIPTION("Dumbahh high school dropout making toy driver for JMTek mic");
