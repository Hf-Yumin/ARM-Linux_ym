//中断方式的按键驱动程序

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <asm/uaccess.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/arch/regs-gpio.h>
#include <asm/hardware.h>

static struct class *thirddrv_class;
static struct class_device *thirddrv_class_dev;
	
static DECLARE_WAIT_QUEUE_HEAD(button_waitq);
static volatile int ev_press = 0;

struct pin_desc{
	unsigned int pin;
	unsigned int key_val;
};

struct pin_desc pins_desc[3] = {
	{S3C2410_GPF0, 0x01},
	{S3C2410_GPF2, 0x02},
	{S3C2410_GPG3, 0x03},
};

unsigned char keyval;

static irqreturn_t buttons_irq(int irq, void *dev_id)
{
	struct pin_desc *pindesc = (struct pin_desc *)dev_id;
	unsigned int pinval;
	pinval = s3c2410_gpio_getpin(pindesc -> pin);
	
	if(pinval)
		{
			keyval = 0x80 | pindesc -> key_val;
		}
	else
		{
			keyval = pindesc -> key_val;
		}
	
	ev_press = 1;
	wake_up_interruptible(&button_waitq);					// 唤醒注册到等待队列上的进程
	
	return IRQ_RETVAL(IRQ_HANDLED);
}

static int third_drv_open(struct inode *inode, struct file *file)
{
	request_irq(IRQ_EINT0, buttons_irq, IRQT_BOTHEDGE, "S1", &pins_desc[0]);
	request_irq(IRQ_EINT2, buttons_irq, IRQT_BOTHEDGE, "S2", &pins_desc[1]);
	request_irq(IRQ_EINT11, buttons_irq, IRQT_BOTHEDGE, "S3", &pins_desc[2]);
	return 0;
}

ssize_t third_drv_read(struct file *file, char __user *buf, size_t size, loff_t *opps)
{
	if(size != 1)
		return -EINVAL;
	
	wait_event_interruptible(button_waitq, ev_press);			//当应用层的read函数 最终映射到 驱动层的drv_read函数， 判断ev_press这个值：若为0，则休眠进程
																								//而只有当中断发生时，进程的休眠才被解除，继续执行之后的代码
	copy_to_user(buf, &keyval, 1);											//驱动层的程序总是“被动地”等待调用
	ev_press = 0;
	
	return 1;					//return sizeof(keyval);
}

int third_drv_close(struct inode *inode, struct file *file)
{
	free_irq(IRQ_EINT0, &pins_desc[0]);
	free_irq(IRQ_EINT2, &pins_desc[1]);
	free_irq(IRQ_EINT11, &pins_desc[2]);
	return 0;
}

static struct file_operations third_drv_fops = {
	.owner		=		THIS_MODULE,
	.open		=		third_drv_open,
	.read		=		third_drv_read,
	.release		=		third_drv_close,
};

int major;
static int third_drv_init(void)
{
	major = register_chrdev(0, "third_drv", &third_drv_fops);
	
	thirddrv_class = class_create(THIS_MODULE, "third_drv");
	thirddrv_class_dev = class_device_create(thirddrv_class, NULL, MKDEV(major,0), NULL, "buttons");
	
	return 0;
}

static void third_drv_exit(void)
{
	unregister_chrdev(major, "third_drv");
	
	class_device_unregister(thirddrv_class_dev);
	class_destroy(thirddrv_class);
	
}

module_init(third_drv_init);
module_exit(third_drv_exit);

MODULE_LICENSE("GPL");
