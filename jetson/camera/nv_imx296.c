// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: Copyright (c) 2024 NVIDIA CORPORATION & AFFILIATES.
/*
 * nv_imx296.c - Sony IMX296 mono global-shutter sensor driver (Tegracam)
 * Ported from NVIDIA nv_imx219.c; register sequences from mainline imx296.c.
 */

#include <nvidia/conftest.h>

#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/module.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>

#include <media/tegra_v4l2_camera.h>
#include <media/tegracam_core.h>

#include "../platform/tegra/camera/camera_gpio.h"
#include "imx296_mode_tbls.h"

/* IMX296 register addresses */
#define IMX296_SENSOR_INFO_MSB	0x3148
#define IMX296_SENSOR_INFO_LSB	0x3149
#define IMX296_GAIN_ADDR_LSB	0x3204	/* GAIN[7:0], 0..480 (0.1 dB) */
#define IMX296_GAIN_ADDR_MSB	0x3205	/* GAIN[15:8] */
#define IMX296_VMAX_ADDR_0	0x3010	/* VMAX (frame length), 24-bit LE */
#define IMX296_VMAX_ADDR_1	0x3011
#define IMX296_VMAX_ADDR_2	0x3012
#define IMX296_SHS1_ADDR_0	0x308d	/* SHS1 (shutter), 24-bit LE */
#define IMX296_SHS1_ADDR_1	0x308e
#define IMX296_SHS1_ADDR_2	0x308f

#define IMX296_GAIN_MIN		0
#define IMX296_GAIN_MAX		480
#define IMX296_MIN_FRAME_LENGTH	0x00044c
#define IMX296_MAX_FRAME_LENGTH	0x0fffff
#define IMX296_MIN_SHS		1
#define IMX296_MAX_COARSE_DIFF	5
#define IMX296_DEFAULT_FRAME_LENGTH	1125		/* VMAX for 1456x1088@60 */
/* Sensor internal pixel clock = HMAX*VMAX*fps = 1100*1125*60. This is the
 * clock that governs SHS1/VMAX line timing — NOT the DT pix_clk_hz (118.8 MHz,
 * which is the MIPI byte/pixel rate used only for VI bandwidth). */
#define IMX296_SENSOR_PIXEL_CLOCK	74250000ULL

static const struct of_device_id imx296_of_match[] = {
	{ .compatible = "sony,imx296", },
	{ .compatible = "sony,imx296ll", },	/* mono variant / proven overlay node */
	{ },
};
MODULE_DEVICE_TABLE(of, imx296_of_match);

static const u32 ctrl_cid_list[] = {
	TEGRA_CAMERA_CID_GAIN,
	TEGRA_CAMERA_CID_EXPOSURE,
	TEGRA_CAMERA_CID_FRAME_RATE,
	TEGRA_CAMERA_CID_SENSOR_MODE_ID,
};

struct imx296 {
	struct i2c_client		*i2c_client;
	struct v4l2_subdev		*subdev;
	u32				frame_length;
	struct camera_common_data	*s_data;
	struct tegracam_device		*tc_dev;
};

static const struct regmap_config sensor_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.use_single_read = true,
	.use_single_write = true,
};

static inline int imx296_read_reg(struct camera_common_data *s_data,
	u16 addr, u8 *val)
{
	int err = 0;
	u32 reg_val = 0;

	err = regmap_read(s_data->regmap, addr, &reg_val);
	*val = reg_val & 0xff;
	return err;
}

static inline int imx296_write_reg(struct camera_common_data *s_data,
	u16 addr, u8 val)
{
	int err = regmap_write(s_data->regmap, addr, val);

	if (err)
		dev_err(s_data->dev, "%s: i2c write failed, 0x%x = %x",
			__func__, addr, val);
	return err;
}

static int imx296_write_table(struct imx296 *priv, const imx296_reg table[])
{
	return regmap_util_write_table_8(priv->s_data->regmap, table, NULL, 0,
		IMX296_TABLE_WAIT_MS, IMX296_TABLE_END);
}

static int imx296_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
	return 0;
}

/* DT exposes gain directly in 0.1 dB units (gain_factor=10, 0..480). */
static int imx296_set_gain(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	u32 gain;
	int err;

	if (val < mode->control_properties.min_gain_val)
		val = mode->control_properties.min_gain_val;
	else if (val > mode->control_properties.max_gain_val)
		val = mode->control_properties.max_gain_val;

	/* normalized gain (val/gain_factor in dB) -> 0.1 dB register units */
	gain = (u32)((val * 10) / mode->control_properties.gain_factor);
	if (gain > IMX296_GAIN_MAX)
		gain = IMX296_GAIN_MAX;

	err  = imx296_write_reg(s_data, IMX296_GAIN_ADDR_LSB, gain & 0xff);
	err |= imx296_write_reg(s_data, IMX296_GAIN_ADDR_MSB, (gain >> 8) & 0xff);
	return err;
}

static int imx296_set_frame_rate(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct imx296 *priv = (struct imx296 *)tc_dev->priv;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	u32 frame_length;
	int err;

	if (val == 0 || mode->image_properties.line_length == 0)
		return -EINVAL;

	frame_length = (u32)(IMX296_SENSOR_PIXEL_CLOCK *
		(u64)mode->control_properties.framerate_factor /
		mode->image_properties.line_length / val);

	if (frame_length < IMX296_MIN_FRAME_LENGTH)
		frame_length = IMX296_MIN_FRAME_LENGTH;
	else if (frame_length > IMX296_MAX_FRAME_LENGTH)
		frame_length = IMX296_MAX_FRAME_LENGTH;

	err  = imx296_write_reg(s_data, IMX296_VMAX_ADDR_0, frame_length & 0xff);
	err |= imx296_write_reg(s_data, IMX296_VMAX_ADDR_1, (frame_length >> 8) & 0xff);
	err |= imx296_write_reg(s_data, IMX296_VMAX_ADDR_2, (frame_length >> 16) & 0x0f);

	priv->frame_length = frame_length;
	return err;
}

static int imx296_set_exposure(struct tegracam_device *tc_dev, s64 val)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct imx296 *priv = (struct imx296 *)tc_dev->priv;
	const struct sensor_mode_properties *mode =
		&s_data->sensor_props.sensor_modes[s_data->mode_prop_idx];
	u32 coarse_time, shs1;
	int err;

	if (mode->control_properties.exposure_factor == 0 ||
		mode->image_properties.line_length == 0)
		return -EINVAL;

	/* exposure (us) -> integration lines (sensor pixel clock, not MIPI rate) */
	coarse_time = (u32)((u64)val * IMX296_SENSOR_PIXEL_CLOCK /
		mode->control_properties.exposure_factor /
		mode->image_properties.line_length);

	if (coarse_time > priv->frame_length - IMX296_MAX_COARSE_DIFF)
		coarse_time = priv->frame_length - IMX296_MAX_COARSE_DIFF;
	if (coarse_time < IMX296_MIN_SHS)
		coarse_time = IMX296_MIN_SHS;

	/* IMX296 shutter: SHS1 = VMAX - integration_lines - 1 */
	shs1 = priv->frame_length - coarse_time - 1;

	err  = imx296_write_reg(s_data, IMX296_SHS1_ADDR_0, shs1 & 0xff);
	err |= imx296_write_reg(s_data, IMX296_SHS1_ADDR_1, (shs1 >> 8) & 0xff);
	err |= imx296_write_reg(s_data, IMX296_SHS1_ADDR_2, (shs1 >> 16) & 0x0f);
	return err;
}

static struct tegracam_ctrl_ops imx296_ctrl_ops = {
	.numctrls = ARRAY_SIZE(ctrl_cid_list),
	.ctrl_cid_list = ctrl_cid_list,
	.set_gain = imx296_set_gain,
	.set_exposure = imx296_set_exposure,
	.set_frame_rate = imx296_set_frame_rate,
	.set_group_hold = imx296_set_group_hold,
};

static int imx296_power_on(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;

	if (pdata && pdata->power_on) {
		err = pdata->power_on(pw);
		if (err)
			dev_err(dev, "%s failed.\n", __func__);
		else
			pw->state = SWITCH_ON;
		return err;
	}

	if (unlikely(!(pw->avdd || pw->iovdd || pw->dvdd)))
		goto skip_power_seqn;

	if (pw->reset_gpio)
		gpio_set_value(pw->reset_gpio, 0);
	usleep_range(10, 20);

	if (pw->avdd) {
		err = regulator_enable(pw->avdd);
		if (err)
			goto imx296_avdd_fail;
	}
	if (pw->iovdd) {
		err = regulator_enable(pw->iovdd);
		if (err)
			goto imx296_iovdd_fail;
	}
	if (pw->dvdd) {
		err = regulator_enable(pw->dvdd);
		if (err)
			goto imx296_dvdd_fail;
	}
	usleep_range(10, 20);

skip_power_seqn:
	if (pw->reset_gpio)
		gpio_set_value(pw->reset_gpio, 1);
	usleep_range(10000, 10100);
	pw->state = SWITCH_ON;
	return 0;

imx296_dvdd_fail:
	regulator_disable(pw->iovdd);
imx296_iovdd_fail:
	regulator_disable(pw->avdd);
imx296_avdd_fail:
	dev_err(dev, "%s failed.\n", __func__);
	return -ENODEV;
}

static int imx296_power_off(struct camera_common_data *s_data)
{
	int err = 0;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;

	if (pdata && pdata->power_off) {
		err = pdata->power_off(pw);
		if (err)
			return err;
	} else {
		if (pw->reset_gpio)
			gpio_set_value(pw->reset_gpio, 0);
		usleep_range(10, 20);
		if (pw->dvdd)
			regulator_disable(pw->dvdd);
		if (pw->iovdd)
			regulator_disable(pw->iovdd);
		if (pw->avdd)
			regulator_disable(pw->avdd);
	}
	usleep_range(5000, 5000);
	pw->state = SWITCH_OFF;
	return 0;
}

static int imx296_power_put(struct tegracam_device *tc_dev)
{
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;

	if (unlikely(!pw))
		return -EFAULT;
	if (likely(pw->dvdd))
		devm_regulator_put(pw->dvdd);
	if (likely(pw->avdd))
		devm_regulator_put(pw->avdd);
	if (likely(pw->iovdd))
		devm_regulator_put(pw->iovdd);
	pw->dvdd = NULL;
	pw->avdd = NULL;
	pw->iovdd = NULL;
	if (likely(pw->reset_gpio))
		gpio_free(pw->reset_gpio);
	return 0;
}

static int imx296_power_get(struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct camera_common_data *s_data = tc_dev->s_data;
	struct camera_common_power_rail *pw = s_data->power;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct clk *parent;
	int err = 0;

	if (!pdata) {
		dev_err(dev, "pdata missing\n");
		return -EFAULT;
	}

	if (pdata->mclk_name) {
		pw->mclk = devm_clk_get(dev, pdata->mclk_name);
		if (IS_ERR(pw->mclk)) {
			dev_err(dev, "unable to get clock %s\n", pdata->mclk_name);
			return PTR_ERR(pw->mclk);
		}
		if (pdata->parentclk_name) {
			parent = devm_clk_get(dev, pdata->parentclk_name);
			if (IS_ERR(parent))
				dev_err(dev, "unable to get parent clk\n");
			else
				clk_set_parent(pw->mclk, parent);
		}
	}

	if (pdata->regulators.avdd)
		err |= camera_common_regulator_get(dev, &pw->avdd,
			pdata->regulators.avdd);
	if (pdata->regulators.iovdd)
		err |= camera_common_regulator_get(dev, &pw->iovdd,
			pdata->regulators.iovdd);
	if (pdata->regulators.dvdd)
		err |= camera_common_regulator_get(dev, &pw->dvdd,
			pdata->regulators.dvdd);
	if (err) {
		dev_err(dev, "%s: unable to get regulator(s)\n", __func__);
		goto done;
	}

	pw->reset_gpio = pdata->reset_gpio;
	err = gpio_request(pw->reset_gpio, "cam_reset_gpio");
	if (err < 0)
		dev_err(dev, "%s: unable to request reset_gpio (%d)\n",
			__func__, err);
done:
	pw->state = SWITCH_OFF;
	return err;
}

static struct camera_common_pdata *imx296_parse_dt(
	struct tegracam_device *tc_dev)
{
	struct device *dev = tc_dev->dev;
	struct device_node *np = dev->of_node;
	struct camera_common_pdata *board_priv_pdata;
	const struct of_device_id *match;
	struct camera_common_pdata *ret = NULL;
	int err = 0;
	int gpio;

	if (!np)
		return NULL;

	match = of_match_device(imx296_of_match, dev);
	if (!match) {
		dev_err(dev, "Failed to find matching dt id\n");
		return NULL;
	}

	board_priv_pdata = devm_kzalloc(dev, sizeof(*board_priv_pdata),
		GFP_KERNEL);
	if (!board_priv_pdata)
		return NULL;

	gpio = of_get_named_gpio(np, "reset-gpios", 0);
	if (gpio < 0) {
		if (gpio == -EPROBE_DEFER)
			ret = ERR_PTR(-EPROBE_DEFER);
		dev_err(dev, "reset-gpios not found\n");
		goto error;
	}
	board_priv_pdata->reset_gpio = (unsigned int)gpio;

	err = of_property_read_string(np, "mclk", &board_priv_pdata->mclk_name);
	if (err)
		dev_dbg(dev, "mclk not present\n");

	err = of_property_read_string(np, "avdd-reg",
		&board_priv_pdata->regulators.avdd);
	err |= of_property_read_string(np, "iovdd-reg",
		&board_priv_pdata->regulators.iovdd);
	err |= of_property_read_string(np, "dvdd-reg",
		&board_priv_pdata->regulators.dvdd);
	if (err)
		dev_dbg(dev, "regulators not present\n");

	board_priv_pdata->has_eeprom =
		of_property_read_bool(np, "has-eeprom");

	return board_priv_pdata;

error:
	devm_kfree(dev, board_priv_pdata);
	return ret;
}

static int imx296_set_mode(struct tegracam_device *tc_dev)
{
	struct imx296 *priv = (struct imx296 *)tegracam_get_privdata(tc_dev);
	struct camera_common_data *s_data = tc_dev->s_data;
	int err;

	err = imx296_write_table(priv, mode_table[IMX296_MODE_COMMON]);
	if (err)
		return err;

	/* Seed frame_length so set_exposure has a valid VMAX even when the
	 * framework applies EXPOSURE before FRAME_RATE (else SHS1 underflows). */
	priv->frame_length = IMX296_DEFAULT_FRAME_LENGTH;

	if (s_data->mode < 0)
		return -EINVAL;
	err = imx296_write_table(priv, mode_table[s_data->mode]);
	if (err)
		return err;

	return 0;
}

static int imx296_start_streaming(struct tegracam_device *tc_dev)
{
	struct imx296 *priv = (struct imx296 *)tegracam_get_privdata(tc_dev);

	return imx296_write_table(priv, mode_table[IMX296_START_STREAM]);
}

static int imx296_stop_streaming(struct tegracam_device *tc_dev)
{
	struct imx296 *priv = (struct imx296 *)tegracam_get_privdata(tc_dev);

	return imx296_write_table(priv, mode_table[IMX296_STOP_STREAM]);
}

static struct camera_common_sensor_ops imx296_common_ops = {
	.numfrmfmts = ARRAY_SIZE(imx296_frmfmt),
	.frmfmt_table = imx296_frmfmt,
	.power_on = imx296_power_on,
	.power_off = imx296_power_off,
	.write_reg = imx296_write_reg,
	.read_reg = imx296_read_reg,
	.parse_dt = imx296_parse_dt,
	.power_get = imx296_power_get,
	.power_put = imx296_power_put,
	.set_mode = imx296_set_mode,
	.start_streaming = imx296_start_streaming,
	.stop_streaming = imx296_stop_streaming,
};

static int imx296_board_setup(struct imx296 *priv)
{
	struct camera_common_data *s_data = priv->s_data;
	struct camera_common_pdata *pdata = s_data->pdata;
	struct device *dev = s_data->dev;
	u8 reg_val[2];
	int err = 0;

	if (pdata->mclk_name) {
		err = camera_common_mclk_enable(s_data);
		if (err) {
			dev_err(dev, "error turning on mclk (%d)\n", err);
			goto done;
		}
	}

	err = imx296_power_on(s_data);
	if (err) {
		dev_err(dev, "error during power on sensor (%d)\n", err);
		goto err_power_on;
	}

	/* IMX296 SENSOR_INFO @0x3148: bit15 = mono */
	err = imx296_read_reg(s_data, IMX296_SENSOR_INFO_MSB, &reg_val[0]);
	err |= imx296_read_reg(s_data, IMX296_SENSOR_INFO_LSB, &reg_val[1]);
	if (err)
		dev_err(dev, "%s: i2c probe read failed (%d)\n", __func__, err);
	else
		dev_info(dev, "imx296 sensor_info: 0x%02x%02x (mono=%d)\n",
			reg_val[0], reg_val[1], !!(reg_val[0] & 0x80));

	imx296_power_off(s_data);

err_power_on:
	if (pdata->mclk_name)
		camera_common_mclk_disable(s_data);
done:
	return err;
}

static int imx296_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return 0;
}

static const struct v4l2_subdev_internal_ops imx296_subdev_internal_ops = {
	.open = imx296_open,
};

#if defined(NV_I2C_DRIVER_STRUCT_PROBE_WITHOUT_I2C_DEVICE_ID_ARG) /* Linux 6.3 */
static int imx296_probe(struct i2c_client *client)
#else
static int imx296_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
#endif
{
	struct device *dev = &client->dev;
	struct tegracam_device *tc_dev;
	struct imx296 *priv;
	int err;

	dev_dbg(dev, "probing imx296 at addr 0x%0x\n", client->addr);

	if (!IS_ENABLED(CONFIG_OF) || !client->dev.of_node)
		return -EINVAL;

	priv = devm_kzalloc(dev, sizeof(struct imx296), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	tc_dev = devm_kzalloc(dev, sizeof(struct tegracam_device), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	priv->i2c_client = tc_dev->client = client;
	tc_dev->dev = dev;
	strncpy(tc_dev->name, "imx296", sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &sensor_regmap_config;
	tc_dev->sensor_ops = &imx296_common_ops;
	tc_dev->v4l2sd_internal_ops = &imx296_subdev_internal_ops;
	tc_dev->tcctrl_ops = &imx296_ctrl_ops;

	err = tegracam_device_register(tc_dev);
	if (err) {
		dev_err(dev, "tegra camera driver registration failed\n");
		return err;
	}
	priv->tc_dev = tc_dev;
	priv->s_data = tc_dev->s_data;
	priv->subdev = &tc_dev->s_data->subdev;
	tegracam_set_privdata(tc_dev, (void *)priv);

	err = imx296_board_setup(priv);
	if (err) {
		tegracam_device_unregister(tc_dev);
		dev_err(dev, "board setup failed\n");
		return err;
	}

	err = tegracam_v4l2subdev_register(tc_dev, true);
	if (err) {
		tegracam_device_unregister(tc_dev);
		dev_err(dev, "tegra camera subdev registration failed\n");
		return err;
	}

	dev_info(dev, "detected imx296 sensor\n");
	return 0;
}

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT) /* Linux 6.1 */
static int imx296_remove(struct i2c_client *client)
#else
static void imx296_remove(struct i2c_client *client)
#endif
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct imx296 *priv;

	if (!s_data) {
#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT)
		return -EINVAL;
#else
		return;
#endif
	}
	priv = (struct imx296 *)s_data->priv;

	tegracam_v4l2subdev_unregister(priv->tc_dev);
	tegracam_device_unregister(priv->tc_dev);

#if defined(NV_I2C_DRIVER_STRUCT_REMOVE_RETURN_TYPE_INT)
	return 0;
#endif
}

static const struct i2c_device_id imx296_id[] = {
	{ "imx296", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, imx296_id);

static struct i2c_driver imx296_i2c_driver = {
	.driver = {
		.name = "imx296",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(imx296_of_match),
	},
	.probe = imx296_probe,
	.remove = imx296_remove,
	.id_table = imx296_id,
};
module_i2c_driver(imx296_i2c_driver);

MODULE_DESCRIPTION("Media Controller driver for Sony IMX296");
MODULE_AUTHOR("Seeker bench");
MODULE_LICENSE("GPL v2");
