/*
 * Copyright (C) 1999-2000 by David Brownell <david-b@pacbell.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
 
 
/*
 * USB driver for Kodak DC-2XX series digital still cameras
 *
 * The protocol here is the same as the one going over a serial line, but
 * it uses USB for speed.  Set up /dev/kodak, get gphoto (www.gphoto.org),
 * and have fun!
 *
 * This should also work for a number of other digital (non-Kodak) cameras,
 * by adding the vendor and product IDs to the table below.
 */

/*
 * HISTORY
 *
 * 26 August, 1999 -- first release (0.1), works with my DC-240.
 * 	The DC-280 (2Mpixel) should also work, but isn't tested.
 *	If you use gphoto, make sure you have the USB updates.
 *	Lives in a 2.3.14 or so Linux kernel, in drivers/usb.
 * 31 August, 1999 -- minor update to recognize DC-260 and handle
 *	its endpoints being in a different order.  Note that as
 *	of gPhoto 0.36pre, the USB updates are integrated.
 * 12 Oct, 1999 -- handle DC-280 interface class (0xff not 0x0);
 *	added timeouts to bulk_msg calls.  Minor updates, docs.
 * 03 Nov, 1999 -- update for 2.3.25 kernel API changes.
 * 08 Jan, 2000 .. multiple camera support
 *
 * Thanks to:  the folk who've provided USB product IDs, sent in
 * patches, and shared their sucesses!
 */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/signal.h>
#include <linux/errno.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/poll.h>
#include <linux/init.h>
#include <linux/malloc.h>
#include <linux/module.h>
#undef DEBUG
#include <linux/usb.h>



/* current USB framework handles max of 16 USB devices per driver */
#define	MAX_CAMERAS		8

/* USB char devs use USB_MAJOR and from USB_CAMERA_MINOR_BASE up */
#define	USB_CAMERA_MINOR_BASE	80


// XXX remove packet size limit, now that bulk transfers seem fixed

/* Application protocol limit is 0x8002; USB has disliked that limit! */
#define	MAX_PACKET_SIZE		0x2000		/* e.g. image downloading */

#define	MAX_READ_RETRY		5		/* times to retry reads */
#define	MAX_WRITE_RETRY		5		/* times to retry writes */
#define	RETRY_TIMEOUT		(HZ)		/* sleep between retries */


/* table of cameras that work through this driver */
static const struct camera {
	short		idVendor;
	short		idProduct;
	/* plus hooks for camera-specific info if needed */
} cameras [] = {
	/* These have the same application level protocol */  
    { 0x040a, 0x0120 },		// Kodak DC-240
    { 0x040a, 0x0130 },		// Kodak DC-280

	/* These have a different application level protocol which
	 * is part of the Flashpoint "DigitaOS".  That supports some
	 * non-camera devices, and some non-Kodak cameras.
	 */  
    { 0x040a, 0x0100 },		// Kodak DC-220
    { 0x040a, 0x0110 },		// Kodak DC-260
    { 0x040a, 0x0111 },		// Kodak DC-265
    { 0x040a, 0x0112 },		// Kodak DC-290
//  { 0x03f0, 0xffff },		// HP PhotoSmart C500

	/* Other USB devices may well work here too, so long as they
	 * just stick to half duplex bulk packet exchanges.
	 */
};


struct camera_state {
	struct usb_device	*dev;		/* USB device handle */
	char			inEP;		/* read endpoint */
	char			outEP;		/* write endpoint */
	const struct camera	*info;		/* DC-240, etc */
	int			subminor;	/* which minor dev #? */
	int			isActive;	/* I/O taking place? */

	/* this is non-null iff the device is open */
	char			*buf;		/* buffer for I/O */

	/* always valid */
	wait_queue_head_t	wait;		/* for timed waits */
};


/* Support multiple cameras, possibly of different types.  */
static struct camera_state *minor_data [MAX_CAMERAS];


static ssize_t camera_read (struct file *file,
	char *buf, size_t len, loff_t *ppos)
{
	struct camera_state	*camera;
	int			retries;

	if (len > MAX_PACKET_SIZE)
		return -EINVAL;

	camera = (struct camera_state *) file->private_data;
	if (!camera->dev)
		return -ENODEV;
	if (camera->isActive++)
		return -EBUSY;

	/* Big reads are common, for image downloading.  Smaller ones
	 * are also common (even "directory listing" commands don't
	 * send very much data).  We preserve packet boundaries here,
	 * they matter in the application protocol.
	 */
	for (retries = 0; retries < MAX_READ_RETRY; retries++) {
		int			count;
		int			result;

		if (signal_pending (current)) {
			camera->isActive = 0;
			return -EINTR;
		}
		if (!camera->dev) {
			camera->isActive = 0;
			return -ENODEV;
		}

		result = usb_bulk_msg (camera->dev,
			  usb_rcvbulkpipe (camera->dev, camera->inEP),
			  camera->buf, len, &count, HZ*10);

		dbg ("read (%d) - 0x%x %ld", len, result, count);

		if (!result) {
			if (copy_to_user (buf, camera->buf, count))
				return -EFAULT;
			camera->isActive = 0;
			return count;
		}
		if (result != USB_ST_TIMEOUT)
			break;
		interruptible_sleep_on_timeout (&camera->wait, RETRY_TIMEOUT);

		dbg ("read (%d) - retry", len);
	}
	camera->isActive = 0;
	return -EIO;
}

static ssize_t camera_write (struct file *file,
	const char *buf, size_t len, loff_t *ppos)
{
	struct camera_state	*camera;
	ssize_t			bytes_written = 0;

	if (len > MAX_PACKET_SIZE)
		return -EINVAL;

	camera = (struct camera_state *) file->private_data;
	if (!camera->dev)
		return -ENODEV;
	if (camera->isActive++)
		return -EBUSY;
	
	/* most writes will be small: simple commands, sometimes with
	 * parameters.  putting images (like borders) into the camera
	 * would be the main use of big writes.
	 */
	while (len > 0) {
		char		*obuf = camera->buf;
		int		maxretry = MAX_WRITE_RETRY;
		unsigned long	copy_size, thistime;

		/* it's not clear that retrying can do any good ... or that
		 * fragmenting application packets into N writes is correct.
		 */
		thistime = copy_size = len;
		if (copy_from_user (obuf, buf, copy_size)) {
			bytes_written = -EFAULT;
			break;
		}
		while (thistime) {
			int		result;
			int		count;

			if (signal_pending (current)) {
				if (!bytes_written)
					bytes_written = -EINTR;
				goto done;
			}
			if (!camera->dev) {
				if (!bytes_written)
					bytes_written = -ENODEV;
				goto done;
			}

			result = usb_bulk_msg (camera->dev,
				 usb_sndbulkpipe (camera->dev, camera->outEP),
				 obuf, thistime, &count, HZ*10);

			if (result)
				dbg ("write USB err - %x", result);

			if (count) {
				obuf += count;
				thistime -= count;
				maxretry = MAX_WRITE_RETRY;
				continue;
			} else if (!result)
				break;
				
			if (result == USB_ST_TIMEOUT) {	/* NAK - delay a bit */
				if (!maxretry--) {
					if (!bytes_written)
						bytes_written = -ETIME;
					goto done;
				}
                                interruptible_sleep_on_timeout (&camera->wait,
					RETRY_TIMEOUT);
				continue;
			} 
			if (!bytes_written)
				bytes_written = -EIO;
			goto done;
		}
		bytes_written += copy_size;
		len -= copy_size;
		buf += copy_size;
	}
done:
	camera->isActive = 0;
	dbg ("wrote %d", bytes_written); 
	return bytes_written;
}

static int camera_open (struct inode *inode, struct file *file)
{
	struct camera_state	*camera;
	int			subminor;

	subminor = MINOR (inode->i_rdev) - USB_CAMERA_MINOR_BASE;
	if (subminor < 0 || subminor >= MAX_CAMERAS
			|| !(camera = minor_data [subminor])) {
		return -ENODEV;
	}

	if (!(camera->buf = (char *) kmalloc (MAX_PACKET_SIZE, GFP_KERNEL))) {
		return -ENOMEM;
	}

	dbg ("open"); 
	
	/* Keep driver from being unloaded while it's in use */
	MOD_INC_USE_COUNT;

	camera->isActive = 0;
	file->private_data = camera;
	return 0;
}

static int camera_release (struct inode *inode, struct file *file)
{
	struct camera_state *camera;

	camera = (struct camera_state *) file->private_data;
	kfree (camera->buf);

	/* If camera was unplugged with open file ... */
	if (!camera->dev) {
		minor_data [camera->subminor] = NULL;
		kfree (camera);
	}

	MOD_DEC_USE_COUNT;

	dbg ("close"); 

	return 0;
}

	/* XXX should define some ioctls to expose camera type
	 * to applications ... what USB exposes should suffice.
	 * apps should be able to see the camera type.
	 */
static /* const */ struct file_operations usb_camera_fops = {
	    /* Uses GCC initializer extension; simpler to maintain */
	read:		camera_read,
	write:		camera_write,
	open:		camera_open,
	release:	camera_release,
};



static void * camera_probe(struct usb_device *dev, unsigned int ifnum)
{
	int				i;
	const struct camera		*camera_info = NULL;
	struct usb_interface_descriptor	*interface;
	struct usb_endpoint_descriptor	*endpoint;
	int				direction, ep;
	struct camera_state		*camera;

	/* Is it a supported camera? */
	for (i = 0; i < sizeof (cameras) / sizeof (struct camera); i++) {
		if (cameras [i].idVendor != dev->descriptor.idVendor)
			continue;
		if (cameras [i].idProduct != dev->descriptor.idProduct)
			continue;
		camera_info = &cameras [i];
		break;
	}
	if (camera_info == NULL)
		return NULL;

	/* these have one config, one interface */
	if (dev->descriptor.bNumConfigurations != 1
			|| dev->config[0].bNumInterfaces != 1) {
		dbg ("Bogus camera config info");
		return NULL;
	}

	/* models differ in how they report themselves */
	interface = &dev->actconfig->interface[ifnum].altsetting[0];
	if ((interface->bInterfaceClass != USB_CLASS_PER_INTERFACE
		&& interface->bInterfaceClass != USB_CLASS_VENDOR_SPEC)
			|| interface->bInterfaceSubClass != 0
			|| interface->bInterfaceProtocol != 0
			|| interface->bNumEndpoints != 2
			) {
		dbg ("Bogus camera interface info");
		return NULL;
	}


	/* select "subminor" number (part of a minor number) */
	for (i = 0; i < MAX_CAMERAS; i++) {
		if (!minor_data [i])
			break;
	}
	if (i >= MAX_CAMERAS) {
		info ("Ignoring additional USB Camera");
		return NULL;
	}

	/* allocate & init camera state */
	camera = minor_data [i] = kmalloc (sizeof *camera, GFP_KERNEL);
	if (!camera) {
		err ("no memory!");
		return NULL;
	}
	camera->dev = dev;
	camera->subminor = i;
	camera->isActive = 0;
	camera->buf = NULL;
	init_waitqueue_head (&camera->wait);
	info ("USB Camera #%d connected", camera->subminor);


	/* get input and output endpoints (either order) */
	endpoint = interface->endpoint;
	camera->outEP = camera->inEP =  -1;

	ep = endpoint [0].bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	direction = endpoint [0].bEndpointAddress & USB_ENDPOINT_DIR_MASK;
	if (direction == USB_DIR_IN)
		camera->inEP = ep;
	else
		camera->outEP = ep;

	ep = endpoint [1].bEndpointAddress & USB_ENDPOINT_NUMBER_MASK;
	direction = endpoint [1].bEndpointAddress & USB_ENDPOINT_DIR_MASK;
	if (direction == USB_DIR_IN)
		camera->inEP = ep;
	else
		camera->outEP = ep;

	if (camera->outEP == -1 || camera->inEP == -1
			|| endpoint [0].bmAttributes != USB_ENDPOINT_XFER_BULK
			|| endpoint [1].bmAttributes != USB_ENDPOINT_XFER_BULK
			) {
		dbg ("Bogus endpoints");
		camera->dev = NULL;
		return NULL;
	}


	if (usb_set_configuration (dev, dev->config[0].bConfigurationValue)) {
		err ("Failed usb_set_configuration");
		camera->dev = NULL;
		return NULL;
	}

	camera->info = camera_info;
	return camera;
}

static void camera_disconnect(struct usb_device *dev, void *ptr)
{
	struct camera_state	*camera = (struct camera_state *) ptr;
	int			subminor = camera->subminor;

	/* If camera's not opened, we can clean up right away.
	 * Else apps see a disconnect on next I/O; the release cleans.
	 */
	if (!camera->buf) {
		minor_data [subminor] = NULL;
		kfree (camera);
	} else
		camera->dev = NULL;

	info ("USB Camera #%d disconnected", subminor);
}

static /* const */ struct usb_driver camera_driver = {
	"dc2xx",
	camera_probe,
	camera_disconnect,
	{ NULL, NULL },
	&usb_camera_fops,
	USB_CAMERA_MINOR_BASE
};


int __init usb_dc2xx_init(void)
{
 	if (usb_register (&camera_driver) < 0)
 		return -1;
	return 0;
}

void __exit usb_dc2xx_cleanup(void)
{
	usb_deregister (&camera_driver);
}


MODULE_AUTHOR("David Brownell, david-b@pacbell.net");
MODULE_DESCRIPTION("USB Camera Driver for Kodak DC-2xx series cameras");

module_init (usb_dc2xx_init);
module_exit (usb_dc2xx_cleanup);
