/*
 * palmas-vac.c -- TI PALMAS vac.
 *
 * Copyright (c) 2013, NVIDIA Corporation. All rights reserved.
 *
 * Author: Pradeep Goudagunta <pgoudagunta@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any kind,
 * whether express or implied; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */
#include <linux/module.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/pm.h>
#include <linux/mfd/palmas.h>
#include <linux/gpio_keys.h>
#include <linux/switch.h>
#include <asm/gpio.h>
#include <linux/workqueue.h>
#include <linux/fb.h>
#include <linux/notifier.h>
#include "../../arch/arm/mach-tegra/gpio-names.h"

#define MOD_NAME "palmas-autoadc"

#define BLANK		1
#define UNBLANK		0

#define GPADC_ADOVE_THRESHOLD 0
#define GPADC_BELOW_THRESHOLD 1

#define GPADC_CONV_MODE_SW
/*#define GPADC_CONV_MODE_AUTO*/

#define SW_DEBUG

#ifdef SW_DEBUG
#define LOG(format, arg...) printk(KERN_DEBUG format, ##arg)
#else
#define LOG(format, arg...)
#endif

#ifdef GPADC_CONV_MODE_SW
#include <linux/iio/consumer.h>
#include <linux/iio/types.h>
#endif

static char usb_name[30] = "null";
struct palmas_autoadc {
	struct device	*dev;
	struct palmas	*palmas;
#ifdef GPADC_CONV_MODE_SW
	struct iio_channel *channel;
#endif
#ifdef GPADC_CONV_MODE_AUTO
	int	irq;
#endif
	struct switch_dev sdev;
	struct delayed_work work;
	struct mutex irq_lock;
	struct notifier_block fb_notif;
	struct palmas_autoadc_platform_data *pdata;
	int    dock_keyboard;
};

#ifdef GPADC_CONV_MODE_AUTO
static int palmas_autoadc_set_threshold(struct palmas_autoadc *autoadc, int gpadc_threshold)
{
	unsigned int val[2];

	val[0] = 0xff & autoadc->pdata->conversion_0_threshold;
	val[1] = (gpadc_threshold << 7) | (autoadc->pdata->conversion_0_threshold >> 8);

	palmas_write(autoadc->palmas, PALMAS_GPADC_BASE,
				PALMAS_GPADC_THRES_CONV0_LSB, val[0]);
	palmas_write(autoadc->palmas, PALMAS_GPADC_BASE,
				PALMAS_GPADC_THRES_CONV0_MSB, val[1]);

	return 0;
}

static int palmas_autoadc_read(struct palmas_autoadc *autoadc)
{
	int ret;
	unsigned int lsb, msb;
	ret = palmas_read(autoadc->palmas, PALMAS_GPADC_BASE,
			PALMAS_GPADC_AUTO_CONV0_LSB, &lsb);
	ret = palmas_read(autoadc->palmas, PALMAS_GPADC_BASE,
			PALMAS_GPADC_AUTO_CONV0_MSB, &msb);
	if (ret < 0) {
		dev_err(autoadc->dev, "ADCDATA read failed: %d\n", ret);
		return ret;
	}

	ret = (((msb<<8)|lsb) & 0xFFF);
	return ret;
}
#endif

static void autoadc_work_func(struct work_struct *work)
{
	struct palmas_autoadc *adc =
		container_of(work, struct palmas_autoadc, work.work);
	int i, val, volt_min, volt_max;
#ifdef GPADC_CONV_MODE_AUTO
	int threshold = GPADC_BELOW_THRESHOLD;
#endif
#ifdef GPADC_CONV_MODE_SW
	int val2,ret;
#endif
	int usb5v_enable = false;

	mutex_lock(&adc->irq_lock);

#ifdef GPADC_CONV_MODE_AUTO
	val = palmas_autoadc_read(adc);
	if (val < adc->pdata->conversion_0_threshold)
		threshold = GPADC_ADOVE_THRESHOLD;
#endif
#ifdef GPADC_CONV_MODE_SW
	ret = iio_st_read_channel_raw(adc->channel, &val, &val2);
	if (ret < 0) {
		dev_err(adc->dev, "%s: Failed to read channel, %d\n",
				__func__, ret);
	}
#endif

	for (i = 0; i < adc->pdata->hid_dev_num; i++) {
		volt_min = adc->pdata->hid_dev[i].volt2adc - adc->pdata->hid_dev[i].adc_limit;
		volt_max = adc->pdata->hid_dev[i].volt2adc + adc->pdata->hid_dev[i].adc_limit;
		pr_debug("sw_adc--> v=%d,[%d:%d], dock:%d", val, volt_min, volt_max, adc->dock_keyboard);

		if ((val > volt_min) && (val < volt_max)) {
			if (adc->pdata->hid_dev[i].init_switch_gpio)
				adc->pdata->hid_dev[i].init_switch_gpio();
			memcpy(usb_name, adc->pdata->hid_dev[i].name, sizeof(usb_name));
			usb5v_enable = true;
		}
	}

	if (!memcmp(usb_name, "mic", 3))
		switch_set_state(&adc->sdev, usb5v_enable);

	if (adc->pdata->usb5v_enable && (!memcmp(usb_name, "keyboard", 8)))	{
		adc->pdata->usb5v_enable(usb5v_enable);
		adc->dock_keyboard = usb5v_enable;
	}

	if (!usb5v_enable) {
		memset(usb_name, '\0', sizeof(usb_name));
		memcpy(usb_name, "null", sizeof(usb_name));
		adc->dock_keyboard = 0;
	}

#ifdef GPADC_CONV_MODE_AUTO
	palmas_autoadc_set_threshold(adc, threshold);
#endif

#ifdef GPADC_CONV_MODE_SW
	schedule_delayed_work(&adc->work, msecs_to_jiffies(200));
#endif
	mutex_unlock(&adc->irq_lock);
}

#ifdef GPADC_CONV_MODE_AUTO
static irqreturn_t palmas_autoadc_irq(int irq, void *data)
{
	struct palmas_autoadc *adc = (struct palmas_autoadc *)data;

	schedule_delayed_work(&adc->work, msecs_to_jiffies(150));
	return IRQ_HANDLED;
}
#endif

static int palmas_autoadc_start(struct palmas_autoadc *autoadc, int enable)
{
	unsigned int val, mask;
	int ret;

	val = (enable ? 0 : 1) << PALMAS_INT3_STATUS_GPADC_AUTO_0_SHIFT;
	ret = palmas_update_bits(autoadc->palmas, PALMAS_INTERRUPT_BASE,
				PALMAS_INT3_MASK,
				PALMAS_INT3_STATUS_GPADC_AUTO_0, val);
	if (ret < 0) {
		dev_err(autoadc->dev, "CTRL1 update failed: %d\n", ret);
		return ret;
	}

	mask = PALMAS_GPADC_CTRL1_GPADC_FORCE;
	ret = palmas_update_bits(autoadc->palmas, PALMAS_GPADC_BASE,
			PALMAS_GPADC_CTRL1, mask, enable);
	if (ret < 0) {
		dev_err(autoadc->dev, "CTRL1_GPADC_FORCE update failed: %d\n", ret);
		return ret;
	}

	//channel 1
	ret = palmas_write(autoadc->palmas, PALMAS_GPADC_BASE,
				PALMAS_GPADC_AUTO_SELECT, autoadc->pdata->adc_channel);
	if (ret < 0) {
		dev_err(autoadc->dev, "PALMAS_GPADC_AUTO_SELECT update failed: %d\n", ret);
		return ret;
	}

	mask = PALMAS_GPADC_AUTO_CTRL_AUTO_CONV0_EN | PALMAS_GPADC_AUTO_CTRL_COUNTER_CONV_MASK;
	val = (enable << PALMAS_GPADC_AUTO_CTRL_AUTO_CONV0_EN_SHIFT) | autoadc->pdata->delay_time;
	ret = palmas_update_bits(autoadc->palmas, PALMAS_GPADC_BASE,
				PALMAS_GPADC_AUTO_CTRL, mask, val);
	if (ret < 0) {
		dev_err(autoadc->dev, "AUTO_CTRL_AUTO_CONV0_EN update failed: %d\n", ret);
		return ret;
	}

	return 0;
}

#if 0
static ssize_t switch_gpio_print_state(struct switch_dev *sdev, char *buf)
{
	struct palmas_autoadc *switch_data =
		container_of(sdev, struct palmas_autoadc, sdev);
	const char *state;
	if (switch_get_state(sdev))
		state = switch_data->usb_name;
	else
		state = "no device!";

	if (state)
		return sprintf(buf, "%s\n", state);
	return -1;
}
#endif

static int fb_notifier_callback(struct notifier_block *self,
				unsigned long event, void *data)
{

	struct fb_event *fb_event = data;
	int *blank = fb_event->data;
	int fb_status = *blank ? BLANK : UNBLANK;
	struct palmas_autoadc  * adc = container_of(self, struct palmas_autoadc, fb_notif);
	if(adc == NULL) return 0;

	if( adc->dock_keyboard ){
		if (fb_status == BLANK) {
			pr_debug("fb notifiler usb5v disable! ");
			gpio_set_value(TEGRA_GPIO_PO3, 0);
			gpio_set_value(TEGRA_GPIO_PY3, 1);
		} else {
			pr_debug("fb notifiler usb5v enable! ");
			gpio_set_value(TEGRA_GPIO_PO3, 1);
			gpio_set_value(TEGRA_GPIO_PY3, 0);
		}
	}
	if (fb_status == BLANK) {
		cancel_delayed_work(&adc->work);
		pr_debug("fb notifiler stop adc polling! ");
	}else {
		schedule_delayed_work(&adc->work, msecs_to_jiffies(200));
		pr_debug("fb notifiler adc polling! ");
	}

	return 0;
}

static ssize_t palmas_dock_name_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", usb_name);
}

static DEVICE_ATTR(dock_name, S_IRUGO, palmas_dock_name_show, NULL);

static int __devinit palmas_autoadc_probe(struct platform_device *pdev)
{
	int ret;
	struct palmas_autoadc *palmas_autoadc = NULL;
	struct palmas_platform_data *palmas_pdata;
	struct palmas_autoadc_platform_data *autoadc_pdata = NULL;

	LOG("func:%s", __func__);

	palmas_autoadc = devm_kzalloc(&pdev->dev, sizeof(struct palmas_autoadc), GFP_KERNEL);
    if (!palmas_autoadc) {
		dev_err(&pdev->dev, "Memory allocation failed.\n");
		return -ENOMEM;
	}
	palmas_pdata = dev_get_platdata(pdev->dev.parent);
	if (palmas_pdata)
		autoadc_pdata = palmas_pdata->autoadc_pdata;

	palmas_autoadc->dev = &pdev->dev;
	palmas_autoadc->pdata = autoadc_pdata;
	palmas_autoadc->palmas = dev_get_drvdata(pdev->dev.parent);
	dev_set_drvdata(&pdev->dev, palmas_autoadc);

	palmas_autoadc->sdev.name = "h3w";
	switch_dev_register(&palmas_autoadc->sdev);

	if (autoadc_pdata && autoadc_pdata->switch_gpio) {
		ret = gpio_request(autoadc_pdata->switch_gpio, "switch_gpio");
		if (ret) {
			printk("failed to request GPIO%d\n", autoadc_pdata->switch_gpio);
			goto switch_gpio_failed;
		}
	}

	INIT_DELAYED_WORK(&palmas_autoadc->work, autoadc_work_func);
	mutex_init(&palmas_autoadc->irq_lock);
#ifdef GPADC_CONV_MODE_AUTO
	palmas_autoadc->irq = platform_get_irq(pdev, 0);
	ret = request_threaded_irq(palmas_autoadc->irq, NULL,
		palmas_autoadc_irq,
		IRQF_ONESHOT | IRQF_EARLY_RESUME, dev_name(palmas_autoadc->dev),
		palmas_autoadc);
	if (ret < 0) {
		dev_err(palmas_autoadc->dev,
			"request irq %d failed: %dn", palmas_autoadc->irq, ret);
		goto request_irq_failed;
	}

	palmas_autoadc_set_threshold(palmas_autoadc, GPADC_BELOW_THRESHOLD);
	palmas_autoadc_start(palmas_autoadc, true);
#endif

#ifdef GPADC_CONV_MODE_SW
	palmas_autoadc->channel = iio_st_channel_get("swich_adc", "padadc");
	if (IS_ERR(palmas_autoadc->channel)) {
		dev_err(palmas_autoadc->dev, "%s: Failed to get channel switch\n",
				__func__);
	}

#endif

	memset(&palmas_autoadc->fb_notif, 0, sizeof(palmas_autoadc->fb_notif));
	palmas_autoadc->fb_notif.notifier_call = fb_notifier_callback;
	fb_register_client(&palmas_autoadc->fb_notif);

	device_create_file(palmas_autoadc->sdev.dev, &dev_attr_dock_name);

	if (palmas_autoadc->pdata->wakeup)
		device_set_wakeup_capable(&pdev->dev, 1);

#ifdef GPADC_CONV_MODE_SW
	schedule_delayed_work(&palmas_autoadc->work, msecs_to_jiffies(1000));
#endif
	LOG("func:%s, ok!", __func__);
	return 0;

#ifdef GPADC_CONV_MODE_AUTO
request_irq_failed:
	gpio_free(palmas_autoadc->pdata->switch_gpio);
#endif
switch_gpio_failed:
	return ret;
}

static int __devexit palmas_autoadc_remove(struct platform_device *pdev)
{
	struct palmas_autoadc *palmas_autoadc = dev_get_platdata(&pdev->dev);

	gpio_free(palmas_autoadc->pdata->switch_gpio);
#ifdef GPADC_CONV_MODE_AUTO
	free_irq(palmas_autoadc->irq, palmas_autoadc);
#endif
	palmas_autoadc_start(palmas_autoadc, false);
	cancel_delayed_work_sync(&palmas_autoadc->work);
	switch_dev_unregister(&palmas_autoadc->sdev);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int palmas_autoadc_suspend(struct device *dev)
{
#if defined(GPADC_CONV_MODE_AUTO) || defined(GPADC_CONV_MODE_SW)
	struct palmas_autoadc *adc = dev_get_drvdata(dev);
#endif
#ifdef GPADC_CONV_MODE_SW
	cancel_delayed_work_sync(&adc->work);
#endif
	if (!memcmp(usb_name, "keyboard", 8)) {
		gpio_set_value(TEGRA_GPIO_PO3, 0);
		gpio_set_value(TEGRA_GPIO_PY3, 1);
	}

#ifdef GPADC_CONV_MODE_AUTO
	if (device_may_wakeup(dev))
		enable_irq_wake(adc->irq);
#endif

	return 0;
}

static int palmas_autoadc_resume(struct device *dev)
{
#if defined(GPADC_CONV_MODE_AUTO) || defined(GPADC_CONV_MODE_SW)
	struct palmas_autoadc *adc = dev_get_drvdata(dev);
#endif
#ifdef GPADC_CONV_MODE_SW
	schedule_delayed_work(&adc->work, msecs_to_jiffies(1000));
#endif
	if (!memcmp(usb_name, "keyboard", 8)) {
		gpio_set_value(TEGRA_GPIO_PO3, 1);
		gpio_set_value(TEGRA_GPIO_PY3, 0);
	}

#ifdef GPADC_CONV_MODE_AUTO
	if (device_may_wakeup(dev))
		disable_irq_wake(adc->irq);
#endif

	return 0;
};

static void palmas_autoadc_shutdown(struct platform_device *pdev)
{
	struct palmas_autoadc *adc = dev_get_drvdata(&pdev->dev);
	palmas_autoadc_start(adc, false);
}
#endif

static const struct dev_pm_ops palmas_pm_ops = {
	.suspend = palmas_autoadc_suspend,
	.resume = palmas_autoadc_resume,
};

static struct platform_driver palmas_autoadc_driver = {
	.probe = palmas_autoadc_probe,
	.remove = __devexit_p(palmas_autoadc_remove),
	.shutdown = palmas_autoadc_shutdown,
	.driver = {
		.name = MOD_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_PM_SLEEP
		.pm = &palmas_pm_ops,
#endif
	},
};

static int __init palmas_init(void)
{
	return platform_driver_register(&palmas_autoadc_driver);
}
module_init(palmas_init);

MODULE_DESCRIPTION("palmas autoadc driver");
MODULE_AUTHOR("Pradeep june<june.cheng@qucii.com>");
MODULE_ALIAS("platform:palmas-autoadc");
MODULE_LICENSE("GPL v2");
