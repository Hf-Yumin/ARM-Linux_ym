#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/init.h>
#include <linux/serio.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <asm/io.h>
#include <asm/irq.h>

#include <asm/plat-s3c24xx/ts.h>

#include <asm/arch/regs-adc.h>
#include <asm/arch/regs-gpio.h>

struct s3c_ts_regs{
	unsigned long adccon;
	unsigned long adctsc;
	unsigned long adcdly;
	unsigned long adcdat0;
	unsigned long adcdat1;
	unsigned long adcupdn;
};

static volatile struct s3c_ts_regs *s3c_ts_regs;

static struct input_dev *s3c_ts_dev;
static struct timer_list ts_timer;		//不加*
static struct clk *clk;
/*****************************************************/

static irqreturn_t pen_down_up_irq(int irq, void *dev_id);
static irqreturn_t adc_irq(int irq, void *dev_id);
static void s3c_ts_timer_function(unsigned long data);
static int pen_is_up(void);
static void pen_up_func(void);
static void pen_down_func(void);
static void enter_wait_pen_down_mode(void);
static void enter_wait_pen_up_mode(void);
static void enter_measure_xy_mode(void);
static void start_adc(void);
static int s3c_filter_ts(int x[], int y[]);

/*****************************************************/

/* UP：上报事件；DOWN：测量-开启ADC*/
//static void pen_down_up_func(void)
//{
//	if(s3c_ts_regs->adcdat0 & (1<<15))
//		{
//			printk("pen up\n");
//			input_report_abs(s3c_ts_dev, ABS_PRESSURE, 0);
//			input_report_key(s3c_ts_sev, BTN_TOUCH, 0);
//			input_sync(s3c_ts_dev);
//			enter_wait_pen_down_mode();
//		}
//	else
//		{
//			printk("pen down\n");
//			enter_measure_xy_mode();
//			satrt_adc();
//		}
//}

/*触摸动作中断服务：开启ADC*/
static irqreturn_t pen_down_up_irq(int irq, void *dev_id)
{
	if(pen_is_up())			//ret
		{
			pen_up_func();
		}
	else
		{
			pen_down_func();
		}
	return IRQ_HANDLED;
}

/*ADC中断服务：上报测量值-启用定时器*/
/*对AD转换值的处理：
	1.转换之后若pen_up，则丢弃数值，等待下一次pen_down
	2.（pen_down的情况下）重复测量4次取平均值，上报：input_report_abs/key
	3.完成之后等待pen_up，启用定时器
	[adctsc-bit8:将检测up/down中断, adcdat0-bit15:检测到up/down；
		若设定将检测的是up，将不可能检测到down]
	4.优化：软件过滤——在上报之前分析平均值
*/
static irqreturn_t adc_irq(int irq, void *dev_id)
{
	static int cnt = 0;
	static int x[4], y[4];			//静态局部变量
	int adcdat0, adcdat1;
	
	adcdat0 = s3c_ts_regs->adcdat0;
	adcdat1 = s3c_ts_regs->adcdat1;
	
	if(pen_is_up())
		{
			cnt = 0;
			pen_up_func();
		}
	else
		{
			x[cnt] = adcdat0 & 0x3ff;
			y[cnt] = adcdat1 & 0x3ff;
			++cnt;
			if(cnt == 4)
				{
					cnt = 0;
					if(s3c_filter_ts(x, y))
						{
							input_report_abs(s3c_ts_dev, ABS_X, (x[0] + x[1] + x[2] + x[3]) / 4);
							input_report_abs(s3c_ts_dev, ABS_Y, (y[0] + y[1] + y[2] + y[3]) / 4);
							input_report_abs(s3c_ts_dev, ABS_PRESSURE, 1);
							input_report_key(s3c_ts_dev, BTN_TOUCH, 1);
							input_sync(s3c_ts_dev);
						}
					enter_wait_pen_up_mode();
					mod_timer(&ts_timer, jiffies + HZ/100);
				}
			else
				{
					pen_down_func();
				}
		}
	return IRQ_HANDLED;
}

/*调用这个函数的情况下（计时时间到），只要不是pen_up状态，都去测量新值
	不管是否停留在原来的点*/
static void s3c_ts_timer_function(unsigned long data)			//??传入的参数没使用
{
			if(pen_is_up())
		{
			pen_up_func();
		}
	else
		{
			pen_down_func();
		}
}

static int pen_is_up(void)
{
	return (s3c_ts_regs->adcdat0 & (1<<15)) ? 1 : 0;
}

static void pen_up_func(void)
{
	printk("pen up\n");
	input_report_abs(s3c_ts_dev, ABS_PRESSURE, 0);
	input_report_key(s3c_ts_dev, BTN_TOUCH, 0);
	input_sync(s3c_ts_dev);
	enter_wait_pen_down_mode();
}

static void pen_down_func(void)
{
	enter_measure_xy_mode();
	start_adc();
}

/* “模式” */
/*平时触摸屏没有被按下或按下未抬起时，XP上拉，XM为高阻态
	ADCTSC-bit[8:0]:0b 1101 0011; bit9用来区别等待的是up/down */
static void enter_wait_pen_down_mode(void)
{
	s3c_ts_regs->adctsc = 0xd3;
}

static void enter_wait_pen_up_mode(void)
{
	s3c_ts_regs->adctsc = 0x1d3;
}

/*测量的情况下，XP禁止上拉，并使用自动模式
	bit3-1;bit2-1*/
static void enter_measure_xy_mode(void)
{
	s3c_ts_regs->adctsc = (1<<3) | (1<<2);	//直接赋值，改变整个寄存器
}

/*ADCCON-bit0: ENABLE_START*/
static void start_adc(void)
{
	s3c_ts_regs->adccon |= (1<<0);			//位操作，不改变整个寄存器值
}

static int s3c_filter_ts(int x[], int y[])
{
	#define ERR_LIMIT 10
	
	int avr_x, avr_y;
	int det_x, det_y;
	
	avr_x = (x[0] + x[1])/2;
	avr_y = (y[0] + y[1])/2;
	
	det_x = (x[2] >avr_x) ? (x[2] - avr_x) : (avr_x - x[2]);
	det_y = (y[2] >avr_y) ? (y[2] - avr_y) : (avr_y - y[2]);
	
	if((det_x > ERR_LIMIT) || (det_y > ERR_LIMIT))
		return 0;
		
	avr_x = (x[1] + x[2])/2;
	avr_y = (y[1] + y[2])/2;
	
	det_x = (x[3] >avr_x) ? (x[3] - avr_x) : (avr_x - x[3]);
	det_y = (y[3] >avr_y) ? (y[3] - avr_y) : (avr_y - y[3]);
	
	if((det_x > ERR_LIMIT) || (det_y > ERR_LIMIT))
		return 0;
		
	return 1;
}

static int __init s3c_ts_init(void)
{
	s3c_ts_dev = input_allocate_device();			//allocate (no alloc)
	
	//设置输入类型：按键，绝对位置
	set_bit(EV_KEY, s3c_ts_dev->evbit);
	set_bit(EV_ABS, s3c_ts_dev->evbit);
	
	//设置输入类型下的具体参数
	set_bit(BTN_TOUCH, s3c_ts_dev->keybit);
	
	input_set_abs_params(s3c_ts_dev, ABS_X, 0, 0x3FF, 0, 0);
	input_set_abs_params(s3c_ts_dev, ABS_Y, 0, 0x3FF, 0, 0);
	input_set_abs_params(s3c_ts_dev, ABS_PRESSURE, 0, 1, 0, 0);
	
	input_register_device(s3c_ts_dev);
	
	//硬件操作
	//1.时钟
	clk = clk_get(NULL, "adc");
	clk_enable(clk);
	
	//2.ADC/TS 寄存器
	s3c_ts_regs = ioremap(0x58000000, sizeof(struct s3c_ts_regs));
	
	s3c_ts_regs->adccon |= (1<<14) | (49<<6);		//使能预分频 - 设置预分频系数
	s3c_ts_regs->adcdly = 0xffff;
	
	//3.申请中断
	request_irq(IRQ_TC, pen_down_up_irq, IRQF_SAMPLE_RANDOM, "ts_pen", NULL);
	request_irq(IRQ_ADC, adc_irq, IRQF_SAMPLE_RANDOM, "adc", NULL);
	
	//4.定时器的加入
	init_timer(&ts_timer);
	ts_timer.function = s3c_ts_timer_function;
	add_timer(&ts_timer);
	
	//进入“待按下”状态(设置ADCTSC:等待按下-XM高阻-等待中断模式)
	enter_wait_pen_down_mode();
	
	return 0;
}

static void __exit s3c_ts_exit(void)
{
	del_timer(&ts_timer);
	free_irq(IRQ_TC, NULL);
	free_irq(IRQ_ADC, NULL);
	iounmap(s3c_ts_regs);
	
	if(clk)
		{
			clk_disable(clk);
			clk_put(clk);	
			clk = NULL;		
		}

	input_unregister_device(s3c_ts_dev);
	input_free_device(s3c_ts_dev);
}

module_init(s3c_ts_init);
module_exit(s3c_ts_exit);
MODULE_LICENSE("GPL");
