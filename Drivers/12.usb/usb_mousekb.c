#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb/input.h>
#include <linux/hid.h>			//hid - human interface device
#include <linux/err.h>

static struct input_dev *uk_dev;
static char *usb_buf;
static dma_addr_t usb_buf_phys;
static int len;
static struct urb *uk_urb;

static struct usb_device_id usb_mousekb_table[] = {
	{USB_INTERFACE_INFO(USB_INTERFACE_CLASS_HID, USB_INTERFACE_SUBCLASS_BOOT, USB_INTERFACE_PROTOCOL_MOUSE)},
	{}
};

/*--------------------------------------------------*/
static void usb_mousekb_irq(struct urb *urb)
{
	static unsigned char pre_val = 0;
	
	/*compare previous value with current value*/
	if((pre_val & (1<<0)) != (usb_buf[0] & (1<<0)))
		{
			input_event(uk_dev, EV_KEY, KEY_L, (usb_buf[0] & (1<<0)) ? 1 : 0);
			input_sync(uk_dev);
		}
	
	if((pre_val & (1<<1)) != (usb_buf[0] & (1<<1)))
		{
			input_event(uk_dev, EV_KEY, KEY_S, (usb_buf[0] & (1<<1)) ? 1 : 0);
			input_sync(uk_dev);
		}
	
	if((pre_val & (1<<2)) != (usb_buf[0] & (1<<2)))
		{
			input_event(uk_dev, EV_KEY, KEY_L, (usb_buf[0] & (1<<2)) ? 1 : 0);
			input_sync(uk_dev);
		}
		
	pre_val = usb_buf[0];
	
	usb_submit_urb(uk_urb, GFP_KERNEL);
}
/*--------------------------------------------------*/
static int usb_mousekb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *endpoint;
	int pipe;
	
	interface = intf->cur_altsetting;
	endpoint = &interface->endpoint[0].desc;
	
	/*allocate memory for our input device*/
	uk_dev = input_allocate_device();
	
	/*set up the input event*/
	set_bit(EV_KEY, uk_dev->evbit);
	set_bit(EV_REP, uk_dev->evbit);
	
	set_bit(KEY_L, uk_dev->keybit);
	set_bit(KEY_S, uk_dev->keybit);
	set_bit(KEY_ENTER, uk_dev->keybit);
	
	/*register input_dev*/
	input_register_device(uk_dev);
	
	/*create a urb, and a buffer for it, and initialize it*/
	len = endpoint->wMaxPacketSize;
	usb_buf = usb_buffer_alloc(dev, len, GFP_ATOMIC, &usb_buf_phys);
	
	uk_urb = usb_alloc_urb(0, GFP_KERNEL);
	
	usb_fill_int_urb(uk_urb, dev, pipe, usb_buf, len, usb_mousekb_irq, NULL, endpoint->bInterval);
	uk_urb->transfer_dma = usb_buf_phys;
	uk_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	
	/*submit our urb to USB core*/
	usb_submit_urb(uk_urb, GFP_KERNEL);
	
	return 0;
}

static void usbmousekb_disconnect(struct usb_interface *intf)
{
	struct usb_device *dev = interface_to_usbdev(intf);
	
	usb_kill_urb(uk_urb);
	usb_free_urb(uk_urb);
	
	usb_buffer_free(dev, len, usb_buf, usb_buf_phys);
	input_unregister_device(uk_dev);
	input_free_device(uk_dev);
}

static struct usb_driver usb_mousekb_driver = {
	.name = "usb_mousekb",
	.probe = usb_mousekb_probe,
	.disconnect = usbmousekb_disconnect,
	.id_table = usb_mousekb_table,
};

/*--------------------------------------------------*/
static int __init usb_mousekb_init(void)
{
	int result;
	result = usb_register(&usb_mousekb_driver);
	
	if(result)
		{
			err("usb_register failed: error number %d", result);
		}
	
	return 0;
}

static void __exit usb_mousekb_exit(void)
{
	usb_deregister(&usb_mousekb_driver);
}

module_init(usb_mousekb_init);
module_exit(usb_mousekb_exit);
MODULE_LICENSE("GPL");
