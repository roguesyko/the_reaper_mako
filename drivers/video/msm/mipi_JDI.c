/* Copyright (c) 2010-2012, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "msm_fb.h"
#include "mipi_dsi.h"
#include "mipi_JDI.h"

#include "../board-8064.h"
#include <asm/mach-types.h>
#include <linux/pwm.h>
#include <linux/mfd/pm8xxx/pm8921.h>
#include <linux/gpio.h>
#include <mach/board_asustek.h>
#include <linux/syscore_ops.h>

#define PWM_FREQ_HZ 300
#define PWM_PERIOD_USEC (USEC_PER_SEC / PWM_FREQ_HZ)
#define PWM_LEVEL 255
#define PWM_DUTY_LEVEL \
	(PWM_PERIOD_USEC / PWM_LEVEL)

#define gpio_EN_VDD_BL PM8921_GPIO_PM_TO_SYS(23)
#define gpio_LCD_BL_EN_SR1 PM8921_GPIO_PM_TO_SYS(30)
#define gpio_LCD_BL_EN_SR2 PM8921_GPIO_PM_TO_SYS(36)
#define gpio_LCM_XRES_SR1 36	/* JDI reset pin */
#define gpio_LCM_XRES_SR2 54	/* JDI reset pin */
#define gpio_PWM PM8921_GPIO_PM_TO_SYS(26)

static int gpio_LCD_BL_EN = gpio_LCD_BL_EN_SR2;
static int gpio_LCM_XRES = gpio_LCM_XRES_SR2;
static bool first = true;
static unsigned gpio;
static struct pm_gpio config;

static struct mipi_dsi_panel_platform_data *mipi_JDI_pdata;
static struct pwm_device *bl_lpm;

static struct dsi_buf JDI_tx_buf;
static struct dsi_buf JDI_rx_buf;
static int mipi_JDI_lcd_init(void);

static char sw_reset[2] = {0x01, 0x00}; /* DTYPE_DCS_WRITE */
static char enter_sleep[2] = {0x10, 0x00}; /* DTYPE_DCS_WRITE */
static char exit_sleep[2] = {0x11, 0x00}; /* DTYPE_DCS_WRITE */
static char display_off[2] = {0x28, 0x00}; /* DTYPE_DCS_WRITE */
static char display_on[2] = {0x29, 0x00}; /* DTYPE_DCS_WRITE */

static char MCAP[2] = {0xB0, 0x00};
static char interface_setting[6] = {0xB3, 0x04, 0x08, 0x00, 0x22, 0x00};
static char interface_ID_setting[2] = {0xB4, 0x0C};
static char DSI_control[3] = {0xB6, 0x3A, 0xD3};
static char set_pixel_format[2] = {0x3A, 0x77};
static char set_column_addr[5] = {0x2A, 0x00, 0x00, 0x04, 0xAF};
static char set_page_addr[5] = {0x2B, 0x00, 0x00, 0x07, 0x7F};

static struct dsi_cmd_desc JDI_display_on_cmds[] = {
	{DTYPE_DCS_WRITE, 1, 0, 0, 5,
		sizeof(sw_reset), sw_reset},
	{DTYPE_GEN_WRITE2, 1, 0, 0, 0,
		sizeof(MCAP), MCAP},
	{DTYPE_GEN_LWRITE, 1, 0, 0, 0,
		sizeof(interface_setting), interface_setting},
	{DTYPE_GEN_LWRITE, 1, 0, 0, 0,
		sizeof(interface_ID_setting), interface_ID_setting},
	{DTYPE_GEN_LWRITE, 1, 0, 0, 0,
		sizeof(DSI_control), DSI_control},
	{DTYPE_DCS_WRITE1, 1, 0, 0, 0,
		sizeof(set_pixel_format), set_pixel_format},
	{DTYPE_DCS_LWRITE, 1, 0, 0, 0,
		sizeof(set_column_addr), set_column_addr},
	{DTYPE_DCS_LWRITE, 1, 0, 0, 0,
		sizeof(set_page_addr), set_page_addr},
	{DTYPE_DCS_WRITE, 1, 0, 0, 120,
		sizeof(exit_sleep), exit_sleep},
	{DTYPE_DCS_WRITE, 1, 0, 0, 50,
		sizeof(display_on), display_on},
};

static struct dsi_cmd_desc JDI_display_off_cmds[] = {
	{DTYPE_DCS_WRITE, 1, 0, 0, 20,
		sizeof(display_off), display_off},
	{DTYPE_DCS_WRITE, 1, 0, 0, 80,
		sizeof(enter_sleep), enter_sleep},
};

struct dcs_cmd_req cmdreq_JDI;

static int mipi_JDI_lcd_on(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;

	pr_info("%s+\n", __func__);

	mfd = platform_get_drvdata(pdev);
	if (!mfd)
		return -ENODEV;
	if (mfd->key != MFD_KEY)
		return -EINVAL;

	if (first)	/* change first in setbacklight */
		return 0;

	msleep(20);

	pr_info("%s, JDI display on command+\n", __func__);
	cmdreq_JDI.cmds = JDI_display_on_cmds;
	cmdreq_JDI.cmds_cnt = ARRAY_SIZE(JDI_display_on_cmds);
	cmdreq_JDI.flags = CMD_REQ_COMMIT;
	cmdreq_JDI.rlen = 0;
	cmdreq_JDI.cb = NULL;
	mipi_dsi_cmdlist_put(&cmdreq_JDI);

	pr_info("%s, JDI display on command-\n", __func__);

	msleep(20);

	pr_info("%s-\n", __func__);
	return 0;
}

static int mipi_JDI_lcd_off(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;

	pr_info("%s+\n", __func__);

	mfd = platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;
	if (mfd->key != MFD_KEY)
		return -EINVAL;

	msleep(20);

	pr_info("%s, JDI display off command+\n", __func__);
	cmdreq_JDI.cmds = JDI_display_off_cmds;
	cmdreq_JDI.cmds_cnt = ARRAY_SIZE(JDI_display_off_cmds);
	cmdreq_JDI.flags = CMD_REQ_COMMIT;
	cmdreq_JDI.rlen = 0;
	cmdreq_JDI.cb = NULL;
	mipi_dsi_cmdlist_put(&cmdreq_JDI);
	pr_info("%s, JDI display off command-\n", __func__);

	msleep(20);

	pr_info("%s-\n", __func__);
	return 0;
}

static void mipi_JDI_set_backlight(struct msm_fb_data_type *mfd)
{
	int ret;
	static int bl_enable_sleep_control;
		/* sleep only when suspend or resume, init value is 0 */

	pr_debug("%s: back light level %d\n", __func__, mfd->bl_level);

	if (first) {
		ret = pm8xxx_gpio_config(gpio, &config);
		if (ret)
			pr_err("%s: pm8xxx_gpio_config failed: ret=%d\n",
				 __func__, ret);
		first = false;
	}

	if (bl_lpm) {
		if (mfd->bl_level) {
			ret = pwm_config(bl_lpm, PWM_DUTY_LEVEL *
				mfd->bl_level, PWM_PERIOD_USEC);
			if (ret) {
				pr_err("pwm_config on lpm failed %d\n", ret);
				return;
			}
			ret = pwm_enable(bl_lpm);
			if (ret)
				pr_err("pwm enable on lpm failed, bl=%d\n",
					mfd->bl_level);
			if (!bl_enable_sleep_control) {
				msleep(20);
				bl_enable_sleep_control = 1;
				pr_info("%s: pwm enable\n", __func__);
			}
			gpio_set_value_cansleep(gpio_LCD_BL_EN, 1);
		} else {
			gpio_set_value_cansleep(gpio_LCD_BL_EN, 0);
			if (bl_enable_sleep_control) {
				msleep(20);
				bl_enable_sleep_control = 0;
				pr_info("%s: pwm disable\n", __func__);
			}
			ret = pwm_config(bl_lpm, PWM_DUTY_LEVEL *
				mfd->bl_level, PWM_PERIOD_USEC);
			if (ret) {
				pr_err("pwm_config on lpm failed %d\n", ret);
				return;
			}
			pwm_disable(bl_lpm);
		}
	}
}

static void mipi_JDI_set_recovery_backlight(struct msm_fb_data_type *mfd)
{
	int ret;
	int recovery_backlight = 100;

	if (mipi_JDI_pdata->recovery_backlight)
		recovery_backlight = mipi_JDI_pdata->recovery_backlight;

	pr_info("%s: backlight level %d\n", __func__, recovery_backlight);

	if (bl_lpm) {
		ret = pwm_config(bl_lpm, PWM_DUTY_LEVEL *
			recovery_backlight, PWM_PERIOD_USEC);
		if (ret) {
			pr_err("pwm_config on lpm failed %d\n", ret);
			return;
		}
		ret = pwm_enable(bl_lpm);
		if (ret)
			pr_err("pwm enable on lpm failed, bl=%d\n",
				recovery_backlight);
		msleep(20);
		gpio_set_value_cansleep(gpio_LCD_BL_EN, 1);
	}
}
static void mipi_JDI_lcd_shutdown(void)
{
	int ret;

	pr_info("%s+\n", __func__);

	gpio_set_value_cansleep(gpio_LCD_BL_EN, 0);
	msleep(20);

	pr_info("%s: backlight off\n", __func__);
	if (bl_lpm) {
		ret = pwm_config(bl_lpm, 0, PWM_PERIOD_USEC);
		if (ret)
			pr_err("pwm_config on lpm failed %d\n", ret);
		pwm_disable(bl_lpm);
	}

	pr_info("%s, JDI display off command+\n", __func__);
	cmdreq_JDI.cmds = JDI_display_off_cmds;
	cmdreq_JDI.cmds_cnt = ARRAY_SIZE(JDI_display_off_cmds);
	cmdreq_JDI.flags = CMD_REQ_COMMIT;
	cmdreq_JDI.rlen = 0;
	cmdreq_JDI.cb = NULL;
	mipi_dsi_cmdlist_put(&cmdreq_JDI);
	pr_info("%s, JDI display off command-\n", __func__);

	pr_info("%s: power gpio off\n", __func__);
	msleep(20);
	gpio_set_value_cansleep(gpio_EN_VDD_BL, 0);
	msleep(20);
	gpio_set_value_cansleep(gpio_LCM_XRES, 0);
	msleep(20);

	pr_info("%s-\n", __func__);
}

struct syscore_ops panel_syscore_ops = {
	.shutdown = mipi_JDI_lcd_shutdown,
};

static int __devinit mipi_JDI_lcd_probe(struct platform_device *pdev)
{
	hw_rev hw_revision = asustek_get_hw_rev();
	pr_info("%s+\n", __func__);

	if (pdev->id == 0) {
		mipi_JDI_pdata = pdev->dev.platform_data;
		return 0;
	}

	if (mipi_JDI_pdata == NULL) {
		pr_err("%s.invalid platform data.\n", __func__);
		return -ENODEV;
	} else {
		bl_lpm = pwm_request(mipi_JDI_pdata->gpio[0],
			"backlight");
	}

	if (bl_lpm == NULL || IS_ERR(bl_lpm)) {
		pr_err("%s pwm_request() failed\n", __func__);
		bl_lpm = NULL;
	}
	pr_debug("bl_lpm = %p lpm = %d\n", bl_lpm,
		mipi_JDI_pdata->gpio[0]);

	msm_fb_add_device(pdev);

	register_syscore_ops(&panel_syscore_ops);

	if (hw_revision == 0) {
		gpio_LCD_BL_EN = gpio_LCD_BL_EN_SR1;
		gpio_LCM_XRES = gpio_LCM_XRES_SR1;
	}

	/* set PWM config */
	gpio = gpio_PWM;
	config.direction = PM_GPIO_DIR_OUT;
	config.output_buffer = PM_GPIO_OUT_BUF_CMOS;
	config.output_value = 0;
	config.pull = PM_GPIO_PULL_NO;
	config.vin_sel = PM_GPIO_VIN_L17;
	config.out_strength = PM_GPIO_STRENGTH_HIGH;
	config.function = PM_GPIO_FUNC_2;
	config.inv_int_pol = 0;
	config.disable_pin = 0;

	pr_info("%s-\n", __func__);
	return 0;
}

static struct platform_driver this_driver = {
	.probe  = mipi_JDI_lcd_probe,
	.driver = {
		.name   = "mipi_JDI",
	},
};

static struct msm_fb_panel_data JDI_panel_data = {
	.on		= mipi_JDI_lcd_on,
	.off		= mipi_JDI_lcd_off,
	.set_backlight = mipi_JDI_set_backlight,
	.set_recovery_backlight = mipi_JDI_set_recovery_backlight,
};

static int ch_used[3];

int mipi_JDI_device_register(struct msm_panel_info *pinfo,
					u32 channel, u32 panel)
{
	struct platform_device *pdev = NULL;
	int ret;

	if ((channel >= 3) || ch_used[channel])
		return -ENODEV;

	ch_used[channel] = TRUE;

	ret = mipi_JDI_lcd_init();
	if (ret) {
		pr_err("mipi_JDI_lcd_init() failed with ret %u\n", ret);
		return ret;
	}

	pdev = platform_device_alloc("mipi_JDI", (panel << 8)|channel);
	if (!pdev)
		return -ENOMEM;

	JDI_panel_data.panel_info = *pinfo;

	ret = platform_device_add_data(pdev, &JDI_panel_data,
		sizeof(JDI_panel_data));
	if (ret) {
		printk(KERN_ERR
		  "%s: platform_device_add_data failed!\n", __func__);
		goto err_device_put;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		printk(KERN_ERR
		  "%s: platform_device_register failed!\n", __func__);
		goto err_device_put;
	}

	return 0;

err_device_put:
	platform_device_put(pdev);
	return ret;
}

static int mipi_JDI_lcd_init(void)
{
	mipi_dsi_buf_alloc(&JDI_tx_buf, DSI_BUF_SIZE);
	mipi_dsi_buf_alloc(&JDI_rx_buf, DSI_BUF_SIZE);

	return platform_driver_register(&this_driver);
}