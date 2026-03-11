#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/circ_buf.h>

/* Get a minor range for your devices from the usb maintainer */
#define usb_fifine_MINOR_BASE	192

/* our private defines. if this grows any larger, use your own .h file */
#define MAX_TRANSFER		(PAGE_SIZE - 512)
/*
 * MAX_TRANSFER is chosen so that the VM is not stressed by
 * allocations > PAGE_SIZE and the number of packets in a page
 * is an integer 512 is the largest possible packet on EHCI
 */
#define NUM_URBS 3
/* arbitrarily chosen */

#define NUM_ISO_PACKETS 8

/* number of bytes to be written to ring buffer before being read by consumers */
#define CHUNK_SIZE 50

/* number of chunks in our ring buffer (buffer size rounded up to power of 2) */
#define CHUNK_COUNT 100

/* necessary circular buffer structure with fields required for blocking */
struct blocking_buf {
	void			*buf;
	unsigned int		head;
	int			committed;
	size_t			size;

	spinlock_t		lock;
	wait_queue_head_t	read_wq;
};

/* Structure to hold all of our device specific stuff */
struct usb_fifine {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct urb		*urb_list[NUM_URBS];
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	struct blocking_buf	b_buf;			/* for storing or reading a smooth PCM stream */
	size_t			buf_commited;		/* number of bytes written to the circ buf */
	void			*coherent_buf;		/* transfer buffer */
	dma_addr_t		dma_handle;
	size_t			isoc_in_size;
	struct usb_endpoint_descriptor *isoc_in;
	__u8			isoc_in_endpointAddr;	/* the address of the bulk in endpoint */
	int			errors;			/* the last request tanked */
	spinlock_t		err_lock;		/* lock for errors */
	int			open_count;
	bool			urb_running;
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	unsigned long		disconnected:1;
};
#define to_fifine_dev(d) container_of(d, struct usb_fifine, kref)

struct circ_reader {
	unsigned int		tail;
	struct usb_fifine	*dev;
};

int fifine_find_endpoint(struct usb_host_interface *alt,
			  struct usb_endpoint_descriptor **isoc_in);
void fifine_fill_iso_urbs(struct usb_fifine *dev);
void fifine_free_isoc_bufs(struct usb_fifine *dev);
void isoc_in_complete(struct urb *urb);
void handle_pcm_packet(struct usb_fifine *dev, __u8 *data, int len);
