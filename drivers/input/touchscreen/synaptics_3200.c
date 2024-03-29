/* drivers/input/touchscreen/synaptics_3200.c - Synaptics 3200 serious touch panel driver
 *
 * Copyright (C) 2011 HTC Corporation.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/earlysuspend.h>
#include <linux/hrtimer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/synaptics_i2c_rmi.h>
#include <linux/slab.h>
#include <linux/rmi.h>
#include <mach/board.h>
#include <mach/msm_hsusb.h>
#include <asm/gpio.h>
#include <linux/input/mt.h>
#include <linux/pl_sensor.h>
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
#include <linux/ctype.h>
#endif
#define SYN_I2C_RETRY_TIMES 10
#define SYN_UPDATE_RETRY_TIMES 5
#define SHIFT_BITS 10
#define SYN_WIRELESS_DEBUG
#define SYN_CALIBRATION_CONTROL

struct synaptics_ts_data {
	uint16_t addr;
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct input_dev *sr_input_dev;
	struct workqueue_struct *syn_wq;
	struct function_t *address_table;
	int use_irq;
	int gpio_irq;
	int gpio_reset;
	struct hrtimer timer;
	struct work_struct  work;
	uint16_t max[2];
	uint32_t flags;
	uint8_t num_function;
	uint8_t finger_support;
	uint16_t finger_pressed;
	int (*power)(int on);
	struct early_suspend early_suspend;
	int pre_finger_data[11][4];
	uint32_t debug_log_level;
	uint32_t raw_base;
	uint32_t raw_ref;
	uint64_t timestamp;
	uint16_t *filter_level;
	uint8_t *reduce_report_level;
	unsigned long tap_timeout[10];
	int16_t *report_data;
	uint8_t *temp_report_data;
	uint8_t grip_suppression;
	uint8_t grip_b_suppression;
	uint16_t tap_suppression;
	uint8_t ambiguous_state;
	uint8_t diag_command;
	uint8_t cable_support;
	uint8_t cable_config;
	uint8_t key_number;
	uint16_t key_postion_x[4];
	uint16_t key_postion_y;
	uint8_t intr_bit;
	uint8_t finger_count;
	uint8_t page_select;
	uint8_t config_table[SYN_CONFIG_SIZE];
	uint8_t x_channel;
	uint8_t y_channel;
	uint8_t *config;
	uint32_t config_version;
	uint16_t package_id;
	uint32_t packrat_number;
	int layout[4];
	uint8_t htc_event;
	atomic_t data_ready;
	uint8_t relaxation;
	uint8_t irq_enabled;
	uint8_t large_obj_check;
	uint8_t default_large_obj;
	uint16_t tw_vendor;
	uint16_t tw_pin_mask;
	uint8_t support_htc_event;
	uint8_t mfg_flag;
	uint8_t first_pressed;
	uint8_t segmentation_bef_unlock;
	uint8_t segmentation_aft_unlock;
	uint8_t psensor_status;
	uint8_t i2c_err_handler_en;
	uint8_t threshold_bef_unlock;
	uint8_t threshold_aft_unlock;
	uint16_t saturation_bef_unlock;
	uint16_t saturation_aft_unlock;
	uint8_t energy_ratio_relaxation;
	uint8_t multitouch_calibration;
	uint32_t width_factor;
	uint32_t height_factor;
	uint8_t psensor_detection;
	uint8_t psensor_resume_enable;
	uint8_t psensor_phone_enable;
	uint8_t PixelTouchThreshold_bef_unlock;
	uint8_t PixelTouchThreshold_aft_unlock;
	struct work_struct  psensor_work;
	struct workqueue_struct *syn_psensor_wq;
	struct synaptics_virtual_key *button;
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void synaptics_ts_early_suspend(struct early_suspend *h);
static void synaptics_ts_late_resume(struct early_suspend *h);
#endif

static DECLARE_WAIT_QUEUE_HEAD(syn_data_ready_wq);
static DEFINE_MUTEX(syn_mutex);

static struct synaptics_ts_data *gl_ts;
static uint16_t syn_panel_version;
static uint8_t vk_press;

static int i2c_syn_write_byte_data(struct i2c_client *client, uint16_t addr, uint8_t value);
static int syn_pdt_scan(struct synaptics_ts_data *ts, int num_page);
static int synaptics_init_panel(struct synaptics_ts_data *ts);

static irqreturn_t synaptics_irq_thread(int irq, void *ptr);

bool scr_suspended = false;

extern uint8_t touchscreen_is_on(void)
{
  if (scr_suspended == false)
  {
    return 1;
  }
  return 0;
} 

#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
int s2w_switch = 1;
bool exec_count = true;
bool scr_on_touch = false, led_exec_count = false, barrier[2] = {false, false};
static struct input_dev * sweep2wake_pwrdev;
static struct led_classdev * sweep2wake_leddev;
static DEFINE_MUTEX(pwrlock);

#ifdef CONFIG_CMDLINE_OPTIONS
static int __init synaptics_read_s2w_cmdline(char *s2w)
{
	if (strcmp(s2w, "2") == 0) {
		printk(KERN_INFO "[cmdline_s2w]: Sweep2Wake enabled. ( With button backlight) | s2w='%s'", s2w);
		s2w_switch = 2;
	} else if (strcmp(s2w, "1") == 0) {
		printk(KERN_INFO "[cmdline_s2w]: Sweep2Wake enabled. | s2w='%s'", s2w);
		s2w_switch = 1;
	} else if (strcmp(s2w, "0") == 0) {
		printk(KERN_INFO "[cmdline_s2w]: Sweep2Wake disabled. | s2w='%s'", s2w);
		s2w_switch = 0;
	} else {
		printk(KERN_INFO "[cmdline_s2w]: No valid input found. Sweep2Wake disabled. | s2w='%s'", s2w);
		s2w_switch = 0;
	}
	return 1;
}
__setup("s2w=", synaptics_read_s2w_cmdline);
#endif

extern void sweep2wake_setdev(struct input_dev * input_device) {
	sweep2wake_pwrdev = input_device;
	return;
}
EXPORT_SYMBOL(sweep2wake_setdev);

extern void sweep2wake_setleddev(struct led_classdev * led_dev) {
	sweep2wake_leddev = led_dev;
	return;
}
EXPORT_SYMBOL(sweep2wake_setleddev);

static void sweep2wake_presspwr(struct work_struct * sweep2wake_presspwr_work) {
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(100);
	input_event(sweep2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(sweep2wake_pwrdev, EV_SYN, 0, 0);
	msleep(100);
	mutex_unlock(&pwrlock);
	return;
}
static DECLARE_WORK(sweep2wake_presspwr_work, sweep2wake_presspwr);

void sweep2wake_pwrtrigger(void) {
	if (mutex_trylock(&pwrlock)) {
		schedule_work(&sweep2wake_presspwr_work);
	}
	return;
}
#endif

extern unsigned int get_tamper_sf(void);

static void syn_page_select(struct i2c_client *client, uint8_t page)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	if (page ^ ts->page_select) {
		i2c_smbus_write_byte_data(client, 0xFF, page);
		ts->page_select = page;
	}

}

static int i2c_syn_read(struct i2c_client *client, uint16_t addr, uint8_t *data, uint16_t length)
{
	uint8_t retry, buf;

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = 1,
			.buf = &buf,
		},
		{
			.addr = client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		}
	};
	buf = addr & 0xFF;

	mutex_lock(&syn_mutex);
	syn_page_select(client, addr >> 8);
	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msg, 2) == 2)
			break;
		msleep(10);
	}
	mutex_unlock(&syn_mutex);

	if (retry == SYN_I2C_RETRY_TIMES) {
		pr_info("[TP] i2c_read retry over %d\n",
			SYN_I2C_RETRY_TIMES);
		return -EIO;
	}

	return 0;
}

static int i2c_syn_write(struct i2c_client *client, uint16_t addr, uint8_t *data, uint16_t length)
{
	uint8_t retry;
	uint8_t buf[length + 1];

	struct i2c_msg msg[] = {
		{
			.addr = client->addr,
			.flags = 0,
			.len = length + 1,
			.buf = buf,
		}
	};

	mutex_lock(&syn_mutex);
	syn_page_select(client, addr >> 8);

	buf[0] = addr & 0xFF;
	memcpy(&buf[1], &data[0], length);

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(client->adapter, msg, 1) == 1)
			break;
		mdelay(10);
	}
	mutex_unlock(&syn_mutex);

	if (retry == SYN_I2C_RETRY_TIMES) {
		pr_info("[TP] i2c_write retry over %d\n",
			SYN_I2C_RETRY_TIMES);
		return -EIO;
	}

	return 0;
}

int i2c_rmi_read(uint16_t addr, uint8_t *data, uint16_t length)
{
	uint8_t retry, buf;
	struct synaptics_ts_data *ts = gl_ts;
	struct i2c_msg msg[] = {
		{
			.addr = ts->client->addr,
			.flags = 0,
			.len = 1,
			.buf = &buf,
		},
		{
			.addr = ts->client->addr,
			.flags = I2C_M_RD,
			.len = length,
			.buf = data,
		}
	};
	buf = addr & 0xFF;

	mutex_lock(&syn_mutex);
	syn_page_select(ts->client, addr >> 8);
	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(ts->client->adapter, msg, 2) == 2)
			break;
		msleep(10);
	}
	mutex_unlock(&syn_mutex);

	if (retry == SYN_I2C_RETRY_TIMES) {
		pr_info("[TP] i2c_read retry over %d\n",
			SYN_I2C_RETRY_TIMES);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(i2c_rmi_read);

int i2c_rmi_write(uint16_t addr, uint8_t *data, uint16_t length)
{
	uint8_t retry;
	uint8_t buf[length + 1];
	struct synaptics_ts_data *ts = gl_ts;
	struct i2c_msg msg[] = {
		{
			.addr = ts->client->addr,
			.flags = 0,
			.len = length + 1,
			.buf = buf,
		}
	};

	mutex_lock(&syn_mutex);
	syn_page_select(ts->client, addr >> 8);

	buf[0] = addr & 0xFF;
	memcpy(&buf[1], &data[0], length);

	for (retry = 0; retry < SYN_I2C_RETRY_TIMES; retry++) {
		if (i2c_transfer(ts->client->adapter, msg, 1) == 1)
			break;
		mdelay(10);
	}
	mutex_unlock(&syn_mutex);

	if (retry == SYN_I2C_RETRY_TIMES) {
		pr_info("[TP] i2c_write retry over %d\n",
			SYN_I2C_RETRY_TIMES);
		return -EIO;
	}

	return 0;
}
EXPORT_SYMBOL(i2c_rmi_write);

static int i2c_syn_write_byte_data(struct i2c_client *client, uint16_t addr, uint8_t value)
{
	return i2c_syn_write(client, addr, &value, 1);
}

static int i2c_syn_error_handler(struct synaptics_ts_data *ts, uint8_t reset, char *reason, const char *fun_name)
{
	int ret;

	if (reason && fun_name)
		printk(KERN_ERR "[TP] TOUCH_ERR: I2C Error: %s:%s, reset = %d\n", fun_name, reason, reset);
	else
		pr_info("[TP] %s: rason and fun_name can't be null\n", __func__);

	if (reset) {
		if (ts->power) {
			ret = ts->power(0);
			if (ret < 0)
				printk(KERN_ERR "[TP] TOUCH_ERR: synaptics i2c error handler power off failed\n");
			msleep(10);
			ret = ts->power(1);
			if (ret < 0)
				printk(KERN_ERR "[TP] TOUCH_ERR: synaptics i2c error handler power on failed\n");
			ret = synaptics_init_panel(ts);
			if (ret < 0)
				printk(KERN_ERR "[TP] TOUCH_ERR: synaptics i2c error handler init panel failed\n");
		} else if (ts->gpio_reset) {
			gpio_direction_output(ts->gpio_reset, 0);
			msleep(1);
			gpio_direction_output(ts->gpio_reset, 1);
			pr_info("[TP] %s: synaptics touch chip reseted.\n", __func__);
		}

		if (!ts->use_irq) {
			hrtimer_cancel(&ts->timer);
			hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
		}
	}

	return -EIO;
}

static int get_address_base(struct synaptics_ts_data *ts, uint8_t command, uint8_t type)
{
	uint8_t i;
	for (i = 0; i < ts->num_function; i++) {
		if (ts->address_table[i].function_type == command) {
			switch (type) {
			case QUERY_BASE:
				return ts->address_table[i].query_base;
			case COMMAND_BASE:
				return ts->address_table[i].command_base;
			case CONTROL_BASE:
				return ts->address_table[i].control_base;
			case DATA_BASE:
				return ts->address_table[i].data_base;
			case INTR_SOURCE:
				return ts->address_table[i].interrupt_source;
			case FUNCTION:
				return 1;
			}
		}
	}
	if (type == FUNCTION)
		return 0;
	else
		return -1;
}
static int get_int_mask(uint8_t number, uint8_t offset)
{
	uint8_t i, mask = 0;
	for (i = 0; i < number; i++)
		mask |= BIT(i);
	return mask << offset;
}

static uint32_t syn_crc(uint16_t *data, uint16_t len)
{
	uint32_t sum1, sum2;
	sum1 = sum2 = 0xFFFF;
	while (len--) {
		sum1 += *data++;
		sum2 += sum1;
		sum1 = (sum1 & 0xFFFF) + (sum1 >> 16);
		sum2 = (sum2 & 0xFFFF) + (sum2 >> 16);
	}
	return sum1 | (sum2 << 16);
}

static int wait_flash_interrupt(struct synaptics_ts_data *ts, int attr)
{
	uint8_t data = 0;
	int i, ret;

	for (i = 0; i < 5; i++) {
#ifdef SYN_FLASH_PROGRAMMING_LOG
		pr_info("[TP] ATT: %d\n", gpio_get_value(attr));
#endif
		if (!gpio_get_value(attr)) {
			ret = i2c_syn_read(ts->client,
				get_address_base(ts, 0x01, DATA_BASE) + 1, &data, 1);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:1", __func__);
			if ((data & 0x01) == 0x01) {
#ifdef SYN_FLASH_PROGRAMMING_LOG
				pr_info("[TP] ATT: %d, status: %x\n", gpio_get_value(attr), data);
#endif
				break;
			}
		}
		msleep(20);
	}

	if (i == 5 && syn_panel_version == 0) {
		ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, DATA_BASE) + 1, &data, 1);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:2", __func__);
	} else if (i == 5) {
		pr_info("[TP] wait_flash_interrupt: interrupt over time!\n");
		return SYN_PROCESS_ERR;
	}

	ret = i2c_syn_read(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 18, &data, 1);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:3", __func__);
	
	if (data != 0x80) {
		pr_info("[TP] wait_flash_interrupt: block config fail!\n");
		return SYN_PROCESS_ERR;
	}
	return 0;
}

static int enable_flash_programming(struct synaptics_ts_data *ts, int attr)
{
	int ret;
	uint8_t data[2];

	
	ret = i2c_syn_read(ts->client,
		get_address_base(ts, 0x34, QUERY_BASE), data, 2);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:1", __func__);
	

	ret = i2c_syn_write(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 2, data, 2);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:2", __func__);

	ret = i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 18, 0x0F);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:3", __func__);

	ret = wait_flash_interrupt(ts, attr);
	if (ret < 0)
		return ret;

	return 0;
}

static int crc_comparison(struct synaptics_ts_data *ts, uint32_t config_crc, int attr)
{
	int ret;
	uint8_t data[17];
	uint32_t flash_crc;
#ifdef SYN_FLASH_PROGRAMMING_LOG
	uint8_t i, j;

	for (i = 0; i < 0x20; i++) {
		data[0] = i;
		data[1] = 0x00;
#else
		data[0] = 0x1F;
		data[1] = 0x00;
#endif
		ret = i2c_syn_write(ts->client,
			get_address_base(ts, 0x34, DATA_BASE), data, 2);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:1", __func__);

		ret = i2c_syn_write_byte_data(ts->client,
			get_address_base(ts, 0x34, DATA_BASE) + 18, 0x05);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:2", __func__);

		ret = wait_flash_interrupt(ts, attr);
		if (ret < 0)
			return ret;

		ret = i2c_syn_read(ts->client,
			get_address_base(ts, 0x34, DATA_BASE) + 2, data, 17);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:3", __func__);

		memcpy(&flash_crc, &data[12], 4);

#ifdef SYN_FLASH_PROGRAMMING_LOG
		pr_info("[TP] config_crc = %X, flash_crc = %X\n", config_crc, flash_crc);
		for (j = 0; j < 0x11; j++)
			pr_info(" %d:%X ", j, data[j]);
		pr_info("\n");
	}
#endif

	if (flash_crc == config_crc)
		return 0;
	else
		return 1;
}

static int program_config(struct synaptics_ts_data *ts, uint8_t *config, int attr)
{
	int ret;
	uint8_t data[19];
	uint16_t i;

	ret = i2c_syn_read(ts->client,
		get_address_base(ts, 0x34, QUERY_BASE), data, 2);
	
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:1", __func__);

	ret = i2c_syn_write(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 2, data, 2);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:2", __func__);

	
	ret = i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 18, 0x07);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:3", __func__);

	ret = wait_flash_interrupt(ts, attr);
	if (ret < 0)
		return ret;

	for (i = 0; i < 0x20; i++) {
		data[0] = i & 0xFF;
		data[1] = (i & 0xFF00) >> 8;
		memcpy(&data[2], &config[16 * i], 16);
		data[18] = 0x06;
		ret = i2c_syn_write(ts->client,
			get_address_base(ts, 0x34, DATA_BASE), data, 19);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:4", __func__);

		ret = wait_flash_interrupt(ts, attr);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int disable_flash_programming(struct synaptics_ts_data *ts, int status)
{
	int ret;
	uint8_t data = 0, i;

	ret = i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x01, COMMAND_BASE), 0x01);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:1", __func__);

	for (i = 0; i < 25; i++) {
		ret = i2c_syn_read(ts->client,
			get_address_base(ts, 0x01, DATA_BASE), &data, 1);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:2", __func__);

		if ((data & 0x40) == 0)
			break;
		else
			msleep(20);
	}

	if (i == 25) {
		pr_info("[TP] Disable flash programming fail! F01_data: %X\n", data);
		return SYN_PROCESS_ERR;
	} else {
		pr_info("[TP] Disable flash programming success! F01_data: %X\n", data);
		return status;
	}
}

static int syn_config_update(struct synaptics_ts_data *ts, int attr)
{
	uint8_t retry;
	uint32_t crc_checksum;
	int ret;

	crc_checksum =
		syn_crc((uint16_t *)ts->config, SYN_CONFIG_SIZE / 2 - 2);
	memcpy(&ts->config[SYN_CONFIG_SIZE - 4], &crc_checksum, 4);
	pr_info("[TP] CRC = %X\n" , syn_crc((uint16_t *)ts->config, SYN_CONFIG_SIZE / 2 - 2));

	if (ts->tw_pin_mask == 0) {
		ret = enable_flash_programming(ts, attr);
		if (ret < 0) {
			pr_info("[TP] syn_config_update: Enable flash programming fail!\n");
			return disable_flash_programming(ts, ret);
		}

		ret = syn_pdt_scan(ts, SYN_BL_PAGE);
		if (ret < 0) {
			pr_info("[TP] syn_config_update: pdt scan failed\n");
			return disable_flash_programming(ts, ret);
		}
	}

	if ((ts->config != NULL && (ts->config[0] << 24 | ts->config[1] << 16 |
		ts->config[2] << 8 | ts->config[3]) == ts->config_version)) {
		ret = crc_comparison(ts, crc_checksum, attr);
		if (ret < 0) {
			pr_info("[TP] syn_config_update: CRC comparison fail!\n");
			return disable_flash_programming(ts, ret);
		} else if (ret == 0)
			return disable_flash_programming(ts, 1);
	}

	for (retry = 0; retry < 3; retry++) {
		ret = program_config(ts, ts->config, attr);
		if (ret < 0) {
#ifdef SYN_FLASH_PROGRAMMING_LOG
			pr_info("[TP] syn_config_update: Program config fail %d!\n", retry + 1);
#endif
			continue;
		}

		ret = disable_flash_programming(ts, 0);
		if (ret == 0)
			break;
		else
			pr_info("[TP] syn_config_update: Disable flash programming fail %d\n", retry + 1);
	}

	if (retry == 3) {
		pr_info("[TP] syn_config_update: Program config fail 3 times\n");
		return ret;
	}
	return 0;
}

static int syn_get_tw_vendor(struct synaptics_ts_data *ts, int attr)
{
	uint8_t data[2] = {0};
	int ret;

	ret = enable_flash_programming(ts, attr);
	if (ret < 0) {
		pr_info("[TP] Enable flash programming fail!\n");
		return disable_flash_programming(ts, -1);
	}

	ret = syn_pdt_scan(ts, SYN_BL_PAGE);
	if (ret < 0) {
		pr_info("[TP] syn_config_update: pdt scan failed\n");
		return disable_flash_programming(ts, ret);
	}

	memcpy(&data, &ts->tw_pin_mask, sizeof(ts->tw_pin_mask));
	pr_info("[TP] tw mask = %X %X , %X\n", data[0], data[1], ts->tw_pin_mask);
	i2c_syn_write(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 2, data, 2);
	i2c_syn_write(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 4, data, 2);
	i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 18, 0x08);

	if (wait_flash_interrupt(ts, attr) < 0)
		return disable_flash_programming(ts, -1);

	i2c_syn_read(ts->client,
		get_address_base(ts, 0x34, DATA_BASE) + 6, data, 2);
	ts->tw_vendor = (data[1] << 8) | data[0];
	pr_info("[TP] tw vendor= %x %x\n", data[1], data[0]);

	return 0;
}

static int synaptics_input_register(struct synaptics_ts_data *ts)
{
	int ret;
	ts->input_dev = input_allocate_device();
	if (ts->input_dev == NULL) {
		ret = -ENOMEM;
		printk(KERN_ERR "[TP] TOUCH_ERR: %s: Failed to allocate input device\n", __func__);
		return ret;
	}
	ts->input_dev->name = "synaptics-rmi-touchscreen";
	set_bit(EV_SYN, ts->input_dev->evbit);
	set_bit(EV_KEY, ts->input_dev->evbit);
	set_bit(EV_ABS, ts->input_dev->evbit);

	set_bit(KEY_BACK, ts->input_dev->keybit);
	set_bit(KEY_HOME, ts->input_dev->keybit);
	set_bit(KEY_MENU, ts->input_dev->keybit);
	set_bit(KEY_SEARCH, ts->input_dev->keybit);
	set_bit(KEY_APP_SWITCH, ts->input_dev->keybit);

	pr_info("[TP] input_set_abs_params: mix_x %d, max_x %d, min_y %d, max_y %d\n",
		ts->layout[0], ts->layout[1], ts->layout[2], ts->layout[3]);

	if (ts->htc_event == SYN_AND_REPORT_TYPE_B) {
		input_mt_init_slots(ts->input_dev, ts->finger_support);
	} else {
		ts->input_dev->mtsize = ts->finger_support;
		input_set_abs_params(ts->input_dev, ABS_MT_TRACKING_ID, 0,
			ts->finger_support - 1, 0, 0);
	}
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_X,
		ts->layout[0], ts->layout[1], 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION_Y,
		ts->layout[2], ts->layout[3], 0, 0);

	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 30, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_PRESSURE, 0, 30, 0, 0);

	input_set_abs_params(ts->input_dev, ABS_MT_AMPLITUDE,
			0, ((255 << 16) | 15), 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_POSITION,
		0, ((1 << 31) | (ts->layout[1] << 16) | ts->layout[3]), 0, 0);

	return input_register_device(ts->input_dev);
}

static ssize_t touch_vendor_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	ssize_t ret = 0;
	char fw_version[2];
	struct synaptics_ts_data *ts;

	ts = gl_ts;
	memcpy(fw_version, &syn_panel_version, 2);
	ret = sprintf(buf, "synaptics-%d_%c.%c", ts->package_id, fw_version[1], fw_version[0]);
	if (ts->tw_pin_mask != 0)
		ret += sprintf(buf+ret, "_twID-%x", ts->tw_vendor);
	else
		ret += sprintf(buf+ret, "\n");
	ret += sprintf(buf+ret, "_PR: %d\n", ts->packrat_number);

	return ret;
}

static DEVICE_ATTR(vendor, S_IRUGO, touch_vendor_show, NULL);

static ssize_t gpio_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;
	struct synaptics_ts_data *ts;

	ts = gl_ts;

	ret = gpio_get_value(ts->gpio_irq);
	pr_debug("[TP] GPIO_TP_INT_N=%d\n", ret);
	sprintf(buf, "GPIO_TP_INT_N=%d\n", ret);
	ret = strlen(buf) + 1;

	return ret;
}

static DEVICE_ATTR(gpio, S_IRUGO, gpio_show, NULL);

static uint16_t syn_reg_addr;

static ssize_t register_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int ret = 0;
	uint8_t data = 0;
	struct synaptics_ts_data *ts;
	ts = gl_ts;

	ret = i2c_syn_read(ts->client, syn_reg_addr, &data, 1);
	if (ret < 0) {
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r", __func__);
		ret += sprintf(buf, "addr: 0x , data: 0x \n");
	} else {
		ret += sprintf(buf, "addr: 0x%X, data: 0x%X\n", syn_reg_addr, data);
	}
	return ret;
}

static ssize_t register_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int ret = 0;
	struct synaptics_ts_data *ts;
	char buf_tmp[4];
	uint8_t write_da;
	unsigned long addr;

	ts = gl_ts;
	memset(buf_tmp, 0x0, sizeof(buf_tmp));
	if ((buf[0] == 'r' || buf[0] == 'w') && buf[1] == ':' &&
		(buf[5] == ':' || buf[5] == '\n')) {
		memcpy(buf_tmp, buf + 2, 3);
		ret = strict_strtoul(buf_tmp, 16, &addr);
		syn_reg_addr = addr;
		pr_debug("[TP] %s: set syn_reg_addr is: 0x%X\n",
						__func__, syn_reg_addr);
		if (buf[0] == 'w' && buf[5] == ':' && buf[9] == '\n') {
			memcpy(buf_tmp, buf + 6, 3);
			ret = strict_strtoul(buf_tmp, 16, &addr);
			write_da = addr;
			pr_debug("[TP] write addr: 0x%X, data: 0x%X\n",
						syn_reg_addr, write_da);
			ret = i2c_syn_write_byte_data(ts->client,
					syn_reg_addr, write_da);
			if (ret < 0) {
				i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w", __func__);
			}
		}
	}

	return count;
}

static DEVICE_ATTR(register, (S_IWUSR|S_IRUGO),
	register_show, register_store);

static ssize_t debug_level_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;

	return sprintf(buf, "%d\n", ts->debug_log_level);
}

static ssize_t debug_level_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;
	int i;

	ts->debug_log_level = 0;
	for(i=0; i<count-1; i++)
	{
		if( buf[i]>='0' && buf[i]<='9' )
			ts->debug_log_level |= (buf[i]-'0');
		else if( buf[i]>='A' && buf[i]<='F' )
			ts->debug_log_level |= (buf[i]-'A'+10);
		else if( buf[i]>='a' && buf[i]<='f' )
			ts->debug_log_level |= (buf[i]-'a'+10);

		if(i!=count-2)
			ts->debug_log_level <<= 4;
	}

	return count;
}

static DEVICE_ATTR(debug_level, (S_IWUSR|S_IRUGO),
	debug_level_show, debug_level_store);

static ssize_t syn_diag_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;
	size_t count = 0;
	uint16_t i, j;
	int ret;

	ret = i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x54, DATA_BASE), ts->diag_command);
	if (ret < 0) {
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:1", __func__);
		count += sprintf(buf, "[TP] TOUCH_ERR: %s: i2c write fail(%d)\n", __func__, ret);
		return count;
	}

	atomic_set(&ts->data_ready, 0);

	ret = i2c_syn_write_byte_data(ts->client,
		get_address_base(ts, 0x54, COMMAND_BASE), 0x01);
	if (ret < 0) {
		atomic_set(&ts->data_ready, 1);
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:2", __func__);
		count += sprintf(buf, "[TP] TOUCH_ERR: %s: i2c write fail(%d)\n", __func__, ret);
		return count;
	}

	wait_event_interruptible_timeout(syn_data_ready_wq,
					 atomic_read(&ts->data_ready), 50);

	for (i = 0; i < ts->y_channel; i++) {
		for (j = 0; j < ts->x_channel; j++) {
			if(ts->package_id == 3201)
				count += sprintf(buf + count, "%5d", ts->report_data[i*ts->x_channel + j]);
			else
				count += sprintf(buf + count, "%5d", ts->report_data[i + j*ts->y_channel]);
		}
		count += sprintf(buf + count, "\n");
	}

	return count;
}

static ssize_t syn_diag_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts;
	ts = gl_ts;
	if (buf[0] == '1')
		ts->diag_command = 2;
	else if (buf[0] == '2')
		ts->diag_command = 3;

	return count;
}

static DEVICE_ATTR(diag, (S_IWUSR|S_IRUGO),
	syn_diag_show, syn_diag_store);

static ssize_t syn_unlock_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts;
	int unlock = -1;
	int ret;

	ts = gl_ts;

	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n')
		unlock = buf[0] - '0';

	pr_info("[TP] Touch: unlock change to %d\n", unlock);

	if (unlock == 2 && ts->first_pressed && ts->pre_finger_data[0][0] < 2) {
		ts->pre_finger_data[0][0] = 2;
		if(ts->psensor_detection) {
			if(ts->psensor_resume_enable == 1) {
				pr_info("[TP] %s: Disable P-sensor by Touch\n", __func__);
				psensor_enable_by_touch_driver(0);
				ts->psensor_resume_enable = 0;
			}
			else if(ts->psensor_resume_enable == 2) {
				ts->psensor_resume_enable = 0;
			}
		}
		if (ts->packrat_number < SYNAPTICS_FW_NOCAL_PACKRAT) {
#ifdef SYN_CALIBRATION_CONTROL
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, CONTROL_BASE) + 0x10, 0x0);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:1", __func__);

			if (ts->energy_ratio_relaxation) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x54, CONTROL_BASE), 0x0);
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:2", __func__);
			}

			if (ts->saturation_bef_unlock) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x54, CONTROL_BASE) + 0x02, ts->saturation_aft_unlock & 0xFF);
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:3", __func__);
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x54, CONTROL_BASE) + 0x03, (ts->saturation_aft_unlock & 0xFF00) >> 8);
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:4", __func__);
				pr_info("[TP] %s: unlock confirmed. set saturation: %x\n"
					, __func__, ts->saturation_aft_unlock);
			}

			if ( ts->PixelTouchThreshold_bef_unlock ) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x54, CONTROL_BASE) + 0x04, ts->PixelTouchThreshold_aft_unlock);
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "F54_ANALOG_CTRL03 Pixel Touch Threshold", __func__);
				pr_info("[TP] %s: set F54_ANALOG_CTRL03 Pixel Touch Threshold: %x\n", __func__, ts->PixelTouchThreshold_aft_unlock);
			}


			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, COMMAND_BASE), 0x04);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:5", __func__);

			if (ts->multitouch_calibration) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x11, COMMAND_BASE), 0x01);
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:6", __func__);
				pr_info("[TP] %s: Touch Calibration Confirmed, rezero\n", __func__);
			}
#endif
			if (ts->large_obj_check) {
				if (ts->package_id == 2200) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x26, ts->default_large_obj);

				} else {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x29, ts->default_large_obj);
				}
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:7", __func__);
				pr_info("[TP] %s: unlock confirmed. set large obj suppression: %x\n"
					, __func__, ts->default_large_obj);
			}

			if (ts->segmentation_bef_unlock) {
				if (ts->package_id == 2200) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x25, ts->segmentation_aft_unlock);

				} else {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x22, ts->segmentation_aft_unlock);
				}
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:8", __func__);
				pr_info("[TP] %s: unlock confirmed. set segmentation aggressiveness: %x\n"
					, __func__, ts->segmentation_aft_unlock);
			}

			if (ts->threshold_bef_unlock) {
				if (ts->package_id == 2200) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x0A, ts->threshold_aft_unlock);
				} else {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x0C, ts->threshold_aft_unlock);
				}
				if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:9", __func__);
				pr_info("[TP] %s: unlock confirmed. set Z Touch threshold: %x\n"
					, __func__, ts->threshold_aft_unlock);
			}
		}
	}

	return count;
}

static DEVICE_ATTR(unlock, (S_IWUSR|S_IRUGO),
	NULL, syn_unlock_store);

static ssize_t syn_config_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;
	uint16_t i, length = 0;
	uint8_t j, temp_func_cmd = 0, temp_func_query = 0, size = 0;
	size_t count = 0;
	int ret;

	pr_info("[TP] ts->num_function: %d\n", ts->num_function);
	for (i = 0; i < SYN_MAX_PAGE; i++) {
		for (j = 0; j < ts->num_function; j++) {
			if (((ts->address_table[j].control_base >> 8) & 0xFF) == i) {
				temp_func_query = 0;
				for (temp_func_cmd = j; temp_func_cmd < ts->num_function; temp_func_cmd++) {
					uint16_t max_addr = (i << 8) | 0xFF;
					uint16_t min_addr = (i << 8) | 0;
					if ((ts->address_table[temp_func_cmd].command_base > min_addr) &&
						(ts->address_table[temp_func_cmd].command_base <= max_addr))
						break;
					if ((ts->address_table[temp_func_cmd].query_base > min_addr) &&
						(ts->address_table[temp_func_cmd].query_base <= max_addr)
						&& temp_func_query == 0)
						temp_func_query = temp_func_cmd;
				}

				if (temp_func_cmd != ts->num_function) {
					size = ts->address_table[temp_func_cmd].command_base -
						ts->address_table[j].control_base;
					pr_info("[TP] page%d has command function, function: %X\n"
						, i, ts->address_table[temp_func_cmd].function_type);
				} else {
					size = ts->address_table[temp_func_query].query_base -
						ts->address_table[j].control_base;
					pr_info("[TP] page%d has no command function, use query function, function: %X\n"
						, i, ts->address_table[temp_func_query].function_type);
				}

				ret = i2c_syn_read(ts->client, ts->address_table[j].control_base,
					&ts->config_table[length], size);
				if (ret < 0) {
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w", __func__);
					count += sprintf(buf, "[TP] TOUCH_ERR: %s: i2c write fail(%d)\n", __func__, ret);
					return count;
				}

				length += size;
				pr_info("[TP] Size: %x, Length: %x\n", size, length);
				break;
			}
		}
	}
	if(length > SYN_CONFIG_SIZE)
		length = SYN_CONFIG_SIZE;

	pr_info("");
	for (i = 0; i < length; i++) {
		pr_info("%2.2X ", ts->config_table[i]);
		if ((i % 16) == 15)
			pr_info("\n");
	}

	for (i = 0; i < length; i++) {
		count += sprintf(buf + count, "%2.2X ", ts->config_table[i]);
		if ((i % 16) == (16 - 1))
			count += sprintf(buf + count, "\n");
	}
	count += sprintf(buf + count, "\n");

	return count;
}

static ssize_t syn_config_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;
	uint8_t i, j, k = 0, length = 0;
	pr_info("[TP] ts->num_function: %d\n", ts->num_function);
	for (i = 0; i < SYN_MAX_PAGE; i++) {
		for (j = 0; j < ts->num_function; j++) {
			if (((ts->address_table[j].control_base >> 8) & 0xFF) == i) {
				for (k = j; k < ts->num_function; k++)
					if (ts->address_table[k].command_base != 0)
						break;
				length += ts->address_table[k].command_base -
					ts->address_table[j].control_base;
				pr_info("[%d]Length: %x\n", i, length);
				break;
			}
		}
	}

	return count;
}

static DEVICE_ATTR(config, (S_IWUSR|S_IRUGO),
	syn_config_show, syn_config_store);


static ssize_t syn_layout_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;
	uint8_t i;
	size_t count = 0;
	for (i = 0; i < 4; i++)
		count += sprintf(buf + count, "%d ", ts->layout[i]);
	count += sprintf(buf + count, "\n");

	return count;
}

static ssize_t syn_layout_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;
	char buf_tmp[5];
	int i = 0, j = 0, k = 0, ret;
	unsigned long value;
	int layout[4] = {0};

	for (i = 0; i < 20; i++) {
		if (buf[i] == ',' || buf[i] == '\n') {
			memset(buf_tmp, 0x0, sizeof(buf_tmp));
			if (i - j <= 5)
				memcpy(buf_tmp, buf + j, i - j);
			else {
				pr_info("[TP] buffer size is over 5 char\n");
				return count;
			}
			j = i + 1;
			if (k < 4) {
				ret = strict_strtol(buf_tmp, 10, &value);
				layout[k++] = value;
			}
		}
	}
	if (k == 4) {
		memcpy(ts->layout, layout, sizeof(layout));
		pr_info("[TP] %d, %d, %d, %d\n",
			ts->layout[0], ts->layout[1], ts->layout[2], ts->layout[3]);
		input_unregister_device(ts->input_dev);
		synaptics_input_register(ts);
	} else
		pr_info("[TP] ERR@%d, %d, %d, %d\n",
			ts->layout[0], ts->layout[1], ts->layout[2], ts->layout[3]);
	return count;

}

static DEVICE_ATTR(layout, (S_IWUSR|S_IRUGO),
	syn_layout_show, syn_layout_store);


static ssize_t syn_pdt_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;
	uint8_t i;
	size_t count = 0;
	for (i = 0; i < ts->num_function; i++) {
		count += sprintf(buf + count,
			"Funtion: %2X, Query: %3X, Command: %3X, "
			"Control: %3X, Data: %3X, INTR: %2X\n",
			ts->address_table[i].function_type, ts->address_table[i].query_base ,
			ts->address_table[i].command_base, ts->address_table[i].control_base,
			ts->address_table[i].data_base, ts->address_table[i].interrupt_source);
	}
	return count;
}

static DEVICE_ATTR(pdt, S_IRUGO, syn_pdt_show, NULL);

static ssize_t syn_htc_event_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;

	return sprintf(buf, "%d\n", ts->htc_event);
}

static ssize_t syn_htc_event_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;

	if (buf[0] >= '0' && buf[0] <= '9' && buf[1] == '\n')
		ts->htc_event = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(htc_event, (S_IWUSR|S_IRUGO),
	syn_htc_event_show, syn_htc_event_store);

#ifdef SYN_WIRELESS_DEBUG
static ssize_t syn_int_status_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct synaptics_ts_data *ts = gl_ts;
	size_t count = 0;

	count += sprintf(buf + count, "%d ", ts->irq_enabled);
	count += sprintf(buf + count, "\n");

	return count;
}

static ssize_t syn_int_status_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;
	int value, ret=0;

	if (sysfs_streq(buf, "0"))
		value = false;
	else if (sysfs_streq(buf, "1"))
		value = true;
	else
		return -EINVAL;

	if (value) {
		ret = request_threaded_irq(ts->client->irq, NULL, synaptics_irq_thread,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT, ts->client->name, ts);
		if (ret == 0) {
			ts->irq_enabled = 1;
			ret = i2c_syn_read(ts->client,
				get_address_base(ts, 0x01, CONTROL_BASE) + 1, &ts->intr_bit, 1);
			pr_info("[TP] %s: interrupt enable: %x\n", __func__, ts->intr_bit);
			if (ret)
				free_irq(ts->client->irq, ts);
		}
	} else {
		disable_irq(ts->client->irq);
		free_irq(ts->client->irq, ts);
		ts->irq_enabled = 0;
	}

	return count;
}

static DEVICE_ATTR(enabled, (S_IWUSR|S_IRUGO),
	syn_int_status_show, syn_int_status_store);

static ssize_t syn_reset(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;

	if (buf[0] == '1' && ts->gpio_reset) {
		gpio_direction_output(ts->gpio_reset, 0);
		msleep(1);
		gpio_direction_output(ts->gpio_reset, 1);
		pr_info("[TP] %s: synaptics touch chip reseted.\n", __func__);
	}

	return count;
}

static DEVICE_ATTR(reset, (S_IWUSR),
	0, syn_reset);

#endif

enum SR_REG_STATE{
	ALLOCATE_DEV_FAIL = -2,
	REGISTER_DEV_FAIL,
	SUCCESS,
};

static char *vk_name = "virtualkeys.sr_touchscreen";
static struct kobj_attribute vk_dev;

static int register_sr_touch_device(void)
{
	struct synaptics_ts_data *ts = gl_ts;
	struct synaptics_i2c_rmi_platform_data *pdata = ts->client->dev.platform_data;
	int ret = 0;

	ts->sr_input_dev = input_allocate_device();

	if (ts->sr_input_dev == NULL) {
		printk(KERN_ERR "[TP][TOUCH_ERR]%s: Failed to allocate SR input device\n", __func__);
		return ALLOCATE_DEV_FAIL;
	}

	if (pdata->vk_obj) {
		memcpy(&vk_dev, pdata->vk2Use, sizeof(struct kobj_attribute));
		vk_dev.attr.name = vk_name;
		ret = sysfs_create_file(pdata->vk_obj, &(vk_dev.attr));
	}

	ts->sr_input_dev->name = "sr_touchscreen";
	set_bit(EV_SYN, ts->sr_input_dev->evbit);
	set_bit(EV_ABS, ts->sr_input_dev->evbit);
	set_bit(EV_KEY, ts->sr_input_dev->evbit);

	set_bit(KEY_BACK, ts->sr_input_dev->keybit);
	set_bit(KEY_HOME, ts->sr_input_dev->keybit);
	set_bit(KEY_MENU, ts->sr_input_dev->keybit);
	set_bit(KEY_SEARCH, ts->sr_input_dev->keybit);
	set_bit(BTN_TOUCH, ts->sr_input_dev->keybit);
	set_bit(KEY_APP_SWITCH, ts->sr_input_dev->keybit);
	set_bit(INPUT_PROP_DIRECT, ts->sr_input_dev->propbit);
	ts->sr_input_dev->mtsize = ts->finger_support;
	input_set_abs_params(ts->sr_input_dev, ABS_MT_TRACKING_ID,
		0, ts->finger_support - 1, 0, 0);
	pr_info("[TP][SR]input_set_abs_params: mix_x %d, max_x %d,"
		" min_y %d, max_y %d\n", ts->layout[0],
		 ts->layout[1], ts->layout[2], ts->layout[3]);

	input_set_abs_params(ts->sr_input_dev, ABS_MT_POSITION_X,
		ts->layout[0], ts->layout[1], 0, 0);
	input_set_abs_params(ts->sr_input_dev, ABS_MT_POSITION_Y,
		ts->layout[2], ts->layout[3], 0, 0);
	input_set_abs_params(ts->sr_input_dev, ABS_MT_TOUCH_MAJOR,
		0, 255, 0, 0);
	input_set_abs_params(ts->sr_input_dev, ABS_MT_PRESSURE,
		0, 30, 0, 0);
	input_set_abs_params(ts->sr_input_dev, ABS_MT_WIDTH_MAJOR,
		0, 30, 0, 0);

	if (input_register_device(ts->sr_input_dev)) {
		input_free_device(ts->sr_input_dev);
		printk(KERN_ERR "[TP][SR][TOUCH_ERR]%s: Unable to register %s input device\n",
			__func__, ts->sr_input_dev->name);
		return REGISTER_DEV_FAIL;
	}
	return SUCCESS;
}

static ssize_t set_en_sr(struct device *dev, struct device_attribute *attr,
						const char *buf, size_t count)
{
	struct synaptics_ts_data *ts = gl_ts;
	if (buf[0]) {
		if (ts->sr_input_dev)
			pr_info("[TP]%s: SR device already exist!\n", __func__);
		else
			pr_info("[TP]%s: SR touch device enable result:%X\n", __func__, register_sr_touch_device());
	}
	return count;
}

static DEVICE_ATTR(sr_en, S_IWUSR, 0, set_en_sr);

#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
static ssize_t synaptics_sweep2wake_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	size_t count = 0;

	count += sprintf(buf, "%d\n", s2w_switch);

	return count;
}

static ssize_t synaptics_sweep2wake_dump(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	if (buf[0] >= '0' && buf[0] <= '2' && buf[1] == '\n')
		if (s2w_switch != buf[0] - '0')
			s2w_switch = buf[0] - '0';

	return count;
}

static DEVICE_ATTR(sweep2wake, (S_IWUSR|S_IRUGO),
	synaptics_sweep2wake_show, synaptics_sweep2wake_dump);
#endif

static struct kobject *android_touch_kobj;

static int synaptics_touch_sysfs_init(void)
{
	int ret;
#ifdef SYN_WIRELESS_DEBUG
	struct synaptics_ts_data *ts = gl_ts;
#endif

	android_touch_kobj = kobject_create_and_add("android_touch", NULL);
	if (android_touch_kobj == NULL) {
		printk(KERN_ERR "[TP] TOUCH_ERR: %s: subsystem_register failed\n", __func__);
		ret = -ENOMEM;
		return ret;
	}
	syn_reg_addr = 0;
	if (sysfs_create_file(android_touch_kobj, &dev_attr_vendor.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_gpio.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_debug_level.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_register.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_unlock.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_config.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_layout.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_pdt.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_htc_event.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_reset.attr) ||
		sysfs_create_file(android_touch_kobj, &dev_attr_sr_en.attr)
#ifdef SYN_WIRELESS_DEBUG
		|| sysfs_create_file(android_touch_kobj, &dev_attr_enabled.attr)
#endif
		)
		return -ENOMEM;
	if (get_address_base(gl_ts, 0x54, FUNCTION))
		if (sysfs_create_file(android_touch_kobj, &dev_attr_diag.attr))
			return -ENOMEM;
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
	ret = sysfs_create_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
	if (ret) {
		printk(KERN_ERR "%s: sysfs_create_file failed\n", __func__);
		return ret;
	}
#endif

#ifdef SYN_WIRELESS_DEBUG
	ret= gpio_request(ts->gpio_irq, "synaptics_attn");
	if (ret) {
		pr_info("[TP]%s: Failed to obtain touchpad IRQ %d. Code: %d.", __func__, ts->gpio_irq, ret);
		return ret;
	}
	if (ts->gpio_reset && !ts->i2c_err_handler_en) {
		ret = gpio_request(ts->gpio_reset, "synaptics_reset");
		if (ret)
			pr_info("[TP]%s: Failed to obtain reset pin: %d. Code: %d.", __func__, ts->gpio_reset, ret);
	}
	ret = gpio_export(ts->gpio_irq, true);
	if (ret) {
		pr_info("[TP]%s: Failed to "
			"export ATTN gpio!\n", __func__);
		ret = 0;
	} else {
		ret = gpio_export_link(&(ts->input_dev->dev), "attn",
			ts->gpio_irq);
		if (ret) {
			pr_info("[TP]%s: Failed to "
				"symlink ATTN gpio!\n", __func__);
			ret = 0;
		} else {
			pr_info("[TP]%s: Exported GPIO %d.", __func__, ts->gpio_irq);
		}
	}
#endif
	return 0;
}

static void synaptics_touch_sysfs_remove(void)
{
	sysfs_remove_file(android_touch_kobj, &dev_attr_vendor.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_gpio.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_debug_level.attr);
	if (get_address_base(gl_ts, 0x54, FUNCTION))
		sysfs_remove_file(android_touch_kobj, &dev_attr_diag.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_register.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_unlock.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_config.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_layout.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_pdt.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_htc_event.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_reset.attr);
	sysfs_remove_file(android_touch_kobj, &dev_attr_sr_en.attr);
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
	sysfs_remove_file(android_touch_kobj, &dev_attr_sweep2wake.attr);
#endif
#ifdef SYN_WIRELESS_DEBUG
	sysfs_remove_file(android_touch_kobj, &dev_attr_enabled.attr);
#endif
	kobject_del(android_touch_kobj);
}

static int synaptics_init_panel(struct synaptics_ts_data *ts)
{
	int ret = 0;

	
	ret = i2c_syn_write_byte_data(ts->client,
			get_address_base(ts, 0x01, CONTROL_BASE), 0x80);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:1", __func__);
	if (ts->pre_finger_data[0][0] < 2) {
		if (ts->large_obj_check) {
			if (ts->package_id == 2200) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x26, ts->default_large_obj & 0x7F);
			} else {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x29, ts->default_large_obj & 0x7F);
			}
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:2", __func__);
			pr_info("[TP] %s: set large obj suppression register to: %x\n", __func__, ts->default_large_obj & 0x7F);
		}

		if (ts->segmentation_bef_unlock) {
			if (ts->package_id == 2200) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x25, ts->segmentation_bef_unlock);
			} else {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x22, ts->segmentation_bef_unlock);
			}
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:3", __func__);
			pr_info("[TP] %s: set segmentation aggressiveness to: %x\n", __func__, ts->segmentation_bef_unlock);
		}

		if (ts->threshold_bef_unlock) {
			if (ts->package_id == 2200) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x0A, ts->threshold_bef_unlock);
			} else {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x0C, ts->threshold_bef_unlock);
			}
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:4", __func__);
			pr_info("[TP] %s: set Z Touch threshold to: %x\n", __func__, ts->threshold_bef_unlock);
		}

		if (ts->saturation_bef_unlock) {
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, CONTROL_BASE) + 0x02, ts->saturation_bef_unlock & 0xFF);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "saturation capacitance", __func__);
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, CONTROL_BASE) + 0x03, (ts->saturation_bef_unlock & 0xFF00) >> 8);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "saturation capacitance", __func__);
			pr_info("[TP] %s: set saturation to: %x\n", __func__, ts->saturation_bef_unlock);
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, COMMAND_BASE), 0x04);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:5", __func__);
		}

		if ( ts->PixelTouchThreshold_bef_unlock ) {
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, CONTROL_BASE) + 0x04, ts->PixelTouchThreshold_bef_unlock );
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "F54_ANALOG_CTRL03 Pixel Touch Threshold", __func__);
			pr_info("[TP] %s: set F54_ANALOG_CTRL03 Pixel Touch Threshold: %x\n", __func__, ts->PixelTouchThreshold_bef_unlock);
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, COMMAND_BASE), 0x04);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:6", __func__);
		}
	}

#ifdef SYN_CALIBRATION_CONTROL
	if (ts->packrat_number < SYNAPTICS_FW_NOCAL_PACKRAT) {
		if (ts->pre_finger_data[0][0] >= 2 || ts->mfg_flag == 1) {
			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, CONTROL_BASE) + 0x10, 0x0);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:6", __func__);

			ret = i2c_syn_write_byte_data(ts->client,
				get_address_base(ts, 0x54, COMMAND_BASE), 0x04);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:7", __func__);

			pr_info("[TP] %s: Touch init: set fast relaxation to 0x0\n", __func__);
		}
	}
#endif

	return ret;
}

static void synaptics_ts_finger_func(struct synaptics_ts_data *ts)
{
	int ret;
	uint8_t buf[((ts->finger_support * 21 + 3) / 4)];
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
	int prevx = 0, nextx = 0;
#endif
	memset(buf, 0x0, sizeof(buf));
	ret = i2c_syn_read(ts->client,
		get_address_base(ts, 0x11, DATA_BASE), buf, sizeof(buf));
	if (ret < 0) {
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:1", __func__);
	} else {
		int finger_data[ts->finger_support][4];
		int base = (ts->finger_support + 3) / 4;
		uint8_t i, j;
		uint16_t finger_press_changed = 0, finger_release_changed = 0, finger_pressed = 0;

		ts->finger_count = 0;
		if (ts->debug_log_level & BIT(0)) {
			pr_info("[TP] Touch:");
			for (i = 0; i < sizeof(buf); i++)
				pr_info(" %2x", buf[i]);
			pr_info("\n");
		}
		for (i = 0; i < ts->finger_support; i++) {
			uint8_t finger_state = buf[(i / 4)] >> ((i * 2) % 8);
			if (finger_state & 0x03) {
				finger_pressed |= BIT(i);
				ts->finger_count++;
				if (finger_state == 0x02)
					pr_info("[TP] Finger state[%d] = 0x02\n", i);
				else if (finger_state == 0x03)
					pr_info("[TP] Finger state[%d] = 0x03\n", i);
			}
#ifdef SYN_FILTER_CONTROL
			else if ((ts->grip_suppression | ts->grip_b_suppression) & BIT(i)) {
				ts->grip_suppression &= ~BIT(i);
				ts->grip_b_suppression &= ~BIT(i);
			}
#endif
		}
		if (ts->finger_pressed != finger_pressed
			) {
			finger_press_changed = ts->finger_pressed ^ finger_pressed;
			finger_release_changed = finger_press_changed & ts->finger_pressed;
			finger_press_changed &= finger_pressed;
			ts->finger_pressed = finger_pressed;
		}

		if(ts->debug_log_level & BIT(3)) {
			for(i = 0; i < ts->finger_support; i++) {
				if (finger_release_changed & BIT(i) ) {
					uint32_t flip_flag = SYNAPTICS_FLIP_X;
					uint8_t pos_mask = 0x0f;
					for (j = 0; j < 2; j++) {
						finger_data[i][j]
							= (buf[base+2] & pos_mask) >> (j * 4) |
							(uint16_t)buf[base + j] << 4;
						if (ts->flags & flip_flag)
							finger_data[i][j] = ts->max[j] - finger_data[i][j];
						flip_flag <<= 1;
						pos_mask <<= 4;
					}
					finger_data[i][2] = (buf[base+3] >> 4 & 0x0F) + (buf[base+3] & 0x0F);
					finger_data[i][3] = buf[base+4];

					if (ts->flags & SYNAPTICS_SWAP_XY)
						swap(finger_data[i][0], finger_data[i][1]);

					if (ts->layout[1] < finger_data[i][0])
						finger_data[i][0] = ts->layout[1];
					if(ts->width_factor && ts->height_factor){
						pr_debug(
							"[TP] Screen:F[%02d]:Up, X=%d, Y=%d, W=%d, Z=%d\n",
							i+1, (finger_data[i][0]*ts->width_factor)>>SHIFT_BITS,
							(finger_data[i][1]*ts->height_factor)>>SHIFT_BITS,
							finger_data[i][2], finger_data[i][3]);
					} else {
						pr_debug(
							"[TP] Raw:F[%02d]:Up, X=%d, Y=%d, W=%d, Z=%d\n",
							i+1, finger_data[i][0], finger_data[i][1],
							finger_data[i][2], finger_data[i][3]);
					}
				}
				base += 5;
			}
		}

		if (ts->htc_event == SYN_AND_REPORT_TYPE_B && finger_release_changed) {
			for (i = 0; i < ts->finger_support; i++) {
				if (finger_release_changed & BIT(i)) {
					input_mt_slot(ts->input_dev, i);
					input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
					ts->tap_suppression &= ~BIT(i);
				}
			}
		}

		if (finger_pressed == 0 
) {
			if (ts->htc_event == SYN_AND_REPORT_TYPE_A) {
				
				if (ts->support_htc_event) {
					input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
					input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
				}
				input_mt_sync(ts->input_dev);
			}
			else if (ts->htc_event == SYN_AND_REPORT_TYPE_HTC) {
				input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
				input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
			}

#ifdef SYN_FILTER_CONTROL
			if (ts->filter_level[0])
				ts->ambiguous_state = 0;
			ts->grip_b_suppression = 0;
#endif
			if (ts->reduce_report_level[0])
				ts->tap_suppression = 0;
			if (ts->debug_log_level & BIT(1))
				pr_info("[TP] Finger leave\n");
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
				/* if finger released, reset count & barriers */
				if ((((ts->finger_count > 0)?1:0) == 0) && (s2w_switch > 0)) {
				if ((s2w_switch == 2) &&
			    	(scr_suspended == true) &&
			    	(led_exec_count == false) &&
			    	(scr_on_touch == false) &&
			    	(exec_count == true)) {
					//pm8921_led_current_set(sweep2wake_leddev, 0);
					printk(KERN_INFO "[sweep2wake]: deactivated button backlight.\n");
				}
				exec_count = true;
				led_exec_count = true;
				barrier[0] = false;
				barrier[1] = false;
				scr_on_touch = false;
				printk(KERN_INFO "[sweep2wake]: Finger released, reseting vars.\n");
			}
#endif
		}

		if (ts->pre_finger_data[0][0] < 2 || finger_pressed) {
			base = (ts->finger_support + 3) / 4;
			for (i = 0; i < ts->finger_support; i++) {
				uint32_t flip_flag = SYNAPTICS_FLIP_X;
				if ((finger_pressed | finger_release_changed) & BIT(i)) {
					uint8_t pos_mask = 0x0f;
					for (j = 0; j < 2; j++) {
						finger_data[i][j]
							= (buf[base+2] & pos_mask) >> (j * 4) |
							(uint16_t)buf[base + j] << 4;
						if (ts->flags & flip_flag)
							finger_data[i][j] = ts->max[j] - finger_data[i][j];
						flip_flag <<= 1;
						pos_mask <<= 4;
					}
					finger_data[i][2] = (buf[base+3] >> 4 & 0x0F) + (buf[base+3] & 0x0F);
					finger_data[i][3] = buf[base+4];

					if (ts->flags & SYNAPTICS_SWAP_XY)
						swap(finger_data[i][0], finger_data[i][1]);

					if (ts->layout[1] < finger_data[i][0])
						finger_data[i][0] = ts->layout[1];

					if ((finger_release_changed & BIT(i)) && ts->pre_finger_data[0][0] < 2) {
						if (!ts->first_pressed) {
							if (ts->finger_count == 0)
								ts->first_pressed = 1;
							pr_debug("[TP] E%d@%d, %d\n", i + 1,
							finger_data[i][0], finger_data[i][1]);
						}
					}
#ifdef SYN_FILTER_CONTROL
					if (abs((buf[base+3] >> 4 & 0x0F) - (buf[base+3] & 0x0F)) >= 10)
						ts->grip_b_suppression |= BIT(i);

					if (ts->filter_level[0] &&
						((finger_press_changed | ts->grip_suppression) & BIT(i))) {
						if ((finger_data[i][0] < (ts->filter_level[0] + ts->ambiguous_state * 20) ||
							finger_data[i][0] > (ts->filter_level[3] - ts->ambiguous_state * 20)) &&
							!(ts->grip_suppression & BIT(i))) {
							ts->grip_suppression |= BIT(i);
						} else if ((finger_data[i][0] < (ts->filter_level[1] + ts->ambiguous_state * 20) ||
							finger_data[i][0] > (ts->filter_level[2] - ts->ambiguous_state * 20)) &&
							(ts->grip_suppression & BIT(i)))
							ts->grip_suppression |= BIT(i);
						else if (finger_data[i][0] > (ts->filter_level[1] + ts->ambiguous_state * 20) &&
							finger_data[i][0] < (ts->filter_level[2] - ts->ambiguous_state * 20)) {
							ts->grip_suppression &= ~BIT(i);
						}
					}
					if ((ts->grip_suppression | ts->grip_b_suppression) & BIT(i)) {
						finger_pressed &= ~BIT(i);
					} else
#endif

					if (ts->htc_event == SYN_AND_REPORT_TYPE_B && ts->reduce_report_level[0]) {
						if (ts->tap_suppression & BIT(i) && finger_pressed & BIT(i)) {
							int dx, dy = 0;
							dx = abs(ts->pre_finger_data[i + 1][2] - finger_data[i][0]);
							dy = abs(ts->pre_finger_data[i + 1][3] - finger_data[i][1]);
							if (dx > ts->reduce_report_level[TAP_DX_OUTER] || dy > ts->reduce_report_level[TAP_DY_OUTER]) {
								ts->tap_suppression &= ~BIT(i);
							} else if (ts->reduce_report_level[TAP_TIMEOUT] && time_after(jiffies, ts->tap_timeout[i]) && (dx > ts->reduce_report_level[TAP_DX_INTER] || dy > ts->reduce_report_level[TAP_DY_INTER])) {
								ts->tap_suppression &= ~BIT(i);
							} else {
								finger_pressed &= ~BIT(i);
								if (ts->debug_log_level & (BIT(1) | BIT(3)))
									pr_debug(
										"[TP] Filtered Finger %d=> X:%d, Y:%d w:%d, z:%d\n",
										i + 1, finger_data[i][0], finger_data[i][1],
										finger_data[i][2], finger_data[i][3]);
							}
						}
					}

					if ((finger_pressed & BIT(i)) == BIT(i)) {
						finger_pressed &= ~BIT(i);

						if (ts->htc_event == SYN_AND_REPORT_TYPE_A) {
							if (ts->support_htc_event) {
								input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE,
									finger_data[i][3] << 16 | finger_data[i][2]);
								input_report_abs(ts->input_dev, ABS_MT_POSITION,
									(finger_pressed == 0) << 31 |
									finger_data[i][0] << 16 | finger_data[i][1]);
							}
							input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, i);
							input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
								finger_data[i][3]);
							input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR,
								finger_data[i][2]);
							input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
								finger_data[i][2]);
							input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
								finger_data[i][0]);
							input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
								finger_data[i][1]);
							input_mt_sync(ts->input_dev);
						} else if (ts->htc_event == SYN_AND_REPORT_TYPE_B) {
							if (ts->support_htc_event) {
								input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE,
									finger_data[i][3] << 16 | finger_data[i][2]);
								input_report_abs(ts->input_dev, ABS_MT_POSITION,
									(finger_pressed == 0) << 31 |
									finger_data[i][0] << 16 | finger_data[i][1]);
							}
							input_mt_slot(ts->input_dev, i);
							input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER,
							1);
							input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
								finger_data[i][3]);
							input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR,
								finger_data[i][2]);
							input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
								finger_data[i][2]);
							input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
								finger_data[i][0]);
							input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
								finger_data[i][1]);
						} else if (ts->htc_event == SYN_AND_REPORT_TYPE_HTC) {
							input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, i);
							input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE,
								finger_data[i][3] << 16 | finger_data[i][2]);
							input_report_abs(ts->input_dev, ABS_MT_POSITION,
								(finger_pressed == 0) << 31 |
								finger_data[i][0] << 16 | finger_data[i][1]);
						}

						if ((finger_press_changed & BIT(i)) && ts->debug_log_level & BIT(3)) {
							if(ts->width_factor && ts->height_factor){
								pr_debug(
									"[TP] Screen:F[%02d]:Down, X=%d, Y=%d, W=%d, Z=%d\n",
									i+1, (finger_data[i][0]*ts->width_factor)>>SHIFT_BITS,
									(finger_data[i][1]*ts->height_factor)>>SHIFT_BITS,
									finger_data[i][2], finger_data[i][3]);
							} else {
								pr_debug(
									"[TP] Raw:F[%02d]:Down, X=%d, Y=%d, W=%d, Z=%d\n",
									i+1, finger_data[i][0], finger_data[i][1],
									finger_data[i][2], finger_data[i][3]);
							}
						}
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE

//			printk(KERN_INFO "[SWEEP2WAKE_DEBUG]: TOUCH at x = %d\n", finger_data[i][0]);
			//left->right
							if ((ts->finger_count == 1) && (scr_suspended == true) && (s2w_switch > 0)) {
								prevx = 30;
								nextx = 250;
								if ((barrier[0] == true) ||
								   ((finger_data[i][0] > prevx) &&
								    (finger_data[i][0] < nextx) &&
								    (finger_data[i][1] > 1780))) {
						if ((led_exec_count == true) && (scr_on_touch == false) && (s2w_switch == 2)) {
 						//pm8921_led_current_set(sweep2wake_leddev, 255);
						printk(KERN_INFO "[sweep2wake]: activated button backlight.\n");
						led_exec_count = false;
						}
									prevx = nextx;
									nextx = 818;
									barrier[0] = true;
									if ((barrier[1] == true) ||
									   ((finger_data[i][0] > prevx) &&
									    (finger_data[i][0] < nextx) &&
									    (finger_data[i][1] > 1780))) {
										prevx = nextx;
										barrier[1] = true;
										if ((finger_data[i][0] > prevx) &&
										    (finger_data[i][1] > 1780)) {
											if (finger_data[i][0] > 880) {
												if (exec_count) {
													printk(KERN_INFO "[sweep2wake]: ON");
													sweep2wake_pwrtrigger();
													exec_count = false;
													break;
												}
											}
										}
									}
								}
							//right->left
							} else if ((ts->finger_count == 1) && (scr_suspended == false) && (s2w_switch > 0)) {
								scr_on_touch=true;
								prevx = 1100;
								nextx = 880;
								if ((barrier[0] == true) ||
								   ((finger_data[i][0] < prevx) &&
								    (finger_data[i][0] > nextx) &&
								    (finger_data[i][1] > 1780))) {
									prevx = nextx;
									nextx = 500;
									barrier[0] = true;
									if ((barrier[1] == true) ||
									   ((finger_data[i][0] < prevx) &&
									    (finger_data[i][0] > nextx) &&
									    (finger_data[i][1] > 1780))) {
										prevx = nextx;
										barrier[1] = true;
										if ((finger_data[i][0] < prevx) &&
										    (finger_data[i][1] > 1780)) {
											if (finger_data[i][0] < 250) {
												if (exec_count) {
													printk(KERN_INFO "[sweep2wake]: OFF");
													sweep2wake_pwrtrigger();
													exec_count = false;
													break;
												}
											}
										}
									}
								}
							}
#endif

						if (ts->pre_finger_data[0][0] < 2) {
							if (finger_press_changed & BIT(i)) {
								ts->pre_finger_data[i + 1][0] = finger_data[i][0];
								ts->pre_finger_data[i + 1][1] = finger_data[i][1];

								if (!ts->first_pressed)
									pr_debug("[TP] S%d@%d, %d\n", i + 1,
										finger_data[i][0], finger_data[i][1]);
								if (ts->packrat_number < SYNAPTICS_FW_NOCAL_PACKRAT) {
#ifdef SYN_CALIBRATION_CONTROL
									if (ts->multitouch_calibration) {
										if (ts->finger_count == ts->finger_support) {
											ret = i2c_syn_write_byte_data(ts->client,
												get_address_base(ts, 0x11, COMMAND_BASE), 0x01);
											if (ret < 0)
												i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:Rezero_1", __func__);
											pr_info("[TP] %s: Touch Calibration Confirmed, rezero\n", __func__);
										} else if (!ts->pre_finger_data[0][0] && ts->finger_count > 1)
											ts->pre_finger_data[0][0] = 1;
									}
#endif
								}
							}
						}

						if (ts->htc_event == SYN_AND_REPORT_TYPE_B && ts->reduce_report_level[TAP_DX_OUTER]) {
							if (finger_press_changed & BIT(i)) {
								ts->tap_suppression &= ~BIT(i);
								ts->tap_suppression |= BIT(i);
								ts->pre_finger_data[i + 1][2] = finger_data[i][0];
								ts->pre_finger_data[i + 1][3] = finger_data[i][1];
								if (ts->reduce_report_level[TAP_TIMEOUT] && (ts->tap_suppression))
									ts->tap_timeout[i] = jiffies + msecs_to_jiffies(ts->reduce_report_level[TAP_TIMEOUT]);
							}
						}

						if (ts->debug_log_level & BIT(1))
							printk(KERN_INFO
								"[TP] Finger %d=> X:%d, Y:%d w:%d, z:%d\n",
								i + 1, finger_data[i][0], finger_data[i][1],
								finger_data[i][2], finger_data[i][3]);
					}
					if (ts->packrat_number < SYNAPTICS_FW_NOCAL_PACKRAT) {
#ifdef SYN_CALIBRATION_CONTROL
						if (ts->multitouch_calibration) {
							if ((finger_release_changed & BIT(i)) && ts->pre_finger_data[0][0] == 1) {
								ret = i2c_syn_write_byte_data(ts->client,
									get_address_base(ts, 0x11, COMMAND_BASE), 0x01);
								if (ret < 0)
									i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:Rezero_2", __func__);
								pr_info("[TP] %s: Touch Calibration Confirmed, rezero\n", __func__);
							}
						}
#endif
					}
					if (!ts->finger_count)
						ts->pre_finger_data[0][0] = 0;
				}
				base += 5;
			}
#ifdef SYN_FILTER_CONTROL
			if (ts->filter_level[0] && ts->grip_suppression) {
				ts->ambiguous_state = 0;
				for (i = 0; i < ts->finger_support; i++)
					if (ts->grip_suppression & BIT(i))
						ts->ambiguous_state++;
			}
			if (ts->debug_log_level & BIT(16))
				pr_info("[TP] ts->grip_suppression: %x, ts->ambiguous_state: %x\n",
					ts->grip_suppression, ts->ambiguous_state);
#endif
		}
	}
	input_sync(ts->input_dev);

}

static void synaptics_ts_report_func(struct synaptics_ts_data *ts)
{
	int ret;
	uint8_t data[2] = {0};

	ret = i2c_syn_write(ts->client,
		get_address_base(ts, 0x54, DATA_BASE) + 1, &data[0], 2);

	if (ret < 0)
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:1", __func__);
	else {
		ret = i2c_syn_read(ts->client,
			get_address_base(ts, 0x54, DATA_BASE) + 3, ts->temp_report_data,
			ts->x_channel * ts->y_channel * 2);
		if (ret >= 0)
			memcpy(&ts->report_data[0], &ts->temp_report_data[0], ts->x_channel * ts->y_channel * 2);
		else {
			memset(&ts->report_data[0], 0x0, sizeof(ts->report_data));
			i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:2", __func__);
		}
	}
	atomic_set(&ts->data_ready, 1);
	wake_up(&syn_data_ready_wq);

}

static void synaptics_ts_button_func(struct synaptics_ts_data *ts)
{
	int ret;
	uint8_t data = 0;
	uint16_t x_position = 0, y_position = 0;

	ret = i2c_syn_read(ts->client,
		get_address_base(ts, 0x1A, DATA_BASE), &data, 1);
	if (data) {
		if (data & 0x01) {
			printk("[TP] back key pressed\n");
			vk_press = 1;
			if (ts->button) {
				if (ts->button[0].index) {
					x_position = (ts->button[0].x_range_min + ts->button[0].x_range_max) / 2;
					y_position = (ts->button[0].y_range_min + ts->button[0].y_range_max) / 2;
				}
			}
			if (ts->htc_event == SYN_AND_REPORT_TYPE_A) {
				input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, 0);
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
					100);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					x_position);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					y_position);
				input_mt_sync(ts->input_dev);
			} else if (ts->htc_event == SYN_AND_REPORT_TYPE_B) {
				input_mt_slot(ts->input_dev, 0);
				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER,
				1);
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
					100);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					x_position);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					y_position);
			}
		}
		else if (data & 0x02) {
			printk("[TP] home key pressed\n");
			vk_press = 1;
			if (ts->button) {
				if (ts->button[1].index) {
					x_position = (ts->button[1].x_range_min + ts->button[1].x_range_max) / 2;
					y_position = (ts->button[1].y_range_min + ts->button[1].y_range_max) / 2;
				}
			}
			if (ts->htc_event == SYN_AND_REPORT_TYPE_A) {
				input_report_abs(ts->input_dev, ABS_MT_TRACKING_ID, 0);
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
					100);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					x_position);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					y_position);
				input_mt_sync(ts->input_dev);
			} else if (ts->htc_event == SYN_AND_REPORT_TYPE_B) {
				input_mt_slot(ts->input_dev, 0);
				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER,
				1);
				input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR,
					100);
				input_report_abs(ts->input_dev, ABS_MT_PRESSURE,
					100);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_X,
					x_position);
				input_report_abs(ts->input_dev, ABS_MT_POSITION_Y,
					y_position);
			}
		}
	}else {
		printk("[TP] virtual key released\n");
		vk_press = 0;
		if (ts->htc_event == SYN_AND_REPORT_TYPE_A) {
			if (ts->support_htc_event) {
				input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
				input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
			}
			input_mt_sync(ts->input_dev);
		}
		else if (ts->htc_event == SYN_AND_REPORT_TYPE_B) {
			input_mt_slot(ts->input_dev, 0);
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
		}
	}
	input_sync(ts->input_dev);
}

static void synaptics_ts_status_func(struct synaptics_ts_data *ts)
{
	int ret;
	uint8_t data = 0;

	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, DATA_BASE), &data, 1);
	if (ret < 0) {
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r", __func__);
	} else {
		data &= 0x0F;
		pr_info("[TP] Device Status = %x\n", data);
		if (data == 1) {
			mutex_lock(&syn_mutex);
			ts->page_select = 0;
			mutex_unlock(&syn_mutex);
			pr_info("[TP] TOUCH: Page Select: %s: %d\n", __func__, ts->page_select);
			ret = synaptics_init_panel(ts);
			if (ret < 0)
				pr_info("[TP]%s: synaptics_init_panel fail\n", __func__);
		}
	}

}

static void synaptics_ts_work_func(struct work_struct *work)
{
	struct synaptics_ts_data *ts = container_of(work, struct synaptics_ts_data, work);
	int ret;
	uint8_t buf = 0;
	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, DATA_BASE) + 1, &buf, 1);

	if (ret < 0) {
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r", __func__);
	} else {
		if (buf & get_address_base(ts, 0x11, INTR_SOURCE))
			synaptics_ts_finger_func(ts);
		if (buf & get_address_base(ts, 0x01, INTR_SOURCE))
			synaptics_ts_status_func(ts);
		if (buf & get_address_base(ts, 0x54, INTR_SOURCE))
			synaptics_ts_report_func(ts);
	}

}

static irqreturn_t synaptics_irq_thread(int irq, void *ptr)
{
	struct synaptics_ts_data *ts = ptr;
	int ret;
	uint8_t buf = 0;
	struct timespec timeStart, timeEnd, timeDelta;

	if (ts->debug_log_level & BIT(2)) {
			getnstimeofday(&timeStart);
	}

	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, DATA_BASE) + 1, &buf, 1);

	if (ret < 0) {
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r", __func__);
	} else {
		if (buf & get_address_base(ts, 0x1A, INTR_SOURCE)) {
			if (!ts->finger_count)
				synaptics_ts_button_func(ts);
			else
				printk("[TP] Ignore VK interrupt due to 2d points did not leave\n");
		}
		if (buf & get_address_base(ts, 0x11, INTR_SOURCE)) {
			if (!vk_press) {
				synaptics_ts_finger_func(ts);
				if(ts->debug_log_level & BIT(2)) {
					getnstimeofday(&timeEnd);
					timeDelta.tv_nsec = (timeEnd.tv_sec*1000000000+timeEnd.tv_nsec)
						-(timeStart.tv_sec*1000000000+timeStart.tv_nsec);
					pr_info("[TP] Touch latency = %ld us\n", timeDelta.tv_nsec/1000);
				}
			}
		}
		if (buf & get_address_base(ts, 0x01, INTR_SOURCE))
			synaptics_ts_status_func(ts);
		if (buf & get_address_base(ts, 0x54, INTR_SOURCE))
			synaptics_ts_report_func(ts);
	}

	return IRQ_HANDLED;
}

static enum hrtimer_restart synaptics_ts_timer_func(struct hrtimer *timer)
{
	struct synaptics_ts_data *ts = container_of(timer, struct synaptics_ts_data, timer);
	queue_work(ts->syn_wq, &ts->work);
	hrtimer_start(&ts->timer, ktime_set(0, 12500000), HRTIMER_MODE_REL);
	return HRTIMER_NORESTART;
}

#ifdef SYN_CABLE_CONTROL
static void cable_tp_status_handler_func(int connect_status)
{
	struct synaptics_ts_data *ts = gl_ts;
	uint8_t data;
	int ret;

	pr_info("[TP] Touch: cable change to %d\n", connect_status);

	if (connect_status)
		connect_status = 1;
	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, CONTROL_BASE), &data, 1);
	if (ret < 0) {
		i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:1", __func__);
	} else {
		ts->cable_config = (data & 0xDF) | (connect_status << 5);
		pr_info("[TP] %s: ts->cable_config: %x\n", __func__, ts->cable_config);
		ret = i2c_syn_write_byte_data(ts->client,
			get_address_base(ts, 0x01, CONTROL_BASE), ts->cable_config);
		if (ret < 0) {
			i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:2", __func__);
		}
	}
}
static struct t_usb_status_notifier cable_status_handler = {
	.name = "usb_tp_connected",
	.func = cable_tp_status_handler_func,
};
#endif

static void synaptics_ts_close_psensor_func(struct work_struct *work)
{
	struct synaptics_ts_data *ts = container_of(work, struct synaptics_ts_data, psensor_work);
	if(ts->psensor_resume_enable == 1) {
		pr_info("[TP] %s: Disable P-sensor by Touch\n", __func__);
		psensor_enable_by_touch_driver(0);
		ts->psensor_resume_enable = 0;
	}
}

static int psensor_tp_status_handler_func(struct notifier_block *this,
	unsigned long status, void *unused)
{
	struct synaptics_ts_data *ts = gl_ts;
	int ret;

	pr_info("[TP] psensor status %d -> %lu\n",
		ts->psensor_status, status);

	if(ts->psensor_detection) {
		if(status == 3 && ts->psensor_resume_enable >= 1) {
			if(!(ts->psensor_status==1 && ts->psensor_resume_enable==1)) {
				ret = i2c_syn_write_byte_data(ts->client, get_address_base(ts, 0x11, COMMAND_BASE), 0x01);
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "w:Rezero_1", __func__);
				pr_info("[TP] %s: Touch Calibration Confirmed, rezero\n", __func__);
			}

			if(ts->psensor_resume_enable == 1)
				queue_work(ts->syn_psensor_wq, &ts->psensor_work);
			else
				ts->psensor_resume_enable = 0;
		}
	}

	if (ts->psensor_status == 0) {
		if (status == 1)
			ts->psensor_status = status;
		else
			ts->psensor_status = 0;
	} else
		ts->psensor_status = status;

	if(ts->psensor_detection) {
		if(ts->psensor_status == 0) {
			ts->psensor_resume_enable = 0;
			ts->psensor_phone_enable = 0;
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block psensor_status_handler = {
	.notifier_call = psensor_tp_status_handler_func,
};

static int syn_pdt_scan(struct synaptics_ts_data *ts, int num_page)
{
	uint8_t intr_count = 0, data[6] = {0}, num_function[SYN_MAX_PAGE] = {0};
	uint16_t i, j, k = 0;
	int ret = 0;
	ts->num_function = 0;

	for (i = 0; i < num_page; i++) {
		for (j = (0xEE | (i << 8)); j >= (0xBE | (i << 8)); j -= 6) {
			ret = i2c_syn_read(ts->client, j, data, 1);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r1", __func__);
			if (data[0] == 0)
				break;
			else
				num_function[i]++;
		}
		ts->num_function += num_function[i];
	}

	if (ts->address_table == NULL) {
		ts->address_table = kzalloc(sizeof(struct function_t) * ts->num_function, GFP_KERNEL);
		if (ts->address_table == NULL) {
			pr_info("[TP] syn_pdt_scan: memory allocate fail\n");
			return -ENOMEM;
		}
		pr_info("[TP] syn_pdt_scan: memory allocate success. ptr: %p\n", ts->address_table);
	}

	pr_info("[TP] synaptics: %d function supported\n", ts->num_function);
	for (i = 0; i < num_page; i++) {
		for (j = 0; j < num_function[i]; j++) {
			ret = i2c_syn_read(ts->client, i << 8 | (0xE9 - 6*j), data, 6);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:2", __func__);
			ts->address_table[j + k].query_base = i << 8 | data[0];
			ts->address_table[j + k].command_base = i << 8 | data[1];
			ts->address_table[j + k].control_base = i << 8 | data[2];
			ts->address_table[j + k].data_base = i << 8 | data[3];
			if (data[4] & 0x07) {
				ts->address_table[j + k].interrupt_source =
					get_int_mask(data[4] & 0x07, intr_count);
				intr_count += (data[4] & 0x07);
			}
			ts->address_table[j + k].function_type = data[5];
			printk(KERN_INFO
				"Query: %2.2X, Command: %4.4X, Control: %2X, Data: %2X, INTR: %2X, Funtion: %2X\n",
				ts->address_table[j + k].query_base , ts->address_table[j + k].command_base,
				ts->address_table[j + k].control_base, ts->address_table[j + k].data_base,
				ts->address_table[j + k].interrupt_source, ts->address_table[j + k].function_type);
		}
		k += num_function[i];
	}
	return ts->num_function;
}
static int syn_get_version(struct synaptics_ts_data *ts)
{
	uint8_t data[16] = {0};
	int ret = 0;
	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, QUERY_BASE) + 17, data, 4);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:1", __func__);
	ts->package_id = data[1] << 8 | data[0];
	pr_info("[TP] %s: package_id: %d\n", __func__, ts->package_id);

	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, QUERY_BASE) + 18, data, 3);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:3", __func__);
	ts->packrat_number = data[2] << 16 | data[1] << 8 | data[0];
	pr_info("[TP] %s: packrat_number: %d\n", __func__, ts->packrat_number);

	if (ts->packrat_number < SYNAPTICS_FW_3_2_PACKRAT) {

		ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, QUERY_BASE) + 16, data, 3);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:2", __func__);
		syn_panel_version = data[0] << 8 | data[2];
		pr_info("[TP] %s: panel_version: %x\n", __func__, syn_panel_version);

	} else {

		ret = i2c_syn_read(ts->client, get_address_base(ts, 0x01, QUERY_BASE) + 28, data, 16);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:2", __func__);
		syn_panel_version = data[5] << 8 | data[7];
		pr_info("[TP] %s: panel_version: %x\n", __func__, syn_panel_version);
	}

	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x34, CONTROL_BASE), data, 4);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:4", __func__);
	ts->config_version = data[0] << 24 | data[1] << 16 | data[2] << 8 | data[3];
	pr_info("[TP] %s: config version: %x\n", __func__, ts->config_version);

	return 0;
}

static int syn_get_information(struct synaptics_ts_data *ts)
{
	uint8_t data[4] = {0}, i, num_channel, *buf;
	int ret = 0;
	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x11, QUERY_BASE) + 1, data, 1);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:1", __func__);
	if ((data[0] & 0x07) == 5)
		ts->finger_support = 10;
	else if ((data[0] & 0x07) < 5)
		ts->finger_support = (data[0] & 0x07) + 1;
	else {
		pr_info("[TP] %s: number of fingers not define: %x\n",
			__func__, data[0] & 0x07);
		return SYN_PROCESS_ERR;
	}

	pr_info("[TP] %s: finger_support: %d\n", __func__, ts->finger_support);

	ret = i2c_syn_read(ts->client, get_address_base(ts, 0x11, CONTROL_BASE) + 6, data, 4);
	if (ret < 0)
		return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:2", __func__);

	ts->max[0] = data[0] | data[1] << 8;
	ts->max[1] = data[2] | data[3] << 8;
	pr_info("[TP] %s: max_x: %d, max_y: %d\n", __func__, ts->max[0], ts->max[1]);

	if (get_address_base(ts, 0x54, FUNCTION)) {
		ret = i2c_syn_read(ts->client, get_address_base(ts, 0x54, QUERY_BASE), data, 2);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:3", __func__);

		ts->y_channel = data[0];
		ts->x_channel = data[1];

		num_channel = ts->y_channel + ts->x_channel;
		buf = kzalloc(num_channel + 1, GFP_KERNEL);
		if (buf == NULL) {
			pr_info("[TP] %s: memory allocate fail\n", __func__);
			return -ENOMEM;
		}
		if (ts->packrat_number < SYNAPTICS_FW_3_2_PACKRAT)
			ret = i2c_syn_read(ts->client, get_address_base(ts, 0x54, CONTROL_BASE) + 17,
				buf, num_channel + 1);
		else
			ret = i2c_syn_read(ts->client, get_address_base(ts, 0x55, CONTROL_BASE),
				buf, num_channel + 1);
		if (ret < 0) {
			kfree(buf);
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:4", __func__);
		}

		for (i = 1; i < num_channel + 1; i++) {
			if (buf[i] == 0xFF) {
				if (i <= num_channel - ts->x_channel)
					ts->y_channel--;
				else
					ts->x_channel--;
			}
		}

		if (buf[0] & 0x01)
			swap(ts->y_channel, ts->x_channel);
		pr_info("[TP] %s: X: %d, Y: %d\n", __func__,
			ts->x_channel, ts->y_channel);
		kfree(buf);

		ts->temp_report_data = kzalloc(2 * ts->x_channel * ts->y_channel, GFP_KERNEL);
		ts->report_data = kzalloc(2 * ts->x_channel * ts->y_channel, GFP_KERNEL);
		if(ts->temp_report_data == NULL || ts->report_data == NULL)
			return -ENOMEM;

		ret = i2c_syn_read(ts->client,
			get_address_base(ts, 0x54, CONTROL_BASE) + 0x10, &ts->relaxation, 1);
		if (ret < 0)
			return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:5", __func__);
		pr_info("[TP] %s: ts->relaxation: %d\n", __func__, ts->relaxation);

	}
	if (ts->packrat_number < SYNAPTICS_FW_NOCAL_PACKRAT) {
		if (ts->large_obj_check) {
			if (ts->package_id == 2200) {
				ret = i2c_syn_read(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x26, &ts->default_large_obj, 1);
			} else {
				ret = i2c_syn_read(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x29, &ts->default_large_obj, 1);
			}
			if (ret < 0)
					return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:6", __func__);
			pr_info("[TP] %s: ts->default_large_obj: %x\n", __func__, ts->default_large_obj);
		}

		if (ts->segmentation_bef_unlock) {
			if (ts->package_id == 2200) {
				ret = i2c_syn_read(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x25, &ts->segmentation_aft_unlock, 1);

			} else {
				ret = i2c_syn_read(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x22, &ts->segmentation_aft_unlock, 1);
			}
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:7", __func__);
			pr_info("[TP] %s: ts->segmentation_aft_unlock: %x\n", __func__, ts->segmentation_aft_unlock);
		}

		if (ts->threshold_bef_unlock) {
			if (ts->package_id == 2200) {
				ret = i2c_syn_read(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x0A, &ts->threshold_aft_unlock, 1);

			} else {
				ret = i2c_syn_read(ts->client,
					get_address_base(ts, 0x11, CONTROL_BASE) + 0x0C, &ts->threshold_aft_unlock, 1);
			}
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:8", __func__);
			pr_info("[TP] %s: ts->z_threshold_aft_unlock: %x\n", __func__, ts->threshold_aft_unlock);
		}

		if (ts->saturation_bef_unlock) {
			ret = i2c_syn_read(ts->client,
				get_address_base(ts, 0x54, CONTROL_BASE) + 0x02, data, 2);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:8", __func__);
			ts->saturation_aft_unlock = (data[1] << 8) | data[0];
			pr_info("[TP] %s: ts->saturation_aft_unlock: %x\n", __func__, ts->saturation_aft_unlock);
		}

		if (ts->PixelTouchThreshold_bef_unlock) {
			ret = i2c_syn_read(ts->client,
				get_address_base(ts, 0x54, CONTROL_BASE) + 0x04, &ts->PixelTouchThreshold_aft_unlock, 1);
			if (ret < 0)
				return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "r:9", __func__);
			pr_info("[TP] %s: ts->PixelTouchThreshold_aft_unlock: %x\n", __func__, ts->PixelTouchThreshold_aft_unlock);
		}
	}

	return 0;
}

static int synaptics_ts_probe(
	struct i2c_client *client, const struct i2c_device_id *id)
{
	struct synaptics_ts_data *ts;
	uint8_t i;
	int ret = 0;
	struct synaptics_i2c_rmi_platform_data *pdata;
	uint8_t data = 0;

	pr_info("[TP] %s: enter", __func__);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		printk(KERN_ERR "[TP] TOUCH_ERR: synaptics_ts_probe: need I2C_FUNC_I2C\n");
		ret = -ENODEV;
		goto err_check_functionality_failed;
	}

	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (ts == NULL) {
		ret = -ENOMEM;
		goto err_alloc_data_failed;
	}
	ts->client = client;
	i2c_set_clientdata(client, ts);
	pdata = client->dev.platform_data;

	if (pdata == NULL) {
		pr_info("[TP] pdata is NULL\n");
		goto err_get_platform_data_fail;
	}

	ret = i2c_syn_read(ts->client, 0x00EE, &data, 1);
	if (ret < 0) {
		pr_info("[TP] No Synaptics chip\n");
		goto err_detect_failed;
	}

	for (i = 0; i < 10; i++) {
		ret = i2c_syn_read(ts->client, SYN_F01DATA_BASEADDR, &data, 1);
		if (ret < 0) {
			i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "read device status failed!", __func__);
			goto err_detect_failed;
		}
		if (data & 0x44) {
			msleep(20);
#ifdef SYN_FLASH_PROGRAMMING_LOG
			pr_info("[TP] synaptics probe: F01_data: %x touch controller stay in bootloader mode!\n", data);
#endif
		} else if (data & 0x40) {
			printk(KERN_ERR "[TP] TOUCH_ERR: synaptics probe: F01_data: %x touch controller stay in bootloader mode!\n", data);
			goto err_detect_failed;
		} else
			break;
	}
	ts->i2c_err_handler_en = pdata->i2c_err_handler_en;
	if (ts->i2c_err_handler_en) {
		ts->gpio_reset = pdata->gpio_reset;
		ts->use_irq = 1;
		if (ts->gpio_reset) {
			ret = gpio_request(ts->gpio_reset, "synaptics_reset");
			if (ret)
				printk(KERN_INFO "[TP]%s: Failed to obtain reset pin: %d. Code: %d.", __func__, ts->gpio_reset, ret);
		}
	}

	if (i == 10) {
		uint8_t num = 0;
		pr_info("[TP] synaptics probe: touch controller doesn't enter UI mode! F01_data: %x\n", data);

		if (syn_pdt_scan(ts, SYN_BL_PAGE) < 0) {
			printk(KERN_ERR "[TP] TOUCH_ERR: PDT scan fail\n");
			goto err_init_failed;
		}

		if (pdata) {
			while (pdata->default_config != 1) {
				if (pdata->default_config == 0) {
					printk(KERN_ERR "[TP] TOUCH_ERR: touch controller stays in bootloader mode "
						"and recovery method doesn't enable\n");
					goto err_init_failed;
				}
				pdata++;
				num++;
			}
			ts->config = pdata->config;

			ret = syn_config_update(ts, pdata->gpio_irq);
			if (ret < 0) {
				printk(KERN_ERR "[TP] TOUCH_ERR: syn_config_update fail\n");
				goto err_init_failed;
			} else if (ret == 0)
				pr_info("[TP] syn_config_update success\n");
			else
				pr_info("[TP] Warning: syn_config_update: the same "
					"config version and CRC but touch controller always stay in bootloader mode\n");
			pdata = pdata - num;
		}

		if (ts->address_table != NULL) {
			kfree(ts->address_table);
			ts->address_table = NULL;
		}
	}

	if (syn_pdt_scan(ts, SYN_MAX_PAGE) < 0) {
		printk(KERN_ERR "[TP] TOUCH_ERR: PDT scan fail\n");
		goto err_init_failed;
	}

	if (syn_get_version(ts) < 0) {
		printk(KERN_ERR "[TP] TOUCH_ERR: syn_get_version fail\n");
		goto err_init_failed;
	}

	if (pdata) {
		while (pdata->packrat_number && pdata->packrat_number > ts->packrat_number) {
			pdata++;
		}

		if (pdata->tw_pin_mask) {
			ts->tw_pin_mask = pdata->tw_pin_mask;
			for (i=0; i<SYN_UPDATE_RETRY_TIMES; i++) {
				ret = syn_get_tw_vendor(ts, pdata->gpio_irq);
				if (ret == 0)
					break;
			}
			if (ret < 0) {
				printk(KERN_ERR "[TP] TOUCH_ERR: syn_get_tw_vendor fail\n");
				goto err_init_failed;
			}
		}

		while (pdata->sensor_id > 0 && pdata->sensor_id != (SENSOR_ID_CHECKING_EN | ts->tw_vendor)) {
			pdata++;
		}

		pr_info("[TP] synaptics_ts_probe: pdata->version = %x, pdata->packrat_number = %d,"
				" pdata->sensor_id = %x\n", pdata->version, pdata->packrat_number, pdata->sensor_id);

		if (!pdata->packrat_number) {
			printk(KERN_ERR "[TP] TOUCH_ERR: get null platform data\n");
				goto err_init_failed;
		}

		ts->power = pdata->power;
		ts->flags = pdata->flags;
		ts->htc_event = pdata->report_type;
		ts->filter_level = pdata->filter_level;
		ts->reduce_report_level = pdata->reduce_report_level;
		ts->gpio_irq = pdata->gpio_irq;
		ts->gpio_reset = pdata->gpio_reset;
		ts->large_obj_check = pdata->large_obj_check;
		ts->support_htc_event = pdata->support_htc_event;
		ts->mfg_flag = pdata->mfg_flag;
		ts->segmentation_bef_unlock = pdata->segmentation_bef_unlock;
		ts->i2c_err_handler_en = pdata->i2c_err_handler_en;
		ts->threshold_bef_unlock = pdata->threshold_bef_unlock;
		ts->saturation_bef_unlock = pdata->saturation_bef_unlock;
		ts->energy_ratio_relaxation = pdata->energy_ratio_relaxation;
		ts->multitouch_calibration = pdata->multitouch_calibration;
		ts->psensor_detection = pdata->psensor_detection;
		ts->PixelTouchThreshold_bef_unlock = pdata->PixelTouchThreshold_bef_unlock;
#ifdef SYN_CABLE_CONTROL
		ts->cable_support = pdata->cable_support; 
#endif
		ts->config = pdata->config;
		if (pdata->virtual_key)
			ts->button = pdata->virtual_key;
	}

#ifndef SYN_DISABLE_CONFIG_UPDATE
	for (i=0; i<SYN_UPDATE_RETRY_TIMES; i++) {
		ret = syn_config_update(ts, pdata->gpio_irq);
		if (ret >= 0)
			break;
	}
	if (ret < 0) {
		printk(KERN_ERR "[TP] TOUCH_ERR: syn_config_update fail\n");
		goto err_init_failed;
	} else if (ret == 0)
		pr_info("[TP] syn_config_update success\n");
	else
		pr_info("[TP] syn_config_update: the same config version and CRC\n");
#else
	if (pdata->tw_pin_mask) {
		ret = disable_flash_programming(ts, 0);
		if (ret < 0) {
			printk(KERN_ERR "[TP] TOUCH_ERR: disable_flash_programming fail\n");
			goto err_init_failed;
		}
	}
#endif
	if (syn_pdt_scan(ts, SYN_MAX_PAGE) < 0) {
		printk(KERN_ERR "[TP] TOUCH_ERR: PDT scan fail\n");
		goto err_init_failed;
	}

#ifndef SYN_DISABLE_CONFIG_UPDATE
	if (pdata->customer_register[CUS_REG_BASE]) {
		ret = i2c_syn_write(ts->client, pdata->customer_register[CUS_REG_BASE],
			&pdata->customer_register[CUS_BALLISTICS_CTRL], CUS_REG_SIZE - 1);
		pr_info("[TP] Loads customer register\n");
	}
#endif

	if (syn_get_information(ts) < 0) {
		printk(KERN_ERR "[TP] TOUCH_ERR: syn_get_information fail\n");
		goto err_syn_get_info_failed;
	}

	if (pdata->abs_x_max  == 0 && pdata->abs_y_max == 0) {
		ts->layout[0] = ts->layout[2] = 0;
		ts->layout[1] = ts->max[0];
		ts->layout[3] = ts->max[1];
	} else {
		ts->layout[0] = pdata->abs_x_min;
		ts->layout[1] = pdata->abs_x_max;
		ts->layout[2] = pdata->abs_y_min;
		ts->layout[3] = pdata->abs_y_max;
	}

	if(pdata->display_width && pdata->display_height){
		pr_info("[TP] Load display resolution: %dx%d\n", pdata->display_width, pdata->display_height);
		ts->width_factor = (pdata->display_width<<SHIFT_BITS)/(ts->layout[1]-ts->layout[0]);
		ts->height_factor = (pdata->display_height<<SHIFT_BITS)/(ts->layout[3]-ts->layout[2]);
	}

	if(get_tamper_sf()==0) {
		ts->debug_log_level |= BIT(3);
		pr_info("[TP] Debug log level=0x%02X\n", ts->debug_log_level);
	}

	if (get_address_base(ts, 0x19, FUNCTION)) {
		ret = i2c_syn_read(ts->client, get_address_base(ts, 0x19, QUERY_BASE) + 1,
			&ts->key_number, 1);
		if (ret < 0) {
			i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "F19 Query fail", __func__);
			goto err_F19_query_failed;
		}
		for (i = 0; i < ts->key_number; i++) {
			ts->key_postion_x[i] =
				(ts->layout[1] - ts->layout[0]) * (i * 2 + 1) / (ts->key_number * 2)
				+ ts->layout[0];
			pr_info("[TP] ts->key_postion_x[%d]: %d\n",
				i, ts->key_postion_x[i]);
		}
		ts->key_postion_y = ts->layout[2] +
			(21 * (ts->layout[3] - ts->layout[2]) / 20);
		pr_info("[TP] ts->key_postion_y: %d\n", ts->key_postion_y);
	}

	ret = synaptics_init_panel(ts);
	if (ret < 0) {
		printk(KERN_ERR "[TP] TOUCH_ERR: synaptics_init_panel fail\n");
		goto err_init_panel_failed;
	}

	init_waitqueue_head(&syn_data_ready_wq);

	if(ts->psensor_detection) {
		INIT_WORK(&ts->psensor_work, synaptics_ts_close_psensor_func);
		ts->syn_psensor_wq = create_singlethread_workqueue("synaptics_psensor_wq");
		if (!ts->syn_psensor_wq)
			goto err_create_wq_failed;
	}

	ret = synaptics_input_register(ts);
	if (ret) {
		printk(KERN_ERR "[TP] TOUCH_ERR: synaptics_ts_probe: "
				"Unable to register %s input device\n",
				ts->input_dev->name);
		goto err_input_register_device_failed;
	}

	gl_ts = ts;

	ts->irq_enabled = 0;
	if (client->irq) {
		ts->use_irq = 1;
		ret = request_threaded_irq(client->irq, NULL, synaptics_irq_thread,
			IRQF_TRIGGER_LOW | IRQF_ONESHOT, client->name, ts);
		if (ret == 0) {
			ts->irq_enabled = 1;
			ret = i2c_syn_read(ts->client,
				get_address_base(ts, 0x01, CONTROL_BASE) + 1, &ts->intr_bit, 1);
			if (ret < 0) {
				free_irq(client->irq, ts);
				i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "get interrupt bit failed", __func__);
				goto err_get_intr_bit_failed;
			}
			pr_info("[TP] %s: interrupt enable: %x\n", __func__, ts->intr_bit);
		} else {
			dev_err(&client->dev, "[TP] TOUCH_ERR: request_irq failed\n");
			ts->use_irq = 0;
		}
	}

	if (!ts->use_irq) {

		ts->syn_wq = create_singlethread_workqueue("synaptics_wq");
		if (!ts->syn_wq)
			goto err_create_wq_failed;

		INIT_WORK(&ts->work, synaptics_ts_work_func);

		hrtimer_init(&ts->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
		ts->timer.function = synaptics_ts_timer_func;
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	ts->early_suspend.level = EARLY_SUSPEND_LEVEL_STOP_DRAWING - 1;
	ts->early_suspend.suspend = synaptics_ts_early_suspend;
	ts->early_suspend.resume = synaptics_ts_late_resume;
	register_early_suspend(&ts->early_suspend);
#endif

#ifdef SYN_CABLE_CONTROL
	if (ts->cable_support) {
		usb_register_notifier(&cable_status_handler);
		
		ret = i2c_syn_read(ts->client,
			get_address_base(ts, 0x11, CONTROL_BASE), &ts->cable_config, 1);
		if (ret < 0) {
			printk(KERN_ERR "[TP] TOUCH_ERR: get cable config failed\n");
			goto err_get_cable_config_failed;
		}
		if (usb_get_connect_type())
			cable_tp_status_handler_func(1);
		pr_info("[TP] %s: ts->cable_config: %x\n", __func__, ts->cable_config);
	}
#endif
	register_notifier_by_psensor(&psensor_status_handler);
	synaptics_touch_sysfs_init();
#ifdef SYN_WIRELESS_DEBUG
	if (rmi_char_dev_register())
		pr_info("[TP] %s: error register char device", __func__);
#endif

	pr_info("[TP] synaptics_ts_probe: Start touchscreen %s in %s mode\n", ts->input_dev->name, ts->use_irq ? "interrupt" : "polling");

	return 0;

#ifdef SYN_CABLE_CONTROL
err_get_cable_config_failed:
	if (ts->use_irq)
		free_irq(client->irq, ts);
	else
		destroy_workqueue(ts->syn_wq);
#endif

err_create_wq_failed:

err_get_intr_bit_failed:
err_input_register_device_failed:
	input_free_device(ts->input_dev);

err_init_panel_failed:
err_F19_query_failed:
err_syn_get_info_failed:
	if(ts->report_data != NULL)
		kfree(ts->report_data);
	if(ts->temp_report_data != NULL)
		kfree(ts->temp_report_data);
err_init_failed:
	if(ts->address_table != NULL)
		kfree(ts->address_table);
err_detect_failed:
err_get_platform_data_fail:
	kfree(ts);

err_alloc_data_failed:
err_check_functionality_failed:
	return ret;
}

static int synaptics_ts_remove(struct i2c_client *client)
{
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
	unregister_early_suspend(&ts->early_suspend);
	if (ts->use_irq)
		free_irq(client->irq, ts);
	else {
		hrtimer_cancel(&ts->timer);
		if (ts->syn_wq)
			destroy_workqueue(ts->syn_wq);
	}
	if(ts->sr_input_dev != NULL)
		input_unregister_device(ts->sr_input_dev);
	input_unregister_device(ts->input_dev);

	synaptics_touch_sysfs_remove();

	if(ts->report_data != NULL)
		kfree(ts->report_data);
	if(ts->temp_report_data != NULL)
		kfree(ts->temp_report_data);
	if(ts->address_table != NULL)
		kfree(ts->address_table);
	kfree(ts);
	return 0;
}

static int synaptics_ts_suspend(struct i2c_client *client, pm_message_t mesg)
{
	int ret;
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
     scr_suspended = true;
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE      
	if (s2w_switch > 0) {
	      
		enable_irq_wake(client->irq);
		printk(KERN_INFO "[sweep2wake]: suspend but keep interupt wake going.\n");
		if (s2w_switch == 2) {
			//ensure backlight is turned off
			//pm8921_led_current_set(sweep2wake_leddev, 0);
			printk(KERN_INFO "[sweep2wake]: deactivated button backlight.\n");
		}
 	} 
#endif
	printk(KERN_INFO "[TP] %s: enter\n", __func__);

	if (ts->use_irq) {
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
                if (s2w_switch == 0) {
#endif
		disable_irq(client->irq);
		ts->irq_enabled = 0;
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
		}
#endif
	} else {
		hrtimer_cancel(&ts->timer);
		ret = cancel_work_sync(&ts->work);
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
        if (s2w_switch == 0) {
                if (ret && ts->use_irq) /* if work was pending disable-count is now 2 */
                        enable_irq(client->irq);
        }
#endif
	}

	if(ts->psensor_detection) {
		if(ts->psensor_resume_enable == 1){
			pr_info("[TP] %s: Disable P-sensor by Touch\n", __func__);
			psensor_enable_by_touch_driver(0);
			ts->psensor_resume_enable = 0;
		}
	}

	if (ts->psensor_status == 0) {
		ts->pre_finger_data[0][0] = 0;
		if (ts->packrat_number < SYNAPTICS_FW_NOCAL_PACKRAT) {
			ts->first_pressed = 0;

#ifdef SYN_CALIBRATION_CONTROL
			if (ts->mfg_flag != 1) {
				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x54, CONTROL_BASE) + 0x10, ts->relaxation);
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "fast relaxation", __func__);

				if (ts->energy_ratio_relaxation) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x54, CONTROL_BASE), 0x20);
					if (ret < 0)
						i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "energy ratio relaxation", __func__);
				}

				if (ts->saturation_bef_unlock) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x54, CONTROL_BASE) + 0x02, ts->saturation_bef_unlock & 0xFF);
					if (ret < 0)
						return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "saturation capacitance", __func__);
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x54, CONTROL_BASE) + 0x03, (ts->saturation_bef_unlock & 0xFF00) >> 8);
					if (ret < 0)
						return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "saturation capacitance", __func__);
					pr_info("[TP] touch suspend, saturation capacitance: %x\n", ts->saturation_bef_unlock);
				}

				if ( ts->PixelTouchThreshold_bef_unlock ) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x54, CONTROL_BASE) + 0x04, ts->PixelTouchThreshold_bef_unlock );
					if (ret < 0)
						return i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "F54_ANALOG_CTRL03 Pixel Touch Threshold", __func__);
					pr_info("[TP] touch suspend, set F54_ANALOG_CTRL03 Pixel Touch Threshold: %x\n", ts->PixelTouchThreshold_bef_unlock);
				}

				ret = i2c_syn_write_byte_data(ts->client,
					get_address_base(ts, 0x54, COMMAND_BASE), 0x04);
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "force update", __func__);
				pr_info("[TP] touch suspend, fast relasxation: %x\n", ts->relaxation);
			}
#endif

			if (ts->large_obj_check) {
				if (ts->package_id == 2200) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x26, ts->default_large_obj & 0x7F);

				} else {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x29, ts->default_large_obj & 0x7F);
				}
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "large obj suppression", __func__);
				pr_info("[TP] touch suspend, set large obj suppression: %x\n", ts->default_large_obj & 0x7F);
			}

			if (ts->segmentation_bef_unlock) {
				if (ts->package_id == 2200) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x25, ts->segmentation_bef_unlock);
				} else {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x22, ts->segmentation_bef_unlock);
				}
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "segmentation aggressiveness", __func__);
				pr_info("[TP] touch suspend, set segmentation aggressiveness: %x\n", ts->segmentation_bef_unlock);
			}

			if (ts->threshold_bef_unlock) {
				if (ts->package_id == 2200) {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x0A, ts->threshold_bef_unlock);

				} else {
					ret = i2c_syn_write_byte_data(ts->client,
						get_address_base(ts, 0x11, CONTROL_BASE) + 0x0C, ts->threshold_bef_unlock);
				}
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "Z Touch threshold", __func__);
				pr_info("[TP] touch suspend, set Z Touch threshold: %x\n", ts->threshold_bef_unlock);
			}
		}
	}
	else if(ts->psensor_detection)
			ts->psensor_phone_enable = 1;
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
        if (s2w_switch == 0) {
#endif
	if (ts->packrat_number < SYNAPTICS_FW_NOCAL_PACKRAT)
		pr_info("[TP][PWR][STATE] get power key state = %d\n", getPowerKeyState());
	if (ts->power)
		ts->power(0);
	else {
		if (ts->packrat_number >= SYNAPTICS_FW_NOCAL_PACKRAT) {
			ret = i2c_syn_write_byte_data(client,
					get_address_base(ts, 0x01, CONTROL_BASE), 0x01); 
			if (ret < 0)
				i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "sleep: 0x01", __func__);
		} else {
			if (ts->psensor_status > 0 && getPowerKeyState() == 0) {
				ret = i2c_syn_write_byte_data(client,
					get_address_base(ts, 0x01, CONTROL_BASE), 0x02); 
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "sleep: 0x02", __func__);
			} else {
				ret = i2c_syn_write_byte_data(client,
					get_address_base(ts, 0x01, CONTROL_BASE), 0x01); 
				if (ret < 0)
					i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "sleep: 0x01", __func__);
			}
		}
	}
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
        }
#endif

	return 0;
}

static int synaptics_ts_resume(struct i2c_client *client)
{
	int ret, i;
	struct synaptics_ts_data *ts = i2c_get_clientdata(client);
     scr_suspended = true;
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE  
        if (s2w_switch > 0) {
                //screen on, disable_irq_wake
                scr_suspended = false;
                disable_irq_wake(client->irq);
        }
#endif
	printk(KERN_INFO "[TP] %s: enter\n", __func__);
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
        if (s2w_switch == 0) {
#endif
	if (ts->power) {
		ts->power(1);
		msleep(100);
#ifdef SYN_CABLE_CONTROL
		if (ts->cable_support) {
			if (usb_get_connect_type())
				cable_tp_status_handler_func(1);
			pr_info("%s: ts->cable_config: %x\n", __func__, ts->cable_config);
		}
#endif
	} else {
		ret = i2c_syn_write_byte_data(client,
			get_address_base(ts, 0x01, CONTROL_BASE), 0x00); 
		if (ret < 0)
			i2c_syn_error_handler(ts, ts->i2c_err_handler_en, "wake up", __func__);
	}
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
        }
#endif
	if (ts->htc_event == SYN_AND_REPORT_TYPE_A) {
		if (ts->support_htc_event) {
			input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
			input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
			input_sync(ts->input_dev);
		}
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_sync(ts->input_dev);
	} else if (ts->htc_event == SYN_AND_REPORT_TYPE_B) {
		if (ts->package_id >= 3400) {
			for (i = 0; i < ts->finger_support; i++) {
				input_mt_slot(ts->input_dev, i);
				input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, 0);
				input_sync(ts->input_dev);
				
			}
			ts->tap_suppression = 0;
			ts->finger_pressed = 0;
		}
	} else if (ts->htc_event == SYN_AND_REPORT_TYPE_HTC) {
		input_report_abs(ts->input_dev, ABS_MT_AMPLITUDE, 0);
		input_report_abs(ts->input_dev, ABS_MT_POSITION, 1 << 31);
	}
	if (ts->psensor_detection) {
		if(ts->psensor_status == 0) {
			ts->psensor_resume_enable = 1;
			pr_info("[TP] %s: Enable P-sensor by Touch\n", __func__);
			psensor_enable_by_touch_driver(1);
		}
		else if(ts->psensor_phone_enable == 0) {
			if(ts->psensor_status != 3)
				ts->psensor_resume_enable = 2;

			ts->psensor_phone_enable = 1;
		}
	}
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
        if (s2w_switch == 0) {
#endif
	if (ts->use_irq) {
		enable_irq(client->irq);
		ts->irq_enabled = 1;
	}
	else
		hrtimer_start(&ts->timer, ktime_set(1, 0), HRTIMER_MODE_REL);
#ifdef CONFIG_TOUCHSCREEN_SYNAPTICS_SWEEP2WAKE
	}
#endif
	return 0;
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void synaptics_ts_early_suspend(struct early_suspend *h)
{
	struct synaptics_ts_data *ts;
	ts = container_of(h, struct synaptics_ts_data, early_suspend);
	synaptics_ts_suspend(ts->client, PMSG_SUSPEND);
}

static void synaptics_ts_late_resume(struct early_suspend *h)
{
	struct synaptics_ts_data *ts;
	ts = container_of(h, struct synaptics_ts_data, early_suspend);
	synaptics_ts_resume(ts->client);
}
#endif

static const struct i2c_device_id synaptics_ts_id[] = {
	{ SYNAPTICS_3200_NAME, 0 },
	{ }
};

static struct i2c_driver synaptics_ts_driver = {
	.probe		= synaptics_ts_probe,
	.remove		= synaptics_ts_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend	= synaptics_ts_suspend,
	.resume		= synaptics_ts_resume,
#endif
	.id_table	= synaptics_ts_id,
	.driver = {
		.name	= SYNAPTICS_3200_NAME,
	},
};

static int __devinit synaptics_ts_init(void)
{
	return i2c_add_driver(&synaptics_ts_driver);
}

static void __exit synaptics_ts_exit(void)
{
	i2c_del_driver(&synaptics_ts_driver);
}

module_init(synaptics_ts_init);
module_exit(synaptics_ts_exit);

MODULE_DESCRIPTION("Synaptics Touchscreen Driver");
MODULE_LICENSE("GPL");
