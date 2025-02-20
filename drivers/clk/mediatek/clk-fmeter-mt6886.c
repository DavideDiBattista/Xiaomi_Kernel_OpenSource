// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 * Author: Chong-ming Wei <chong-ming.wei@mediatek.com>
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "clk-fmeter.h"
#include "clk-mt6886-fmeter.h"

#define FM_TIMEOUT			30
#define SUBSYS_PLL_NUM			4
#define VLP_FM_WAIT_TIME		40	/* ~= 38.64ns * 1023 */

#define FM_PLL_CK			0
#define FM_PLL_CKDIV_CK			1
#define FM_CKDIV_SHIFT			(7)
#define FM_CKDIV_MASK			GENMASK(10, 7)
#define FM_POSTDIV_SHIFT		(24)
#define FM_POSTDIV_MASK			GENMASK(26, 24)

static DEFINE_SPINLOCK(meter_lock);
#define fmeter_lock(flags)   spin_lock_irqsave(&meter_lock, flags)
#define fmeter_unlock(flags) spin_unlock_irqrestore(&meter_lock, flags)

static DEFINE_SPINLOCK(subsys_meter_lock);
#define subsys_fmeter_lock(flags)   spin_lock_irqsave(&subsys_meter_lock, flags)
#define subsys_fmeter_unlock(flags) spin_unlock_irqrestore(&subsys_meter_lock, flags)

#define clk_readl(addr)		readl(addr)
#define clk_writel(addr, val)	\
	do { writel(val, addr); wmb(); } while (0) /* sync write */

#define CLK26CALI_0					(0x220)
#define CLK26CALI_1					(0x224)
#define CLK_MISC_CFG_0					(0x240)
#define CLK_DBG_CFG					(0x28C)
#define VLP_FQMTR_CON0					(0x230)
#define VLP_FQMTR_CON1					(0x234)
/* MFGPLL_PLL_CTRL Register */
#define MFGPLL_CON0					(0x0008)
#define MFGPLL_CON1					(0x000C)
#define MFGPLL_FQMTR_CON0				(0x0040)
#define MFGPLL_FQMTR_CON1				(0x0044)

/* MFGSCPLL_PLL_CTRL Register */
#define MFGSCPLL_CON0					(0x0008)
#define MFGSCPLL_CON1					(0x000C)
#define MFGSCPLL_FQMTR_CON0				(0x0040)
#define MFGSCPLL_FQMTR_CON1				(0x0044)

/* CCIPLL_PLL_CTRL Register */
#define CCIPLL_CON0					(0x0008)
#define CCIPLL_CON1					(0x000C)
#define CCIPLL_FQMTR_CON0				(0x0040)
#define CCIPLL_FQMTR_CON1				(0x0044)

/* ARMPLL_LL_PLL_CTRL Register */
#define ARMPLL_LL_CON0					(0x0008)
#define ARMPLL_LL_CON1					(0x000C)
#define ARMPLL_LL_FQMTR_CON0				(0x0040)
#define ARMPLL_LL_FQMTR_CON1				(0x0044)

/* ARMPLL_BL_PLL_CTRL Register */
#define ARMPLL_BL_CON0					(0x0008)
#define ARMPLL_BL_CON1					(0x000C)
#define ARMPLL_BL_FQMTR_CON0				(0x0040)
#define ARMPLL_BL_FQMTR_CON1				(0x0044)

/* PTPPLL_PLL_CTRL Register */
#define PTPPLL_CON0					(0x0008)
#define PTPPLL_CON1					(0x000C)
#define PTPPLL_FQMTR_CON0				(0x0040)
#define PTPPLL_FQMTR_CON1				(0x0044)

static void __iomem *fm_base[FM_SYS_NUM];

struct fmeter_data {
	enum fm_sys_id type;
	const char *name;
	unsigned int pll_con0;
	unsigned int pll_con1;
	unsigned int con0;
	unsigned int con1;
};

static struct fmeter_data subsys_fm[] = {
	[FM_VLP_CKSYS] = {FM_VLP_CKSYS, "fm_vlp_cksys",
		0, 0, VLP_FQMTR_CON0, VLP_FQMTR_CON1},
	[FM_MFGPLL] = {FM_MFGPLL, "fm_mfgpll",
		MFGPLL_CON0, MFGPLL_CON1, MFGPLL_FQMTR_CON0, MFGPLL_FQMTR_CON1},
	[FM_MFGSCPLL] = {FM_MFGSCPLL, "fm_mfgscpll",
		MFGSCPLL_CON0, MFGSCPLL_CON1, MFGSCPLL_FQMTR_CON0, MFGSCPLL_FQMTR_CON1},
	[FM_CCIPLL] = {FM_CCIPLL, "fm_ccipll",
		CCIPLL_CON0, CCIPLL_CON1, CCIPLL_FQMTR_CON0, CCIPLL_FQMTR_CON1},
	[FM_ARMPLL_LL] = {FM_ARMPLL_LL, "fm_armpll_ll",
		ARMPLL_LL_CON0, ARMPLL_LL_CON1, ARMPLL_LL_FQMTR_CON0, ARMPLL_LL_FQMTR_CON1},
	[FM_ARMPLL_BL] = {FM_ARMPLL_BL, "fm_armpll_bl",
		ARMPLL_BL_CON0, ARMPLL_BL_CON1, ARMPLL_BL_FQMTR_CON0, ARMPLL_BL_FQMTR_CON1},
	[FM_PTPPLL] = {FM_PTPPLL, "fm_ptppll",
		PTPPLL_CON0, PTPPLL_CON1, PTPPLL_FQMTR_CON0, PTPPLL_FQMTR_CON1},
};

const char *comp_list[] = {
	[FM_TOPCKGEN] = "mediatek,mt6886-topckgen",
	[FM_APMIXED] = "mediatek,mt6886-apmixedsys",
	[FM_VLP_CKSYS] = "mediatek,mt6886-vlp_cksys",
	[FM_MFGPLL] = "mediatek,mt6886-mfgpll_pll_ctrl",
	[FM_MFGSCPLL] = "mediatek,mt6886-mfgscpll_pll_ctrl",
	[FM_CCIPLL] = "mediatek,mt6886-ccipll_pll_ctrl",
	[FM_ARMPLL_LL] = "mediatek,mt6886-armpll_ll_pll_ctrl",
	[FM_ARMPLL_BL] = "mediatek,mt6886-armpll_bl_pll_ctrl",
	[FM_PTPPLL] = "mediatek,mt6886-ptppll_pll_ctrl",
};

/*
 * clk fmeter
 */

#define FMCLK3(_t, _i, _n, _o, _g, _c) { .type = _t, \
		.id = _i, .name = _n, .ofs = _o, .grp = _g, .ck_div = _c}
#define FMCLK2(_t, _i, _n, _o, _p, _c) { .type = _t, \
		.id = _i, .name = _n, .ofs = _o, .pdn = _p, .ck_div = _c}
#define FMCLK(_t, _i, _n, _c) { .type = _t, .id = _i, .name = _n, .ck_div = _c}

static const struct fmeter_clk fclks[] = {
	/* CKGEN Part */
	FMCLK2(CKGEN, FM_AXI_CK, "fm_axi_ck", 0x0010, 7, 1),
	FMCLK2(CKGEN, FM_AXI_CK_2, "fm_axi_ck_2", 0x0010, 15, 1),
	FMCLK2(CKGEN, FM_U_HAXI_CK, "fm_u_haxi_ck", 0x0010, 23, 1),
	FMCLK2(CKGEN, FM_BUS_AXIMEM_CK, "fm_bus_aximem_ck", 0x0010, 31, 1),
	FMCLK2(CKGEN, FM_DISP0_CK, "fm_disp0_ck", 0x0020, 7, 1),
	FMCLK2(CKGEN, FM_MDP0_CK, "fm_mdp0_ck", 0x0020, 15, 1),
	FMCLK2(CKGEN, FM_MMINFRA_CK, "fm_mminfra_ck", 0x0020, 23, 1),
	FMCLK2(CKGEN, FM_MMUP_CK, "fm_mmup_ck", 0x0020, 31, 1),
	FMCLK2(CKGEN, FM_DSP_CK, "fm_dsp_ck", 0x0030, 7, 1),
	FMCLK(CKGEN, FM_DSP1_CK, "fm_dsp1_ck", 1),
	FMCLK(CKGEN, FM_DSP2_CK, "fm_dsp2_ck", 1),
	FMCLK(CKGEN, FM_DSP3_CK, "fm_dsp3_ck", 1),
	FMCLK(CKGEN, FM_DSP4_CK, "fm_dsp4_ck", 1),
	FMCLK(CKGEN, FM_DSP5_CK, "fm_dsp5_ck", 1),
	FMCLK(CKGEN, FM_DSP6_CK, "fm_dsp6_ck", 1),
	FMCLK(CKGEN, FM_DSP7_CK, "fm_dsp7_ck", 1),
	FMCLK2(CKGEN, FM_CAMTG_CK, "fm_camtg_ck", 0x0050, 7, 1),
	FMCLK2(CKGEN, FM_CAMTG2_CK, "fm_camtg2_ck", 0x0050, 15, 1),
	FMCLK2(CKGEN, FM_CAMTG3_CK, "fm_camtg3_ck", 0x0050, 23, 1),
	FMCLK2(CKGEN, FM_CAMTG4_CK, "fm_camtg4_ck", 0x0050, 31, 1),
	FMCLK2(CKGEN, FM_CAMTG5_CK, "fm_camtg5_ck", 0x0060, 7, 1),
	FMCLK2(CKGEN, FM_CAMTG6_CK, "fm_camtg6_ck", 0x0060, 15, 1),
	FMCLK2(CKGEN, FM_UART_CK, "fm_uart_ck", 0x0060, 23, 1),
	FMCLK2(CKGEN, FM_SPI_CK, "fm_spi_ck", 0x0060, 31, 1),
	FMCLK2(CKGEN, FM_MSDC_MACRO_CK, "fm_msdc_macro_ck", 0x0070, 7, 1),
	FMCLK2(CKGEN, FM_MSDC30_1_CK, "fm_msdc30_1_ck", 0x0070, 15, 1),
	FMCLK2(CKGEN, FM_MSDC30_2_CK, "fm_msdc30_2_ck", 0x0070, 23, 1),
	FMCLK2(CKGEN, FM_AUDIO_CK, "fm_audio_ck", 0x0070, 31, 1),
	FMCLK2(CKGEN, FM_AUD_INTBUS_CK, "fm_aud_intbus_ck", 0x0080, 7, 1),
	FMCLK2(CKGEN, FM_ATB_CK, "fm_atb_ck", 0x0080, 15, 1),
	FMCLK2(CKGEN, FM_DISP_PWM_CK, "fm_disp_pwm_ck", 0x0080, 23, 1),
	FMCLK2(CKGEN, FM_USB_CK, "fm_usb_ck", 0x0080, 31, 1),
	FMCLK2(CKGEN, FM_USB_XHCI_CK, "fm_usb_xhci_ck", 0x0090, 7, 1),
	FMCLK(CKGEN, FM_USB_1P_CK, "fm_usb_1p_ck", 1),
	FMCLK(CKGEN, FM_USB_XHCI_1P_CK, "fm_usb_xhci_1p_ck", 1),
	FMCLK2(CKGEN, FM_I2C_CK, "fm_i2c_ck", 0x0090, 31, 1),
	FMCLK2(CKGEN, FM_SENINF_CK, "fm_seninf_ck", 0x00A0, 7, 1),
	FMCLK2(CKGEN, FM_SENINF1_CK, "fm_seninf1_ck", 0x00A0, 15, 1),
	FMCLK2(CKGEN, FM_SENINF2_CK, "fm_seninf2_ck", 0x00A0, 23, 1),
	FMCLK2(CKGEN, FM_SENINF3_CK, "fm_seninf3_ck", 0x00A0, 31, 1),
	FMCLK(CKGEN, FM_DXCC_CK, "fm_dxcc_ck", 1),
	FMCLK2(CKGEN, FM_AUD_ENGEN1_CK, "fm_aud_engen1_ck", 0x00B0, 15, 1),
	FMCLK2(CKGEN, FM_AUD_ENGEN2_CK, "fm_aud_engen2_ck", 0x00B0, 23, 1),
	FMCLK2(CKGEN, FM_AES_UFSFDE_CK, "fm_aes_ufsfde_ck", 0x00B0, 31, 1),
	FMCLK2(CKGEN, FM_U_CK, "fm_u_ck", 0x00C0, 7, 1),
	FMCLK2(CKGEN, FM_U_MBIST_CK, "fm_u_mbist_ck", 0x00C0, 15, 1),
	FMCLK(CKGEN, FM_PEXTP_MBIST_CK, "fm_pextp_mbist_ck", 1),
	FMCLK2(CKGEN, FM_AUD_1_CK, "fm_aud_1_ck", 0x00C0, 31, 1),
	FMCLK2(CKGEN, FM_AUD_2_CK, "fm_aud_2_ck", 0x00D0, 7, 1),
	FMCLK2(CKGEN, FM_ADSP_CK, "fm_adsp_ck", 0x00D0, 15, 1),
	FMCLK2(CKGEN, FM_DPMAIF_MAIN_CK, "fm_dpmaif_main_ck", 0x00D0, 23, 1),
	FMCLK2(CKGEN, FM_VENC_CK, "fm_venc_ck", 0x00D0, 31, 1),
	FMCLK2(CKGEN, FM_VDEC_CK, "fm_vdec_ck", 0x00E0, 7, 1),
	FMCLK2(CKGEN, FM_PWM_CK, "fm_pwm_ck", 0x00E0, 15, 1),
	FMCLK2(CKGEN, FM_AUDIO_H_CK, "fm_audio_h_ck", 0x00E0, 23, 1),
	FMCLK2(CKGEN, FM_MCUPM_CK, "fm_mcupm_ck", 0x00E0, 31, 1),
	FMCLK2(CKGEN, FM_MEM_SUB_CK, "fm_mem_sub_ck", 0x00F0, 7, 1),
	FMCLK2(CKGEN, FM_MEM_CK, "fm_mem_ck", 0x00F0, 15, 1),
	FMCLK2(CKGEN, FM_U_MEM_CK, "fm_u_mem_ck", 0x00F0, 23, 1),
	FMCLK2(CKGEN, FM_EMI_N_CK, "fm_emi_n_ck", 0x00F0, 31, 1),
	FMCLK2(CKGEN, FM_DSI_OCC_CK, "fm_dsi_occ_ck", 0x0100, 7, 1),
	FMCLK2(CKGEN, FM_CCU_AHB_CK, "fm_ccu_ahb_ck", 0x0100, 15, 1),
	FMCLK2(CKGEN, FM_AP2CONN_HOST_CK, "fm_ap2conn_host_ck", 0x0100, 23, 1),
	FMCLK2(CKGEN, FM_IMG1_CK, "fm_img1_ck", 0x0100, 31, 1),
	FMCLK2(CKGEN, FM_IPE_CK, "fm_ipe_ck", 0x0110, 7, 1),
	FMCLK2(CKGEN, FM_CAM_CK, "fm_cam_ck", 0x0110, 15, 1),
	FMCLK2(CKGEN, FM_CCUSYS_CK, "fm_ccusys_ck", 0x0110, 23, 1),
	FMCLK2(CKGEN, FM_CAMTM_CK, "fm_camtm_ck", 0x0110, 31, 1),
	FMCLK2(CKGEN, FM_MCU_ACP_CK, "fm_mcu_acp_ck", 0x0120, 7, 1),
	FMCLK2(CKGEN, FM_CSI_OCC_SCAN_CK, "fm_csi_occ_scan_ck", 0x0120, 15, 1),
	FMCLK2(CKGEN, FM_IPSWEST_CK, "fm_ipswest_ck", 0x0120, 23, 1),
	FMCLK2(CKGEN, FM_IPSNORTH_CK, "fm_ipsnorth_ck", 0x0120, 31, 1),
	FMCLK(CKGEN, FM_MCU_ATB_CK, "fm_mcu_atb_ck", 1),
	FMCLK2(CKGEN, FM_AXI_L3GIC_CK, "fm_axi_l3gic_ck", 0x0130, 15, 1),
	FMCLK(CKGEN, FM_DUMMY_1_CK, "fm_dummy_1_ck", 1),
	FMCLK(CKGEN, FM_DUMMY_2_CK, "fm_dummy_2_ck", 1),
	/* ABIST Part */
	FMCLK(ABIST, FM_LVTS_CKMON_LM, "fm_lvts_ckmon_lm", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L8, "fm_lvts_ckmon_l8", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L7, "fm_lvts_ckmon_l7", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L6, "fm_lvts_ckmon_l6", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L5, "fm_lvts_ckmon_l5", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L4, "fm_lvts_ckmon_l4", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L3, "fm_lvts_ckmon_l3", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L2, "fm_lvts_ckmon_l2", 1),
	FMCLK(ABIST, FM_LVTS_CKMON_L1, "fm_lvts_ckmon_l1", 1),
	FMCLK(ABIST, FM_RCLRPLL_DIV4_CHC, "fm_rclrpll_div4_chc", 1),
	FMCLK(ABIST, FM_RPHYPLL_DIV4_CHC, "fm_rphypll_div4_chc", 1),
	FMCLK(ABIST, FM_RCLRPLL_DIV4_CHA, "fm_rclrpll_div4_cha", 1),
	FMCLK(ABIST, FM_RPHYPLL_DIV4_CHA, "fm_rphypll_div4_cha", 1),
	FMCLK(ABIST, FM_ADSPPLL_CK, "fm_adsppll_ck", 1),
	FMCLK(ABIST, FM_APLL1_CK, "fm_apll1_ck", 1),
	FMCLK(ABIST, FM_APLL2_CK, "fm_apll2_ck", 1),
	FMCLK(ABIST, FM_APPLLGP_MON_FM_CK, "fm_appllgp_mon_fm_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_DIV3_CK, "fm_univpll_div3_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_DIV4_CK, "fm_univpll_div4_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_DIV5_CK, "fm_univpll_div5_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_DIV6_CK, "fm_univpll_div6_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_DIV7_CK, "fm_univpll_div7_ck", 1),
	FMCLK(ABIST, FM_CSI0A_CDPHY_DELAYCAL_CK, "fm_csi0a_cdphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_CSI0B_CDPHY_DELAYCAL_CK, "fm_csi0b_cdphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_CSI1A_DPHY_DELAYCAL_CK, "fm_csi1a_dphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_CSI1B_DPHY_DELAYCAL_CK, "fm_csi1b_dphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_CSI2A_DPHY_DELAYCAL_CK, "fm_csi2a_dphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_CSI2B_DPHY_DELAYCAL_CK, "fm_csi2b_dphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_CSI3A_DPHY_DELAYCAL_CK, "fm_csi3a_dphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_CSI3B_DPHY_DELAYCAL_CK, "fm_csi3b_dphy_delaycal_ck", 1),
	FMCLK(ABIST, FM_DSI0_LNTC_DSICLK, "fm_dsi0_lntc_dsiclk", 1),
	FMCLK(ABIST, FM_DSI0_MPPLL_TST_CK, "fm_dsi0_mppll_tst_ck", 1),
	FMCLK(ABIST, FM_MMPLL_D4_CK, "fm_mmpll_d4_ck", 1),
	FMCLK(ABIST, FM_MAINPLL_CK, "fm_mainpll_ck", 1),
	FMCLK(ABIST, FM_MDPLL1_FS26M_GUIDE, "fm_mdpll1_fs26m_guide", 1),
	FMCLK(ABIST, FM_MMPLL_D5_CK, "fm_mmpll_d5_ck", 1),
	FMCLK(ABIST, FM_MMPLL_CK, "fm_mmpll_ck", 1),
	FMCLK(ABIST, FM_MMPLL_D3_CK, "fm_mmpll_d3_ck", 1),
	FMCLK(ABIST, FM_MPLL_CK, "fm_mpll_ck", 1),
	FMCLK(ABIST, FM_MSDCPLL_CK, "fm_msdcpll_ck", 1),
	FMCLK(ABIST, FM_IMGPLL_CK, "fm_imgpll_ck", 1),
	FMCLK(ABIST, FM_EMIPLL_CK, "fm_emipll_ck", 1),
	FMCLK(ABIST, FM_UFSPLL_CK, "fm_ufspll_ck", 1),
	FMCLK(ABIST, FM_ULPOSC2_MON_V_VCORE_CK, "fm_ulposc2_mon_v_vcore_ck", 1),
	FMCLK(ABIST, FM_ULPOSC_MON_VCROE_CK, "fm_ulposc_mon_vcroe_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_CK, "fm_univpll_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_DIV2_CK, "fm_univpll_div2_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_192M_CK, "fm_univpll_192m_ck", 1),
	FMCLK(ABIST, FM_U_CLK2FREQ, "fm_u_clk2freq", 1),
	FMCLK(ABIST, FM_WBG_DIG_BPLL_CK, "fm_wbg_dig_bpll_ck", 1),
	FMCLK(ABIST, FM_WBG_DIG_WPLL_CK960, "fm_wbg_dig_wpll_ck960", 1),
	FMCLK(ABIST, FM_466M_FMEM_INFRASYS, "fm_466m_fmem_infrasys", 1),
	FMCLK(ABIST, FM_MCUSYS_ARM_OUT_ALL, "fm_mcusys_arm_out_all", 1),
	FMCLK(ABIST, FM_APPLLGP_MON_FM_CK_2, "fm_appllgp_mon_fm_ck_2", 1),
	FMCLK(ABIST, FM_MMPLL_D6_CK, "fm_mmpll_d6_ck", 1),
	FMCLK(ABIST, FM_MMPLL_D7_CK, "fm_mmpll_d7_ck", 1),
	FMCLK(ABIST, FM_MMPLL_D9_CK, "fm_mmpll_d9_ck", 1),
	FMCLK(ABIST, FM_MAINPLL_DIV3_CK, "fm_mainpll_div3_ck", 1),
	FMCLK(ABIST, FM_MSDC11_IN_CK, "fm_msdc11_in_ck", 1),
	FMCLK(ABIST, FM_MSDC01_IN_CK, "fm_msdc01_in_ck", 1),
	FMCLK(ABIST, FM_F32K_VCORE_CK, "fm_f32k_vcore_ck", 1),
	FMCLK(ABIST, FM_MAINPLL_DIV4_CK, "fm_mainpll_div4_ck", 1),
	FMCLK(ABIST, FM_MAINPLL_DIV5_CK, "fm_mainpll_div5_ck", 1),
	FMCLK(ABIST, FM_MAINPLL_DIV6_CK, "fm_mainpll_div6_ck", 1),
	FMCLK(ABIST, FM_MAINPLL_DIV7_CK, "fm_mainpll_div7_ck", 1),
	FMCLK(ABIST, FM_UNIVPLL_DIV3_CK_2, "fm_univpll_div3_ck_2", 1),
	FMCLK3(ABIST, FM_APLL2_CKDIV_CK, "fm_apll2_ckdiv_ck", 0x0340, 1, 8),
	FMCLK3(ABIST, FM_APLL1_CKDIV_CK, "fm_apll1_ckdiv_ck", 0x032c, 1, 8),
	FMCLK3(ABIST, FM_ADSPPLL_CKDIV_CK, "fm_adsppll_ckdiv_ck", 0x0384, 1, 8),
	FMCLK3(ABIST, FM_UFSPLL_CKDIV_CK, "fm_ufspll_ckdiv_ck", 0x024c, 1, 8),
	FMCLK3(ABIST, FM_MPLL_CKDIV_CK, "fm_mpll_ckdiv_ck", 0x0394, 1, 8),
	FMCLK3(ABIST, FM_MMPLL_CKDIV_CK, "fm_mmpll_ckdiv_ck", 0x03A4, 1, 8),
	FMCLK3(ABIST, FM_MAINPLL_CKDIV_CK, "fm_mainpll_ckdiv_ck", 0x0354, 1, 8),
	FMCLK3(ABIST, FM_IMGPLL_CKDIV_CK, "fm_imgpll_ckdiv_ck", 0x0374, 1, 8),
	FMCLK3(ABIST, FM_EMIPLL_CKDIV_CK, "fm_emipll_ckdiv_ck", 0x03B4, 1, 8),
	FMCLK3(ABIST, FM_MSDCPLL_CKDIV_CK, "fm_msdcpll_ckdiv_ck", 0x0364, 1, 8),
	/* VLPCK Part */
	FMCLK2(VLPCK, FM_SCP_VLP_CK, "fm_scp_vlp_ck", 0x0008, 7, 1),
	FMCLK2(VLPCK, FM_PWRAP_ULPOSC_CK, "fm_pwrap_ulposc_ck", 0x0008, 15, 1),
	FMCLK2(VLPCK, FM_F26M_APXGPT_VLP_CK, "fm_f26m_apxgpt_vlp_ck", 0x0008, 23, 1),
	FMCLK2(VLPCK, FM_DXCC_VLP_CK, "fm_dxcc_vlp_ck", 0x0008, 31, 1),
	FMCLK2(VLPCK, FM_SPMI_P_CK, "fm_spmi_p_ck", 0x0014, 7, 1),
	FMCLK2(VLPCK, FM_SPMI_M_CK, "fm_spmi_m_ck", 0x0014, 15, 1),
	FMCLK2(VLPCK, FM_DVFSRC_CK, "fm_dvfsrc_ck", 0x0014, 23, 1),
	FMCLK2(VLPCK, FM_PWM_VLP_CK, "fm_pwm_vlp_ck", 0x0014, 31, 1),
	FMCLK2(VLPCK, FM_AXI_VLP_CK, "fm_axi_vlp_ck", 0x0020, 7, 1),
	FMCLK2(VLPCK, FM_DBGAO_26M_VLP_CK, "fm_dbgao_26m_vlp_ck", 0x0020, 15, 1),
	FMCLK2(VLPCK, FM_SYSTIMER_26M_VLP_CK, "fm_systimer_26m_vlp_ck", 0x0020, 23, 1),
	FMCLK2(VLPCK, FM_SSPM_VLP_CK, "fm_sspm_vlp_ck", 0x0020, 31, 1),
	FMCLK2(VLPCK, FM_SSPM_F26M_CK, "fm_sspm_f26m_ck", 0x002C, 7, 1),
	FMCLK2(VLPCK, FM_SRCK_CK, "fm_srck_ck", 0x002C, 15, 1),
	FMCLK2(VLPCK, FM_SCP_SPI_CK, "fm_scp_spi_ck", 0x002C, 23, 1),
	FMCLK2(VLPCK, FM_SCP_IIC_CK, "fm_scp_iic_ck", 0x002C, 31, 1),
	FMCLK2(VLPCK, FM_PSVLP_CK, "fm_psvlp_ck", 0x0038, 7, 1),
	FMCLK(VLPCK, FM_MD_BUCK_26M_CK, "fm_md_buck_26m_ck", 1),
	FMCLK(VLPCK, FM_SSPM_ULPOSC_CK, "fm_sspm_ulposc_ck", 1),
	FMCLK(VLPCK, FM_CLKRTC, "fm_clkrtc", 1),
	FMCLK(VLPCK, FM_OSC_2, "fm_osc_2", 1),
	FMCLK(VLPCK, FM_OSC_CK, "fm_osc_ck", 1),
	FMCLK(VLPCK, FM_OSC_CKDIV_CK, "fm_osc_ckdiv_ck", 1),
	FMCLK(VLPCK, FM_OSC_CKDIV_2, "fm_osc_ckdiv_2", 1),
	{},
};

const struct fmeter_clk *mt6886_get_fmeter_clks(void)
{
	return fclks;
}

static unsigned int check_pdn(void __iomem *base,
		unsigned int type, unsigned int ID)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fclks) - 1; i++) {
		if (fclks[i].type == type && fclks[i].id == ID)
			break;
	}

	if (i >= ARRAY_SIZE(fclks) - 1)
		return 1;

	if (!fclks[i].ofs)
		return 0;

	if (type == SUBSYS) {
		if ((clk_readl(base + fclks[i].ofs) & fclks[i].pdn)
				!= fclks[i].pdn) {
			return 1;
		}
	} else if (type != SUBSYS && ((clk_readl(base + fclks[i].ofs)
			& BIT(fclks[i].pdn)) == BIT(fclks[i].pdn)))
		return 1;

	return 0;
}

static unsigned int get_post_div(unsigned int type, unsigned int ID)
{
	unsigned int post_div = 1;
	int i;

	if ((ID <= 0) || (ID >= FM_ABIST_NUM))
		return post_div;

	for (i = 0; i < ARRAY_SIZE(fclks) - 1; i++) {
		if (fclks[i].type == type && fclks[i].id == ID
				&& fclks[i].grp == 1) {
			post_div =  clk_readl(fm_base[FM_APMIXED] + fclks[i].ofs);
			post_div = 1 << ((post_div >> 24) & 0x7);
			break;
		}
	}

	return post_div;
}

static unsigned int get_clk_div(unsigned int type, unsigned int ID)
{
	unsigned int clk_div = 1;
	int i;

	if ((ID <= 0) || (ID >= FM_CKGEN_NUM))
		return clk_div;

	for (i = 0; i < ARRAY_SIZE(fclks) - 1; i++)
		if (fclks[i].type == type && fclks[i].id == ID)
			break;

	if (i >= ARRAY_SIZE(fclks) - 1)
		return clk_div;

	return fclks[i].ck_div;
}

/* need implement ckgen&abist api here */

/* implement done */
static int __mt_get_freq(unsigned int ID, int type)
{
	void __iomem *dbg_addr = fm_base[FM_TOPCKGEN] + CLK_DBG_CFG;
	void __iomem *misc_addr = fm_base[FM_TOPCKGEN] + CLK_MISC_CFG_0;
	void __iomem *cali0_addr = fm_base[FM_TOPCKGEN] + CLK26CALI_0;
	void __iomem *cali1_addr = fm_base[FM_TOPCKGEN] + CLK26CALI_1;
	unsigned int temp, clk_dbg_cfg, clk_misc_cfg_0, clk26cali_1 = 0;
	unsigned int clk_div = 1, post_div = 1;
	unsigned long flags;
	int output = 0, i = 0;

	fmeter_lock(flags);

	if (type == CKGEN && check_pdn(fm_base[FM_TOPCKGEN], CKGEN, ID)) {
		pr_notice("ID-%d: MUX PDN, return 0.\n", ID);
		fmeter_unlock(flags);
		return 0;
	}

	while (clk_readl(cali0_addr) & 0x10) {
		udelay(10);
		i++;
		if (i > FM_TIMEOUT)
			break;
	}

	/* CLK26CALI_0[15]: rst 1 -> 0 */
	clk_writel(cali0_addr, (clk_readl(cali0_addr) & 0xFFFF7FFF));
	/* CLK26CALI_0[15]: rst 0 -> 1 */
	clk_writel(cali0_addr, (clk_readl(cali0_addr) | 0x00008000));

	if (type == CKGEN) {
		clk_dbg_cfg = clk_readl(dbg_addr);
		clk_writel(dbg_addr,
			(clk_dbg_cfg & 0xFFFF80FC) | (ID << 8) | (0x1));
	} else if (type == ABIST) {
		clk_dbg_cfg = clk_readl(dbg_addr);
		clk_writel(dbg_addr,
			(clk_dbg_cfg & 0xFF80FFFC) | (ID << 16));
	} else {
		fmeter_unlock(flags);
		return 0;
	}

	clk_misc_cfg_0 = clk_readl(misc_addr);
	clk_writel(misc_addr, (clk_misc_cfg_0 & 0x00FFFFFF) | (3 << 24));

	clk26cali_1 = clk_readl(cali1_addr);
	clk_writel(cali0_addr, 0x9000);
	clk_writel(cali0_addr, 0x9010);

	/* wait frequency meter finish */
	i = 0;
	do {
		udelay(1);
		i++;
		if (i > FM_TIMEOUT)
			break;
	} while (clk_readl(cali0_addr) & 0x10);

	temp = clk_readl(cali1_addr) & 0xFFFF;

	if (type == ABIST)
		post_div = get_post_div(type, ID);

	clk_div = get_clk_div(type, ID);

	output = (temp * 26000) / 1024 * clk_div / post_div;

	clk_writel(dbg_addr, clk_dbg_cfg);
	clk_writel(misc_addr, clk_misc_cfg_0);
	/*clk_writel(CLK26CALI_0, clk26cali_0);*/
	/*clk_writel(CLK26CALI_1, clk26cali_1);*/

	clk_writel(cali0_addr, 0x8000);
	fmeter_unlock(flags);

	if (i > FM_TIMEOUT)
		return 0;

	if ((output * 4) < 1000) {
		pr_notice("%s(%d): CLK_DBG_CFG = 0x%x, CLK_MISC_CFG_0 = 0x%x, CLK26CALI_0 = 0x%x, CLK26CALI_1 = 0x%x\n",
			__func__,
			ID,
			clk_readl(dbg_addr),
			clk_readl(misc_addr),
			clk_readl(cali0_addr),
			clk_readl(cali1_addr));
	}

	return (output * 4);
}

static int __mt_get_freq2(unsigned int  type, unsigned int id)
{
	void __iomem *pll_con0 = fm_base[type] + subsys_fm[type].pll_con0;
	void __iomem *pll_con1 = fm_base[type] + subsys_fm[type].pll_con1;
	void __iomem *con0 = fm_base[type] + subsys_fm[type].con0;
	void __iomem *con1 = fm_base[type] + subsys_fm[type].con1;
	unsigned int temp, clk_div = 1, post_div = 1;
	unsigned long flags;
	int output = 0, i = 0;

	fmeter_lock(flags);

	/* PLL4H_FQMTR_CON1[15]: rst 1 -> 0 */
	clk_writel(con0, clk_readl(con0) & 0xFFFF7FFF);
	/* PLL4H_FQMTR_CON1[15]: rst 0 -> 1 */
	clk_writel(con0, clk_readl(con0) | 0x8000);

	/* sel fqmtr_cksel */
	if (type == FM_VLP_CKSYS)
		clk_writel(con0, (clk_readl(con0) & 0xFFE0FFFF) | (id << 16));
	else
		clk_writel(con0, (clk_readl(con0) & 0x00FFFFF8) | (id << 0));
	/* set ckgen_load_cnt to 1024 */
	clk_writel(con1, (clk_readl(con1) & 0xFC00FFFF) | (0x3FF << 16));

	/* sel fqmtr_cksel and set ckgen_k1 to 0(DIV4) */
	clk_writel(con0, (clk_readl(con0) & 0x00FFFFFF) | (3 << 24));

	/* fqmtr_en set to 1, fqmtr_exc set to 0, fqmtr_start set to 0 */
	clk_writel(con0, (clk_readl(con0) & 0xFFFF8007) | 0x1000);
	/*fqmtr_start set to 1 */
	clk_writel(con0, clk_readl(con0) | 0x10);

	/* wait frequency meter finish */
	if (type == FM_VLP_CKSYS) {
		udelay(VLP_FM_WAIT_TIME);
	} else {
		while (clk_readl(con0) & 0x10) {
			udelay(10);
			i++;
			if (i > 30) {
				pr_notice("[%d]con0: 0x%x, con1: 0x%x\n",
					id, clk_readl(con0), clk_readl(con1));
				break;
			}
		}
	}

	temp = clk_readl(con1) & 0xFFFF;
	output = ((temp * 26000)) / 1024; // Khz

	if (type != FM_VLP_CKSYS && id == FM_PLL_CKDIV_CK)
		clk_div = (clk_readl(pll_con0) & FM_CKDIV_MASK) >> FM_CKDIV_SHIFT;

	if (clk_div == 0)
		clk_div = 1;

	if (type != FM_VLP_CKSYS)
		post_div = 1 << ((clk_readl(pll_con1) & FM_POSTDIV_MASK) >> FM_POSTDIV_SHIFT);

	clk_writel(con0, 0x8000);

	fmeter_unlock(flags);

	return (output * 4 * clk_div) / post_div;
}

static unsigned int mt6886_get_ckgen_freq(unsigned int ID)
{
	return __mt_get_freq(ID, CKGEN);
}

static unsigned int mt6886_get_abist_freq(unsigned int ID)
{
	return __mt_get_freq(ID, ABIST);
}

static unsigned int mt6886_get_vlpck_freq(unsigned int ID)
{
	return __mt_get_freq2(FM_VLP_CKSYS, ID);
}

static unsigned int mt6886_get_subsys_freq(unsigned int ID)
{
	int output = 0;
	unsigned long flags;

	subsys_fmeter_lock(flags);

	pr_notice("subsys ID: %d\n", ID);
	if (ID >= FM_SYS_NUM)
		return 0;

	output = __mt_get_freq2(ID, FM_PLL_CKDIV_CK);

	subsys_fmeter_unlock(flags);

	return output;
}

static unsigned int mt6886_get_fmeter_freq(unsigned int id,
		enum FMETER_TYPE type)
{
	if (type == CKGEN)
		return mt6886_get_ckgen_freq(id);
	else if (type == ABIST)
		return mt6886_get_abist_freq(id);
	else if (type == SUBSYS)
		return mt6886_get_subsys_freq(id);
	else if (type == VLPCK)
		return mt6886_get_vlpck_freq(id);

	return FT_NULL;
}

static int mt6886_get_fmeter_id(enum FMETER_ID fid)
{
	if (fid == FID_DISP_PWM)
		return FM_DISP_PWM_CK;
	else if (fid == FID_ULPOSC1)
		return FM_OSC_CK;
	else if (fid == FID_ULPOSC2)
		return FM_OSC_2;

	return FID_NULL;
}

static void __iomem *get_base_from_comp(const char *comp)
{
	struct device_node *node;
	static void __iomem *base;

	node = of_find_compatible_node(NULL, NULL, comp);
	if (node) {
		base = of_iomap(node, 0);
		if (!base) {
			pr_err("%s() can't find iomem for %s\n",
					__func__, comp);
			return ERR_PTR(-EINVAL);
		}

		return base;
	}

	pr_err("%s can't find compatible node\n", __func__);

	return ERR_PTR(-EINVAL);
}

/*
 * init functions
 */

static struct fmeter_ops fm_ops = {
	.get_fmeter_clks = mt6886_get_fmeter_clks,
	.get_fmeter_freq = mt6886_get_fmeter_freq,
	.get_fmeter_id = mt6886_get_fmeter_id,
};

static int clk_fmeter_mt6886_probe(struct platform_device *pdev)
{
	int i;

	for (i = 0; i < FM_SYS_NUM; i++) {
		fm_base[i] = get_base_from_comp(comp_list[i]);
		if (IS_ERR(fm_base[i]))
			goto ERR;

	}

	fmeter_set_ops(&fm_ops);

	return 0;
ERR:
	pr_err("%s(%s) can't find base\n", __func__, comp_list[i]);

	return -EINVAL;
}

static struct platform_driver clk_fmeter_mt6886_drv = {
	.probe = clk_fmeter_mt6886_probe,
	.driver = {
		.name = "clk-fmeter-mt6886",
		.owner = THIS_MODULE,
	},
};

static int __init clk_fmeter_init(void)
{
	static struct platform_device *clk_fmeter_dev;

	clk_fmeter_dev = platform_device_register_simple("clk-fmeter-mt6886", -1, NULL, 0);
	if (IS_ERR(clk_fmeter_dev))
		pr_warn("unable to register clk-fmeter device");

	return platform_driver_register(&clk_fmeter_mt6886_drv);
}

static void __exit clk_fmeter_exit(void)
{
	platform_driver_unregister(&clk_fmeter_mt6886_drv);
}

subsys_initcall(clk_fmeter_init);
module_exit(clk_fmeter_exit);
MODULE_LICENSE("GPL");
