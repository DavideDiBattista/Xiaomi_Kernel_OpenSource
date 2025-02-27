// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2021 MediaTek Inc.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>

#ifndef DRM_CMDQ_DISABLE
#include <linux/soc/mediatek/mtk-cmdq-ext.h>
#else
#include "mtk-cmdq-ext.h"
#endif

#include "mtk_drm_crtc.h"
#include "mtk_drm_ddp_comp.h"
#include "mtk_drm_drv.h"
#include "mtk_log.h"
#include "mtk_disp_gamma.h"
#include "mtk_dump.h"
#include "mtk_drm_mmp.h"
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/sched.h>
#include <uapi/linux/sched/types.h>

#ifdef CONFIG_LEDS_MTK_MODULE
#define CONFIG_LEDS_BRIGHTNESS_CHANGED
#include <linux/leds-mtk.h>
#else
#define mtk_leds_brightness_set(x, y) do { } while (0)
#endif

#define DISP_GAMMA_EN 0x0000
#define DISP_GAMMA_SHADOW_SRAM 0x0014
#define DISP_GAMMA_CFG 0x0020
#define DISP_GAMMA_SIZE 0x0030
#define DISP_GAMMA_PURE_COLOR 0x0038
#define DISP_GAMMA_BANK 0x0100
#define DISP_GAMMA_LUT 0x0700
#define DISP_GAMMA_LUT_0 0x0700
#define DISP_GAMMA_LUT_1 0x0B00

#define DISP_GAMMA_BLOCK_0_R_GAIN 0x0054
#define DISP_GAMMA_BLOCK_0_G_GAIN 0x0058
#define DISP_GAMMA_BLOCK_0_B_GAIN 0x005C

#define DISP_GAMMA_BLOCK_12_R_GAIN 0x0060
#define DISP_GAMMA_BLOCK_12_G_GAIN 0x0064
#define DISP_GAMMA_BLOCK_12_B_GAIN 0x0068


#define LUT_10BIT_MASK 0x03ff

#define GAMMA_EN BIT(0)
#define GAMMA_LUT_EN BIT(1)
#define GAMMA_RELAYMODE BIT(0)
#define DISP_GAMMA_BLOCK_SIZE 256
#define DISP_GAMMA_GAIN_SIZE 3

static unsigned int g_gamma_relay_value;
#define index_of_gamma(module) ((module == DDP_COMPONENT_GAMMA0) ? 0 : 1)
// It's a work around for no comp assigned in functions.
static struct mtk_ddp_comp *default_comp;
static struct mtk_ddp_comp *default_comp1;

unsigned int g_gamma_data_mode;

struct gamma_color_protect {
	unsigned int gamma_color_protect_support;
	unsigned int gamma_color_protect_lsb;
};

static struct gamma_color_protect g_gamma_color_protect;

struct gamma_color_protect_mode {
	unsigned int red_support;
	unsigned int green_support;
	unsigned int blue_support;
	unsigned int black_support;
	unsigned int white_support;
};

static struct DISP_GAMMA_LUT_T *g_disp_gamma_lut;
static struct DISP_GAMMA_12BIT_LUT_T *g_disp_gamma_12bit_lut;
static struct DISP_GAMMA_LUT_T g_disp_gamma_lut_db;
static struct DISP_GAMMA_12BIT_LUT_T g_disp_gamma_12bit_lut_db;

static DEFINE_MUTEX(g_gamma_global_lock);
static DEFINE_MUTEX(g_gamma_sram_lock);

static atomic_t g_gamma_is_clock_on[DISP_GAMMA_TOTAL] = { ATOMIC_INIT(0),
	ATOMIC_INIT(0)};

static DEFINE_SPINLOCK(g_gamma_clock_lock);

static atomic_t g_gamma_sof_filp = ATOMIC_INIT(0);
static struct task_struct *gamma_sof_irq_event_task;
static DECLARE_WAIT_QUEUE_HEAD(g_gamma_sof_irq_wq);
static atomic_t g_gamma_sof_irq_available = ATOMIC_INIT(0);
static struct DISP_GAMMA_12BIT_LUT_T ioctl_data;
struct mtk_ddp_comp *g_gamma_flip_comp[2];
// g_force_delay_check_trig: 0: non-delay 1: delay 2: default setting
//                           3: not check trigge
static atomic_t g_force_delay_check_trig = ATOMIC_INIT(2);

/* TODO */
/* static ddp_module_notify g_gamma_ddp_notify; */

enum GAMMA_IOCTL_CMD {
	SET_GAMMALUT = 0,
	SET_12BIT_GAMMALUT,
	BYPASS_GAMMA,
	SET_GAMMAGAIN,
	DISABLE_MUL_EN
};

enum GAMMA_MODE {
	HW_8BIT = 0,
	HW_12BIT_MODE_8BIT,
	HW_12BIT_MODE_12BIT,
};

struct mtk_disp_gamma {
	struct mtk_ddp_comp ddp_comp;
	struct drm_crtc *crtc;
};

struct mtk_disp_gamma_tile_overhead {
	unsigned int left_in_width;
	unsigned int left_overhead;
	unsigned int left_comp_overhead;
	unsigned int right_in_width;
	unsigned int right_overhead;
	unsigned int right_comp_overhead;
};

struct mtk_disp_gamma_tile_overhead gamma_tile_overhead = { 0 };

struct mtk_disp_gamma_sb_param {
	unsigned int gain[3];
	unsigned int bl;
};
struct mtk_disp_gamma_sb_param g_sb_param;

static void mtk_gamma_init(struct mtk_ddp_comp *comp,
	struct mtk_ddp_config *cfg, struct cmdq_pkt *handle)
{
	unsigned int width;

	if (comp->mtk_crtc->is_dual_pipe && cfg->tile_overhead.is_support)
		width = gamma_tile_overhead.left_in_width;
	else {
		if (comp->mtk_crtc->is_dual_pipe)
			width = cfg->w / 2;
		else
			width = cfg->w;
	}

	cmdq_pkt_write(handle, comp->cmdq_base,
		comp->regs_pa + DISP_GAMMA_SIZE,
		(width << 16) | cfg->h, ~0);
	if (g_gamma_data_mode == HW_12BIT_MODE_8BIT ||
		g_gamma_data_mode == HW_12BIT_MODE_12BIT) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_BANK,
			(g_gamma_data_mode - 1) << 2, 0x4);
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_PURE_COLOR,
			g_gamma_color_protect.gamma_color_protect_support |
			g_gamma_color_protect.gamma_color_protect_lsb, ~0);
	}
	cmdq_pkt_write(handle, comp->cmdq_base,
		comp->regs_pa + DISP_GAMMA_EN, GAMMA_EN, ~0);

//	atomic_set(&g_gamma_sof_filp, 0);
	atomic_set(&g_gamma_sof_irq_available, 0);
}


static void mtk_disp_gamma_config_overhead(struct mtk_ddp_comp *comp,
	struct mtk_ddp_config *cfg)
{
	DDPINFO("line: %d\n", __LINE__);

	if (cfg->tile_overhead.is_support) {
		/*set component overhead*/
		if (comp->id == DDP_COMPONENT_GAMMA0) {
			gamma_tile_overhead.left_comp_overhead = 0;
			/*add component overhead on total overhead*/
			cfg->tile_overhead.left_overhead += gamma_tile_overhead.left_comp_overhead;
			cfg->tile_overhead.left_in_width += gamma_tile_overhead.left_comp_overhead;
			/*copy from total overhead info*/
			gamma_tile_overhead.left_in_width = cfg->tile_overhead.left_in_width;
			gamma_tile_overhead.left_overhead = cfg->tile_overhead.left_overhead;
		}
		if (comp->id == DDP_COMPONENT_GAMMA1) {
			gamma_tile_overhead.right_comp_overhead = 0;
			/*add component overhead on total overhead*/
			cfg->tile_overhead.right_overhead +=
				gamma_tile_overhead.right_comp_overhead;
			cfg->tile_overhead.right_in_width +=
				gamma_tile_overhead.right_comp_overhead;
			/*copy from total overhead info*/
			gamma_tile_overhead.right_in_width = cfg->tile_overhead.right_in_width;
			gamma_tile_overhead.right_overhead = cfg->tile_overhead.right_overhead;
		}
	}

}

static void mtk_gamma_config(struct mtk_ddp_comp *comp,
			     struct mtk_ddp_config *cfg,
			     struct cmdq_pkt *handle)
{
	/* TODO: only call init function if frame dirty */
	mtk_gamma_init(comp, cfg, handle);
	//cmdq_pkt_write(handle, comp->cmdq_base,
	//	comp->regs_pa + DISP_GAMMA_SIZE,
	//	(cfg->w << 16) | cfg->h, ~0);
	//cmdq_pkt_write(handle, comp->cmdq_base,
	//	comp->regs_pa + DISP_GAMMA_CFG,
	//	GAMMA_RELAYMODE, BIT(0));
}

static int mtk_gamma_write_lut_reg(struct mtk_ddp_comp *comp,
	struct cmdq_pkt *handle, int lock)
{
	struct DISP_GAMMA_LUT_T *gamma_lut;
	int i;
	int ret = 0;

	if (lock)
		mutex_lock(&g_gamma_global_lock);
	gamma_lut = g_disp_gamma_lut;
	if (gamma_lut == NULL) {
		DDPINFO("%s: table not initialized\n", __func__);
		ret = -EFAULT;
		goto gamma_write_lut_unlock;
	}

	for (i = 0; i < DISP_GAMMA_LUT_SIZE; i++) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			(comp->regs_pa + DISP_GAMMA_LUT + i * 4),
			gamma_lut->lut[i], ~0);

		if ((i & 0x3f) == 0) {
			DDPINFO("[0x%08lx](%d) = 0x%x\n",
				(long)(comp->regs_pa + DISP_GAMMA_LUT + i * 4),
				i, gamma_lut->lut[i]);
		}
	}
	i--;
	DDPINFO("[0x%08lx](%d) = 0x%x\n",
		(long)(comp->regs_pa + DISP_GAMMA_LUT + i * 4),
		i, gamma_lut->lut[i]);

	if ((int)(gamma_lut->lut[0] & 0x3FF) -
		(int)(gamma_lut->lut[510] & 0x3FF) > 0) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x1 << 2, 0x4);
		DDPINFO("decreasing LUT\n");
	} else {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x0 << 2, 0x4);
		DDPINFO("Incremental LUT\n");
	}

	cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG,
			0x2 | g_gamma_relay_value, 0x3);

gamma_write_lut_unlock:
	if (lock)
		mutex_unlock(&g_gamma_global_lock);

	return ret;
}

void disp_gamma_on_start_of_frame(void)
{

	if ((!atomic_read(&g_gamma_sof_irq_available)) && (atomic_read(&g_gamma_sof_filp))) {
		DDPINFO("%s: wake up thread\n", __func__);
		atomic_set(&g_gamma_sof_irq_available, 1);
		wake_up_interruptible(&g_gamma_sof_irq_wq);
	}
}

static int mtk_gamma_write_12bit_lut_reg(struct mtk_ddp_comp *comp,
	struct cmdq_pkt *handle, int lock)
{
	struct DISP_GAMMA_12BIT_LUT_T *gamma_lut;
	int i, j, block_num;
	int ret = 0;
	unsigned int table_config_sel, table_out_sel;

	if (lock)
		mutex_lock(&g_gamma_global_lock);
	gamma_lut = g_disp_gamma_12bit_lut;
	if (gamma_lut == NULL) {
		DDPINFO("%s: table not initialized\n", __func__);
		ret = -EFAULT;
		goto gamma_write_lut_unlock;
	}

	if (g_gamma_data_mode == HW_12BIT_MODE_12BIT) {
		block_num = DISP_GAMMA_12BIT_LUT_SIZE / DISP_GAMMA_BLOCK_SIZE;
	} else if (g_gamma_data_mode == HW_12BIT_MODE_8BIT) {
		block_num = DISP_GAMMA_LUT_SIZE / DISP_GAMMA_BLOCK_SIZE;
	} else {
		DDPINFO("%s: g_gamma_data_mode is error\n", __func__);
		return -1;
	}

	if (readl(comp->regs + DISP_GAMMA_SHADOW_SRAM) & 0x2) {
		table_config_sel = 0;
		table_out_sel = 0;
	} else {
		table_config_sel = 1;
		table_out_sel = 1;
	}

	writel(table_config_sel << 1 |
		(readl(comp->regs + DISP_GAMMA_SHADOW_SRAM) & 0x1),
		comp->regs + DISP_GAMMA_SHADOW_SRAM);

	for (i = 0; i < block_num; i++) {
		writel(i | (g_gamma_data_mode - 1) << 2,
			comp->regs + DISP_GAMMA_BANK);
		for (j = 0; j < DISP_GAMMA_BLOCK_SIZE; j++) {
			writel(gamma_lut->lut_0[i * DISP_GAMMA_BLOCK_SIZE + j],
				comp->regs + DISP_GAMMA_LUT_0 + j * 4);
			writel(gamma_lut->lut_1[i * DISP_GAMMA_BLOCK_SIZE + j],
				comp->regs + DISP_GAMMA_LUT_1 + j * 4);
		}
	}

	if ((int)(gamma_lut->lut_0[0] & 0x3FF) -
		(int)(gamma_lut->lut_0[510] & 0x3FF) > 0) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x1 << 2, 0x4);
		DDPINFO("decreasing LUT\n");
	} else {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x0 << 2, 0x4);
		DDPINFO("Incremental LUT\n");
	}

	cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG,
			0x2 | g_gamma_relay_value, 0x3);

	cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_SHADOW_SRAM,
			table_config_sel << 1 | table_out_sel, ~0);
gamma_write_lut_unlock:
	if (lock)
		mutex_unlock(&g_gamma_global_lock);

	return ret;
}

static int mtk_gamma_write_gain_reg(struct mtk_ddp_comp *comp,
	struct cmdq_pkt *handle, int lock)
{
	int i;
	int ret = 0;

	if (lock)
		mutex_lock(&g_gamma_global_lock);

	if ((g_sb_param.gain[0] == 8192) && (g_sb_param.gain[1] == 8192)
		&& (g_sb_param.gain[2] == 8192)) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x0 << 3, 0x8);
		DDPINFO("all gain == 8192\n");
		return ret;
	}

	if ((g_sb_param.gain[0] == 0) && (g_sb_param.gain[1] == 0) && (g_sb_param.gain[2] == 0)) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x0 << 3, 0x8);
		DDPINFO("all gain == 0\n");
		return ret;
	}

	for (i = 0; i < DISP_GAMMA_GAIN_SIZE; i++) {
		if (g_sb_param.gain[i] == 8192)
			g_sb_param.gain[i] = 8191;
	}

	for (i = 0; i < DISP_GAMMA_GAIN_SIZE; i++) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_BLOCK_0_R_GAIN + i * 4, g_sb_param.gain[i], ~0);
	}

	cmdq_pkt_write(handle, comp->cmdq_base,
		comp->regs_pa + DISP_GAMMA_CFG, 0x1 << 3, 0x8);

	if (lock)
		mutex_unlock(&g_gamma_global_lock);
	return ret;
}

static int mtk_gamma_set_lut(struct mtk_ddp_comp *comp,
	struct cmdq_pkt *handle, struct DISP_GAMMA_LUT_T *user_gamma_lut)
{
	/* TODO: use CPU to write register */
	int ret = 0;
	int id;
	struct DISP_GAMMA_LUT_T *gamma_lut, *old_lut;

	DDPINFO("%s\n", __func__);

	gamma_lut = kmalloc(sizeof(struct DISP_GAMMA_LUT_T),
		GFP_KERNEL);
	if (gamma_lut == NULL) {
		DDPPR_ERR("%s: no memory\n", __func__);
		return -EFAULT;
	}

	if (user_gamma_lut == NULL) {
		ret = -EFAULT;
		kfree(gamma_lut);
	} else {
		memcpy(gamma_lut, user_gamma_lut,
			sizeof(struct DISP_GAMMA_LUT_T));
		id = index_of_gamma(comp->id);

		if (id >= 0 && id < DISP_GAMMA_TOTAL) {
			mutex_lock(&g_gamma_global_lock);

			old_lut = g_disp_gamma_lut;
			g_disp_gamma_lut = gamma_lut;

			DDPINFO("%s: Set module(%d) lut\n", __func__, comp->id);
			ret = mtk_gamma_write_lut_reg(comp, handle, 0);

			mutex_unlock(&g_gamma_global_lock);

			if (old_lut != NULL)
				kfree(old_lut);

			//if (comp->mtk_crtc != NULL)
			//	mtk_crtc_check_trigger(comp->mtk_crtc, false,
			//		false);
		} else {
			DDPPR_ERR("%s: invalid ID = %d\n", __func__, comp->id);
			ret = -EFAULT;
		}
	}

	return ret;
}

static int mtk_gamma_12bit_set_lut(struct mtk_ddp_comp *comp,
	struct cmdq_pkt *handle, struct DISP_GAMMA_12BIT_LUT_T *user_gamma_lut)
{
	/* TODO: use CPU to write register */
	int ret = 0;
	int id;
	struct DISP_GAMMA_12BIT_LUT_T *gamma_lut, *old_lut;

	DDPINFO("%s\n", __func__);

	gamma_lut = kmalloc(sizeof(struct DISP_GAMMA_12BIT_LUT_T),
		GFP_KERNEL);
	if (gamma_lut == NULL) {
		DDPPR_ERR("%s: no memory\n", __func__);
		return -EFAULT;
	}

	if (user_gamma_lut == NULL) {
		ret = -EFAULT;
		kfree(gamma_lut);
	} else {
		memcpy(gamma_lut, user_gamma_lut,
			sizeof(struct DISP_GAMMA_12BIT_LUT_T));
		id = index_of_gamma(comp->id);

		if (id >= 0 && id < DISP_GAMMA_TOTAL) {
			mutex_lock(&g_gamma_global_lock);

			old_lut = g_disp_gamma_12bit_lut;
			g_disp_gamma_12bit_lut = gamma_lut;

			DDPINFO("%s: Set module(%d) lut\n", __func__, comp->id);
			ret = mtk_gamma_write_12bit_lut_reg(comp, handle, 0);

			mutex_unlock(&g_gamma_global_lock);

			if (old_lut != NULL)
				kfree(old_lut);

			//if (comp->mtk_crtc != NULL)
			//	mtk_crtc_check_trigger(comp->mtk_crtc, false,
			//		false);
		} else {
			DDPPR_ERR("%s: invalid ID = %d\n", __func__, comp->id);
			ret = -EFAULT;
		}
	}

	return ret;
}
static int mtk_gamma_set_gain(struct mtk_ddp_comp *comp,
	struct cmdq_pkt *handle, struct mtk_disp_gamma_sb_param *user_gamma_gain)
{

	int ret = 0;
	CRTC_MMP_MARK(0, gammawithbacklight, (unsigned long)handle, 0);

	if (user_gamma_gain == NULL) {
		ret = -EFAULT;
	} else {
		mutex_lock(&g_gamma_global_lock);
		ret = mtk_gamma_write_gain_reg(comp, handle, 0);
		mutex_unlock(&g_gamma_global_lock);
	}

	return ret;
}

int mtk_drm_ioctl_set_gammalut(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct mtk_drm_private *private = dev->dev_private;
	struct mtk_ddp_comp *comp = private->ddp_comp[DDP_COMPONENT_GAMMA0];
	struct drm_crtc *crtc = private->crtc[0];

	g_disp_gamma_lut_db = *((struct DISP_GAMMA_LUT_T *)data);

	return mtk_crtc_user_cmd(crtc, comp, SET_GAMMALUT, data);
}

int mtk_drm_ioctl_set_12bit_gammalut(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct mtk_drm_private *private = dev->dev_private;

	g_gamma_flip_comp[0] = private->ddp_comp[DDP_COMPONENT_GAMMA0];
	//g_gamma_flip_comp[1] = private->ddp_comp[DDP_COMPONENT_GAMMA1];

	mutex_lock(&g_gamma_sram_lock);
	CRTC_MMP_EVENT_START(0, gamma_ioctl, 0, 0);
	memcpy(&ioctl_data, (struct DISP_GAMMA_12BIT_LUT_T *)data,
			sizeof(struct DISP_GAMMA_12BIT_LUT_T));
	atomic_set(&g_gamma_sof_filp, 1);
	if (g_gamma_flip_comp[0]->mtk_crtc != NULL) {
		mtk_drm_idlemgr_kick(__func__, &g_gamma_flip_comp[0]->mtk_crtc->base, 1);

		if (atomic_read(&g_force_delay_check_trig) == 0)
		        mtk_crtc_check_trigger(g_gamma_flip_comp[0]->mtk_crtc, false, true);
		else if (atomic_read(&g_force_delay_check_trig) == 1)
		        mtk_crtc_check_trigger(g_gamma_flip_comp[0]->mtk_crtc, true, true);
		else if (atomic_read(&g_force_delay_check_trig) == 2)
			mtk_crtc_check_trigger(g_gamma_flip_comp[0]->mtk_crtc, true, true);
		else if (atomic_read(&g_force_delay_check_trig) == 3)
			DDPINFO("%s: not check trigger\n", __func__);
		else
			DDPINFO("%s: value is not support!\n", __func__);
	}
	DDPINFO("%s:update IOCTL g_gamma_sof_filp to 1\n", __func__);
	CRTC_MMP_EVENT_END(0, gamma_ioctl, 0, 1);
	mutex_unlock(&g_gamma_sram_lock);

	return 0;
}

int mtk_drm_12bit_gammalut_ioctl_impl(void)
{
	int ret = 0;
	struct mtk_drm_crtc *mtk_crtc = g_gamma_flip_comp[0]->mtk_crtc;
	struct drm_crtc *crtc = &mtk_crtc->base;

	mutex_lock(&g_gamma_sram_lock);
	g_disp_gamma_12bit_lut_db = ioctl_data;
	ret = mtk_crtc_user_cmd(crtc, g_gamma_flip_comp[0], SET_12BIT_GAMMALUT,
			(void *)(&ioctl_data));
	mutex_unlock(&g_gamma_sram_lock);
	return ret;
}

int mtk_drm_ioctl_bypass_disp_gamma(struct drm_device *dev, void *data,
	struct drm_file *file_priv)
{
	struct mtk_drm_private *private = dev->dev_private;
	struct mtk_ddp_comp *comp = private->ddp_comp[DDP_COMPONENT_GAMMA0];
	struct drm_crtc *crtc = private->crtc[0];

	return mtk_crtc_user_cmd(crtc, comp, BYPASS_GAMMA, data);
}

static void disp_gamma_wait_sof_irq(void)
{
	int ret = 0;

	if (atomic_read(&g_gamma_sof_irq_available) == 0) {
		DDPINFO("wait_event_interruptible\n");
		ret = wait_event_interruptible(g_gamma_sof_irq_wq,
				atomic_read(&g_gamma_sof_irq_available) == 1);
		CRTC_MMP_EVENT_START(0, gamma_sof, 0, 0);
		DDPINFO("sof_irq_available = 1, waken up, ret = %d", ret);
	} else {
		DDPINFO("sof_irq_available = 0");
		return;
	}

	ret = mtk_drm_12bit_gammalut_ioctl_impl();
	if (ret != 0) {
		DDPPR_ERR("%s:12bit gammalut ioctl impl failed!!\n", __func__);
		CRTC_MMP_MARK(0, gamma_sof, 0, 1);
	}

	atomic_set(&g_gamma_sof_filp, 0);
	DDPINFO("set g_gamma_ioctl_lock to 0\n");
	CRTC_MMP_EVENT_END(0, gamma_sof, 0, 2);
}

static int mtk_gamma_sof_irq_trigger(void *data)
{
	while (!kthread_should_stop()) {
		disp_gamma_wait_sof_irq();
		atomic_set(&g_gamma_sof_irq_available, 0);
	}

	return 0;
}

static void mtk_gamma_start(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle)
{
	DDPINFO("%s\n", __func__);

	cmdq_pkt_write(handle, comp->cmdq_base,
		comp->regs_pa + DISP_GAMMA_EN, GAMMA_EN, ~0);
	if (m_new_pq_persist_property[DISP_PQ_GAMMA_SILKY_BRIGHTNESS])
		mtk_gamma_write_gain_reg(comp, handle, 0);

	if (g_gamma_data_mode == HW_12BIT_MODE_8BIT ||
		g_gamma_data_mode == HW_12BIT_MODE_12BIT)
		mtk_gamma_write_12bit_lut_reg(comp, handle, 0);
	else
		mtk_gamma_write_lut_reg(comp, handle, 0);
}

int mtk_drm_ioctl_gamma_mul_disable(struct drm_device *dev, void *data,
		struct drm_file *file_priv)
{
	struct mtk_drm_private *private = dev->dev_private;
	struct mtk_ddp_comp *comp = private->ddp_comp[DDP_COMPONENT_GAMMA0];
	struct drm_crtc *crtc = private->crtc[0];
	int ret = 0;

	ret = mtk_crtc_user_cmd(crtc, comp, DISABLE_MUL_EN, data);

	return ret;
}

static void mtk_gamma_stop(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle)
{
	cmdq_pkt_write(handle, comp->cmdq_base,
		comp->regs_pa + DISP_GAMMA_EN, 0x0, ~0);
}

static void mtk_gamma_bypass(struct mtk_ddp_comp *comp, int bypass,
	struct cmdq_pkt *handle)
{
	DDPINFO("%s\n", __func__);
	cmdq_pkt_write(handle, comp->cmdq_base,
		comp->regs_pa + DISP_GAMMA_CFG, bypass, 0x1);
	g_gamma_relay_value = bypass;

}

static void mtk_gamma_set(struct mtk_ddp_comp *comp,
			  struct drm_crtc_state *state, struct cmdq_pkt *handle)
{
	unsigned int i;
	struct drm_color_lut *lut;
	u32 word = 0;
	u32 word_first = 0;
	u32 word_last = 0;

	DDPINFO("%s\n", __func__);

	if (state->gamma_lut) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			       comp->regs_pa + DISP_GAMMA_CFG,
			       1<<GAMMA_LUT_EN, 1<<GAMMA_LUT_EN);
		lut = (struct drm_color_lut *)state->gamma_lut->data;
		for (i = 0; i < MTK_LUT_SIZE; i++) {
			word = GAMMA_ENTRY(lut[i].red >> 6,
				lut[i].green >> 6, lut[i].blue >> 6);
			cmdq_pkt_write(handle, comp->cmdq_base,
				comp->regs_pa
				+ (DISP_GAMMA_LUT + i * 4),
				word, ~0);

			// first & last word for
			//	decreasing/incremental LUT
			if (i == 0)
				word_first = word;
			else if (i == MTK_LUT_SIZE - 1)
				word_last = word;
		}
	}
	if ((word_first - word_last) > 0) {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x1 << 2, 0x4);
		DDPINFO("decreasing LUT\n");
	} else {
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x0 << 2, 0x4);
		DDPINFO("Incremental LUT\n");
	}
}

static void calculateGammaLut(struct DISP_GAMMA_LUT_T *data)
{
	int i;

	for (i = 0; i < DISP_GAMMA_LUT_SIZE; i++)
		data->lut[i] = (((g_disp_gamma_lut_db.lut[i] & 0x3ff) *
			g_sb_param.gain[gain_b] + 4096) / 8192) |
			(((g_disp_gamma_lut_db.lut[i] >> 10 & 0x3ff) *
			g_sb_param.gain[gain_g] + 4096) / 8192) << 10 |
			(((g_disp_gamma_lut_db.lut[i] >> 20 & 0x3ff) *
			g_sb_param.gain[gain_r] + 4096) / 8192) << 20;

}

static void calculateGamma12bitLut(struct DISP_GAMMA_12BIT_LUT_T *data)
{
	int i, lut_size = DISP_GAMMA_LUT_SIZE;

	if (g_gamma_data_mode == HW_12BIT_MODE_12BIT)
		lut_size = DISP_GAMMA_12BIT_LUT_SIZE;

	for (i = 0; i < lut_size; i++) {
		data->lut_0[i] =
			(((g_disp_gamma_12bit_lut_db.lut_0[i] & 0xfff) *
			g_sb_param.gain[gain_r] + 4096) / 8192) |
			(((g_disp_gamma_12bit_lut_db.lut_0[i] >> 12 & 0xfff) *
			g_sb_param.gain[gain_g] + 4096) / 8192) << 12;
		data->lut_1[i] =
			(((g_disp_gamma_12bit_lut_db.lut_1[i] & 0xfff) *
			g_sb_param.gain[gain_b] + 4096) / 8192);
	}
}

static struct DISP_GAMMA_LUT_T lut_8bit_data;
static struct DISP_GAMMA_12BIT_LUT_T lut_12bit_data;
void mtk_trans_gain_to_gamma(struct drm_crtc *crtc,
	unsigned int gain[3], unsigned int bl, void *param)
{
	int ret;
	unsigned int mmsys_id = 0;
	struct DISP_AAL_ESS20_SPECT_PARAM *ess20_spect_param = param;

	if (param == NULL)
		ret = -EFAULT;

	mmsys_id = mtk_get_mmsys_id(crtc);
	if (g_sb_param.gain[gain_r] != gain[gain_r] ||
		g_sb_param.gain[gain_g] != gain[gain_g] ||
		g_sb_param.gain[gain_b] != gain[gain_b]) {

		g_sb_param.gain[gain_r] = gain[gain_r];
		g_sb_param.gain[gain_g] = gain[gain_g];
		g_sb_param.gain[gain_b] = gain[gain_b];

		if (mmsys_id != MMSYS_MT6985) {
			if (g_gamma_data_mode == HW_8BIT) {
				calculateGammaLut(&lut_8bit_data);
				mtk_crtc_user_cmd(crtc, default_comp,
					SET_GAMMALUT, (void *)&lut_8bit_data);
			}

			if (g_gamma_data_mode == HW_12BIT_MODE_8BIT ||
				g_gamma_data_mode == HW_12BIT_MODE_12BIT) {
				calculateGamma12bitLut(&lut_12bit_data);
				mtk_crtc_user_cmd(crtc, default_comp,
					SET_12BIT_GAMMALUT, (void *)&lut_12bit_data);
			}
		} else {
			CRTC_MMP_EVENT_START(0, gamma_gain, gain[gain_r], bl);
			mtk_crtc_user_cmd(crtc, default_comp,
				SET_GAMMAGAIN, (void *)&g_sb_param);
			CRTC_MMP_EVENT_END(0, gamma_gain, gain[gain_r], bl);
		}
		DDPINFO("[aal_kernel]ELVSSPN = %d, flag = %d",
			ess20_spect_param->ELVSSPN, ess20_spect_param->flag);

		CRTC_MMP_EVENT_START(0, gamma_backlight, gain[gain_r], bl);
		mtk_leds_brightness_set("lcd-backlight", bl, ess20_spect_param->ELVSSPN,
					ess20_spect_param->flag);
		CRTC_MMP_EVENT_END(0, gamma_backlight, gain[gain_r], bl);

		if (atomic_read(&g_force_delay_check_trig) == 0)
		        mtk_crtc_check_trigger(default_comp->mtk_crtc, false, true);
		else if (atomic_read(&g_force_delay_check_trig) == 1)
		        mtk_crtc_check_trigger(default_comp->mtk_crtc, true, true);
		else if (atomic_read(&g_force_delay_check_trig) == 2)
			mtk_crtc_check_trigger(default_comp->mtk_crtc, false, true);
		else if (atomic_read(&g_force_delay_check_trig) == 3)
			DDPINFO("%s: not check trigger\n", __func__);
		else
			DDPINFO("%s: value is not support!\n", __func__);

		DDPINFO("%s : gain = %d, backlight = %d",
			__func__, g_sb_param.gain[gain_r], bl);
	} else {
		g_sb_param.bl = bl;
		mtk_leds_brightness_set("lcd-backlight", bl, ess20_spect_param->ELVSSPN,
					ess20_spect_param->flag);
		CRTC_MMP_MARK(0, gamma_backlight, ess20_spect_param->flag, bl);
		DDPINFO("%s : backlight = %d, flag = %d, elvss = %d", __func__, bl,
			ess20_spect_param->flag, ess20_spect_param->ELVSSPN);
	}
}

static int mtk_gamma_user_cmd(struct mtk_ddp_comp *comp,
	struct cmdq_pkt *handle, unsigned int cmd, void *data)
{
	DDPINFO("%s: cmd: %d\n", __func__, cmd);
	switch (cmd) {
	case SET_GAMMALUT:
	{
		struct DISP_GAMMA_LUT_T *config = data;

		if (mtk_gamma_set_lut(comp, handle, config) < 0) {
			DDPPR_ERR("%s: failed\n", __func__);
			return -EFAULT;
		}
		if ((comp->mtk_crtc != NULL) && comp->mtk_crtc->is_dual_pipe) {
			struct mtk_drm_crtc *mtk_crtc = comp->mtk_crtc;
			struct drm_crtc *crtc = &mtk_crtc->base;
			struct mtk_drm_private *priv = crtc->dev->dev_private;
			struct mtk_ddp_comp *comp_gamma1 = priv->ddp_comp[DDP_COMPONENT_GAMMA1];

			if (mtk_gamma_set_lut(comp_gamma1, handle, config) < 0) {
				DDPPR_ERR("%s: comp_gamma1 failed\n", __func__);
				return -EFAULT;
			}
		}

		if (comp->mtk_crtc != NULL) {
			if (atomic_read(&g_force_delay_check_trig) == 0)
			        mtk_crtc_check_trigger(comp->mtk_crtc, false, false);
			else if (atomic_read(&g_force_delay_check_trig) == 1)
			        mtk_crtc_check_trigger(comp->mtk_crtc, true, false);
			else if (atomic_read(&g_force_delay_check_trig) == 2)
				mtk_crtc_check_trigger(comp->mtk_crtc, true, false);
			else if (atomic_read(&g_force_delay_check_trig) == 3)
				DDPINFO("%s: not check trigger\n", __func__);
			else
				DDPINFO("%s: value is not support!\n", __func__);
		}
	}
	break;
	case SET_12BIT_GAMMALUT:
	{
		struct DISP_GAMMA_12BIT_LUT_T *config = data;

		CRTC_MMP_MARK(0, aal_ess20_gamma, comp->id, 0);
		if (mtk_gamma_12bit_set_lut(comp, handle, config) < 0) {
			DDPPR_ERR("%s: failed\n", __func__);
			return -EFAULT;
		}
		if ((comp->mtk_crtc != NULL) && comp->mtk_crtc->is_dual_pipe) {
			struct mtk_drm_crtc *mtk_crtc = comp->mtk_crtc;
			struct drm_crtc *crtc = &mtk_crtc->base;
			struct mtk_drm_private *priv = crtc->dev->dev_private;
			struct mtk_ddp_comp *comp_gamma1 = priv->ddp_comp[DDP_COMPONENT_GAMMA1];

			if (mtk_gamma_12bit_set_lut(comp_gamma1, handle, config) < 0) {
				DDPPR_ERR("%s: comp_gamma1 failed\n", __func__);
				return -EFAULT;
			}
		}

		if (comp->mtk_crtc != NULL) {
			if (atomic_read(&g_force_delay_check_trig) == 0)
			        mtk_crtc_check_trigger(comp->mtk_crtc, false, false);
			else if (atomic_read(&g_force_delay_check_trig) == 1)
			        mtk_crtc_check_trigger(comp->mtk_crtc, true, false);
			else if (atomic_read(&g_force_delay_check_trig) == 2)
				mtk_crtc_check_trigger(comp->mtk_crtc, true, false);
			else if (atomic_read(&g_force_delay_check_trig) == 3)
				DDPINFO("%s: not check trigger\n", __func__);
			else
				DDPINFO("%s: value is not support!\n", __func__);
		}
	}
	break;
	case BYPASS_GAMMA:
	{
		int *value = data;

		mtk_gamma_bypass(comp, *value, handle);
		if (comp->mtk_crtc->is_dual_pipe) {
			struct mtk_drm_crtc *mtk_crtc = comp->mtk_crtc;
			struct drm_crtc *crtc = &mtk_crtc->base;
			struct mtk_drm_private *priv = crtc->dev->dev_private;
			struct mtk_ddp_comp *comp_gamma1 = priv->ddp_comp[DDP_COMPONENT_GAMMA1];

			mtk_gamma_bypass(comp_gamma1, *value, handle);
		}
	}
	break;
	case SET_GAMMAGAIN:
	{
		struct mtk_disp_gamma_sb_param *config = data;

		if (mtk_gamma_set_gain(comp, handle, config) < 0)
			return -EFAULT;

		if (comp->mtk_crtc->is_dual_pipe) {
			struct mtk_drm_crtc *mtk_crtc = comp->mtk_crtc;
			struct drm_crtc *crtc = &mtk_crtc->base;
			struct mtk_drm_private *priv = crtc->dev->dev_private;
			struct mtk_ddp_comp *comp_gamma1 = priv->ddp_comp[DDP_COMPONENT_GAMMA1];

			if (mtk_gamma_set_gain(comp_gamma1, handle, config) < 0)
				return -EFAULT;
		}
	}
	break;
	case DISABLE_MUL_EN:
	{
		cmdq_pkt_write(handle, comp->cmdq_base,
			comp->regs_pa + DISP_GAMMA_CFG, 0x0 << 3, 0x08);

		if (comp->mtk_crtc->is_dual_pipe) {
			struct mtk_drm_crtc *mtk_crtc = comp->mtk_crtc;
			struct drm_crtc *crtc = &mtk_crtc->base;
			struct mtk_drm_private *priv = crtc->dev->dev_private;
			struct mtk_ddp_comp *comp_gamma1 = priv->ddp_comp[DDP_COMPONENT_GAMMA1];

			cmdq_pkt_write(handle, comp_gamma1->cmdq_base,
				comp_gamma1->regs_pa + DISP_GAMMA_CFG, 0x0 << 3, 0x08);

		}
	}
	break;

	default:
		DDPPR_ERR("%s: error cmd: %d\n", __func__, cmd);
		return -EINVAL;
	}
	return 0;
}

struct gamma_backup {
	unsigned int GAMMA_CFG;
};
static struct gamma_backup g_gamma_backup;

static void ddp_dither_backup(struct mtk_ddp_comp *comp)
{
	g_gamma_backup.GAMMA_CFG =
		readl(comp->regs + DISP_GAMMA_CFG);
}

static void ddp_dither_restore(struct mtk_ddp_comp *comp)
{
	writel(g_gamma_backup.GAMMA_CFG, comp->regs + DISP_GAMMA_CFG);
}

static void mtk_gamma_prepare(struct mtk_ddp_comp *comp)
{
	mtk_ddp_comp_clk_prepare(comp);
	atomic_set(&g_gamma_is_clock_on[index_of_gamma(comp->id)], 1);
	ddp_dither_restore(comp);
}

static void mtk_gamma_unprepare(struct mtk_ddp_comp *comp)
{
	unsigned long flags;

	DDPINFO("%s @ %d......... spin_trylock_irqsave ++ ",
		__func__, __LINE__);
	spin_lock_irqsave(&g_gamma_clock_lock, flags);
	DDPINFO("%s @ %d......... spin_trylock_irqsave -- ",
		__func__, __LINE__);
	atomic_set(&g_gamma_is_clock_on[index_of_gamma(comp->id)], 0);
	spin_unlock_irqrestore(&g_gamma_clock_lock, flags);
	DDPINFO("%s @ %d......... spin_unlock_irqrestore ",
		__func__, __LINE__);
	ddp_dither_backup(comp);
	mtk_ddp_comp_clk_unprepare(comp);
}

int mtk_gamma_io_cmd(struct mtk_ddp_comp *comp, struct cmdq_pkt *handle,
	      enum mtk_ddp_io_cmd cmd, void *params)
{
	uint32_t force_delay_trigger;

	if (cmd == FORCE_TRIG_CTL) {
		force_delay_trigger = *(uint32_t *)params;
		atomic_set(&g_force_delay_check_trig, force_delay_trigger);
	}

	return 0;
}

void mtk_gamma_first_cfg(struct mtk_ddp_comp *comp,
	       struct mtk_ddp_config *cfg, struct cmdq_pkt *handle)
{
	mtk_gamma_config(comp, cfg, handle);
}

static const struct mtk_ddp_comp_funcs mtk_disp_gamma_funcs = {
	.gamma_set = mtk_gamma_set,
	.config = mtk_gamma_config,
	.first_cfg = mtk_gamma_first_cfg,
	.start = mtk_gamma_start,
	.stop = mtk_gamma_stop,
	.bypass = mtk_gamma_bypass,
	.user_cmd = mtk_gamma_user_cmd,
	.io_cmd = mtk_gamma_io_cmd,
	.prepare = mtk_gamma_prepare,
	.unprepare = mtk_gamma_unprepare,
	.config_overhead = mtk_disp_gamma_config_overhead,
};

static int mtk_disp_gamma_bind(struct device *dev, struct device *master,
			       void *data)
{
	struct mtk_disp_gamma *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;
	int ret;

	DDPINFO("%s\n", __func__);

	ret = mtk_ddp_comp_register(drm_dev, &priv->ddp_comp);
	if (ret < 0) {
		dev_err(dev, "Failed to register component %s: %d\n",
			dev->of_node->full_name, ret);
		return ret;
	}

	return 0;
}

static void mtk_disp_gamma_unbind(struct device *dev, struct device *master,
				  void *data)
{
	struct mtk_disp_gamma *priv = dev_get_drvdata(dev);
	struct drm_device *drm_dev = data;

	mtk_ddp_comp_unregister(drm_dev, &priv->ddp_comp);
}

static const struct component_ops mtk_disp_gamma_component_ops = {
	.bind = mtk_disp_gamma_bind, .unbind = mtk_disp_gamma_unbind,
};

void mtk_gamma_dump(struct mtk_ddp_comp *comp)
{
	void __iomem *baddr = comp->regs;

	if (!baddr) {
		DDPDUMP("%s, %s is NULL!\n", __func__, mtk_dump_comp_str(comp));
		return;
	}

	DDPDUMP("== %s REGS:0x%llx ==\n", mtk_dump_comp_str(comp), comp->regs_pa);
	mtk_cust_dump_reg(baddr, 0x0, 0x20, 0x24, 0x28);
	mtk_cust_dump_reg(baddr, 0x54, 0x58, 0x5c, 0x50);
	mtk_cust_dump_reg(baddr, 0x14, 0x20, 0x700, 0xb00);
}

void mtk_gamma_regdump(void)
{
	void __iomem  *baddr = default_comp->regs;
	int k;

	DDPDUMP("== %s REGS:0x%x ==\n", mtk_dump_comp_str(default_comp),
			default_comp->regs_pa);
	DDPDUMP("[%s REGS Start Dump]\n", mtk_dump_comp_str(default_comp));
	for (k = 0; k <= 0xff0; k += 16) {
		DDPDUMP("0x%04x: 0x%08x 0x%08x 0x%08x 0x%08x\n", k,
			readl(baddr + k),
			readl(baddr + k + 0x4),
			readl(baddr + k + 0x8),
			readl(baddr + k + 0xc));
	}
	DDPDUMP("[%s REGS End Dump]\n", mtk_dump_comp_str(default_comp));
	if (default_comp->mtk_crtc->is_dual_pipe && default_comp1) {
		baddr = default_comp1->regs;
		DDPDUMP("== %s REGS:0x%x ==\n", mtk_dump_comp_str(default_comp1),
				default_comp1->regs_pa);
		DDPDUMP("[%s REGS Start Dump]\n", mtk_dump_comp_str(default_comp1));
		for (k = 0; k <= 0xff0; k += 16) {
			DDPDUMP("0x%04x: 0x%08x 0x%08x 0x%08x 0x%08x\n", k,
				readl(baddr + k),
				readl(baddr + k + 0x4),
				readl(baddr + k + 0x8),
				readl(baddr + k + 0xc));
		}
		DDPDUMP("[%s REGS End Dump]\n", mtk_dump_comp_str(default_comp1));
	}
}

static void mtk_disp_gamma_dts_parse(const struct device_node *np,
	enum mtk_ddp_comp_id comp_id)
{
	struct gamma_color_protect_mode color_protect_mode;

	if (of_property_read_u32(np, "gamma_data_mode",
		&g_gamma_data_mode)) {
		DDPPR_ERR("comp_id: %d, gamma_data_mode = %d\n",
			comp_id, g_gamma_data_mode);
		g_gamma_data_mode = HW_8BIT;
	}

	if (of_property_read_u32(np, "color_protect_lsb",
		&g_gamma_color_protect.gamma_color_protect_lsb)) {
		DDPPR_ERR("comp_id: %d, color_protect_lsb = %d\n",
			comp_id, g_gamma_color_protect.gamma_color_protect_lsb);
		g_gamma_color_protect.gamma_color_protect_lsb = 0;
	}

	if (of_property_read_u32(np, "color_protect_red",
		&color_protect_mode.red_support)) {
		DDPPR_ERR("comp_id: %d, color_protect_red = %d\n",
			comp_id, color_protect_mode.red_support);
		color_protect_mode.red_support = 0;
	}

	if (of_property_read_u32(np, "color_protect_green",
		&color_protect_mode.green_support)) {
		DDPPR_ERR("comp_id: %d, color_protect_green = %d\n",
			comp_id, color_protect_mode.green_support);
		color_protect_mode.green_support = 0;
	}

	if (of_property_read_u32(np, "color_protect_blue",
		&color_protect_mode.blue_support)) {
		DDPPR_ERR("comp_id: %d, color_protect_blue = %d\n",
			comp_id, color_protect_mode.blue_support);
		color_protect_mode.blue_support = 0;
	}

	if (of_property_read_u32(np, "color_protect_black",
		&color_protect_mode.black_support)) {
		DDPPR_ERR("comp_id: %d, color_protect_black = %d\n",
			comp_id, color_protect_mode.black_support);
		color_protect_mode.black_support = 0;
	}

	if (of_property_read_u32(np, "color_protect_white",
		&color_protect_mode.white_support)) {
		DDPPR_ERR("comp_id: %d, color_protect_white = %d\n",
			comp_id, color_protect_mode.white_support);
		color_protect_mode.white_support = 0;
	}

	g_gamma_color_protect.gamma_color_protect_support =
		color_protect_mode.red_support << 4 |
		color_protect_mode.green_support << 5 |
		color_protect_mode.blue_support << 6 |
		color_protect_mode.black_support << 7 |
		color_protect_mode.white_support << 8;
}

static int mtk_disp_gamma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_disp_gamma *priv;
	enum mtk_ddp_comp_id comp_id;
	int ret;
	struct sched_param param = {.sched_priority = 84 };

	DDPINFO("%s+\n", __func__);

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (priv == NULL)
		return -ENOMEM;

	comp_id = mtk_ddp_comp_get_id(dev->of_node, MTK_DISP_GAMMA);
	if ((int)comp_id < 0) {
		DDPPR_ERR("Failed to identify by alias: %d\n", comp_id);
		return comp_id;
	}

	if (comp_id == DDP_COMPONENT_GAMMA0)
		mtk_disp_gamma_dts_parse(dev->of_node, comp_id);

	ret = mtk_ddp_comp_init(dev, dev->of_node, &priv->ddp_comp, comp_id,
				&mtk_disp_gamma_funcs);
	if (ret != 0) {
		DDPPR_ERR("Failed to initialize component: %d\n", ret);
		return ret;
	}

	if (!default_comp && comp_id == DDP_COMPONENT_GAMMA0)
		default_comp = &priv->ddp_comp;
	if (!default_comp1 && comp_id == DDP_COMPONENT_GAMMA1)
		default_comp1 = &priv->ddp_comp;

	platform_set_drvdata(pdev, priv);

	mtk_ddp_comp_pm_enable(&priv->ddp_comp);

	ret = component_add(dev, &mtk_disp_gamma_component_ops);
	if (ret != 0) {
		dev_err(dev, "Failed to add component: %d\n", ret);
		mtk_ddp_comp_pm_disable(&priv->ddp_comp);
	}

	if (comp_id == DDP_COMPONENT_GAMMA0) {
		gamma_sof_irq_event_task =
			kthread_create(mtk_gamma_sof_irq_trigger,
				NULL, "gamma_sof");

		if (sched_setscheduler(gamma_sof_irq_event_task, SCHED_RR, &param))
			pr_notice("gamma_sof_irq_event_task setschedule fail");

		wake_up_process(gamma_sof_irq_event_task);
	}

	DDPINFO("%s-\n", __func__);

	return ret;
}

static int mtk_disp_gamma_remove(struct platform_device *pdev)
{
	struct mtk_disp_gamma *priv = dev_get_drvdata(&pdev->dev);

	component_del(&pdev->dev, &mtk_disp_gamma_component_ops);
	mtk_ddp_comp_pm_disable(&priv->ddp_comp);

	return 0;
}

static const struct of_device_id mtk_disp_gamma_driver_dt_match[] = {
	{ .compatible = "mediatek,mt6779-disp-gamma",},
	{ .compatible = "mediatek,mt6885-disp-gamma",},
	{ .compatible = "mediatek,mt6873-disp-gamma",},
	{ .compatible = "mediatek,mt6853-disp-gamma",},
	{ .compatible = "mediatek,mt6833-disp-gamma",},
	{ .compatible = "mediatek,mt6983-disp-gamma",},
	{ .compatible = "mediatek,mt6895-disp-gamma",},
	{ .compatible = "mediatek,mt6879-disp-gamma",},
	{ .compatible = "mediatek,mt6855-disp-gamma",},
	{ .compatible = "mediatek,mt6985-disp-gamma",},
	{ .compatible = "mediatek,mt6886-disp-gamma",},
	{ .compatible = "mediatek,mt6835-disp-gamma",},
	{},
};

MODULE_DEVICE_TABLE(of, mtk_disp_gamma_driver_dt_match);

struct platform_driver mtk_disp_gamma_driver = {
	.probe = mtk_disp_gamma_probe,
	.remove = mtk_disp_gamma_remove,
	.driver = {

			.name = "mediatek-disp-gamma",
			.owner = THIS_MODULE,
			.of_match_table = mtk_disp_gamma_driver_dt_match,
		},
};

int disp_gamma_set_bypass(struct drm_crtc *crtc, int bypass)
{
	int ret = 0;

	ret = mtk_crtc_user_cmd(crtc, default_comp, BYPASS_GAMMA, &bypass);
	DDPINFO("%s : ret = %d", __func__, ret);
	return ret;
}
