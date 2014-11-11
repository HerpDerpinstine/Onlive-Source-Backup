/*
 * X-Box gamepad driver
 *
 * Copyright (c) 2002 Marko Friedemann <mfr@bmx-chemnitz.de>
 *               2004 Oliver Schwartz <Oliver.Schwartz@gmx.de>,
 *                    Steven Toth <steve@toth.demon.co.uk>,
 *                    Franz Lehner <franz@caos.at>,
 *                    Ivan Hawkes <blackhawk@ivanhawkes.com>
 *               2005 Dominic Cerquetti <binary1230@yahoo.com>
 *               2006 Adam Buchbinder <adam.buchbinder@gmail.com>
 *               2007 Jan Kratochvil <honza@jikos.cz>
 * Portion(s) copyright (c) OnLive, Inc. 2010. 
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 * This driver is based on:
 *  - information from     http://euc.jp/periphs/xbox-controller.ja.html
 *  - the iForce driver    drivers/char/joystick/iforce.c
 *  - the skeleton-driver  drivers/usb/usb-skeleton.c
 *  - Xbox 360 information http://www.free60.org/wiki/Gamepad
 *
 * Thanks to:
 *  - ITO Takayuki for providing essential xpad information on his website
 *  - Vojtech Pavlik     - iforce driver / input subsystem
 *  - Greg Kroah-Hartman - usb-skeleton driver
 *  - XBOX Linux project - extra USB id's
 *
 * TODO:
 *  - fine tune axes (especially trigger axes)
 *  - fix "analog" buttons (reported as digital now)
 *  - get rumble working
 *  - need USB IDs for other dance pads
 *
 * History:
 *
 * 2002-06-27 - 0.0.1 : first version, just said "XBOX HID controller"
 *
 * 2002-07-02 - 0.0.2 : basic working version
 *  - all axes and 9 of the 10 buttons work (german InterAct device)
 *  - the black button does not work
 *
 * 2002-07-14 - 0.0.3 : rework by Vojtech Pavlik
 *  - indentation fixes
 *  - usb + input init sequence fixes
 *
 * 2002-07-16 - 0.0.4 : minor changes, merge with Vojtech's v0.0.3
 *  - verified the lack of HID and report descriptors
 *  - verified that ALL buttons WORK
 *  - fixed d-pad to axes mapping
 *
 * 2002-07-17 - 0.0.5 : simplified d-pad handling
 *
 * 2004-10-02 - 0.0.6 : DDR pad support
 *  - borrowed from the XBOX linux kernel
 *  - USB id's for commonly used dance pads are present
 *  - dance pads will map D-PAD to buttons, not axes
 *  - pass the module paramater 'dpad_to_buttons' to force
 *    the D-PAD to map to buttons if your pad is not detected
 *
 * Later changes can be tracked in SCM.
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/usb/input.h>

#define DRIVER_AUTHOR "Marko Friedemann <mfr@bmx-chemnitz.de>"
#define DRIVER_DESC "X-Box pad driver"

#define XPAD_PKT_LEN 32

/* xbox d-pads should map to buttons, as is required for DDR pads
   but we map them to axes when possible to simplify things */
#define MAP_DPAD_TO_BUTTONS    0
#define MAP_DPAD_TO_AXES       1
#define MAP_DPAD_UNKNOWN       2

#define XTYPE_XBOX        0
#define XTYPE_XBOX360     1
#define XTYPE_XBOX360W    2
#define XTYPE_UNKNOWN     3

static int dpad_to_buttons;
module_param(dpad_to_buttons, bool, S_IRUGO);
MODULE_PARM_DESC(dpad_to_buttons, "Map D-PAD to buttons rather than axes for unknown pads");

static const struct xpad_device {
	u16 idVendor;
	u16 idProduct;
	char *name;
	u8 dpad_mapping;
	u8 xtype;
} xpad_device[] = {
	{ 0x045e, 0x0202, "Microsoft X-Box pad v1 (US)", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x045e, 0x0289, "Microsoft X-Box pad v2 (US)", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x045e, 0x0285, "Microsoft X-Box pad (Japan)", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x045e, 0x0287, "Microsoft Xbox Controller S", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x045e, 0x0719, "Xbox 360 Wireless Receiver", MAP_DPAD_TO_AXES, XTYPE_XBOX360W },
	{ 0x0c12, 0x8809, "RedOctane Xbox Dance Pad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x044f, 0x0f07, "Thrustmaster, Inc. Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x046d, 0xc242, "Logitech Chillstream Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX360 },
	{ 0x046d, 0xca84, "Logitech Xbox Cordless Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x046d, 0xca88, "Logitech Compact Controller for Xbox", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x05fd, 0x1007, "Mad Catz Controller (unverified)", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x05fd, 0x107a, "InterAct 'PowerPad Pro' X-Box pad (Germany)", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0738, 0x4516, "Mad Catz Control Pad", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0738, 0x4522, "Mad Catz LumiCON", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0738, 0x4526, "Mad Catz Control Pad Pro", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0738, 0x4536, "Mad Catz MicroCON", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0738, 0x4540, "Mad Catz Beat Pad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0738, 0x4556, "Mad Catz Lynx Wireless Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0738, 0x4716, "Mad Catz Wired Xbox 360 Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX360 },
	{ 0x0738, 0x6040, "Mad Catz Beat Pad Pro", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x0c12, 0x8802, "Zeroplus Xbox Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0c12, 0x880a, "Pelican Eclipse PL-2023", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0c12, 0x8810, "Zeroplus Xbox Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0c12, 0x9902, "HAMA VibraX - *FAULTY HARDWARE*", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0e4c, 0x1097, "Radica Gamester Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0e4c, 0x2390, "Radica Games Jtech Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0e6f, 0x0003, "Logic3 Freebird wireless Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0e6f, 0x0005, "Eclipse wireless Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0e6f, 0x0006, "Edge wireless Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0e6f, 0x0006, "Pelican 'TSZ' Wired Xbox 360 Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX360 },
	{ 0x0e8f, 0x0201, "SmartJoy Frag Xpad/PS2 adaptor", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0f30, 0x0202, "Joytech Advanced Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0f30, 0x8888, "BigBen XBMiniPad Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x102c, 0xff0c, "Joytech Wireless Advanced Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x12ab, 0x8809, "Xbox DDR dancepad", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x1430, 0x4748, "RedOctane Guitar Hero X-plorer", MAP_DPAD_TO_AXES, XTYPE_XBOX360 },
	{ 0x1430, 0x8888, "TX6500+ Dance Pad (first generation)", MAP_DPAD_TO_BUTTONS, XTYPE_XBOX },
	{ 0x1bad, 0xF016, "Mad Catz Wired Xbox 360 Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX360 },
	{ 0x1bad, 0xF900, "PDP Afterglow", MAP_DPAD_TO_AXES, XTYPE_XBOX360 },
	{ 0x045e, 0x028e, "Microsoft X-Box 360 pad", MAP_DPAD_TO_AXES, XTYPE_XBOX360 },
	{ 0xffff, 0xffff, "Chinese-made Xbox Controller", MAP_DPAD_TO_AXES, XTYPE_XBOX },
	{ 0x0000, 0x0000, "Generic X-Box pad", MAP_DPAD_UNKNOWN, XTYPE_UNKNOWN }
};

/* buttons shared with xbox and xbox360 */
static const signed short xpad_common_btn[] = {
	BTN_A, BTN_B, BTN_X, BTN_Y,			/* "analog" buttons */
	BTN_START, BTN_BACK, BTN_THUMBL, BTN_THUMBR,	/* start/back/sticks */
	-1						/* terminating entry */
};

/* original xbox controllers only */
static const signed short xpad_btn[] = {
	BTN_C, BTN_Z,		/* "analog" buttons */
	-1			/* terminating entry */
};

/* only used if MAP_DPAD_TO_BUTTONS */
static const signed short xpad_btn_pad[] = {
	BTN_LEFT, BTN_RIGHT,		/* d-pad left, right */
	BTN_0, BTN_1,			/* d-pad up, down (XXX names??) */
	-1				/* terminating entry */
};

static const signed short xpad360_btn[] = {  /* buttons for x360 controller */
	BTN_TL, BTN_TR,		/* Button LB/RB */
	BTN_MODE,		/* The big X button */
	-1
};

static const signed short xpad_abs[] = {
	ABS_X, ABS_Y,		/* left stick */
	ABS_RX, ABS_RY,		/* right stick */
	ABS_Z, ABS_RZ,		/* triggers left/right */
	-1			/* terminating entry */
};

/* only used if MAP_DPAD_TO_AXES */
static const signed short xpad_abs_pad[] = {
	ABS_HAT0X, ABS_HAT0Y,	/* d-pad axes */
	-1			/* terminating entry */
};

/* Xbox 360 has a vendor-specific class, so we cannot match it with only
 * USB_INTERFACE_INFO (also specifically refused by USB subsystem), so we
 * match against vendor id as well. Wired Xbox 360 devices have protocol 1,
 * wireless controllers have protocol 129. */
#define XPAD_XBOX360_VENDOR_PROTOCOL(vend,pr) \
	.match_flags = USB_DEVICE_ID_MATCH_VENDOR | USB_DEVICE_ID_MATCH_INT_INFO, \
	.idVendor = (vend), \
	.bInterfaceClass = USB_CLASS_VENDOR_SPEC, \
	.bInterfaceSubClass = 93, \
	.bInterfaceProtocol = (pr)
#define XPAD_XBOX360_VENDOR(vend) \
	{ XPAD_XBOX360_VENDOR_PROTOCOL(vend,1) }, \
	{ XPAD_XBOX360_VENDOR_PROTOCOL(vend,130) }, \
	{ XPAD_XBOX360_VENDOR_PROTOCOL(vend,129) }

static struct usb_device_id xpad_table [] = {
	{ USB_INTERFACE_INFO('X', 'B', 0) },	/* X-Box USB-IF not approved class */
	XPAD_XBOX360_VENDOR(0x045e),		/* Microsoft X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x046d),		/* Logitech X-Box 360 style controllers */
	XPAD_XBOX360_VENDOR(0x0738),		/* Mad Catz X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x0e6f),		/* 0x0e6f X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x1430),		/* RedOctane X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x1bad),		/* Mad Catz X-Box 360 controllers */
	XPAD_XBOX360_VENDOR(0x1bad),		/* Performance Designed Products(PDP) X-Box 360 controllers */
	{ }
};

MODULE_DEVICE_TABLE (usb, xpad_table);

struct usb_xpad {
	struct input_dev *dev;		/* input device interface */
	struct usb_device *udev;	/* usb device */

	int pad_present;
	const char * input_name;
	struct device * parent_dev;

	struct urb *irq_in;		/* urb for interrupt in report */
	unsigned char *idata;		/* input data */
	dma_addr_t idata_dma;

#if defined(CONFIG_JOYSTICK_XPAD_FF) || defined(CONFIG_JOYSTICK_XPAD_LEDS)
	struct urb *irq_out;		/* urb for interrupt out report */
	unsigned char *odata;		/* output data */
	dma_addr_t odata_dma;
	struct mutex odata_mutex;
#endif

#if defined(CONFIG_JOYSTICK_XPAD_LEDS)
	struct xpad_led *led;
#endif

	char phys[64];			/* physical device path */

	int dpad_mapping;		/* map d-pad to buttons or to axes */
	int xtype;			/* type of xbox device */
};

static int s_fast_device_count = 0;



#if defined(CONFIG_JOYSTICK_XPAD_LEDS)
#include <linux/leds.h>

struct xpad_led {
	char name[16];
	struct led_classdev led_cdev;
	struct usb_xpad *xpad;
};

//static inline void led_set_brightness(struct led_classdev *led_cdev,
//					enum led_brightness value)
//{
//	if (value > led_cdev->max_brightness)
//		value = led_cdev->max_brightness;
//	led_cdev->brightness = value;
//	if (!(led_cdev->flags & LED_SUSPENDED))
//		led_cdev->brightness_set(led_cdev, value);
//}
//
static void xpad_send_led_command(struct usb_xpad *xpad, int command)
{
	int error;

	printk(KERN_INFO "xpad_send_led_command %d\n", command);

	if (command >= 0 && command < 14) 
	{
		mutex_lock(&xpad->odata_mutex);
		if (xpad->xtype != XTYPE_XBOX360W) 
		{
			xpad->odata[0] = 0x01;
			xpad->odata[1] = 0x03;
			xpad->odata[2] = command;
			xpad->irq_out->transfer_buffer_length = 3;
		}
		else
		{
			xpad->odata[0] = 0x00;
			xpad->odata[1] = 0x00;
			xpad->odata[2] = 0x08;
			xpad->odata[3] = 0x40 | (command % 0x0e);
			xpad->odata[4] = 0x00;
			xpad->odata[5] = 0x00;
			xpad->odata[6] = 0x00;
			xpad->odata[7] = 0x00;
			xpad->odata[8] = 0x00;
			xpad->odata[9] = 0x00;
			xpad->odata[10] = 0x00;
			xpad->odata[11] = 0x00;
			xpad->irq_out->transfer_buffer_length = 12;
		}

		error = usb_submit_urb(xpad->irq_out, GFP_KERNEL);
		mutex_unlock(&xpad->odata_mutex);
	}
}

static void xpad_led_set(struct led_classdev *led_cdev,
			 enum led_brightness value)
{
	struct xpad_led *xpad_led = container_of(led_cdev,
						 struct xpad_led, led_cdev);


	xpad_send_led_command(xpad_led->xpad, value);
}

static int xpad_led_probe(struct usb_xpad *xpad)
{
	static atomic_t led_seq	= ATOMIC_INIT(0);
	long led_no;
	struct xpad_led *led;
	struct led_classdev *led_cdev;
	int error;
	const char *path;
	const char *path_number;

	if (xpad->xtype != XTYPE_XBOX360 && 
		xpad->xtype != XTYPE_XBOX360W )
		return 0;

	xpad->led = led = kzalloc(sizeof(struct xpad_led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	path_number = NULL;
	path = kobject_get_path(&xpad->dev->dev.kobj, GFP_KERNEL);

	/*find last number*/
	if ( path != NULL && *path != '\0' )
	{
		path_number = path;
		while( *path_number != '\0' ) 
			path_number++;
		
		path_number--;
		while( *path_number >= '0' && *path_number <= '9' && path_number != path )
			path_number--;

		path_number++;
		if ( *path_number == '\0' )
			path_number = NULL;
	}

	if (path_number == NULL)
	{
		led_no = (long)atomic_inc_return(&led_seq) - 1;
		snprintf(led->name, sizeof(led->name), "xpad%ld", led_no);
	}
	else
	{ 
		snprintf(led->name, sizeof(led->name), "xpad_ol%s", path_number);
	}
	kfree(path);

	//Makes lights flashing.
	//do this before creating device to prevent overwriting values set via dev interface
	xpad_send_led_command(xpad, 10);

	led->xpad = xpad;

	led_cdev = &led->led_cdev;
	led_cdev->name = led->name;
	led_cdev->brightness_set = xpad_led_set;

	error = led_classdev_register(&xpad->udev->dev, led_cdev);
	if (error) {
		kfree(led);
		xpad->led = NULL;
		return error;
	}

	return 0;
}

static void xpad_led_disconnect(struct usb_xpad *xpad)
{
	struct xpad_led *xpad_led = xpad->led;

	if (xpad_led) 
	{
		led_classdev_unregister(&xpad_led->led_cdev);
		kfree(xpad_led);
	}
	xpad->led = NULL;
}
#else
static int xpad_led_probe(struct usb_xpad *xpad) { return 0; }
static void xpad_led_disconnect(struct usb_xpad *xpad) { }
#endif



/*
 *	xpad_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem.
 *
 *	The used report descriptor was taken from ITO Takayukis website:
 *	 http://euc.jp/periphs/xbox-controller.ja.html
 */

static void xpad_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	struct input_dev *dev = xpad->dev;

	/* left stick */
	input_report_abs(dev, ABS_X,
			 (__s16) le16_to_cpup((__le16 *)(data + 12)));
	input_report_abs(dev, ABS_Y,
			 ~(__s16) le16_to_cpup((__le16 *)(data + 14)));

	/* right stick */
	input_report_abs(dev, ABS_RX,
			 (__s16) le16_to_cpup((__le16 *)(data + 16)));
	input_report_abs(dev, ABS_RY,
			 ~(__s16) le16_to_cpup((__le16 *)(data + 18)));

	/* triggers left/right */
	input_report_abs(dev, ABS_Z, data[10]);
	input_report_abs(dev, ABS_RZ, data[11]);

	/* digital pad */
	if (xpad->dpad_mapping == MAP_DPAD_TO_AXES) {
		input_report_abs(dev, ABS_HAT0X,
				 !!(data[2] & 0x08) - !!(data[2] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				 !!(data[2] & 0x02) - !!(data[2] & 0x01));
	} else /* xpad->dpad_mapping == MAP_DPAD_TO_BUTTONS */ {
		input_report_key(dev, BTN_LEFT,  data[2] & 0x04);
		input_report_key(dev, BTN_RIGHT, data[2] & 0x08);
		input_report_key(dev, BTN_0,     data[2] & 0x01); /* up */
		input_report_key(dev, BTN_1,     data[2] & 0x02); /* down */
	}

	/* start/back buttons and stick press left/right */
	input_report_key(dev, BTN_START,  data[2] & 0x10);
	input_report_key(dev, BTN_BACK,   data[2] & 0x20);
	input_report_key(dev, BTN_THUMBL, data[2] & 0x40);
	input_report_key(dev, BTN_THUMBR, data[2] & 0x80);

	/* "analog" buttons A, B, X, Y */
	input_report_key(dev, BTN_A, data[4]);
	input_report_key(dev, BTN_B, data[5]);
	input_report_key(dev, BTN_X, data[6]);
	input_report_key(dev, BTN_Y, data[7]);

	/* "analog" buttons black, white */
	input_report_key(dev, BTN_C, data[8]);
	input_report_key(dev, BTN_Z, data[9]);

	input_sync(dev);
}

/*
 *	xpad360_process_packet
 *
 *	Completes a request by converting the data into events for the
 *	input subsystem. It is version for xbox 360 controller
 *
 *	The used report descriptor was taken from:
 *		http://www.free60.org/wiki/Gamepad
 */

static void xpad360_process_packet(struct usb_xpad *xpad,
				   u16 cmd, unsigned char *data)
{
	struct input_dev *dev = xpad->dev;
	if (dev == NULL)
		return;

	if( data[0] != 0 && data[1] < 14 )
	{
		return; 
	}

	/* digital pad */
	if (xpad->dpad_mapping == MAP_DPAD_TO_AXES) {
		input_report_abs(dev, ABS_HAT0X,
				 !!(data[2] & 0x08) - !!(data[2] & 0x04));
		input_report_abs(dev, ABS_HAT0Y,
				 !!(data[2] & 0x02) - !!(data[2] & 0x01));
	} else if (xpad->dpad_mapping == MAP_DPAD_TO_BUTTONS) {
		/* dpad as buttons (right, left, down, up) */
		input_report_key(dev, BTN_LEFT, data[2] & 0x04);
		input_report_key(dev, BTN_RIGHT, data[2] & 0x08);
		input_report_key(dev, BTN_0, data[2] & 0x01);	/* up */
		input_report_key(dev, BTN_1, data[2] & 0x02);	/* down */
	}

	/* start/back buttons */
	input_report_key(dev, BTN_START,  data[2] & 0x10);
	input_report_key(dev, BTN_BACK,   data[2] & 0x20);

	/* stick press left/right */
	input_report_key(dev, BTN_THUMBL, data[2] & 0x40);
	input_report_key(dev, BTN_THUMBR, data[2] & 0x80);

	/* buttons A,B,X,Y,TL,TR and MODE */
	input_report_key(dev, BTN_A,	data[3] & 0x10);
	input_report_key(dev, BTN_B,	data[3] & 0x20);
	input_report_key(dev, BTN_X,	data[3] & 0x40);
	input_report_key(dev, BTN_Y,	data[3] & 0x80);
	input_report_key(dev, BTN_TL,	data[3] & 0x01);
	input_report_key(dev, BTN_TR,	data[3] & 0x02);
	input_report_key(dev, BTN_MODE,	data[3] & 0x04);

	/* left stick */
	input_report_abs(dev, ABS_X,
			 (__s16) le16_to_cpup((__le16 *)(data + 6)));
	input_report_abs(dev, ABS_Y,
			 ~(__s16) le16_to_cpup((__le16 *)(data + 8)));

	/* right stick */
	input_report_abs(dev, ABS_RX,
			 (__s16) le16_to_cpup((__le16 *)(data + 10)));
	input_report_abs(dev, ABS_RY,
			 ~(__s16) le16_to_cpup((__le16 *)(data + 12)));

	/* triggers left/right */
	input_report_abs(dev, ABS_Z, data[4]);
	input_report_abs(dev, ABS_RZ, data[5]);

	input_sync(dev);
}

/*
 * xpad360w_process_packet
 *
 * Completes a request by converting the data into events for the
 * input subsystem. It is version for xbox 360 wireless controller.
 *
 * Byte.Bit
 * 00.1 - Status change: The controller or headset has connected/disconnected
 *                       Bits 01.7 and 01.6 are valid
 * 01.7 - Controller present
 * 01.6 - Headset present
 * 01.1 - Pad state (Bytes 4+) valid
 *
 */

static int s_xpad_count = 0;
static struct usb_xpad * s_xpad_process[16];
static const int s_xpad_max_count = sizeof(s_xpad_process)/sizeof(s_xpad_process[0]);

static int allocate_xpad_input_device(struct usb_xpad * xpad);

void xpad360w_do_attach(struct work_struct *work);
static DECLARE_WORK(xpad360w_attach, xpad360w_do_attach );

void xpad360w_do_attach(struct work_struct *tmp)
{
	unsigned long flags;
	int i;
	struct usb_xpad * xpad = NULL;

	tmp = NULL;

	for(;;)
	{
		local_irq_save(flags); 
		preempt_disable(); 

		if (s_xpad_count == 0)
			xpad = NULL;
		else
		{
			xpad = s_xpad_process[0];
			for(i = 1; i < s_xpad_count; i++)
				s_xpad_process[i-1] = s_xpad_process[i];
			s_xpad_count--;
		}

		local_irq_restore(flags); 
		preempt_enable(); 

		if (xpad == NULL)
			return;

		printk(KERN_INFO "xpad360w_do_attach %p %d %p\n", xpad, xpad->pad_present, xpad->dev);

		if (xpad->pad_present)
		{
			int error;

			if (xpad->dev != NULL)
				continue;

			error = allocate_xpad_input_device(xpad);
			if (error)
			{
				err ("%s - allocate_xpad_input_device failed with result %d", __func__, error);
			}
			else
			{
				error = xpad_led_probe(xpad);
				if (error)
					err ("%s - xpad_led_probe failed with result %d", __func__, error);
			}
		}
		else
		{
			if (xpad->dev == NULL)
				continue;

			xpad_led_disconnect(xpad);
			input_unregister_device(xpad->dev);
			xpad->dev = NULL;
		}
	}
}


static void xpad360w_process_packet(struct usb_xpad *xpad, u16 cmd, unsigned char *data)
{
	int changed = 0;
	/* Presence change */
	if (data[0] & 0x08) 
	{
		if (data[1] & 0x80) 
		{
			if (xpad->pad_present != 1)
			{
				printk(KERN_INFO "attached xbox360W %d\n", xpad->pad_present);
				xpad->pad_present = 1;
				changed = 1;
			}
		} 
		else
		{
			if (xpad->pad_present != 0)
			{
				printk(KERN_INFO "detached xbox360W %d\n", xpad->pad_present);
				xpad->pad_present = 0;
				changed = 1;
			}
		}

		if (changed)
		{
			s_xpad_process[s_xpad_count++] = xpad;
			schedule_work(&xpad360w_attach);
		}
		return;

	}

	/* Valid pad data */
	if (!(data[1] & 0x1))
		return;

	xpad360_process_packet(xpad, cmd, &data[4]);
}

static void xpad_irq_in(struct urb *urb)
{
	struct usb_xpad *xpad = urb->context;
	int retval, status;

	status = urb->status;

	switch (status) {
	case 0:
		/* success */
		break;
	case -ECONNRESET:
	case -ENOENT:
	case -ESHUTDOWN:
		/* this urb is terminated, clean up */
		dbg("%s - urb shutting down with status: %d",
			__func__, status);
		return;
	default:
		dbg("%s - nonzero urb status received: %d",
			__func__, status);
		goto exit;
	}

	switch (xpad->xtype) {
	case XTYPE_XBOX360:
		xpad360_process_packet(xpad, 0, xpad->idata);
		break;
	case XTYPE_XBOX360W:
		xpad360w_process_packet(xpad, 0, xpad->idata);
		break;
	default:
		xpad_process_packet(xpad, 0, xpad->idata);
	}

exit:
	retval = usb_submit_urb (urb, GFP_ATOMIC);
	if (retval)
		err ("%s - usb_submit_urb failed with result %d",
		     __func__, retval);
}

#if defined(CONFIG_JOYSTICK_XPAD_FF) || defined(CONFIG_JOYSTICK_XPAD_LEDS)
static void xpad_irq_out(struct urb *urb)
{
	int retval, status;

	status = urb->status;

	switch (status) {
		case 0:
			/* success */
			return;
		case -ECONNRESET:
		case -ENOENT:
		case -ESHUTDOWN:
			/* this urb is terminated, clean up */
			dbg("%s - urb shutting down with status: %d",
				__func__, status);
			return;
		default:
			dbg("%s - nonzero urb status received: %d",
				__func__, status);
			goto exit;
	}

exit:
	printk(KERN_INFO "xpad_irq_out resubmit urb\n");

	//resumbit urb
	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		err("%s - usb_submit_urb failed with result %d",
		    __func__, retval);
}

static int xpad_init_output(struct usb_interface *intf, struct usb_xpad *xpad)
{
	struct usb_endpoint_descriptor *ep_irq_out;
	int error = -ENOMEM;

	xpad->led = NULL;

	if (xpad->xtype != XTYPE_XBOX360 && 
		xpad->xtype != XTYPE_XBOX360W )
		return 0;

	xpad->odata = usb_buffer_alloc(xpad->udev, XPAD_PKT_LEN,
				       GFP_KERNEL, &xpad->odata_dma);
	if (!xpad->odata)
		goto fail1;

	mutex_init(&xpad->odata_mutex);

	xpad->irq_out = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_out)
		goto fail2;

	ep_irq_out = &intf->cur_altsetting->endpoint[1].desc;
	usb_fill_int_urb(xpad->irq_out, xpad->udev,
			 usb_sndintpipe(xpad->udev, ep_irq_out->bEndpointAddress),
			 xpad->odata, XPAD_PKT_LEN,
			 xpad_irq_out, xpad, ep_irq_out->bInterval);
	xpad->irq_out->transfer_dma = xpad->odata_dma;
	xpad->irq_out->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	return 0;

 fail2:	usb_buffer_free(xpad->udev, XPAD_PKT_LEN, xpad->odata, xpad->odata_dma);
	    xpad->odata = NULL;
 fail1:	return error;
}

static void xpad_deinit_output(struct usb_xpad *xpad)
{
	if (xpad->xtype != XTYPE_XBOX360 && 
		xpad->xtype != XTYPE_XBOX360W )
		return;

	usb_kill_urb(xpad->irq_out);
	usb_free_urb(xpad->irq_out);
	usb_buffer_free(xpad->udev, XPAD_PKT_LEN,
			xpad->odata, xpad->odata_dma);
}
#else
static int xpad_init_output(struct usb_interface *intf, struct usb_xpad *xpad) { return 0; }
static void xpad_deinit_output(struct usb_xpad *xpad) {}
#endif

#ifdef CONFIG_JOYSTICK_XPAD_FF
static int xpad_play_effect(struct input_dev *dev, void *data,
			    struct ff_effect *effect)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	if (effect->type == FF_RUMBLE) {
		__u16 strong = effect->u.rumble.strong_magnitude;
		__u16 weak = effect->u.rumble.weak_magnitude;
		xpad->odata[0] = 0x00;
		xpad->odata[1] = 0x08;
		xpad->odata[2] = 0x00;
		xpad->odata[3] = strong / 256;
		xpad->odata[4] = weak / 256;
		xpad->odata[5] = 0x00;
		xpad->odata[6] = 0x00;
		xpad->odata[7] = 0x00;
		xpad->irq_out->transfer_buffer_length = 8;
		usb_submit_urb(xpad->irq_out, GFP_KERNEL);
	}

	return 0;
}

static int xpad_init_ff(struct usb_xpad *xpad)
{
	if (xpad->xtype != XTYPE_XBOX360)
		return 0;

	input_set_capability(xpad->dev, EV_FF, FF_RUMBLE);

	return input_ff_create_memless(xpad->dev, NULL, xpad_play_effect);
}

#else
static int xpad_init_ff(struct usb_xpad *xpad) { return 0; }
#endif


static int xpad_open(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	int error;

	/* URB was submitted in probe */
	if(xpad->xtype == XTYPE_XBOX360W)
		return 0;

	xpad->irq_in->dev = xpad->udev;
	error = usb_submit_urb(xpad->irq_in, GFP_KERNEL);
	if (error)
		return -EIO;

	return 0;
}

static void xpad_close(struct input_dev *dev)
{
	struct usb_xpad *xpad = input_get_drvdata(dev);

	if(xpad->xtype != XTYPE_XBOX360W)
		usb_kill_urb(xpad->irq_in);
}

static void xpad_set_up_abs(struct input_dev *input_dev, signed short abs)
{
	set_bit(abs, input_dev->absbit);

	switch (abs) {
	case ABS_X:
	case ABS_Y:
	case ABS_RX:
	case ABS_RY:	/* the two sticks */
		input_set_abs_params(input_dev, abs, -32768, 32767, 16, 128);
		break;
	case ABS_Z:
	case ABS_RZ:	/* the triggers */
		input_set_abs_params(input_dev, abs, 0, 255, 0, 0);
		break;
	case ABS_HAT0X:
	case ABS_HAT0Y:	/* the d-pad (only if MAP_DPAD_TO_AXES) */
		input_set_abs_params(input_dev, abs, -1, 1, 0, 0);
		break;
	}
}


static int allocate_xpad_input_device(struct usb_xpad * xpad)
{
	struct input_dev *input_dev;
	int error;
	int i;

	input_dev = input_allocate_device();
	if (!input_dev)
		return -ENOMEM;

	input_dev->name = xpad->input_name;
	input_dev->phys = xpad->phys;
	input_dev->uniq = xpad->phys + ( strlen(xpad->phys) - 20 ); //use the last 20 chars of phys for uniq
	usb_to_input_id(xpad->udev, &input_dev->id);
	input_dev->dev.parent = xpad->parent_dev;

	input_set_drvdata(input_dev, xpad);

	input_dev->open = xpad_open;
	input_dev->close = xpad_close;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS);

	/* set up buttons */
	for (i = 0; xpad_common_btn[i] >= 0; i++)
		set_bit(xpad_common_btn[i], input_dev->keybit);
	if ((xpad->xtype == XTYPE_XBOX360) || (xpad->xtype == XTYPE_XBOX360W))
		for (i = 0; xpad360_btn[i] >= 0; i++)
			set_bit(xpad360_btn[i], input_dev->keybit);
	else
		for (i = 0; xpad_btn[i] >= 0; i++)
			set_bit(xpad_btn[i], input_dev->keybit);
	if (xpad->dpad_mapping == MAP_DPAD_TO_BUTTONS)
		for (i = 0; xpad_btn_pad[i] >= 0; i++)
			set_bit(xpad_btn_pad[i], input_dev->keybit);

	/* set up axes */
	for (i = 0; xpad_abs[i] >= 0; i++)
		xpad_set_up_abs(input_dev, xpad_abs[i]);
	if (xpad->dpad_mapping == MAP_DPAD_TO_AXES)
		for (i = 0; xpad_abs_pad[i] >= 0; i++)
		    xpad_set_up_abs(input_dev, xpad_abs_pad[i]);

	error = input_register_device(input_dev);
	if (error)
	{
		err ("%s - input_register_device failed with result %d", __func__, error);
		input_free_device(input_dev);
		xpad->dev = NULL;
	}
	else
	{
		xpad->dev = input_dev;
	}

	return error;
}

static int xpad_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_xpad *xpad;
	struct usb_endpoint_descriptor *ep_irq_in;
	int i;
	int error = -ENOMEM;
	int interval;

	//skip not used interface
	if (intf->cur_altsetting->desc.bInterfaceProtocol == 130)
		return 0;

	for (i = 0; xpad_device[i].idVendor; i++) {
		if ((le16_to_cpu(udev->descriptor.idVendor) == xpad_device[i].idVendor) &&
		    (le16_to_cpu(udev->descriptor.idProduct) == xpad_device[i].idProduct))
			break;
	}


	if ( s_fast_device_count >= 8 ) //do not allow to many fast devices
	{
		err ("%s - too many xbox devices, ignoring... %d", __func__, s_fast_device_count);
		return 0;
	}

	xpad = kzalloc(sizeof(struct usb_xpad), GFP_KERNEL);
	if (!xpad)
		goto fail1;

	xpad->dev = NULL;
	xpad->idata = usb_buffer_alloc(udev, XPAD_PKT_LEN,
				       GFP_KERNEL, &xpad->idata_dma);
	if (!xpad->idata)
		goto fail2;

	xpad->irq_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!xpad->irq_in)
		goto fail3;

	xpad->udev = udev;
	xpad->dpad_mapping = xpad_device[i].dpad_mapping;
	xpad->xtype = xpad_device[i].xtype;
	if (xpad->dpad_mapping == MAP_DPAD_UNKNOWN)
		xpad->dpad_mapping = !dpad_to_buttons;
	if (xpad->xtype == XTYPE_UNKNOWN) {
		if (intf->cur_altsetting->desc.bInterfaceClass == USB_CLASS_VENDOR_SPEC) 
		{
			if (intf->cur_altsetting->desc.bInterfaceProtocol == 129)
				xpad->xtype = XTYPE_XBOX360W;
			else
				xpad->xtype = XTYPE_XBOX360;
		} else
			xpad->xtype = XTYPE_XBOX;
	}
	usb_make_path(udev, xpad->phys, sizeof(xpad->phys));
	strlcat(xpad->phys, "/input0", sizeof(xpad->phys));

	xpad->input_name = xpad_device[i].name;
	xpad->parent_dev = &intf->dev;

	error = xpad_init_output(intf, xpad);
	if (error)
		goto fail4;

	error = xpad_init_ff(xpad);
	if (error)
		goto fail5;

	ep_irq_in = &intf->cur_altsetting->endpoint[0].desc;

	interval = ep_irq_in->bInterval;
	if (interval <= 4)
	{
		interval = 4; //do not use intervals less then 4ms
	}

	usb_fill_int_urb(xpad->irq_in, udev,
			 usb_rcvintpipe(udev, ep_irq_in->bEndpointAddress),
			 xpad->idata, XPAD_PKT_LEN, xpad_irq_in,
			 xpad, interval );
	xpad->irq_in->transfer_dma = xpad->idata_dma;
	xpad->irq_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	if (xpad->xtype == XTYPE_XBOX360W) 
	{
		usb_set_intfdata(intf, xpad);

		/*
		 * Submit the int URB immediatly rather than waiting for open
		 * because we get status messages from the device whether
		 * or not any controllers are attached.  In fact, it's
		 * exactly the message that a controller has arrived that
		 * we're waiting for.
		 */

		xpad->irq_in->dev = xpad->udev;
		error = usb_submit_urb(xpad->irq_in, GFP_KERNEL);
		if (error)
		{
			err ("%s - usb_submit_urb failed with result %d",
				 __func__, error);
			goto fail5;
		}
	}
	else
	{
		error = allocate_xpad_input_device(xpad);
		if (error)
			goto fail5;

		error = xpad_led_probe(xpad);
		if (error)
			goto fail5;

		usb_set_intfdata(intf, xpad);
	}
	s_fast_device_count++;
	return 0;

 fail5:	xpad_deinit_output(xpad);
 fail4:	usb_free_urb(xpad->irq_in);
 fail3:	usb_buffer_free(udev, XPAD_PKT_LEN, xpad->idata, xpad->idata_dma);
 fail2:	kfree(xpad);
 fail1:	
	return error;

}

static void xpad_disconnect(struct usb_interface *intf)
{
	struct usb_xpad *xpad = usb_get_intfdata (intf);

	usb_set_intfdata(intf, NULL);
	if (xpad) 
	{

		xpad_led_disconnect(xpad);
		if (xpad->dev != NULL)
			input_unregister_device(xpad->dev);
		xpad->dev = NULL;
		xpad_deinit_output(xpad);

		//technically we should not call kill_urb - should be canceled by driver
		if (xpad->xtype == XTYPE_XBOX360W) 
		{
			usb_kill_urb(xpad->irq_in);
		}
		usb_free_urb(xpad->irq_in);
		usb_buffer_free(xpad->udev, XPAD_PKT_LEN,
				xpad->idata, xpad->idata_dma);
		kfree(xpad);

		s_fast_device_count--;
	}
}

static struct usb_driver xpad_driver = {
	.name		= "xpad",
	.probe		= xpad_probe,
	.disconnect	= xpad_disconnect,
	.id_table	= xpad_table,
};

static int __init usb_xpad_init(void)
{
	int result = usb_register(&xpad_driver);
	if (result == 0)
		printk(KERN_INFO KBUILD_MODNAME ": " DRIVER_DESC "\n");
	return result;
}

static void __exit usb_xpad_exit(void)
{
	usb_deregister(&xpad_driver);
}

module_init(usb_xpad_init);
module_exit(usb_xpad_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
