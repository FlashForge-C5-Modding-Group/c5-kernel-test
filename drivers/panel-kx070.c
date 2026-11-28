// SPDX-License-Identifier: GPL-2.0
/*
 * KX070: the Creator 5's 480x800 LVDS panel, for Ingenic's ingenicfb. The
 * timings and clock are what FlashForge's kernel programs into the display
 * controller, read back from a running printer. No reset, power or init
 * commands: FlashForge's lcd_kx070.ko has none either.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/fb.h>

#include "../include/ingenicfb.h"
#include "../include/jz_dsim.h"

static bool jeida;
module_param(jeida, bool, 0444);
MODULE_PARM_DESC(jeida, "JEIDA LVDS bit mapping instead of VESA");

static struct fb_videomode kx070_mode = {
	.name = "kx070",
	.xres = 480,
	.yres = 800,
	.pixclock = 31000,	/* kHz: EPLL 434 MHz / 14 */
	.hsync_len = 20,
	.left_margin = 40,
	.right_margin = 40,
	.vsync_len = 8,
	.upper_margin = 17,
	.lower_margin = 70,
	.sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT,
	.vmode = FB_VMODE_NONINTERLACED,
};

static struct jzdsi_data kx070_dsi = {
	.modes = &kx070_mode,
	.video_config.pixel_clock = 31000,	/* kHz, the same as pixclock */
	.video_config.virtual_channel = 0,
	.video_config.color_coding = COLOR_CODE_24BIT,
	.video_config.video_mode = VIDEO_BURST_WITH_SYNC_PULSES,
	.video_config.receive_ack_packets = 0,
	.video_config.is_18_loosely = 0,
	.video_config.h_polarity = 0,
	.video_config.v_polarity = 0,
	.video_config.data_en_polarity = 0,
	.video_config.byte_clock = 0,
	.video_config.byte_clock_coef = MIPI_PHY_BYTE_CLK_COEF_MUL6_DIV5,
	.dsi_config.max_lanes = 4,
	.dsi_config.max_hs_to_lp_cycles = 100,
	.dsi_config.max_lp_to_hs_cycles = 40,
	.dsi_config.max_bta_cycles = 4095,
	.dsi_config.color_mode_polarity = 1,
	.dsi_config.shut_down_polarity = 1,
	.dsi_config.max_bps = 2750,
	.bpp_info = 24,
};

static struct tft_config kx070_tft = {
	.pix_clk_inv = 0,
	.de_dl = 0,
	.sync_dl = 0,
	.color_even = TFT_LCD_COLOR_EVEN_RGB,
	.color_odd = TFT_LCD_COLOR_ODD_RGB,
	.mode = TFT_LCD_MODE_PARALLEL_888,
};

static struct lcd_panel kx070_panel = {
	.name = "kx070",
	.num_modes = 1,
	.modes = &kx070_mode,
	.dsi_pdata = &kx070_dsi,
	.lcd_type = LCD_TYPE_LVDS_VESA,
	.tft_config = &kx070_tft,
	.bpp = 24,
};

static int kx070_probe(struct platform_device *pdev)
{
	if (jeida)
		kx070_panel.lcd_type = LCD_TYPE_LVDS_JEIDA;
	return ingenicfb_register_panel(&kx070_panel);
}

static const struct of_device_id kx070_of_match[] = {
	{ .compatible = "flashforge,kx070" },
	{}
};

static struct platform_driver kx070_driver = {
	.probe = kx070_probe,
	.driver = {
		.name = "panel-kx070",
		.of_match_table = kx070_of_match,
	},
};
module_platform_driver(kx070_driver);

MODULE_DESCRIPTION("Creator 5 KX070 LVDS panel for ingenicfb");
MODULE_LICENSE("GPL");
