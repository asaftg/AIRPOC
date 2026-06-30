// SPDX-License-Identifier: GPL-2.0-only
/*
 * imx296_mode_tbls.h - Sony IMX296 (mono, global shutter) sensor mode tables
 * for the NVIDIA Tegracam framework on Jetson Orin Nano (p3768), L4T r36.4.
 *
 * Register sequences derived from the mainline Linux imx296 driver
 * (drivers/media/i2c/imx296.c) — vendor init table + setup.
 * Full active array: 1456 x 1088, RAW10, INCK 37.125 MHz.
 */

#ifndef __IMX296_I2C_TABLES__
#define __IMX296_I2C_TABLES__

#include <media/camera_common.h>
#include <linux/miscdevice.h>

#define IMX296_TABLE_WAIT_MS	0
#define IMX296_TABLE_END	1

#define imx296_reg struct reg_8

/* ---- stream control (CTRL00 STANDBY @0x3000, CTRL0A XMSTA @0x300a) ---- */
static imx296_reg imx296_start_stream[] = {
	{0x3000, 0x00},			/* clear STANDBY */
	{IMX296_TABLE_WAIT_MS, 2},
	{0x300a, 0x00},			/* clear XMSTA -> master mode streaming */
	{IMX296_TABLE_WAIT_MS, 2},
	{IMX296_TABLE_END, 0x00}
};

static imx296_reg imx296_stop_stream[] = {
	{0x300a, 0x01},			/* XMSTA: stop */
	{0x3000, 0x01},			/* STANDBY */
	{IMX296_TABLE_END, 0x00}
};

/*
 * Common init: the 41-entry vendor table (first write 0x3005=0xf0 enables the
 * CSI-2 output) followed by clock/timing setup for INCK 37.125 MHz, full-res
 * window (FID0_ROI off), HMAX=1100, default VMAX (~60fps), black level, gain delay.
 */
static imx296_reg imx296_mode_common[] = {
	/* vendor init table (undocumented, from mainline imx296_init_table) */
	{0x3005, 0xf0}, {0x309e, 0x04}, {0x30a0, 0x04}, {0x30a1, 0x3c},
	{0x30a4, 0x5f}, {0x30a8, 0x91}, {0x30ac, 0x28}, {0x30af, 0x09},
	{0x30df, 0x00}, {0x3165, 0x00}, {0x3169, 0x10}, {0x316a, 0x02},
	{0x31c8, 0xf3}, {0x31d0, 0xf4}, {0x321a, 0x00}, {0x3226, 0x02},
	{0x3256, 0x01}, {0x3541, 0x72}, {0x3516, 0x77}, {0x350b, 0x7f},
	{0x3758, 0xa3}, {0x3759, 0x00}, {0x375a, 0x85}, {0x375b, 0x00},
	{0x3832, 0xf5}, {0x3833, 0x00}, {0x38a2, 0xf6}, {0x38a3, 0x00},
	{0x3a00, 0x80}, {0x3d48, 0xa3}, {0x3d49, 0x00}, {0x3d4a, 0x85},
	{0x3d4b, 0x00}, {0x400e, 0x58}, {0x4014, 0x1c}, {0x4041, 0x2a},
	{0x40a2, 0x06}, {0x40c1, 0xf6}, {0x40c7, 0x0f}, {0x40c8, 0x00},
	{0x4174, 0x00},

	/* full-resolution window (ROI off) */
	{0x3300, 0x00},			/* FID0_ROI */
	/* MIPIC_AREA3W = 1088 (0x0440) MIPI active height, 16-bit LE @0x4182.
	 * Tegra-specific; written by the known-good FRC971 driver, absent from
	 * mainline RPi imx296 (RPi unicam tolerates it, Tegra VI does not).
	 * Without it the MIPI frame never forms -> VI timeout, zero data. */
	{0x4182, 0x40}, {0x4183, 0x04},
	{0x300d, 0x00},			/* CTRL0D: WINMODE_ALL, no binning */

	/* HMAX = 1100 (0x044c), 16-bit little-endian @0x3014 */
	{0x3014, 0x4c}, {0x3015, 0x04},
	/* VMAX = 0x000465 (1125 lines, ~60fps), 24-bit LE @0x3010 */
	{0x3010, 0x65}, {0x3011, 0x04}, {0x3012, 0x00},

	/* INCKSEL[0..3] for 37.125 MHz @0x3089 */
	{0x3089, 0x80}, {0x308a, 0x0b}, {0x308b, 0x80}, {0x308c, 0x08},
	{0x4114, 0xc5},			/* GTTABLENUM */
	{0x418c, 0x74},			/* CTRL418C = 116 (37.125 MHz) */

	{0x3212, 0x09},			/* GAINDLY = 1FRAME (matches FRC971) */
	{0x3254, 0x3c}, {0x3255, 0x00},	/* BLKLEVEL = 0x03c */

	{IMX296_TABLE_END, 0x00}
};

/* 1456x1088 @ up to 60fps, RAW10 mono — full array (window set in common) */
static imx296_reg imx296_mode_1456x1088[] = {
	{IMX296_TABLE_END, 0x00}
};

enum {
	IMX296_MODE_1456x1088,
	IMX296_MODE_COMMON,
	IMX296_START_STREAM,
	IMX296_STOP_STREAM,
};

static imx296_reg *mode_table[] = {
	[IMX296_MODE_1456x1088]	= imx296_mode_1456x1088,
	[IMX296_MODE_COMMON]	= imx296_mode_common,
	[IMX296_START_STREAM]	= imx296_start_stream,
	[IMX296_STOP_STREAM]	= imx296_stop_stream,
};

static const int imx296_60fps[] = { 60 };

static const struct camera_common_frmfmt imx296_frmfmt[] = {
	{{1456, 1088}, imx296_60fps, 1, 0, IMX296_MODE_1456x1088},
};

#endif /* __IMX296_I2C_TABLES__ */
