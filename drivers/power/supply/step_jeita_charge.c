/*
 * step/jeita charge controller
 *
 * published by the Free Software Foundation.

 * THIS PACKAGE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 */

/*	date			author			comment
 *	2021-06-01		chenyichun@xiaomi.com	create
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/power_supply.h>
#include <linux/iio/consumer.h>
#include "mtk_charger.h"
#include "adapter_class.h"
#include "bq28z610.h"

static void get_index(struct step_jeita_cfg0 *cfg, int fallback_hyst, int forward_hyst, int value, int *index, bool ignore_hyst)
{
	int new_index = 0, i = 0;

	chr_err("%s: value = %d, index[0] = %d, index[1] = %d\n", __func__, value, index[0], index[1]);

	if (value < cfg[0].low_threshold) {
		index[0] = index[1] = 0;
		return;
	}

	if (value > cfg[STEP_JEITA_TUPLE_COUNT - 1].high_threshold)
		new_index = STEP_JEITA_TUPLE_COUNT - 1;

	for (i = 0; i < STEP_JEITA_TUPLE_COUNT; i++) {
		if (is_between(cfg[i].low_threshold, cfg[i].high_threshold, value)) {
			new_index = i;
			break;
		}
	}

	if (ignore_hyst) {
		index[0] = index[1] = new_index;
	} else {
		if (new_index > index[0]) {
			if (value < (cfg[new_index].low_threshold + forward_hyst))
				new_index = index[0];
		} else if (new_index < index[0]) {
			if (value > (cfg[new_index].high_threshold - fallback_hyst))
				new_index = index[0];
		}
		index[1] = index[0];
		index[0] = new_index;
	}

	chr_err("%s: value = %d, index[0] = %d, index[1] = %d, new_index = %d\n", __func__, value, index[0], index[1], new_index);
	return;
}

static void monitor_sw_cv(struct mtk_charger *info)
{
	int ibat = 0;

	if (info->step_chg_index[0] > info->step_chg_index[1] && (info->step_chg_cfg[info->step_chg_index[0]].value != info->step_chg_cfg[info->step_chg_index[1]].value)) {
		info->sw_cv_count = 0;
		info->sw_cv = info->step_chg_cfg[info->step_chg_index[0]].low_threshold + info->step_forward_hyst;
	}

	if (info->sw_cv || info->suspend_recovery) {
		ibat = info->current_now;
		if ((-ibat) <= info->step_chg_fcc) {
			info->sw_cv_count++;
			if (info->sw_cv_count >= SW_CV_COUNT) {
				info->sw_cv = 0;
				info->sw_cv_count = 0;
				vote(info->fcc_votable, STEP_CHARGE_VOTER, true, info->step_chg_fcc);
			}
		} else {
			info->sw_cv_count = 0;
		}
	}
}

static void monitor_jeita_descent(struct mtk_charger *info)
{
	int current_fcc = 0;

	current_fcc = get_client_vote(info->fcc_votable, JEITA_CHARGE_VOTER);
	if (current_fcc != info->jeita_chg_fcc) {
		if (current_fcc >= info->jeita_chg_fcc + JEITA_FCC_DESCENT_STEP)
			vote(info->fcc_votable, JEITA_CHARGE_VOTER, true, current_fcc - JEITA_FCC_DESCENT_STEP);
		else if (current_fcc >= info->jeita_chg_fcc - JEITA_FCC_DESCENT_STEP)
			vote(info->fcc_votable, JEITA_CHARGE_VOTER, true, info->jeita_chg_fcc);
		else
			vote(info->fcc_votable, JEITA_CHARGE_VOTER, true, current_fcc + JEITA_FCC_DESCENT_STEP);
	chr_err("current_fcc:%d,%d,%d,%d",current_fcc,
	current_fcc - JEITA_FCC_DESCENT_STEP, info->jeita_chg_fcc, current_fcc + JEITA_FCC_DESCENT_STEP);
	}
}

static int typec_connect_ntc_set_vbus(struct mtk_charger *info, bool is_on)
{
	struct regulator *vbus = info->vbus_contral;
	int ret = 0, vbus_vol = 3300000;

	/* vbus is optional */
	if (!vbus)
		return 0;

	chr_err("contral vbus turn %s\n", is_on ? "on" : "off");

	if (is_on) {
		ret = regulator_set_voltage(vbus, vbus_vol, vbus_vol);
		if (ret)
			chr_err("vbus regulator set voltage failed\n");

		ret = regulator_enable(vbus);
		if (ret)
			chr_err("vbus regulator enable failed\n");
	} else {
		ret = regulator_disable(vbus);
		if (ret)
			chr_err("vbus regulator disable failed\n");
	}

	return 0;
}
static void charger_set_dpdm_voltage(struct mtk_charger *info, int dp, int dm)
{
	info->chg1_dev = get_charger_by_name("primary_chg");
	if (info->chg1_dev)
		chr_err("Found primary charger\n");
	else {
		chr_err("*** Error : can't find primary charger ***\n");
		return;
	}
	charger_dev_set_dpdm_voltage(info->chg1_dev, dp, dm);
}
static void usbotg_enable_otg(struct mtk_charger *info, bool is_on)
{
	info->chg1_dev = get_charger_by_name("primary_chg");
	if (info->chg1_dev)
		chr_err("Found primary charger\n");
	else {
		chr_err("*** Error : can't find primary charger ***\n");
		return;
	}
	charger_dev_enable_otg_regulator(info->chg1_dev, is_on);
}

static void monitor_usb_otg_burn(struct work_struct *work)
{
	int otg_monitor_delay_time = 5000;
	struct mtk_charger *info = container_of(work, struct mtk_charger, usb_otg_monitor_work.work);
	int type_temp = 0, pmic_vbus = 0;
	usb_get_property(USB_PROP_CONNECTOR_TEMP, &type_temp);
	usb_get_property(USB_PROP_PMIC_VBUS, &pmic_vbus);
	if(type_temp <= 450)
		otg_monitor_delay_time = 5000;
	else if(type_temp > 450 && type_temp <= 550)
		otg_monitor_delay_time = 2000;
	else if(type_temp > 550)
		otg_monitor_delay_time = 1000;
	chr_err("%s:get typec temp =%d otg_monitor_delay_time = %d\n", __func__, type_temp, otg_monitor_delay_time);
	if(type_temp >= TYPEC_BURN_TEMP && !info->typec_otg_burn)
	{
		info->typec_otg_burn = true;
		usbotg_enable_otg(info, false);
		chr_err("%s:disable otg\n", __func__);
	}
	else if((info->typec_otg_burn && type_temp <= (TYPEC_BURN_TEMP - TYPEC_BURN_HYST)) ||(pmic_vbus < 1000 && info->otg_enable && (type_temp <= TYPEC_BURN_TEMP - TYPEC_BURN_HYST)))
	{
		info->typec_otg_burn = false;
		usbotg_enable_otg(info, true);
		chr_err("%s:enable otg\n", __func__);
	}
	schedule_delayed_work(&info->usb_otg_monitor_work, msecs_to_jiffies(otg_monitor_delay_time));
}

#if defined(CONFIG_RUST_DETECTION)
static void generate_xm_lpd_uevent(struct mtk_charger *info);
static void monitor_usb_rust(struct mtk_charger *info, int pin)
{
	int res = 0;

	charger_dev_rust_detection_choose_channel(info->et7480_chg_dev, pin);
	charger_dev_rust_detection_enable(info->et7480_chg_dev, true);
	msleep(100);
	res = charger_dev_rust_detection_read_res(info->et7480_chg_dev);
	chr_err("%s: pin=%d, res=%d\n", __func__, pin, res);
	info->lpd_res[pin] = res;
}

static void rust_detection_work_func(struct work_struct *work)
{
	struct timespec64 time;
	ktime_t tmp_time = 0;
	struct mtk_charger *info = container_of(work, struct mtk_charger, rust_detection_work.work);
	int i = 0;
	static int rust_det_interval = 3000;

	tmp_time = ktime_get_boottime();
	time = ktime_to_timespec64(tmp_time);
	if(time.tv_sec < 50) {
		chr_err("%s boot do not enter\n", __func__);
		//return;
		goto out;
	}

	if (info->et7480_chg_dev == NULL) {
		info->et7480_chg_dev = get_charger_by_name("et7480_chg");
		if (info->et7480_chg_dev)
			chr_err("Found et7480 charger\n");
		else {
			chr_err("*** Error : can't find et7480 charger ***\n");
			//return;
			goto out;
		}
	}

	charger_dev_rust_detection_init(info->et7480_chg_dev);

	for (i=1; i<=RUST_DET_SBU2_PIN; i++) {
		if (info->typec_attach) {
			break;
		}
		monitor_usb_rust(info, i);
	}

	if(info->lpd_res[1] < 40 || info->lpd_res[2] < 40 || info->lpd_res[3] < 40 || info->lpd_res[4] < 40)
	{
		info->lpd_debounce_times ++;
		rust_det_interval = 1000;
	}else{
		info->lpd_debounce_times = 0;
		rust_det_interval = 60000;
	}
	chr_err("[XM_LPD] lpd_debounce_times=%d lpd_update_en=%d\n", info->lpd_debounce_times, info->lpd_update_en);
	if(info->lpd_debounce_times >= 2 && info->lpd_update_en)
	{
		chr_err("[XM_LPD] UEVENT\n");
		generate_xm_lpd_uevent(info);
	}

out:
	schedule_delayed_work(&info->rust_detection_work, msecs_to_jiffies(rust_det_interval));
}

#define MAX_UEVENT_LENGTH 100
static void generate_xm_lpd_uevent(struct mtk_charger *info)
{
	static char uevent_string[MAX_UEVENT_LENGTH+1] = {
		"POWER_SUPPLY_LPD_INFOMATION=\n"  //length=28
	};
	u32 cnt=0, i=0;
	char *envp[5] = { NULL };  //the length of array need adjust when uevent number increase

	sprintf(uevent_string+28,"LPD_DP_RES=%d\nLPD_DM_RES=%d\nLPD_SBU1_RES=%d\nLPD_SBU2_RES=%d\n", 
					info->lpd_res[1], info->lpd_res[2], info->lpd_res[3], info->lpd_res[4]);
	if(info->lpd_update_en)
		envp[cnt++] = uevent_string;

	envp[cnt]=NULL;
	for(i = 0; i < cnt; ++i)
	      chr_err("%s\n", envp[i]);
	kobject_uevent_env(&info->pdev->dev.kobj, KOBJ_CHANGE, envp);
	chr_err("%s: LPD_KOBJECT_UEVENT END\n", __func__);
	return;
}
#endif

static void monitor_typec_burn(struct mtk_charger *info)
{
	int type_temp = 0, retry_count = 2;
	bool cp_master_enable = false, cp_slave_enable = false;

	usb_get_property(USB_PROP_CONNECTOR_TEMP, &type_temp);
	chr_err("%s get typec temp=%d otg_enable=%d\n", __func__, type_temp, info->otg_enable);
	if (type_temp > TYPEC_BURN_TEMP -  TYPEC_BURN_HYST)
		update_connect_temp(info);
	if (type_temp >= TYPEC_BURN_TEMP && !info->typec_burn_status && !info->otg_enable) {
		info->typec_burn = true;
		while (retry_count) {
			if (info->cp_master && info->cp_slave) {
				charger_dev_is_enabled(info->cp_master, &cp_master_enable);
				charger_dev_is_enabled(info->cp_slave, &cp_slave_enable);
			}
			if (!cp_master_enable && !cp_slave_enable)
				break;
			msleep(80);
			retry_count--;
		}
		if (info->real_type == XMUSB350_TYPE_HVCHG)
			charger_set_dpdm_voltage(info, 0, 0);
		else if(info->real_type == XMUSB350_TYPE_HVDCP_2 || info->real_type == XMUSB350_TYPE_HVDCP_3 ||
						info->real_type == XMUSB350_TYPE_HVDCP_35_18 || info->real_type == XMUSB350_TYPE_HVDCP_35_27 ||
						info->real_type == XMUSB350_TYPE_HVDCP_3_18 || info->real_type == XMUSB350_TYPE_HVDCP_3_27)
			charger_dev_select_qc_mode(info->usb350_dev, QC_MODE_QC2_5);
		usb_set_property(USB_PROP_INPUT_SUSPEND, info->typec_burn);
		vote(info->icl_votable, TYPEC_BURN_VOTER, true, 0);
		msleep(1000);
		if (info->mt6373_moscon1_control)
			mtk_set_mt6373_moscon1(info, 1, 1);
		else
			typec_connect_ntc_set_vbus(info, true);
		info->typec_burn_status = true;
	} else if (info->typec_burn && type_temp <= TYPEC_BURN_TEMP - TYPEC_BURN_HYST && info->typec_burn_status) {
		info->typec_burn = false;
		if (info->mt6373_moscon1_control)
			mtk_set_mt6373_moscon1(info, 0, 0);
		else
			typec_connect_ntc_set_vbus(info, false);
		usb_set_property(USB_PROP_INPUT_SUSPEND, info->typec_burn);
		vote(info->icl_votable, TYPEC_BURN_VOTER, false, 0);
		info->typec_burn_status = false;
	}
}

static void handle_jeita_charge(struct mtk_charger *info)
{
	static bool jeita_vbat_low = true;

	get_index(info->jeita_fv_cfg, info->jeita_fallback_hyst, info->jeita_forward_hyst, info->temp_now, info->jeita_chg_index, false);
	if(info->jeita_chg_index[0] == 0)
	{
		vote(info->fv_votable, JEITA_CHARGE_VOTER, true, info->jeita_fv_cfg[info->jeita_chg_index[0]].value  - info->diff_fv_val -10);
	}else{
		vote(info->fv_votable, JEITA_CHARGE_VOTER, true, info->jeita_fv_cfg[info->jeita_chg_index[0]].value  - info->diff_fv_val);
	}

	if(info->jeita_chg_index[0] == 4 && info->jeita_chg_index[1] == 5)
	{
		info->high_temp_rec_soc = info->soc;
		chr_err("high temp recharging high_temp_rec_soc=%d\n",  info->high_temp_rec_soc);
	}

	if (jeita_vbat_low) {
		if (info->vbat_now < (info->jeita_fcc_cfg[info->jeita_chg_index[0]].extra_threshold + 50)) {
			info->jeita_chg_fcc = info->jeita_fcc_cfg[info->jeita_chg_index[0]].low_value;
		} else {
			info->jeita_chg_fcc = info->jeita_fcc_cfg[info->jeita_chg_index[0]].high_value;
			jeita_vbat_low = false;
		}
	} else {
		if (info->vbat_now < (info->jeita_fcc_cfg[info->jeita_chg_index[0]].extra_threshold - 100)) {
			info->jeita_chg_fcc = info->jeita_fcc_cfg[info->jeita_chg_index[0]].low_value;
			jeita_vbat_low = true;
		} else {
			info->jeita_chg_fcc = info->jeita_fcc_cfg[info->jeita_chg_index[0]].high_value;
		}
	}

	info->source_jeita_fcc = info->jeita_chg_fcc;// source jeita fcc
	if(info->div_jeita_fcc_flag)
	{
		info->jeita_chg_fcc = info->jeita_chg_fcc * 8 / 10;
		chr_err("[%s]: soa or isp induce jeita current drop: %d\n", __func__, info->jeita_chg_fcc);
	}

	if (is_between(info->jeita_fcc_cfg[0].low_threshold, info->jeita_fcc_cfg[STEP_JEITA_TUPLE_COUNT - 1].high_threshold, info->temp_now) && !info->input_suspend && !info->typec_burn && info->bms_i2c_error_count < 10)
        chr_err("handle_jeita_charge index = %d,jeita_chg_fcc = %d", info->jeita_chg_index[0], info->jeita_chg_fcc);
	if(info->bms_i2c_error_count >= 10)
	{
		vote_override(info->fcc_votable, FG_I2C_VOTER, true, 500);
		vote_override(info->icl_votable, FG_I2C_VOTER, true, 500);
	}
	return;
}

static void handle_step_charge(struct mtk_charger *info)
{
	get_index(info->step_chg_cfg, info->step_fallback_hyst, info->step_forward_hyst, info->vbat_now, info->step_chg_index, false);

	if (info->step_chg_index[0] == STEP_JEITA_TUPLE_COUNT - 1)
		info->step_chg_fcc = info->step_chg_cfg[info->step_chg_index[0]].value + 100;
	else
		info->step_chg_fcc = info->step_chg_cfg[info->step_chg_index[0]].value;

	 chr_err("handle_step_charge index = %d", info->step_chg_index[0]);
	return;
}

static void monitor_thermal_limit(struct mtk_charger *info)
{
	int thermal_level = 0;
	int iterm_effective = get_effective_result(info->iterm_votable);
	if (info->thermal_level < 0)
		thermal_level = -1 - info->thermal_level;
	else
		thermal_level = info->thermal_level;

	switch(info->real_type) {
	case XMUSB350_TYPE_DCP:
		info->thermal_current = info->thermal_limit[0][thermal_level];
		vote(info->fcc_votable, THERMAL_VOTER, true, info->thermal_limit[0][thermal_level]);
		break;
	case XMUSB350_TYPE_HVDCP:
	case XMUSB350_TYPE_HVDCP_2:
		info->thermal_current = info->thermal_limit[1][thermal_level];
		vote(info->fcc_votable, THERMAL_VOTER, true, info->thermal_limit[1][thermal_level]);
		break;
	case XMUSB350_TYPE_HVDCP_3:
	case XMUSB350_TYPE_HVDCP_3_18:
		info->thermal_current = info->thermal_limit[2][thermal_level];
		vote(info->fcc_votable, THERMAL_VOTER, true, info->thermal_limit[2][thermal_level]);
		break;
	case XMUSB350_TYPE_HVDCP_3_27:
		info->thermal_current = info->thermal_limit[3][thermal_level];
		vote(info->fcc_votable, THERMAL_VOTER, true, info->thermal_limit[3][thermal_level]);
		break;
	case XMUSB350_TYPE_HVDCP_35_18:
	case XMUSB350_TYPE_HVDCP_35_27:
		info->thermal_current = info->thermal_limit[4][thermal_level];
		vote(info->fcc_votable, THERMAL_VOTER, true, info->thermal_limit[4][thermal_level]);
		break;
	case XMUSB350_TYPE_PD:
		info->thermal_current = info->thermal_limit[5][thermal_level];
		vote(info->fcc_votable, THERMAL_VOTER, true, info->thermal_limit[5][thermal_level]);
		break;
	case XMUSB350_TYPE_HVCHG:
		info->thermal_current = info->thermal_limit[1][thermal_level];
		vote(info->fcc_votable, THERMAL_VOTER, true, info->thermal_limit[1][thermal_level]);
		break;
	default:
		chr_err("not support psy_type to check charger parameters");
	}
	if (info->last_thermal_level == 15 && thermal_level < 15) {
		chr_err("[CHARGE_LOOP] disable TE\n");
		charger_dev_enable_termination(info->chg1_dev, false);
		info->disable_te_count = 0;
	}

	if(abs(info->current_now) <= (iterm_effective + 150))
		info->disable_te_count ++;
	else
		info->disable_te_count = 0;
	chr_err("[CHARGE_LOOP] disable_te_count = %d\n", info->disable_te_count);
	if(info->disable_te_count == 5)
	{
		chr_err("[CHARGE_LOOP] enable TE\n");
		charger_dev_enable_termination(info->chg1_dev, true);
	}
	info->last_thermal_level = thermal_level;
}

static int handle_ffc_charge(struct mtk_charger *info)
{
	int ret = 0, val = 0, iterm_ffc = 0, iterm = 0;
	bool reduceMv_flag = false;
	bool chg1_en = false;

	if(info->bms_psy){
		bms_get_property(BMS_PROP_CHARGE_DONE, &val);
		if (val != info->fg_full) {
			info->fg_full = val;
			power_supply_changed(info->psy1);
		}
	} else {
		chr_err("get ti_gauge bms failed\n");
		return 0;
	}
	if ((!info->recharge && info->entry_soc <= info->ffc_high_soc && is_between(info->ffc_low_tbat, info->ffc_high_tbat, info->temp_now) && info->high_temp_rec_soc <= 95 &&
			(info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO && info->pd_adapter->verifed) && (info->thermal_level <= 14)) || info->suspend_recovery)
		info->ffc_enable = true;
	else
		info->ffc_enable = false;

	if (is_between(info->ffc_medium_tbat, info->ffc_high_tbat, info->temp_now)){
		iterm = min(info->iterm_warm, 100);
		if(info->cycle_count <= 100){
			reduceMv_flag = true;
			chr_err("temp between 35~48 and cycle <= 100, fv will be set 4460 by sub %d mV\n", 10);

			charger_dev_is_enabled(info->chg1_dev, &chg1_en);
			if(!info->cycle_count_reduceXMv && chg1_en && info->vbat_now > 4450){
				info->cycle_count_reduceXMv = 10;
				chr_err("avoid vbat axceeding 4460mV, fv will be set 4450 by sub %dmV\n", info->cycle_count_reduceXMv);
			}
		}else
			reduceMv_flag = false;
	}
	else{
		iterm = min(info->iterm, 100);

		if(480 < info->temp_now && info->temp_now <= 600)
			iterm = min(info->iterm, 250);

		if(info->cycle_count_reduceXMv){
			info->cycle_count_reduceXMv = 0;
			chr_err("[CHARGE_LOOP] temp isn't bwteen 15 and 48, so will set cycle_count_reduceXMv as 0\n");
		}
	}

	if (is_between(info->ffc_medium_tbat, info->ffc_high_tbat, info->temp_now))
		iterm_ffc = min(info->iterm_ffc_warm, 600);
	else
		iterm_ffc = min(info->iterm_ffc, 600);

	if (!info->gauge_authentic)
		vote(info->fcc_votable, FFC_VOTER, true, 2000);
	else
		vote(info->fcc_votable, FFC_VOTER, true, 22000);

	if (info->ffc_enable) {
		vote(info->fv_votable, FFC_VOTER, true, info->fv_ffc - info->diff_fv_val);
		vote(info->iterm_votable, FFC_VOTER, true, iterm_ffc - 100);
		val = true;
		bms_set_property(BMS_PROP_FASTCHARGE_MODE, val);
		val = FG_MONITOR_DELAY_5S;
		bms_set_property(BMS_PROP_MONITOR_DELAY, val);
	} else if (info->pd_type == MTK_PD_CONNECT_PE_READY_SNK_APDO) {
		vote(info->fv_votable, FFC_VOTER, true, reduceMv_flag ? info->fv - 10 - info->cycle_count_reduceXMv : info->fv - info->diff_fv_val);
		vote(info->iterm_votable, FFC_VOTER, true, iterm);
		val = false;
		bms_set_property(BMS_PROP_FASTCHARGE_MODE, val);
		val = FG_MONITOR_DELAY_5S;
		bms_set_property(BMS_PROP_MONITOR_DELAY, val);
	} else {
		vote(info->fv_votable, FFC_VOTER, true, reduceMv_flag ? info->fv - 10 - info->cycle_count_reduceXMv : info->fv - info->diff_fv_val);
		vote(info->iterm_votable, FFC_VOTER, true, iterm);
		val = false;
		bms_set_property(BMS_PROP_FASTCHARGE_MODE, val);
		val = FG_MONITOR_DELAY_30S;
		bms_set_property(BMS_PROP_MONITOR_DELAY, val);
	}

	return ret;
}

static void check_full_recharge(struct mtk_charger *info)
{
	static int full_count = 0, recharge_count = 0, iterm = 0, threshold_mv = 0, real_full = 0, recharge_voltage = 0;
	int iterm_effective = get_effective_result(info->iterm_votable);
	int fv_effective = get_effective_result(info->fv_votable);
	int rsoc = 0;

	if (info->ffc_enable) {
		if (is_between(info->ffc_medium_tbat, info->ffc_high_tbat, info->temp_now))
			iterm = info->iterm_ffc_warm;
		else
			iterm = info->iterm_ffc;
	} else {
		if (info->temp_now >= info->ffc_high_tbat)
			iterm = info->iterm_warm;
		else
			iterm = info->iterm;
	}

	if (info->charge_full) {
		full_count = 0;
		if (info->temp_now > 0)
			recharge_voltage = 150;
		else
			recharge_voltage = 170;
		if (info->vbat_now <= fv_effective - recharge_voltage)
			recharge_count++;
		else
			recharge_count = 0;

		bms_get_property(BMS_PROP_RSOC, &rsoc);
		chr_err("[%s] rsoc: %d\n", __func__, rsoc);
		if (recharge_count >= 15 || (rsoc < 97 && info->temp_now < 460 && info->temp_now > 0)) {
			info->charge_full = false;
			info->warm_term = false;
			if(real_full){
				info->recharge = true;
			}
			chr_err("start recharge, and recharge_count:%d,temp:%d\n", recharge_count, info->temp_now);
			recharge_count = 0;
			if(info->bms_i2c_error_count < 10)
			{
				charger_dev_enable(info->chg1_dev, true);
				charger_dev_enable_powerpath(info->chg1_dev, true);
			}
			power_supply_changed(info->psy1);

			if(info->ov_check_only_once){
				vote(info->fv_votable, FV_DEC_VOTER, false, 0);
				info->ov_check_only_once = false;
				chr_err("[CHARGE_LOOP] recharge will remove vote of FV_DEC_VOTER function\n");
			}
			if(info->cycle_count_reduceXMv){
				info->cycle_count_reduceXMv = 0;
				chr_err("[CHARGE_LOOP] recharge will set cycle_count_reduceXMv as 0\n");
			}
		}
	} else {
		recharge_count = 0;
		if (info->temp_now > 0)
			threshold_mv = 50;
		else
			threshold_mv = 100;
		if ((info->soc == 100 && (info->vbat_now >= fv_effective - threshold_mv) && (((-info->current_now <= iterm) && info->fg_full) || ((-info->current_now <= iterm_effective + 100) && info->bbc_charge_done)))
                    || (info->soc == 100 && (info->vbat_now >= fv_effective - 30) && (-info->current_now <= iterm) && (info->temp_now >= 480)))
			full_count++;
		else
			full_count = 0;

		if (full_count >= 6) {
			full_count = 0;
			info->charge_full = true;
			info->warm_term = true;
			chr_err("report charge_full\n");
			adapter_dev_set_cap_xm(info->pd_adapter, MTK_PD_APDO, 5000, 3000);
			if(info->temp_now < 480){
				real_full = 1;
				info->warm_term = false;
				charger_dev_do_event(info->chg1_dev, EVENT_FULL, 0);
				power_supply_changed(info->psy1);
			}
			info->recharge = false;
			charger_dev_enable(info->chg1_dev, false);
		}

		if (info->soc < 100 && info->bbc_charge_done && info->temp_now < 475 && info->temp_now >0) {
			charger_dev_enable_powerpath(info->chg1_dev, false);
			charger_dev_enable(info->chg1_dev, false);
			chr_err("charge done but soc below 100 so reenable charge\n");
			msleep(500);
			charger_dev_enable_powerpath(info->chg1_dev, true);
			charger_dev_enable(info->chg1_dev, true);
		}
	}
}

static void check_charge_data(struct mtk_charger *info)
{
	union power_supply_propval pval = {0,};
	int ret = 0, val = 0;
	bool chg1_en = false;
	int charge_status = 0;

	if (!info->bms_psy) {
		info->bms_psy = power_supply_get_by_name("bms");
		chr_err("[XMCHG_MONITOR] failed to get bms_psy\n");
		return;
	}

	if (!info->battery_psy) {
		info->battery_psy = power_supply_get_by_name("battery");
		chr_err("[XMCHG_MONITOR] failed to get battery_psy\n");
		return;
	}

	//TODO: bring up cp charging
	if (!info->cp_master || !info->cp_slave) {
		info->cp_master = get_charger_by_name("cp_master");
		info->cp_slave = get_charger_by_name("cp_slave");
		chr_err("[XMCHG_MONITOR] failed to get cp charger\n");
	}

	charger_dev_is_enabled(info->chg1_dev, &info->bbc_charge_enable);
	charger_dev_is_charging_done(info->chg1_dev, &info->bbc_charge_done);


	if (info->chg_psy) {
		ret = power_supply_get_property(info->chg_psy, POWER_SUPPLY_PROP_STATUS, &pval);
		if (ret)
			chr_err("failed to get charge status\n");
		charge_status = pval.intval;
		chr_err("%s: charge_status=%d,%d \n", __func__,
				info->charge_status, charge_status);
	}

	if (info->charge_status != charge_status) {
		power_supply_changed(info->psy1);
		info->charge_status = charge_status;
	}

	ret = power_supply_get_property(info->bms_psy, POWER_SUPPLY_PROP_CAPACITY, &pval);
	if (ret)
		chr_err("[XMCHG_MONITOR]failed to get soc\n");
	else
		info->soc = pval.intval;

	ret = power_supply_get_property(info->bms_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
	if (ret)
		chr_err("[XMCHG_MONITOR] failed to get vbat\n");
	else
		info->vbat_now = pval.intval / 1000;

	ret = power_supply_get_property(info->bms_psy, POWER_SUPPLY_PROP_CURRENT_NOW, &pval);
	if (ret)
		chr_err("[XMCHG_MONITOR] failed to get ibat\n");
	else
		info->current_now = pval.intval / 1000;

	ret = power_supply_get_property(info->bms_psy, POWER_SUPPLY_PROP_TEMP, &pval);
	if (ret)
		chr_err("[XMCHG_MONITOR] failed to get tbat\n");
	else
		info->temp_now = pval.intval;

	ret = power_supply_get_property(info->battery_psy, POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, &pval);
	if (ret)
		chr_err("[XMCHG_MONITOR] failed to get thermal level\n");
	else
		info->thermal_level = pval.intval;

	ret = bms_get_property(BMS_PROP_I2C_ERROR_COUNT, &val);
	if (ret)
		chr_err("[XMCHG_MONITOR] failed to get i2c error count\n");
	else
		info->bms_i2c_error_count = val;

	ret = bms_get_property(BMS_PROP_AUTHENTIC, &val);
	if (ret)
		chr_err("[XMCHG_MONITOR] failed to get gauge authentic\n");
	else
		info->gauge_authentic = val;

	charger_dev_is_enabled(info->chg1_dev, &chg1_en);
	if(chg1_en && !info->ffc_enable && !info->ov_check_only_once && info->vbat_now >= (info->fv-10))
	{
		vote(info->fv_votable, FV_DEC_VOTER, true, info->fv - 10);
		info->ov_check_only_once = true;
		chr_err("[CHARGE_LOOP] fv vote reduce cv 10 mv\n");
	}

	chr_err("last time diff_fv_val:%d, clear diff_fv_val as 0 to start next time\n", info->diff_fv_val);
	info->diff_fv_val = 0;

	if(info->cycle_count > 800 && info->jeita_chg_index[0] == 4)
	{
		info->diff_fv_val = 40;
		chr_err("cycle_count active reduce fv %d mv\n", info->diff_fv_val);
	}else if (info->cycle_count > 100 && info->jeita_chg_index[0] == 4) {
		info->diff_fv_val = 20;
		chr_err("cycle_count active reduce fv %d mv\n", info->diff_fv_val);
	}

	if(info->smart_batt_reduceXMv)
	{
		info->diff_fv_val = max(info->diff_fv_val, info->smart_batt_reduceXMv);
		chr_err("smart batt active as %d mv\n", info->smart_batt_reduceXMv);
	}

	chr_err("[CHARGE_LOOP] TYPE = [%d %d %d %d], BMS = [%d %d %d %d], FULL = [%d %d %d %d %d], thermal_level=%d, FFC = %d, sw_cv=%d, gauge_authentic=%d,warm_term=%d\n",
		info->pd_type, info->pd_adapter->verifed, info->real_type, info->qc3_type, info->soc, info->vbat_now, info->current_now, info->temp_now,
		info->bbc_charge_enable, info->charge_full, info->fg_full, info->bbc_charge_done, info->recharge, info->thermal_level, info->ffc_enable,
		info->sw_cv, info->gauge_authentic,info->warm_term);

	if (info->input_suspend || info->typec_burn)
		chr_err("[CHARGE_LOOP] input_suspend = %d, typec_burn = %d\n", info->input_suspend, info->typec_burn);
}

static void monitor_night_charging(struct mtk_charger *info)
{
	if(info == NULL || !info->fcc_votable)
		return;
	if(info->night_charging && info->soc >=80 && !info->night_charge_enable)
	{
		info->night_charge_enable = true;
		charger_dev_enable(info->chg1_dev, false);
	}
	else if((!info->night_charging || info->soc <=75) && info->night_charge_enable)
	{
		info->night_charge_enable = false;
		charger_dev_enable(info->chg1_dev, true);
	}
}

static void monitor_slave_connector(struct mtk_charger *info)
{
		int val;
		int ret;
		if(info->mt6363_auxadc4 == NULL)
			return;
		ret = iio_read_channel_processed(info->mt6363_auxadc4, &val);
		if(ret < 0)
		{
			chr_err("[%s]: can not read slave_connector mt6363_auxadc4\n",__func__);
			return;
		}
		usb_set_property(USB_PROP_BATTCONT_ONLINE, val);
		chr_err("[%s]: mt6363_auxadc4's val =%d\n",__func__, val);
		if(val >= 1200 && !info->mt6363_auxadc4_enable)
		{
			vote(info->fcc_votable, SLAVE_CON_ERR_VOTER, true, 2000);
			info->mt6363_auxadc4_enable = true;
			chr_err("[%s]:slave_connector disconnect\n",__func__);
		}
		else if(val < 1200 && info->mt6363_auxadc4_enable)
		{
			vote(info->fcc_votable, SLAVE_CON_ERR_VOTER, false, 0);
			info->mt6363_auxadc4_enable = false;
			chr_err("[%s]:slave_connector connect\n",__func__);
		}
}

static void monitor_isc_and_soa_charge(struct mtk_charger *info)//NFG1000A & NFG1000B支持该功能
{
	int ret = 0;
	int batt_isc_alert_level = 0;
	int batt_soa_alert_level = 0;

	ret = bms_get_property(BMS_PROP_ISC_ALERT_LEVEL, &batt_isc_alert_level);
	if(ret < 0)
	{
		chr_err("[%s]:get isc occur error.%d\n", __func__);
		return;
	}

	ret = bms_get_property(BMS_PROP_SOA_ALERT_LEVEL, &batt_soa_alert_level);
	if(ret < 0)
	{
		chr_err("[%s]:get soa occur error.%d\n", __func__);
		return;
	}
	chr_err("[%s]:isc_alert_level=%d, soa_alert_level=%d\n", __func__, batt_isc_alert_level, batt_soa_alert_level);
	if((batt_isc_alert_level == 3) || batt_soa_alert_level)
	{
		info->diff_fv_val= info->diff_fv_val > 15 ? info->diff_fv_val : 15;
		info->div_jeita_fcc_flag = true;
	}
}

static void charge_monitor_func(struct work_struct *work)
{
	struct mtk_charger *info = container_of(work, struct mtk_charger, charge_monitor_work.work);

	check_charge_data(info);

	monitor_isc_and_soa_charge(info);

	handle_ffc_charge(info);

	check_full_recharge(info);

	monitor_thermal_limit(info);

	handle_step_charge(info);

	handle_jeita_charge(info);

	monitor_night_charging(info);

	monitor_sw_cv(info);

	monitor_jeita_descent(info);

	monitor_typec_burn(info);

	monitor_slave_connector(info);

	schedule_delayed_work(&info->charge_monitor_work, msecs_to_jiffies(FCC_DESCENT_DELAY));
}

static int debug_cycle_count = 0;
module_param_named(debug_cycle_count, debug_cycle_count, int, 0600);

static int parse_step_charge_config(struct mtk_charger *info, bool farce_update)
{
	struct device_node *np = info->pdev->dev.of_node;
	union power_supply_propval pval = {0,};
	static bool low_cycle = true;
	static bool middle_cycle = false;
	bool update = false;
	int total_length = 0, i = 0, ret = 0;

	if (!info->bms_psy)
		info->bms_psy = power_supply_get_by_name("bms");

	if (info->bms_psy) {
		ret = power_supply_get_property(info->bms_psy, POWER_SUPPLY_PROP_CYCLE_COUNT, &pval);
		if (ret)
			pval.intval = 0;
	} else {
		pval.intval = 0;
	}

	if (debug_cycle_count > 0)
		info->cycle_count = debug_cycle_count;
	else
		info->cycle_count = pval.intval;

	if (low_cycle) {
		if (info->cycle_count > 100 && info->cycle_count <= 800) {
			low_cycle = false;
			middle_cycle = true;
			update = true;
		}else if(info->cycle_count > 800){
			low_cycle = false;
			middle_cycle = false;
			update = true;
		}
	} else if(middle_cycle){
		if (info->cycle_count > 800) {
			low_cycle = false;
			middle_cycle = false;
			update = true;
		}else if(info->cycle_count <= 100){
			low_cycle = true;
			middle_cycle = false;
			update = true;
		}
	}else{
		if (info->cycle_count <= 100) {
			low_cycle = true;
			update = true;
		}else if(info->cycle_count > 100 && info->cycle_count <= 800){
			low_cycle = false;
			middle_cycle = true;
			update = true;
		}else if(info->cycle_count > 800){
			low_cycle = false;
			middle_cycle = false;
			update = true;
		}
	}

	if (update || farce_update) {
		if (low_cycle) {
			total_length = of_property_count_elems_of_size(np, "step_chg_cfg_low_cycle", sizeof(u32));
			if (total_length < 0) {
				chr_err("failed to read total_length of config\n");
				return -EINVAL;
			}

			ret = of_property_read_u32_array(np, "step_chg_cfg_low_cycle", (u32 *)info->step_chg_cfg, total_length);
			if (ret) {
				chr_err("failed to parse step_chg_cfg_low_cycle\n");
				return ret;
			}
		}else if(middle_cycle){
			total_length = of_property_count_elems_of_size(np, "step_chg_cfg_middle_cycle", sizeof(u32));
			if (total_length < 0) {
				chr_err("failed to read total_length of config\n");
				return -EINVAL;
			}

			ret = of_property_read_u32_array(np, "step_chg_cfg_middle_cycle", (u32 *)info->step_chg_cfg, total_length);
			if (ret) {
				chr_err("failed to parse step_chg_cfg_middle_cycle\n");
				return ret;
			}
		} else {
			total_length = of_property_count_elems_of_size(np, "step_chg_cfg_high_cycle", sizeof(u32));
			if (total_length < 0) {
				chr_err("failed to read total_length of config\n");
				return -EINVAL;
			}

			ret = of_property_read_u32_array(np, "step_chg_cfg_high_cycle", (u32 *)info->step_chg_cfg, total_length);
			if (ret) {
				chr_err("failed to parse step_chg_cfg_high_cycle\n");
				return ret;
			}
		}

		for (i = 0; i < STEP_JEITA_TUPLE_COUNT; i++)
			chr_err("%d STEP %d %d %d\n", info->cycle_count, info->step_chg_cfg[i].low_threshold, info->step_chg_cfg[i].high_threshold, info->step_chg_cfg[i].value);
	}

	return ret;
}

void reset_step_jeita_charge(struct mtk_charger *info)
{
	get_index(info->step_chg_cfg, info->step_fallback_hyst, info->step_forward_hyst, info->vbat_now, info->step_chg_index, true);
	info->step_chg_fcc = info->step_chg_cfg[info->step_chg_index[0]].value;
	vote(info->fcc_votable, STEP_CHARGE_VOTER, true, info->step_chg_fcc);

	get_index(info->jeita_fv_cfg, info->jeita_fallback_hyst, info->jeita_forward_hyst, info->temp_now, info->jeita_chg_index, true);
	if (info->vbat_now < info->jeita_fcc_cfg[info->jeita_chg_index[0]].extra_threshold)
		info->jeita_chg_fcc = info->jeita_fcc_cfg[info->jeita_chg_index[0]].low_value;
	else
		info->jeita_chg_fcc = info->jeita_fcc_cfg[info->jeita_chg_index[0]].high_value;
	vote(info->fcc_votable, JEITA_CHARGE_VOTER, true, info->jeita_chg_fcc);
	chr_err(" fcc:%d,low_value:%d,high_value:%d",info->jeita_chg_fcc,
	info->jeita_fcc_cfg[info->jeita_chg_index[0]].low_value, info->jeita_fcc_cfg[info->jeita_chg_index[0]].high_value);

	parse_step_charge_config(info, false);
}

int step_jeita_init(struct mtk_charger *info, struct device *dev)
{
	struct device_node *np = dev->of_node;
	int total_length = 0, i = 0, ret = 0;
	if (!np) {
		chr_err("no device node\n");
		return -EINVAL;
	}
	total_length = of_property_count_elems_of_size(np, "thermal_limit_dcp", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "thermal_limit_dcp", (u32 *)(info->thermal_limit[0]), total_length);
	if (ret) {
		chr_err("failed to parse thermal_limit_dcp\n");
		return ret;
	}

	for (i = 0; i < THERMAL_LIMIT_COUNT; i++) {
		chr_info("thermal_limit_dcp %d\n", info->thermal_limit[0][i]);
		if (info->thermal_limit[0][i] > MAX_THERMAL_FCC || info->thermal_limit[0][i] < MIN_THERMAL_FCC) {
			chr_err("thermal_limit_dcp over range\n");
			return -1;
		}
		if (i != 0) {
			if (info->thermal_limit[0][i] > info->thermal_limit[0][i - 1]) {
				chr_err("thermal_limit_dcp order error\n");
				return -1;
			}
		}
	}

	total_length = of_property_count_elems_of_size(np, "thermal_limit_qc2", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "thermal_limit_qc2", (u32 *)(info->thermal_limit[1]), total_length);
	if (ret) {
		chr_err("failed to parse thermal_limit_qc2\n");
		return ret;
	}

	for (i = 0; i < THERMAL_LIMIT_COUNT; i++) {
		chr_info("thermal_limit_qc2 %d\n", info->thermal_limit[1][i]);
		if (info->thermal_limit[1][i] > MAX_THERMAL_FCC || info->thermal_limit[1][i] < MIN_THERMAL_FCC) {
			chr_err("thermal_limit_qc2 over range\n");
			return -1;
		}
		if (i != 0) {
			if (info->thermal_limit[1][i] > info->thermal_limit[1][i - 1]) {
				chr_err("thermal_limit_qc2 order error\n");
				return -1;
			}
		}
	}

	total_length = of_property_count_elems_of_size(np, "thermal_limit_qc3_18w", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "thermal_limit_qc3_18w", (u32 *)(info->thermal_limit[2]), total_length);
	if (ret) {
		chr_err("failed to parse thermal_limit_qc3_18w\n");
		return ret;
	}

	for (i = 0; i < THERMAL_LIMIT_COUNT; i++) {
		chr_info("thermal_limit_qc3_18w %d\n", info->thermal_limit[2][i]);
		if (info->thermal_limit[2][i] > MAX_THERMAL_FCC || info->thermal_limit[2][i] < MIN_THERMAL_FCC) {
			chr_err("thermal_limit_qc3_18w over range\n");
			return -1;
		}
		if (i != 0) {
			if (info->thermal_limit[2][i] > info->thermal_limit[2][i - 1]) {
				chr_err("thermal_limit_qc3_18w order error\n");
				return -1;
			}
		}
	}

	total_length = of_property_count_elems_of_size(np, "thermal_limit_qc3_27w", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "thermal_limit_qc3_27w", (u32 *)(info->thermal_limit[3]), total_length);
	if (ret) {
		chr_err("failed to parse thermal_limit_qc3_27w\n");
		return ret;
	}

	for (i = 0; i < THERMAL_LIMIT_COUNT; i++) {
		chr_info("thermal_limit_qc3_27w %d\n", info->thermal_limit[3][i]);
		if (info->thermal_limit[3][i] > MAX_THERMAL_FCC || info->thermal_limit[3][i] < MIN_THERMAL_FCC) {
			chr_err("thermal_limit_qc3_27w over range\n");
			return -1;
		}
		if (i != 0) {
			if (info->thermal_limit[3][i] > info->thermal_limit[3][i - 1]) {
				chr_err("thermal_limit_qc3_27w order error\n");
				return -1;
			}
		}
	}

	total_length = of_property_count_elems_of_size(np, "thermal_limit_qc35", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "thermal_limit_qc35", (u32 *)(info->thermal_limit[4]), total_length);
	if (ret) {
		chr_err("failed to parse thermal_limit_qc35\n");
		return ret;
	}

	for (i = 0; i < THERMAL_LIMIT_COUNT; i++) {
		chr_info("thermal_limit_qc35 %d\n", info->thermal_limit[4][i]);
		if (info->thermal_limit[4][i] > MAX_THERMAL_FCC || info->thermal_limit[4][i] < MIN_THERMAL_FCC) {
			chr_err("thermal_limit_qc35 over range\n");
			return -1;
		}
		if (i != 0) {
			if (info->thermal_limit[4][i] > info->thermal_limit[4][i - 1]) {
				chr_err("thermal_limit_qc35 order error\n");
				return -1;
			}
		}
	}

	total_length = of_property_count_elems_of_size(np, "thermal_limit_pd", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "thermal_limit_pd", (u32 *)(info->thermal_limit[5]), total_length);
	if (ret) {
		chr_err("failed to parse thermal_limit_pd\n");
		return ret;
	}

	for (i = 0; i < THERMAL_LIMIT_COUNT; i++) {
		chr_info("thermal_limit_pd %d\n", info->thermal_limit[5][i]);
		if (info->thermal_limit[5][i] > MAX_THERMAL_FCC || info->thermal_limit[5][i] < MIN_THERMAL_FCC) {
			chr_err("thermal_limit_pd over range\n");
			return -1;
		}
		if (i != 0) {
			if (info->thermal_limit[5][i] > info->thermal_limit[5][i - 1]) {
				chr_err("thermal_limit_pd order error\n");
				return -1;
			}
		}
	}

	info->cycle_count = 0;
	ret = parse_step_charge_config(info, true);
	if (ret) {
		chr_err("failed to parse step_chg_cfg\n");
		return ret;
	}

	total_length = of_property_count_elems_of_size(np, "jeita_fcc_cfg", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "jeita_fcc_cfg", (u32 *)info->jeita_fcc_cfg, total_length);
	if (ret) {
		chr_err("failed to parse jeita_fcc_cfg\n");
		return ret;
	}

	for (i = 0; i < STEP_JEITA_TUPLE_COUNT; i++)
		chr_info("JEITA_FCC %d %d %d %d %d\n", info->jeita_fcc_cfg[i].low_threshold, info->jeita_fcc_cfg[i].high_threshold, info->jeita_fcc_cfg[i].extra_threshold, info->jeita_fcc_cfg[i].low_value, info->jeita_fcc_cfg[i].high_value);

	total_length = of_property_count_elems_of_size(np, "jeita_fv_cfg", sizeof(u32));
	if (total_length < 0) {
		chr_err("failed to read total_length of config\n");
		return -EINVAL;
	}

	ret = of_property_read_u32_array(np, "jeita_fv_cfg", (u32 *)info->jeita_fv_cfg, total_length);
	if (ret) {
		chr_err("failed to parse jeita_fv_cfg\n");
		return ret;
	}

	for (i = 0; i < STEP_JEITA_TUPLE_COUNT; i++)
		chr_info("JEITA_FV %d %d %d\n", info->jeita_fv_cfg[i].low_threshold, info->jeita_fv_cfg[i].high_threshold, info->jeita_fv_cfg[i].value);

	ret = of_property_read_u32(np, "step_fallback_hyst", &info->step_fallback_hyst);
	if (ret) {
		chr_err("failed to parse step_fallback_hyst\n");
		return ret;
	}

	ret = of_property_read_u32(np, "step_forward_hyst", &info->step_forward_hyst);
	if (ret) {
		chr_err("failed to parse step_forward_hyst\n");
		return ret;
	}

	ret = of_property_read_u32(np, "jeita_fallback_hyst", &info->jeita_fallback_hyst);
	if (ret) {
		chr_err("failed to parse jeita_fallback_hyst\n");
		return ret;
	}

	ret = of_property_read_u32(np, "jeita_forward_hyst", &info->jeita_forward_hyst);
	if (ret) {
		chr_err("failed to parse jeita_forward_hyst\n");
		return ret;
	}

	info->vbus_contral = devm_regulator_get(dev, "vbus_control");
	if (IS_ERR(info->vbus_contral))
		chr_err("failed to get vbus contral\n");

	info->mt6373_moscon1_control = of_property_read_bool(np, "mt6373_moscon1_control");
	if (info->mt6373_moscon1_control)
		chr_err("successed to parse mt6373_moscon1_control\n");

	info->sic_support =  of_property_read_bool(np, "sic_support");
	if (info->sic_support)
		chr_err("successed to parse sic_support\n");

	info->mt6363_auxadc4 = devm_iio_channel_get(dev, "mt6363_auxadc4");
	if(IS_ERR(info->mt6363_auxadc4)) {
		info->mt6363_auxadc4 = NULL;
		chr_err("[%s]:failed get iio_channel device\n",__func__);
	}

	INIT_DELAYED_WORK(&info->charge_monitor_work, charge_monitor_func);
	INIT_DELAYED_WORK(&info->usb_otg_monitor_work, monitor_usb_otg_burn);
#if defined(CONFIG_RUST_DETECTION)
	INIT_DELAYED_WORK(&info->rust_detection_work, rust_detection_work_func);
	if (!info->typec_attach) {
		schedule_delayed_work(&info->rust_detection_work, msecs_to_jiffies(3000));
	}
#endif


	return ret;
}
