/* SPDX-License-Identifier: GPL-2.0 */
/*
 * USB Fifine Mic driver
 *
 * Copyright (C) 2026 Omer El Idrissi (omer.e.idrissi@gmail.com)
 *
 * This driver is based on drivers/usb/usb-skeleton.c version 2.2
 */

#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/types.h>
#include "core.h"

int fifine_find_endpoint(struct usb_host_interface *alt,
				 struct usb_endpoint_descriptor **isoc_in)
{
	struct usb_endpoint_descriptor *epd;
	int i;

	if (isoc_in)
		*isoc_in = NULL;

	for (i = 0; i < alt->desc.bNumEndpoints; ++i) {
		epd = &alt->endpoint[i].desc;

		if (usb_endpoint_is_isoc_in(epd)) {
			*isoc_in = epd;
			return 0;
		}
	}
	return -ENXIO;
}

void fifine_fill_iso_urbs(struct usb_fifine *dev) 
{
	struct urb *urb;
	struct usb_iso_packet_descriptor *desc;
	void *coherent_buf;
	dma_addr_t dma_handle;
	size_t urb_buf_size; 
	int i;
	int j;

	urb_buf_size = dev->isoc_in_size*NUM_ISO_PACKETS; 

	coherent_buf = usb_alloc_coherent(dev->udev,
					urb_buf_size*NUM_URBS,
					GFP_KERNEL, &dma_handle);
	pr_info("coherent_buf = %p\n", coherent_buf);
	pr_info("dma_handle = %llu\n", dma_handle);

	if (coherent_buf == NULL) {
		pr_info("urb: sybau no dma for you\n");
		return;
	}

	dev->coherent_buf = coherent_buf;
	dev->dma_handle = dma_handle;

	for (i = 0; i < NUM_URBS; ++i) {
		urb = dev->urb_list[i];
		
		urb->pipe = usb_rcvisocpipe(dev->udev, dev->isoc_in_endpointAddr);
		urb->dev = dev->udev;
		urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP | URB_ISO_ASAP;

		urb->transfer_buffer = coherent_buf+(i*urb_buf_size);
		urb->transfer_dma = dma_handle+(i*urb_buf_size);

		urb->transfer_buffer_length = urb_buf_size;
		urb->number_of_packets = NUM_ISO_PACKETS;

		urb->interval = dev->isoc_in->bInterval;

		for (j = 0; j < NUM_ISO_PACKETS; ++j) {
			desc = &urb->iso_frame_desc[j];

			desc->offset = dev->isoc_in_size*j;
			desc->length = dev->isoc_in_size;
		}

		urb->context = dev;
		urb->complete = isoc_in_complete;
	}

}

void fifine_free_isoc_bufs(struct usb_fifine *dev) 
{
	size_t urb_buf_size;

	urb_buf_size = dev->isoc_in_size*NUM_ISO_PACKETS*NUM_URBS;

	usb_free_coherent(dev->udev,
			  urb_buf_size,
			  dev->coherent_buf,
			  dev->dma_handle);
}

void isoc_in_complete(struct urb *urb)
{
	struct usb_fifine *dev;
	struct usb_iso_packet_descriptor *desc;
	__u8 *data;
	int len;
	int i;
	dev = urb->context;

	urb->transfer_flags = URB_NO_TRANSFER_DMA_MAP;

	if (urb->status) {
		dev_warn(&dev->interface->dev,
			 "urb likely unlinked. status code: %d\n",
			 urb->status);
		if (urb->status == -ENOENT || urb->status == -ESHUTDOWN
				|| urb->status == -EBUSY || urb->status == -ENODEV)
			return;

		goto resubmit;
	}

	for (i = 0; i < urb->number_of_packets; ++i) {
		desc = &urb->iso_frame_desc[i];

		if (desc->status != 0 || desc->actual_length == 0) {
			pr_err("fifine: DBG: pcm packet err status = %d\n", desc->status);
			continue;
		}

		data = urb->transfer_buffer + desc->offset;
		len = desc->actual_length;
		
		handle_pcm_packet(dev, data, len);
	}
	
resubmit:
	usb_submit_urb(urb, GFP_ATOMIC);
}

/* Logic for writing new data to circular buffer */
void handle_pcm_packet(struct usb_fifine *dev, __u8 *data, int len) {
	struct blocking_buf *b_buf;
	size_t space_to_end;
	size_t head; 
	unsigned long flags;

	b_buf = &dev->b_buf;
	
	spin_lock_irqsave(&b_buf->lock, flags);

	head = b_buf->head;

	space_to_end = b_buf->size - head;
	if (len > space_to_end) {
		memcpy(b_buf->buf+head, data, space_to_end);
		memcpy(b_buf->buf, data+space_to_end, len-space_to_end);
	} else {
		memcpy(b_buf->buf+head, data, len);
	}

	smp_wmb();

	b_buf->head = (head + len) & (b_buf->size - 1);
	
	wake_up_interruptible(&b_buf->read_wq);

	spin_unlock_irqrestore(&b_buf->lock, flags);

}
