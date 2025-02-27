/* Copyright (c) 2018-2019 The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/device.h>
#include <linux/regmap.h>
#include <linux/delay.h>
#include <linux/power_supply.h>
#include <linux/regulator/driver.h>
#include <linux/qpnp/qpnp-revid.h>
#include <linux/irq.h>
#include <linux/pmic-voter.h>
#include <linux/of_batterydata.h>
#include <linux/alarmtimer.h>
#include "smb5-lib.h"
#include "smb5-reg.h"
#include "battery.h"
#include "schgm-flash.h"
#include "step-chg-jeita.h"
#include "storm-watch.h"
#include "schgm-flash.h"
#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
#include <linux/afc/afc.h>
#endif
#endif
#ifdef CONFIG_USB_NOTIFY_LAYER
#include <linux/usb_notify.h>
#endif

#if defined(CONFIG_TYPEC)
#include <linux/usb/typec/pdic_core.h>
#endif

#define smblib_err(chg, fmt, ...)		\
	pr_err("%s: %s: " fmt, chg->name,	\
		__func__, ##__VA_ARGS__)	\

#define smblib_dbg(chg, reason, fmt, ...)			\
	do {							\
		if (*chg->debug_mask & (reason))		\
			pr_info("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
		else						\
			pr_debug("%s: %s: " fmt, chg->name,	\
				__func__, ##__VA_ARGS__);	\
	} while (0)


/* HS60 add for HS60-293 Import main-source ATL battery profile by gaochao at 2019/07/31 start */
static void smblib_update_chg_info(struct smb_charger *chg)
{
	union power_supply_propval info_val = {0,};
	struct smbchg_info chg_info = {0,};

	if (chg->batt_psy)
	{
			power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_CAPACITY, &info_val);
			chg_info.cap = info_val.intval;

			power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &info_val);
			chg_info.vbat = info_val.intval;

			power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_CURRENT_NOW, &info_val);
			chg_info.bat_c= info_val.intval;

			power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_TEMP, &info_val);
			chg_info.bat_t= info_val.intval;

			power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_STATUS, &info_val);
			chg_info.sts= info_val.intval;
	}

	if (chg->usb_psy)
	{
			power_supply_get_property(chg->usb_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &info_val);
			chg_info.vbus= info_val.intval;

			power_supply_get_property(chg->usb_psy, POWER_SUPPLY_PROP_INPUT_CURRENT_NOW, &info_val);
			chg_info.usb_c= info_val.intval;

			power_supply_get_property(chg->usb_psy, POWER_SUPPLY_PROP_INPUT_CURRENT_SETTLED, &info_val);
			chg_info.icl_settled= info_val.intval;

			power_supply_get_property(chg->usb_psy, POWER_SUPPLY_PROP_REAL_TYPE, &info_val);
			chg_info.chg_type= info_val.intval;
	}

	smblib_err(chg, "cap=%d, vbat=%d, FCC=%d, bat_temp=%d, status=%d, Vbus=%d, usb_cur=%d, ICL=%d, chg_type=%d\n",
		chg_info.cap, chg_info.vbat, chg_info.bat_c, chg_info.bat_t, chg_info.sts, chg_info.vbus, chg_info.usb_c, chg_info.icl_settled, chg_info.chg_type);
}
/* HS60 add for HS60-293 Import main-source ATL battery profile by gaochao at 2019/07/31 end */


#define typec_rp_med_high(chg, typec_mode)			\
	((typec_mode == POWER_SUPPLY_TYPEC_SOURCE_MEDIUM	\
	|| typec_mode == POWER_SUPPLY_TYPEC_SOURCE_HIGH)	\
	&& !chg->typec_legacy)

int smblib_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int value;
	int rc = 0;

	rc = regmap_read(chg->regmap, addr, &value);
	if (rc >= 0)
		*val = (u8)value;

	return rc;
}

int smblib_batch_read(struct smb_charger *chg, u16 addr, u8 *val,
			int count)
{
	return regmap_bulk_read(chg->regmap, addr, val, count);
}

int smblib_write(struct smb_charger *chg, u16 addr, u8 val)
{
	return regmap_write(chg->regmap, addr, val);
}

int smblib_batch_write(struct smb_charger *chg, u16 addr, u8 *val,
			int count)
{
	return regmap_bulk_write(chg->regmap, addr, val, count);
}

int smblib_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val)
{
	return regmap_update_bits(chg->regmap, addr, mask, val);
}

int smblib_get_jeita_cc_delta(struct smb_charger *chg, int *cc_delta_ua)
{
	int rc, cc_minus_ua;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}

	if (stat & BAT_TEMP_STATUS_HOT_SOFT_BIT) {
		rc = smblib_get_charge_param(chg, &chg->param.jeita_cc_comp_hot,
					&cc_minus_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n",
					rc);
			return rc;
		}
	} else if (stat & BAT_TEMP_STATUS_COLD_SOFT_BIT) {
		rc = smblib_get_charge_param(chg,
					&chg->param.jeita_cc_comp_cold,
					&cc_minus_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get jeita cc minus rc=%d\n",
					rc);
			return rc;
		}
	} else {
		cc_minus_ua = 0;
	}

	*cc_delta_ua = -cc_minus_ua;

	return 0;
}

int smblib_icl_override(struct smb_charger *chg, bool override)
{
	int rc;

	rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG,
				ICL_OVERRIDE_AFTER_APSD_BIT,
				override ? ICL_OVERRIDE_AFTER_APSD_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't override ICL rc=%d\n", rc);

	return rc;
}

int smblib_stat_sw_override_cfg(struct smb_charger *chg, bool override)
{
	int rc = 0;

	/* override  = 1, SW STAT override; override = 0, HW auto mode */
	rc = smblib_masked_write(chg, MISC_SMB_EN_CMD_REG,
				SMB_EN_OVERRIDE_BIT,
				override ? SMB_EN_OVERRIDE_BIT : 0);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure SW STAT override rc=%d\n",
			rc);
		return rc;
	}

	return rc;
}

static void smblib_notify_extcon_props(struct smb_charger *chg, int id)
{
	union extcon_property_value val;
	union power_supply_propval prop_val;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_TYPEC) {
		smblib_get_prop_typec_cc_orientation(chg, &prop_val);
		val.intval = ((prop_val.intval == 2) ? 1 : 0);
		extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_TYPEC_POLARITY, val);
	}

	val.intval = true;
	extcon_set_property(chg->extcon, id,
				EXTCON_PROP_USB_SS, val);
}

static void smblib_notify_device_mode(struct smb_charger *chg, bool enable)
{
#ifdef CONFIG_USB_NOTIFY_LAYER
	struct otg_notify *o_notify = get_otg_notify();

	smblib_dbg(chg, PR_MISC, "enable=%d\n", enable);
#endif

	if (enable)
		smblib_notify_extcon_props(chg, EXTCON_USB);

	extcon_set_state_sync(chg->extcon, EXTCON_USB, enable);

#ifdef CONFIG_USB_NOTIFY_LAYER
	send_otg_notify(o_notify, NOTIFY_EVENT_VBUS, enable);
#endif
}

/*HS60 add for P220517-05405 add usb_date_enable by duanweiping at 20220613 start*/
inline __attribute__((always_inline)) void notify_device_mode(struct smb_charger *chg, bool enable)
{
	smblib_notify_device_mode(chg,enable);
}
/*HS60 add for P220517-05405 add usb_date_enable by duanweiping at 20220613 end*/

static void smblib_notify_usb_host(struct smb_charger *chg, bool enable)
{
#ifdef CONFIG_USB_NOTIFY_LAYER
	struct otg_notify *o_notify = get_otg_notify();

	cancel_delayed_work_sync(&chg->microb_otg_work);

	if (!o_notify) {
		schedule_delayed_work(&chg->microb_otg_work,
					msecs_to_jiffies(1000));
		smblib_dbg(chg, PR_MISC, "enable=%d, otg_notify not registered yet\n", enable);
		chg->otg_enable = enable;
		return;
	}

	smblib_dbg(chg, PR_MISC, "enable=%d\n", enable);
#endif

	if (enable)
		smblib_notify_extcon_props(chg, EXTCON_USB_HOST);

	extcon_set_state_sync(chg->extcon, EXTCON_USB_HOST, enable);

#ifdef CONFIG_USB_NOTIFY_LAYER
	send_otg_notify(o_notify, NOTIFY_EVENT_HOST, enable);
#endif
}

/*HS60 add for P220517-05405 add usb_date_enable by duanweiping at 20220613 start*/
inline __attribute__((always_inline)) void notify_usb_host(struct smb_charger *chg, bool enable)
{
	smblib_notify_usb_host(chg,enable);
}
/*HS60 add for P220517-05405 add usb_date_enable by duanweiping at 20220613 end*/

/********************
 * REGISTER GETTERS *
 ********************/

int smblib_get_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int *val_u)
{
	int rc = 0;
	u8 val_raw;

	rc = smblib_read(chg, param->reg, &val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't read from 0x%04x rc=%d\n",
			param->name, param->reg, rc);
		return rc;
	}

	if (param->get_proc)
		*val_u = param->get_proc(param, val_raw);
	else
		*val_u = val_raw * param->step_u + param->min_u;
	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, *val_u, val_raw);

	return rc;
}

int smblib_get_usb_suspend(struct smb_charger *chg, int *suspend)
{
	int rc = 0;
	u8 temp;

	rc = smblib_read(chg, USBIN_CMD_IL_REG, &temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_CMD_IL rc=%d\n", rc);
		return rc;
	}
	*suspend = temp & USBIN_SUSPEND_BIT;

	return rc;
}

struct apsd_result {
	const char * const name;
	const u8 bit;
	const enum power_supply_type pst;
};

enum {
	UNKNOWN,
	SDP,
	CDP,
	DCP,
	OCP,
	FLOAT,
	HVDCP2,
	HVDCP3,
	MAX_TYPES
};

static const struct apsd_result smblib_apsd_results[] = {
	[UNKNOWN] = {
		.name	= "UNKNOWN",
		.bit	= 0,
		.pst	= POWER_SUPPLY_TYPE_UNKNOWN
	},
	[SDP] = {
		.name	= "SDP",
		.bit	= SDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB
	},
	[CDP] = {
		.name	= "CDP",
		.bit	= CDP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_CDP
	},
	[DCP] = {
		.name	= "DCP",
		.bit	= DCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[OCP] = {
		.name	= "OCP",
		.bit	= OCP_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_DCP
	},
	[FLOAT] = {
		.name	= "FLOAT",
		.bit	= FLOAT_CHARGER_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_FLOAT
	},
	[HVDCP2] = {
		.name	= "HVDCP2",
		.bit	= DCP_CHARGER_BIT | QC_2P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP
	},
	[HVDCP3] = {
		.name	= "HVDCP3",
		.bit	= DCP_CHARGER_BIT | QC_3P0_BIT,
		.pst	= POWER_SUPPLY_TYPE_USB_HVDCP_3,
	},
};

static const struct apsd_result *smblib_get_apsd_result(struct smb_charger *chg)
{
	int rc, i;
	u8 apsd_stat, stat;
	const struct apsd_result *result = &smblib_apsd_results[UNKNOWN];

	rc = smblib_read(chg, APSD_STATUS_REG, &apsd_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return result;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", apsd_stat);

	if (!(apsd_stat & APSD_DTC_STATUS_DONE_BIT))
		return result;

	rc = smblib_read(chg, APSD_RESULT_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_RESULT_STATUS rc=%d\n",
			rc);
		return result;
	}
	stat &= APSD_RESULT_STATUS_MASK;

	for (i = 0; i < ARRAY_SIZE(smblib_apsd_results); i++) {
		if (smblib_apsd_results[i].bit == stat)
			result = &smblib_apsd_results[i];
	}

	if (apsd_stat & QC_CHARGER_BIT) {
		/* since its a qc_charger, either return HVDCP3 or HVDCP2 */
		if (result != &smblib_apsd_results[HVDCP3])
			result = &smblib_apsd_results[HVDCP2];
	}

	return result;
}

#define AICL_RANGE2_MIN_MV		5600
#define AICL_RANGE2_STEP_DELTA_MV	200
#define AICL_RANGE2_OFFSET		16
int smblib_get_aicl_cont_threshold(struct smb_chg_param *param, u8 val_raw)
{
	int base = param->min_u;
	u8 reg = val_raw;
	int step = param->step_u;


	if (val_raw >= AICL_RANGE2_OFFSET) {
		reg = val_raw - AICL_RANGE2_OFFSET;
		base = AICL_RANGE2_MIN_MV;
		step = AICL_RANGE2_STEP_DELTA_MV;
	}

	return base + (reg * step);
}

/********************
 * REGISTER SETTERS *
 ********************/
static const struct buck_boost_freq chg_freq_list[] = {
	[0] = {
		.freq_khz	= 2400,
		.val		= 7,
	},
	[1] = {
		.freq_khz	= 2100,
		.val		= 8,
	},
	[2] = {
		.freq_khz	= 1600,
		.val		= 11,
	},
	[3] = {
		.freq_khz	= 1200,
		.val		= 15,
	},
};

int smblib_set_chg_freq(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	u8 i;

	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	/* Charger FSW is the configured freqency / 2 */
	val_u *= 2;
	for (i = 0; i < ARRAY_SIZE(chg_freq_list); i++) {
		if (chg_freq_list[i].freq_khz == val_u)
			break;
	}
	if (i == ARRAY_SIZE(chg_freq_list)) {
		pr_err("Invalid frequency %d Hz\n", val_u / 2);
		return -EINVAL;
	}

	*val_raw = chg_freq_list[i].val;

	return 0;
}

int smblib_set_opt_switcher_freq(struct smb_charger *chg, int fsw_khz)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher, fsw_khz);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_buck rc=%d\n", rc);

	if (chg->mode == PARALLEL_MASTER && chg->pl.psy) {
		pval.intval = fsw_khz;
		/*
		 * Some parallel charging implementations may not have
		 * PROP_BUCK_FREQ property - they could be running
		 * with a fixed frequency
		 */
		power_supply_set_property(chg->pl.psy,
				POWER_SUPPLY_PROP_BUCK_FREQ, &pval);
	}

	return rc;
}

int smblib_set_charge_param(struct smb_charger *chg,
			    struct smb_chg_param *param, int val_u)
{
	int rc = 0;
	u8 val_raw;

	if (param->set_proc) {
		rc = param->set_proc(param, val_u, &val_raw);
		if (rc < 0)
			return -EINVAL;
	} else {
		if (val_u > param->max_u || val_u < param->min_u)
			smblib_dbg(chg, PR_MISC,
				"%s: %d is out of range [%d, %d]\n",
				param->name, val_u, param->min_u, param->max_u);

		if (val_u > param->max_u)
			val_u = param->max_u;
		if (val_u < param->min_u)
			val_u = param->min_u;

		val_raw = (val_u - param->min_u) / param->step_u;
	}

	rc = smblib_write(chg, param->reg, val_raw);
	if (rc < 0) {
		smblib_err(chg, "%s: Couldn't write 0x%02x to 0x%04x rc=%d\n",
			param->name, val_raw, param->reg, rc);
		return rc;
	}

	smblib_dbg(chg, PR_REGISTER, "%s = %d (0x%02x)\n",
		   param->name, val_u, val_raw);

	return rc;
}

int smblib_set_usb_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;
	int irq = chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq;

	if (suspend && irq) {
		if (chg->usb_icl_change_irq_enabled) {
			disable_irq_nosync(irq);
			chg->usb_icl_change_irq_enabled = false;
		}
	}

	rc = smblib_masked_write(chg, USBIN_CMD_IL_REG, USBIN_SUSPEND_BIT,
				 suspend ? USBIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to USBIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	if (!suspend && irq) {
		if (!chg->usb_icl_change_irq_enabled) {
			enable_irq(irq);
			chg->usb_icl_change_irq_enabled = true;
		}
	}

	return rc;
}

int smblib_set_dc_suspend(struct smb_charger *chg, bool suspend)
{
	int rc = 0;

	rc = smblib_masked_write(chg, DCIN_CMD_IL_REG, DCIN_SUSPEND_BIT,
				 suspend ? DCIN_SUSPEND_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't write %s to DCIN_SUSPEND_BIT rc=%d\n",
			suspend ? "suspend" : "resume", rc);

	return rc;
}

static int smblib_set_adapter_allowance(struct smb_charger *chg,
					u8 allowed_voltage)
{
	int rc = 0;

	/* PMI632 only support max. 9V */
	if (chg->smb_version == PMI632_SUBTYPE) {
		switch (allowed_voltage) {
		case USBIN_ADAPTER_ALLOW_12V:
		case USBIN_ADAPTER_ALLOW_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_OR_12V:
		case USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_OR_9V;
			break;
		case USBIN_ADAPTER_ALLOW_5V_TO_12V:
			allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
			break;
		}
	}

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	if(chg->afc_sts >= AFC_5V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
		smblib_err(chg, "smblib_set_adapter_allowance changed to 5V_TO_9V\n");
	}
#endif
#endif
	rc = smblib_write(chg, USBIN_ADAPTER_ALLOW_CFG_REG, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to USBIN_ADAPTER_ALLOW_CFG rc=%d\n",
			allowed_voltage, rc);
		return rc;
	}

	return rc;
}

#define MICRO_5V	5000000
#define MICRO_9V	9000000
#define MICRO_12V	12000000
static int smblib_set_usb_pd_allowed_voltage(struct smb_charger *chg,
					int min_allowed_uv, int max_allowed_uv)
{
	int rc;
	u8 allowed_voltage;

	if (min_allowed_uv == MICRO_5V && max_allowed_uv == MICRO_5V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V;
		smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_5V);
	} else if (min_allowed_uv == MICRO_9V && max_allowed_uv == MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V;
		smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_9V);
	} else if (min_allowed_uv == MICRO_12V && max_allowed_uv == MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_12V;
		smblib_set_opt_switcher_freq(chg, chg->chg_freq.freq_12V);
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_9V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_9V;
	} else if (min_allowed_uv < MICRO_9V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_5V_TO_12V;
	} else if (min_allowed_uv < MICRO_12V && max_allowed_uv <= MICRO_12V) {
		allowed_voltage = USBIN_ADAPTER_ALLOW_9V_TO_12V;
	} else {
		smblib_err(chg, "invalid allowed voltage [%d, %d]\n",
			min_allowed_uv, max_allowed_uv);
		return -EINVAL;
	}

	rc = smblib_set_adapter_allowance(chg, allowed_voltage);
	if (rc < 0) {
		smblib_err(chg, "Couldn't configure adapter allowance rc=%d\n",
				rc);
		return rc;
	}

	return rc;
}

int smblib_set_aicl_cont_threshold(struct smb_chg_param *param,
				int val_u, u8 *val_raw)
{
	int base = param->min_u;
	int offset = 0;
	int step = param->step_u;

	if (val_u > param->max_u)
		val_u = param->max_u;
	if (val_u < param->min_u)
		val_u = param->min_u;

	if (val_u >= AICL_RANGE2_MIN_MV) {
		base = AICL_RANGE2_MIN_MV;
		step = AICL_RANGE2_STEP_DELTA_MV;
		offset = AICL_RANGE2_OFFSET;
	};

	*val_raw = ((val_u - base) / step) + offset;

	return 0;
}

/********************
 * HELPER FUNCTIONS *
 ********************/

int smblib_get_prop_from_bms(struct smb_charger *chg,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	int rc;

	if (!chg->bms_psy)
		return -EINVAL;

	rc = power_supply_get_property(chg->bms_psy, psp, val);

	return rc;
}

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
void sec_bat_monitor_work(struct smb_charger *chg)
{
	const char *usb_icl_voter, *fcc_voter = NULL;

	usb_icl_voter = get_effective_client(chg->usb_icl_votable);
	fcc_voter = get_effective_client(chg->fcc_votable);

	pr_info("%s: cable_type:%d, flash_active(%d), afc_sts(%d), usb_icl_votable(%s): %d, fcc_votable(%s): %d\n", __func__,
		chg->real_charger_type, chg->flash_active, chg->afc_sts,
		usb_icl_voter ? usb_icl_voter : "None", get_effective_result(chg->usb_icl_votable),
		fcc_voter ? fcc_voter : "None", get_effective_result(chg->fcc_votable));
}

void smblib_hvdcp_detect_enable(struct smb_charger *chg, bool enable)
{
	int rc;
	u8 mask;

/*	if (chg->hvdcp_disable || chg->pd_not_supported)
		return; */

	smblib_err(chg, "smblib_hvdcp_detect_enable: %d\n",enable);
	mask = HVDCP_AUTH_ALG_EN_CFG_BIT | HVDCP_EN_BIT;
	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG, mask,
						enable ? mask : 0);
	if (rc < 0)
		smblib_err(chg, "failed to write USBIN_OPTIONS_1_CFG rc=%d\n",
				rc);

	return;
}
#endif
#endif

int smblib_configure_hvdcp_apsd(struct smb_charger *chg, bool enable)
{
	int rc;
	u8 mask = HVDCP_EN_BIT | BC1P2_SRC_DETECT_BIT;

	if (chg->pd_not_supported)
		return 0;

	rc = smblib_masked_write(chg, USBIN_OPTIONS_1_CFG_REG, mask,
						enable ? mask : 0);
	if (rc < 0)
		smblib_err(chg, "failed to write USBIN_OPTIONS_1_CFG rc=%d\n",
				rc);

	return rc;
}

static int smblib_request_dpdm(struct smb_charger *chg, bool enable)
{
	int rc = 0;

	/* fetch the DPDM regulator */
	if (!chg->dpdm_reg && of_get_property(chg->dev->of_node,
				"dpdm-supply", NULL)) {
		chg->dpdm_reg = devm_regulator_get(chg->dev, "dpdm");
		if (IS_ERR(chg->dpdm_reg)) {
			rc = PTR_ERR(chg->dpdm_reg);
			smblib_err(chg, "Couldn't get dpdm regulator rc=%d\n",
					rc);
			chg->dpdm_reg = NULL;
			return rc;
		}
	}

	if (enable) {
		if (chg->dpdm_reg && !regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "enabling DPDM regulator\n");
			rc = regulator_enable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't enable dpdm regulator rc=%d\n",
					rc);
		}
	} else {
		if (chg->dpdm_reg && regulator_is_enabled(chg->dpdm_reg)) {
			smblib_dbg(chg, PR_MISC, "disabling DPDM regulator\n");
			rc = regulator_disable(chg->dpdm_reg);
			if (rc < 0)
				smblib_err(chg,
					"Couldn't disable dpdm regulator rc=%d\n",
					rc);
		}
	}

	return rc;
}

static void smblib_rerun_apsd(struct smb_charger *chg)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "re-running APSD\n");

	rc = smblib_masked_write(chg, CMD_APSD_REG,
				APSD_RERUN_BIT, APSD_RERUN_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't re-run APSD rc=%d\n", rc);
}

static const struct apsd_result *smblib_update_usb_type(struct smb_charger *chg)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	if ((chg->real_charger_type == POWER_SUPPLY_TYPE_AFC) && (apsd_result->pst == POWER_SUPPLY_TYPE_USB_DCP)) {
		pr_info("%s: Ignore DCP after AFC\n", __func__);
		return apsd_result;
	}
#endif
#endif

	/* if PD is active, APSD is disabled so won't have a valid result */
	if (chg->pd_active) {
		chg->real_charger_type = POWER_SUPPLY_TYPE_USB_PD;
	} else {
		/*
		 * Update real charger type only if its not FLOAT
		 * detected as as SDP
		 */
		if (!(apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT &&
			chg->real_charger_type == POWER_SUPPLY_TYPE_USB))
			chg->real_charger_type = apsd_result->pst;
	}

	smblib_dbg(chg, PR_MISC, "APSD=%s PD=%d\n",
					apsd_result->name, chg->pd_active);
	return apsd_result;
}

/* HS60 add for HS60-163 Set usb thermal by gaochao at 2019/07/30 start */
u32 hq_get_huaqin_pcba_config(void);
void ss_vbus_control_gpio_init(struct smb_charger *chg);

void hq_usb_thermal_work_init(struct smb_charger *chg)
{
	pr_debug("line=%d\n", __LINE__);

	schedule_delayed_work(&chg->usb_thermal_status_change_work, msecs_to_jiffies(USB_TERMAL_START_DETECT_TIME));
	//ss_vbus_control_gpio_init(chg);
	//cancel_delayed_work_sync(&chg->usb_thermal_status_change_work);
}

void hq_usb_thermal_work_init_config(struct smb_charger *chg)
{
	if (!chg)
	{
		pr_err("[ss]line=%d: ichg is null\n", __LINE__);
		return;
	}
	/* HS60 add for HS60-542 enable thermal work by wangzikang at 2019/09/16 start */
	hq_usb_thermal_work_init(chg);
	/* HS60 add for HS60-542 enable thermal work by wangzikang at 2019/09/16 end */
	pr_debug("[ss]line=%d: ready hq_vbus_control_gpio_init\n", __LINE__);
}
/* HS60 add for HS60-163 Set usb thermal by gaochao at 2019/07/30 end */

void ss_vbus_control_gpio_init(struct smb_charger *chg)
{
	int val = 0;
	int rc = 0;

	val = gpio_get_value(chg->vbus_control_gpio);
	if (val == DRAW_VBUS_GPIO59_HIGH)
	{
		rc = gpio_direction_output(chg->vbus_control_gpio, DRAW_VBUS_GPIO59_LOW);
		if (rc)
		{
			pr_err("[%s]line=%d: failed to pull low vbus_control_gpio\n", __FUNCTION__, __LINE__);
		}
		else
		{
			pr_debug("[%s]line=%d: Pull low vbus_control_gpio\n", __FUNCTION__, __LINE__);
		}
	}
	else
	{
		pr_debug("[%s]line=%d: get vbus_control_gpio gpio val=%d\n", __FUNCTION__, __LINE__, val);
	}
}

void ss_vbus_control_gpio_set(struct smb_charger *chg, int set_gpio_val)
{
	int val = 0;
	int rc = 0;

	if (set_gpio_val == DRAW_VBUS_GPIO59_HIGH)
	{
		val = gpio_get_value(chg->vbus_control_gpio);
		if (val == DRAW_VBUS_GPIO59_LOW)
		{
			rc = gpio_direction_output(chg->vbus_control_gpio, DRAW_VBUS_GPIO59_HIGH);
			if (rc)
			{
				pr_err("[%s]line=%d: failed to pull high vbus_control_gpio\n", __FUNCTION__, __LINE__);
			}
			else
			{
				pr_debug("[%s]line=%d: Pull high vbus_control_gpio\n", __FUNCTION__, __LINE__);
			}
		}
		else
		{
			pr_debug("[%s]line=%d: keep vbus_control_gpio high=%d\n", __FUNCTION__, __LINE__, val);
		}
	}
	else
	{
		val = gpio_get_value(chg->vbus_control_gpio);
		if (val == DRAW_VBUS_GPIO59_HIGH)
		{
			rc = gpio_direction_output(chg->vbus_control_gpio, DRAW_VBUS_GPIO59_LOW);
			if (rc)
			{
				pr_err("[%s]line=%d: failed to pull low vbus_control_gpio\n", __FUNCTION__, __LINE__);
			}
			else
			{
				pr_debug("[%s]line=%d: Pull low vbus_control_gpio\n", __FUNCTION__, __LINE__);
			}
		}
		else
		{
			pr_debug("[%s]line=%d: keep vbus_control_gpio low=%d\n", __FUNCTION__, __LINE__, val);
		}
	}
}

int ss_smblib_get_adc_data(struct smb_charger *chg)
{
	int rc = 0;
	struct qpnp_vadc_result result;
	int64_t vadc = 0;

	chg->vadc_usb_alert = qpnp_get_vadc(chg->dev, "pm-gpio1");
	if (IS_ERR(chg->vadc_usb_alert))
	{
		pr_err("[%s]line=%d: Error get vadc_usb_alert rc=%d\n", __FUNCTION__, __LINE__, rc);
		rc = PTR_ERR(chg->vadc_usb_alert);
		if (rc != -EPROBE_DEFER)
		{
			pr_err("[%s]line=%d: Couldn't get chg_alert vadc rc=%d\n", __FUNCTION__, __LINE__, rc);
		}

		return rc;
	}

	if (chg->vadc_usb_alert)
	{
		/* HS70 add for HS70-250 Set usb thermal by gaochao at 2019/10/28 start */
		if (chg->distinguish_sdm439_sdm450_others == DETECT_SDM439_PLATFORM)	//HS60
		{
			rc = qpnp_vadc_read(chg->vadc_usb_alert, VADC_AMUX1_GPIO_PU2, &result);
		}
		else
		{
			rc = qpnp_vadc_read(chg->vadc_usb_alert, VADC_AMUX2_GPIO_PU2, &result);
		}
		/* HS70 add for HS70-250 Set usb thermal by gaochao at 2019/10/28 end */
		vadc =  result.physical;

		pr_info("[%s]line=%d: qpnp_vadc_read->usb_alert_voltage=%lld\n", __FUNCTION__, __LINE__, vadc);
	}
	else
	{
		pr_err("[%s]line=%d: NONE vadc_usb_alert\n", __FUNCTION__, __LINE__);
		return  -1;
	}

	return vadc;
}

void ss_update_usb_connector_state(struct smb_charger *chg)
{
	int64_t  phy_voltage = 0;

	phy_voltage = ss_smblib_get_adc_data(chg);
	if (phy_voltage <= 0)
	{
		return;
	}

	if (phy_voltage  < CHG_ALERT_HOT_NTC_VOLTAFE)	//USB thermal > 70
	{

		//extcon_set_state_sync(chg->extcon, EXTCON_CHG_USB_DCP, true);
		ss_vbus_control_gpio_set(chg, DRAW_VBUS_GPIO59_HIGH);

		pr_info("[%s]line=%d: USB connector hot, connect VBUS to GND\n", __FUNCTION__, __LINE__);
	}
	/*
	else if ((phy_voltage >= CHG_ALERT_HOT_NTC_VOLTAFE) && (phy_voltage <= CHG_ALERT_WARM_NTC_VOLTAGE))	//USB thermal [60, 70]
	{
		ss_vbus_control_gpio_set(chg, DRAW_VBUS_GPIO59_HIGH);

		printk("[%s]line=%d: USB connector former state is GOOD, now is warm, disconnect VBUS to GND\n", __FUNCTION__, __LINE__);
	}
	*/
	else if ((phy_voltage > CHG_ALERT_WARM_NTC_VOLTAGE))		//USB thermal < 60
	{
		//extcon_set_state_sync(chg->extcon, EXTCON_CHG_USB_DCP, false);
		ss_vbus_control_gpio_set(chg, DRAW_VBUS_GPIO59_LOW);

		pr_info("[%s]line=%d: USB connector temperature is GOOD, disconnect VBUS to GND\n", __FUNCTION__, __LINE__);
	}
}

/* HS60 add for HS60-163 Add usb thermal at DVT1 stage by gaochao at 2019/08/23 start */
void ss_determine_update_usb_connector_state(struct smb_charger *chg)
{
	if (!chg)
	{
		pr_err("[ss][%s]line=%d: ichg is null\n", __FUNCTION__, __LINE__);
		return;
	}

	/* HS70 add for HS70-135 Distinguish HS60 and HS70 charging by gaochao at 2019/10/10 start */
	if (chg->usb_connector_thermal_enable)
	{
		if (chg->vbus_control_gpio_enable)
		{
			ss_update_usb_connector_state(chg);
		}
	}

	pr_debug("[ss][%s]line=%d: start ss_update_usb_connector_state, usb_connector_thermal_enable=%d, vbus_control_gpio_enable=%d\n",
			__FUNCTION__, __LINE__, chg->usb_connector_thermal_enable, chg->vbus_control_gpio_enable);
	/* HS70 add for HS70-135 Distinguish HS60 and HS70 charging by gaochao at 2019/10/10 end */
}
/* HS60 add for HS60-163 Add usb thermal at DVT1 stage by gaochao at 2019/08/23 end */

static void ss_usb_thermal_status_change_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger, usb_thermal_status_change_work.work);

	pr_debug("[%s]line=%d: detect usb connector temperature every per 60s\n", __FUNCTION__, __LINE__);

	/* HS60 add for HS60-163 Add usb thermal at DVT1 stage by gaochao at 2019/08/23 start */
	ss_determine_update_usb_connector_state(chg);
	//ss_update_usb_connector_state(chg);
	/* HS60 add for HS60-163 Add usb thermal at DVT1 stage by gaochao at 2019/08/23 end */

	/* HS60 add for HS60-293 Import main-source ATL battery profile by gaochao at 2019/07/31 start */
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 start */
	smblib_update_chg_info(chg);
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 end */
	/* HS60 add for HS60-293 Import main-source ATL battery profile by gaochao at 2019/07/31 end */

	schedule_delayed_work(&chg->usb_thermal_status_change_work, msecs_to_jiffies(USB_TERMAL_DETECT_TIMER));
}
/* HS60 add for HS60-163 Set usb thermal by gaochao at 2019/07/30 end */


/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 start */
/* keep battery level at [59%, 70%] for every cycle */
#define SALE_CODE_STR_LEN		3
static char sales_code_from_cmdline[SALE_CODE_STR_LEN + 1];

static int __init sales_code_setup(char *str)
{
	strlcpy(sales_code_from_cmdline, str,
			ARRAY_SIZE(sales_code_from_cmdline));

	return 1;
	}
__setup("androidboot.sales_code=", sales_code_setup);

bool sales_code_is(char* str)
{
	return !strncmp(sales_code_from_cmdline, str,
			SALE_CODE_STR_LEN + 1);
}
/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 end */
/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 start*/
#define STORE_MODE_FCC 500000
/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 end*/
void ss_retail_app_rule(struct smb_charger *chg)
{
	union power_supply_propval prop = {0, };
	int bat_capacity = 0;
	int rc = 0;
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 start */
	int retail_app_dischg_threshold =0;
	int retail_app_chg_threshold =0;
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 end */

	if (chg)
	{
		/* obtain battery capacity */
		rc = power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_CAPACITY, &prop);
		if (rc < 0)
			pr_err("Failed to get battery capacity, rc=%d\n", rc);

		bat_capacity = prop.intval;
		/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 start */
		/* HS60 add for P200502-00042 by wangzikang at 2020/05/12 start */
		/*if (sales_code_is("VZW")) {*/
		if (sales_code_is("VZW") || sales_code_is("VPP")) {
			pr_err("%s: Sales is VZW or VPP\n", __func__);
			/* HS60 add for P200502-00042 by wangzikang at 2020/05/12 end */
			retail_app_dischg_threshold = SS_RETAIL_APP_DISCHG_THRESHOLD_VZW;
			retail_app_chg_threshold = SS_RETAIL_APP_CHG_THRESHOLD_VZW;
		} else {
			retail_app_dischg_threshold = SS_RETAIL_APP_DISCHG_THRESHOLD;
			retail_app_chg_threshold = SS_RETAIL_APP_CHG_THRESHOLD;
		}
		if (bat_capacity >= retail_app_dischg_threshold) {
		/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 end */
			rc = smblib_set_usb_suspend(chg, true);
			if (rc < 0)
				smblib_err(chg, "Couldn't suspend input rc=%d\n", rc);

			printk("[%s]line=%d: retail_app stop usb charging\n", __FUNCTION__, __LINE__);
		/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 start */
		} else if (bat_capacity < retail_app_chg_threshold) {
		/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 end */
			rc = smblib_set_usb_suspend(chg, false);
			if (rc < 0)
				smblib_err(chg, "Couldn't resume input rc=%d\n", rc);

			printk("[%s]line=%d: retail_app resume usb charging\n", __FUNCTION__, __LINE__);
		} else
			printk("[%s]line=%d: retail_app keep previous state\n", __FUNCTION__, __LINE__);
	} else
		smblib_err(chg, "[ss]chg is NULL\n");

}

static void ss_retail_app_status_change_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
		struct smb_charger, retail_app_status_change_work.work);
	/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 start*/
	int rc=0;
	/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 end*/
	pr_info("[%s]line=%d: retail_app run every per 60s, chg->store_mode_ss=%d\n",
			__FUNCTION__, __LINE__, chg->store_mode_ss);

	/* when store_mode is set as 1, check battery level to charge or discharge */
	if (chg->store_mode_ss == STORE_MODE_ENABLE_SS)
	{
		ss_retail_app_rule(chg);
		/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 start*/
		rc = vote(chg->fcc_votable,STORE_MODE_VOTER,true,STORE_MODE_FCC);
		if(rc < 0)
			pr_err("Failed to vote STORE_MODE_VOTER, rc=%d\n", rc);
		/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 end*/

	}
	/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 start*/
	else
	{
		rc = vote(chg->fcc_votable,STORE_MODE_VOTER,false,STORE_MODE_FCC);
		if(rc < 0)
			pr_err("Failed to vote STORE_MODE_VOTER, rc=%d\n", rc);
	}
	/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 end*/

	/* update charging status while adapter is present */
	//smblib_update_chg_info(chg);

	schedule_delayed_work(&chg->retail_app_status_change_work, msecs_to_jiffies(RETAIL_APP_DETECT_TIMER));
}

void smblib_get_prop_batt_store_mode_samsung(struct smb_charger *chg,
				union power_supply_propval *val)
{
	if (chg)
	{
		val->intval = chg->store_mode_ss;

		smblib_dbg(chg, PR_MISC, "[ss]val->intval=%d, chg->store_mode_ss=%d\n", val->intval, chg->store_mode_ss);
	} else
		smblib_err(chg, "[ss]chg is NULL\n");
}

void smblib_set_prop_batt_store_mode_samsung(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 start */
	bool vbus_rising;
	union power_supply_propval vbus_val = {0, };
	int rc;

	rc = smblib_get_prop_usb_present(chg, &vbus_val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
	}
	vbus_rising = vbus_val.intval;
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 end */

	if (chg) {
		chg->store_mode_ss = val->intval;

		smblib_dbg(chg, PR_MISC, "[ss]val->intval=%d, chg->store_mode_ss=%d\n", val->intval, chg->store_mode_ss);
	} else
		smblib_err(chg, "[ss]chg is NULL\n");
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 start */
	if(vbus_rising){
		if(chg->store_mode_ss == 1){
			printk("Set store mode while usb is present!");
			schedule_delayed_work(&chg->retail_app_status_change_work, msecs_to_jiffies(RETAIL_APP_START_DETECT_TIME));
		}else{
			/* HS60 add for HS70-4387 by wangzikang at 2020/02/25 end */
			rc = vote(chg->fcc_votable,STORE_MODE_VOTER,false,STORE_MODE_FCC);
			if(rc < 0)
				pr_err("Failed to vote STORE_MODE_VOTER, rc=%d\n", rc);
			/* HS60 add for HS70-4387 by wangzikang at 2020/02/25 end */
			cancel_delayed_work_sync(&chg->retail_app_status_change_work);
			printk("Clear store mode while usb is present!");
		}
	}
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 end */
}
#endif
/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 end */


/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 start */
static void hq_rerun_apsd_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger, rerun_apsd_work.work);

	int rc = 0;
	u8 stat = 0;
	const struct apsd_result *apsd_result = NULL;

	if (chg)
	{
			rc = smblib_read(chg, APSD_STATUS_REG, &stat);
			if (rc < 0)
			{
				smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
			}

			apsd_result = smblib_update_usb_type(chg);
			/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 start */
			pr_info("line=%d, APSD_STATUS=0x%02x, pst=%d, bit=%d, real_charger_type=%d, typec_mode=%d\n",
					__LINE__, stat, apsd_result->pst, apsd_result->bit, chg->real_charger_type, chg->typec_mode);
			/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 end */

			switch (apsd_result->pst) {
			case POWER_SUPPLY_TYPE_USB_DCP:
				chg->real_charger_type = POWER_SUPPLY_TYPE_USB_DCP;

				rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
				if (rc < 0)
				{
					pr_err("line=%d: Couldn't vote USB_PSY_VOTER rc=%d\n", __LINE__, rc);
				}

				rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, DCP_CURRENT_UA);
				if (rc < 0)
				{
					pr_err("line=%d: Couldn't vote SW_ICL_MAX_VOTER rc=%d\n", __LINE__, rc);
				}

				break;
			case POWER_SUPPLY_TYPE_USB_FLOAT:
				/* HS60 add for P191120-04834 DCP is detected as float charger by slowly inserting or floating D+ and D- by gaochao at 2019/11/25 start */
				#if !defined(HQ_FACTORY_BUILD)	//ss version
				/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 start */
				if ((chg->boot_to_detect_charger == BOOT_TO_DETECT_START) || (chg->boot_to_detect_charger == BOOT_TO_DETECT_SECOND))
				{
					chg->boot_to_detect_charger = BOOT_TO_DETECT_MAX;
				}
				else
				{
					/* HS70 add for P200615-07791 force float charger to sdp to resolve disconnectiong of USB port by gaochao at 2020/07/16 start */
					chg->real_charger_type = POWER_SUPPLY_TYPE_USB;
					/* HS70 add for P200615-07791 force float charger to sdp to resolve disconnectiong of USB port by gaochao at 2020/07/16 end */
				}
				/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 end */
				#endif
				/* HS60 add for P191120-04834 DCP is detected as float charger by slowly inserting or floating D+ and D- by gaochao at 2019/11/25 end */

				rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
				if (rc < 0)
				{
					pr_err("line=%d: Couldn't vote USB_PSY_VOTER rc=%d\n", __LINE__, rc);
				}

				rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, SDP_CURRENT_UA);
				if (rc < 0)
				{
					pr_err("line=%d: Couldn't vote SW_ICL_MAX_VOTER rc=%d\n", __LINE__, rc);
				}

				break;
			/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 start */
			case POWER_SUPPLY_TYPE_USB:
				/* HS60 add for P191120-04834 DCP is detected as float charger by slowly inserting or floating D+ and D- by gaochao at 2019/11/25 start */
				#if !defined(HQ_FACTORY_BUILD)	//ss version
				/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 start */
				if ((chg->boot_to_detect_charger == BOOT_TO_DETECT_START) || (chg->boot_to_detect_charger == BOOT_TO_DETECT_SECOND))
				{
					chg->boot_to_detect_charger = BOOT_TO_DETECT_MAX;
				}
				else
				{
					/* HS70 add for P200615-07791 force float charger to sdp to resolve disconnectiong of USB port by gaochao at 2020/07/16 start */
					// chg->real_charger_type = POWER_SUPPLY_TYPE_USB_FLOAT;
					/* HS70 add for P200615-07791 force float charger to sdp to resolve disconnectiong of USB port by gaochao at 2020/07/16 end */
				}
				/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 end */
				#endif
				/* HS60 add for P191120-04834 DCP is detected as float charger by slowly inserting or floating D+ and D- by gaochao at 2019/11/25 end */

				rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
				if (rc < 0)
				{
					pr_err("line=%d: Couldn't vote SW_ICL_MAX_VOTER rc=%d\n", __LINE__, rc);
				}

				rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, true, SDP_CURRENT_UA);
				if (rc < 0)
				{
					pr_err("line=%d: Couldn't vote USB_PSY_VOTER rc=%d\n", __LINE__, rc);
				}

				break;
			/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 end */
			default:
				smblib_err(chg, "Unknown APSD %d; forcing 500mA\n", apsd_result->pst);
				rc = smblib_set_icl_current(chg, SDP_CURRENT_UA);
				if (rc < 0)
				{
					smblib_err(chg, "Couldn't set ICL options rc=%d\n", rc);
				}

				break;
			}
			/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 start */
			pr_info("line=%d, APSD_STATUS=0x%02x, pst=%d, bit=%d, real_charger_type=%d, typec_mode=%d\n",
					__LINE__, stat, apsd_result->pst, apsd_result->bit, chg->real_charger_type, chg->typec_mode);
			/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 end */

			//smblib_handle_apsd_done(chg, (bool)(stat & APSD_DTC_STATUS_DONE_BIT));
			pr_info("line=%d, end hq_rerun_apsd_work\n", __LINE__);
	}
	else
	{
		smblib_err(chg, "chg is null\n");
	}
}
/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 end */

/* HS60 add for HS60-811 Set float charger by gaochao at 2019/08/27 start */
static void hq_float_charger_detect_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
		struct smb_charger, float_charger_detect_work.work);

	int rc = 0;
	int settled_icl = 0;

	if (chg)
	{
		rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_icl);
		if (rc < 0)
		{
			smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
			return;
		}

		/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 start */
		pr_info("line=%d, settled_icl=%d, real_charger_type=%d, typec_mode=%d\n",
				__LINE__, settled_icl, chg->real_charger_type, chg->typec_mode);
		/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 end */

		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 start */
		/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 start */
		//if ((settled_icl <= SDP_100_MA) && (chg->real_charger_type == POWER_SUPPLY_TYPE_USB || chg->real_charger_type == POWER_SUPPLY_TYPE_UNKNOWN))
		if ((settled_icl <= SDP_100_MA) && (chg->real_charger_type == POWER_SUPPLY_TYPE_USB
			|| chg->real_charger_type == POWER_SUPPLY_TYPE_UNKNOWN || chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT))
		/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 end */
		{
			/*
			rc = smblib_set_icl_current(chg, SDP_CURRENT_UA);
			if (rc < 0)
			{
				smblib_err(chg, "Couldn't set ICL options rc=%d\n", rc);
				return;
			}
			*/
			rc = smblib_rerun_apsd_if_required(chg);
			if (rc < 0)
			{
				smblib_err(chg, "smblib_rerun_apsd_if_required error rc=%d\n", rc);
			}

			cancel_delayed_work_sync(&chg->rerun_apsd_work);
			schedule_delayed_work(&chg->rerun_apsd_work, msecs_to_jiffies(RERUN_APSD_DETECT_TIME));

			pr_info("line=%d, end float_charger_detect_work, start rerun_apsd_work after %d ms\n",
					__LINE__, RERUN_APSD_DETECT_TIME);
			/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 end */
		}
	}
	else
	{
		smblib_err(chg, "chg is null\n");
	}
}
/* HS60 add for HS60-811 Set float charger by gaochao at 2019/08/27 end */

static int smblib_notifier_call(struct notifier_block *nb,
		unsigned long ev, void *v)
{
	struct power_supply *psy = v;
	struct smb_charger *chg = container_of(nb, struct smb_charger, nb);

	if (!strcmp(psy->desc->name, "bms")) {
		if (!chg->bms_psy)
			chg->bms_psy = psy;
		if (ev == PSY_EVENT_PROP_CHANGED)
			schedule_work(&chg->bms_update_work);
	}

	if (!chg->jeita_configured)
		schedule_work(&chg->jeita_update_work);

	if (!chg->pl.psy && !strcmp(psy->desc->name, "parallel")) {
		chg->pl.psy = psy;
		schedule_work(&chg->pl_update_work);
	}

	return NOTIFY_OK;
}

static int smblib_register_notifier(struct smb_charger *chg)
{
	int rc;

	chg->nb.notifier_call = smblib_notifier_call;
	rc = power_supply_reg_notifier(&chg->nb);
	if (rc < 0) {
		smblib_err(chg, "Couldn't register psy notifier rc = %d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_mapping_soc_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u)
		return -EINVAL;

	*val_raw = val_u << 1;

	return 0;
}

int smblib_mapping_cc_delta_to_field_value(struct smb_chg_param *param,
					   u8 val_raw)
{
	int val_u  = val_raw * param->step_u + param->min_u;

	if (val_u > param->max_u)
		val_u -= param->max_u * 2;

	return val_u;
}

int smblib_mapping_cc_delta_from_field_value(struct smb_chg_param *param,
					     int val_u, u8 *val_raw)
{
	if (val_u > param->max_u || val_u < param->min_u - param->max_u)
		return -EINVAL;

	val_u += param->max_u * 2 - param->min_u;
	val_u %= param->max_u * 2;
	*val_raw = val_u / param->step_u;

	return 0;
}

static void smblib_uusb_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	cancel_delayed_work_sync(&chg->pl_enable_work);

	if (chg->wa_flags & CHG_TERMINATION_WA)
		alarm_cancel(&chg->chg_termination_alarm);

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* reset both usbin current and voltage votes */
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
			is_flash_active(chg) ? SDP_CURRENT_UA : SDP_100_MA);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

	/* reset USBOV votes and cancel work */
	cancel_delayed_work_sync(&chg->usbov_dbc_work);
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	chg->dbc_usbov = false;

	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;
	chg->usb_icl_delta_ua = 0;
	chg->pulse_cnt = 0;
	chg->uusb_apsd_rerun_done = false;

	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);

	/* clear USB ICL vote for USB_PSY_VOTER */
	rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't un-vote for USB ICL rc=%d\n", rc);

	/* clear USB ICL vote for DCP_VOTER */
	rc = vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	if (rc < 0)
		smblib_err(chg,
			"Couldn't un-vote DCP from USB ICL rc=%d\n", rc);
}

void smblib_suspend_on_debug_battery(struct smb_charger *chg)
{
	int rc;
	union power_supply_propval val;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_DEBUG_BATTERY, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get debug battery prop rc=%d\n", rc);
		return;
	}
	if (chg->suspend_input_on_debug_batt) {
		vote(chg->usb_icl_votable, DEBUG_BOARD_VOTER, val.intval, 0);
		vote(chg->dc_suspend_votable, DEBUG_BOARD_VOTER, val.intval, 0);
		if (val.intval)
			pr_info("Input suspended: Fake battery\n");
	} else {
		vote(chg->chg_disable_votable, DEBUG_BOARD_VOTER,
					val.intval, 0);
	}
}

int smblib_rerun_apsd_if_required(struct smb_charger *chg)
{
	union power_supply_propval val;
	int rc;

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
		return rc;
	}

	if (!val.intval)
		return 0;

	rc = smblib_request_dpdm(chg, true);
	if (rc < 0)
		smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

	chg->uusb_apsd_rerun_done = true;
	smblib_rerun_apsd(chg);

	return 0;
}

static int smblib_get_pulse_cnt(struct smb_charger *chg, int *count)
{
	*count = chg->pulse_cnt;
	return 0;
}

#define USBIN_25MA	25000
#define USBIN_100MA	100000
#define USBIN_150MA	150000
#define USBIN_500MA	500000
#define USBIN_900MA	900000
static int set_sdp_current(struct smb_charger *chg, int icl_ua)
{
	int rc;
	u8 icl_options;
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);

	/* power source is SDP */
	switch (icl_ua) {
	case USBIN_100MA:
		/* USB 2.0 100mA */
		icl_options = 0;
		break;
	case USBIN_150MA:
		/* USB 3.0 150mA */
		icl_options = CFG_USB3P0_SEL_BIT;
		break;
	case USBIN_500MA:
		/* USB 2.0 500mA */
		icl_options = USB51_MODE_BIT;
		break;
	case USBIN_900MA:
		/* USB 3.0 900mA */
		icl_options = CFG_USB3P0_SEL_BIT | USB51_MODE_BIT;
		break;
	default:
		smblib_err(chg, "ICL %duA isn't supported for SDP\n", icl_ua);
		return -EINVAL;
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB &&
		apsd_result->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		/*
		 * change the float charger configuration to SDP, if this
		 * is the case of SDP being detected as FLOAT
		 */
		rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
			FORCE_FLOAT_SDP_CFG_BIT, FORCE_FLOAT_SDP_CFG_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set float ICL options rc=%d\n",
						rc);
			return rc;
		}
	}

	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		CFG_USB3P0_SEL_BIT | USB51_MODE_BIT, icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL options rc=%d\n", rc);
		return rc;
	}

	return rc;
}

static int get_sdp_current(struct smb_charger *chg, int *icl_ua)
{
	int rc;
	u8 icl_options;
	bool usb3 = false;

	rc = smblib_read(chg, USBIN_ICL_OPTIONS_REG, &icl_options);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL options rc=%d\n", rc);
		return rc;
	}

	usb3 = (icl_options & CFG_USB3P0_SEL_BIT);

	if (icl_options & USB51_MODE_BIT)
		*icl_ua = usb3 ? USBIN_900MA : USBIN_500MA;
	else
		*icl_ua = usb3 ? USBIN_150MA : USBIN_100MA;

	return rc;
}

int smblib_set_icl_current(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;
	bool hc_mode = false, override = false;

	/* suspend and return if 25mA or less is requested */
	if (icl_ua <= USBIN_25MA)
		return smblib_set_usb_suspend(chg, true);

	if (icl_ua == INT_MAX)
		goto set_mode;

	/* configure current */
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB
		&& (chg->typec_legacy
		|| chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		|| chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)) {
		rc = set_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set SDP ICL rc=%d\n", rc);
			goto out;
		}
	} else {
		set_sdp_current(chg, 100000);
		rc = smblib_set_charge_param(chg, &chg->param.usb_icl, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
			goto out;
		}
		hc_mode = true;

		/*
		 * Micro USB mode follows ICL register independent of override
		 * bit, configure override only for typeC mode.
		 */
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 start */
		//if (chg->connector_type == POWER_SUPPLY_CONNECTOR_TYPEC)
		if (chg->connector_type == POWER_SUPPLY_CONNECTOR_TYPEC || chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 end */
			override = true;
	}

set_mode:
	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		USBIN_MODE_CHG_BIT, hc_mode ? USBIN_MODE_CHG_BIT : 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set USBIN_ICL_OPTIONS rc=%d\n", rc);
		goto out;
	}

	rc = smblib_icl_override(chg, override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto out;
	}

	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto out;
	}

	/* Re-run AICL */
	if (chg->real_charger_type != POWER_SUPPLY_TYPE_USB)
		rc = smblib_run_aicl(chg, RERUN_AICL);
out:
	return rc;
}

int smblib_get_icl_current(struct smb_charger *chg, int *icl_ua)
{
	int rc = 0;
	u8 load_cfg;
	bool override;

	if ((chg->typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT
		|| chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		&& (chg->usb_psy->desc->type == POWER_SUPPLY_TYPE_USB)) {
		rc = get_sdp_current(chg, icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get SDP ICL rc=%d\n", rc);
			return rc;
		}
	} else {
		rc = smblib_read(chg, USBIN_LOAD_CFG_REG, &load_cfg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get load cfg rc=%d\n", rc);
			return rc;
		}
		override = load_cfg & ICL_OVERRIDE_AFTER_APSD_BIT;
		if (!override)
			return INT_MAX;

		/* override is set */
		rc = smblib_get_charge_param(chg, &chg->param.icl_max_stat,
					icl_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get HC ICL rc=%d\n", rc);
			return rc;
		}
	}

	return 0;
}

/********************
 * Moisture Protection *
 ********************/
#define MICRO_USB_DETECTION_ON_TIME_20_MS 0x08
#define MICRO_USB_DETECTION_PERIOD_X_100 0x03
#define U_USB_STATUS_WATER_PRESENT 0x00
static int smblib_set_moisture_protection(struct smb_charger *chg,
				bool enable)
{
	int rc = 0;

	if (chg->moisture_present == enable) {
		smblib_dbg(chg, PR_MISC, "No change in moisture protection status\n");
		return rc;
	}

	if (enable) {
		chg->moisture_present = true;

		/* Disable uUSB factory mode detection */
		rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable uUSB factory mode detection rc=%d\n",
				rc);
			return rc;
		}

		/* Disable moisture detection and uUSB state change interrupt */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable moisture detection interrupt rc=%d\n",
			rc);
			return rc;
		}

		/* Set 1% duty cycle on ID detection */
		rc = smblib_masked_write(chg,
					TYPEC_U_USB_WATER_PROTECTION_CFG_REG,
					EN_MICRO_USB_WATER_PROTECTION_BIT |
					MICRO_USB_DETECTION_ON_TIME_CFG_MASK |
					MICRO_USB_DETECTION_PERIOD_CFG_MASK,
					EN_MICRO_USB_WATER_PROTECTION_BIT |
					MICRO_USB_DETECTION_ON_TIME_20_MS |
					MICRO_USB_DETECTION_PERIOD_X_100);
		if (rc < 0) {
			smblib_err(chg, "Couldn't set 1 percent CC_ID duty cycle rc=%d\n",
				rc);
			return rc;
		}

		vote(chg->usb_icl_votable, MOISTURE_VOTER, true, 0);
	} else {
		chg->moisture_present = false;
		vote(chg->usb_icl_votable, MOISTURE_VOTER, false, 0);

		/* Enable moisture detection and uUSB state change interrupt */
		rc = smblib_masked_write(chg, TYPE_C_INTERRUPT_EN_CFG_2_REG,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT,
					TYPEC_WATER_DETECTION_INT_EN_BIT |
					MICRO_USB_STATE_CHANGE_INT_EN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable moisture detection and uUSB state change interrupt rc=%d\n",
				rc);
			return rc;
		}

		/* Disable periodic monitoring of CC_ID pin */
		rc = smblib_write(chg, TYPEC_U_USB_WATER_PROTECTION_CFG_REG, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable 1 percent CC_ID duty cycle rc=%d\n",
				rc);
			return rc;
		}

		/* Enable uUSB factory mode detection */
		rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT,
					EN_MICRO_USB_FACTORY_MODE_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable uUSB factory mode detection rc=%d\n",
				rc);
			return rc;
		}
	}

	smblib_dbg(chg, PR_MISC, "Moisture protection %s\n",
			chg->moisture_present ? "enabled" : "disabled");
	return rc;
}

/*********************
 * VOTABLE CALLBACKS *
 *********************/

static int smblib_dc_suspend_vote_callback(struct votable *votable, void *data,
			int suspend, const char *client)
{
	struct smb_charger *chg = data;

	if (chg->smb_version == PMI632_SUBTYPE)
		return 0;

	/* resume input if suspend is invalid */
	if (suspend < 0)
		suspend = 0;

	return smblib_set_dc_suspend(chg, (bool)suspend);
}

static int smblib_awake_vote_callback(struct votable *votable, void *data,
			int awake, const char *client)
{
	struct smb_charger *chg = data;

	if (awake)
		pm_stay_awake(chg->dev);
	else
		pm_relax(chg->dev);

	return 0;
}

static int smblib_chg_disable_vote_callback(struct votable *votable, void *data,
			int chg_disable, const char *client)
{
	struct smb_charger *chg = data;
	int rc;

	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 chg_disable ? 0 : CHARGING_ENABLE_CMD_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't %s charging rc=%d\n",
			chg_disable ? "disable" : "enable", rc);
		return rc;
	}

	return 0;
}

static int smblib_usb_irq_enable_vote_callback(struct votable *votable,
				void *data, int enable, const char *client)
{
	struct smb_charger *chg = data;

	if (!chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq ||
				!chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		return 0;

	if (enable) {
		enable_irq(chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq);
		enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	} else {
		disable_irq_nosync(
			chg->irq_info[INPUT_CURRENT_LIMITING_IRQ].irq);
		disable_irq_nosync(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
	}

	return 0;
}

/*******************
 * VCONN REGULATOR *
 * *****************/

int smblib_vconn_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 stat, orientation;

	smblib_dbg(chg, PR_OTG, "enabling VCONN\n");

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}

	/* VCONN orientation is opposite to that of CC */
	orientation =
		stat & TYPEC_CCOUT_VALUE_BIT ? 0 : VCONN_EN_ORIENTATION_BIT;
	rc = smblib_masked_write(chg, TYPE_C_VCONN_CONTROL_REG,
				VCONN_EN_VALUE_BIT | VCONN_EN_ORIENTATION_BIT,
				VCONN_EN_VALUE_BIT | orientation);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
			rc);
		return rc;
	}

	return 0;
}

int smblib_vconn_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;

	smblib_dbg(chg, PR_OTG, "disabling VCONN\n");
	rc = smblib_masked_write(chg, TYPE_C_VCONN_CONTROL_REG,
				 VCONN_EN_VALUE_BIT, 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable vconn regulator rc=%d\n", rc);

	return 0;
}

int smblib_vconn_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;
	u8 cmd;

	rc = smblib_read(chg, TYPE_C_VCONN_CONTROL_REG, &cmd);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			rc);
		return rc;
	}

	return (cmd & VCONN_EN_VALUE_BIT) ? 1 : 0;
}

/*****************
 * OTG REGULATOR *
 *****************/

int smblib_vbus_regulator_enable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	smblib_dbg(chg, PR_OTG, "enabling OTG\n");
#ifdef CONFIG_USB_NOTIFY_LAYER
	pr_info("smblib_vbus_regulator_enable, enabling OTG\n");
#endif

	rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG, OTG_EN_BIT, OTG_EN_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable OTG rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_vbus_regulator_disable(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc;

	smblib_dbg(chg, PR_OTG, "disabling OTG\n");
#ifdef CONFIG_USB_NOTIFY_LAYER
	pr_info("smblib_vbus_regulator_disable, disabling OTG\n");
#endif

	rc = smblib_masked_write(chg, DCDC_CMD_OTG_REG, OTG_EN_BIT, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable OTG regulator rc=%d\n", rc);
		return rc;
	}

	return 0;
}

int smblib_vbus_regulator_is_enabled(struct regulator_dev *rdev)
{
	struct smb_charger *chg = rdev_get_drvdata(rdev);
	int rc = 0;
	u8 cmd;

	rc = smblib_read(chg, DCDC_CMD_OTG_REG, &cmd);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read CMD_OTG rc=%d", rc);
		return rc;
	}

	return (cmd & OTG_EN_BIT) ? 1 : 0;
}

/********************
 * BATT PSY GETTERS *
 ********************/

int smblib_get_prop_input_suspend(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	val->intval
		= (get_client_vote(chg->usb_icl_votable, USER_VOTER) == 0)
		 && get_client_vote(chg->dc_suspend_votable, USER_VOTER);
	return 0;
}

int smblib_get_prop_batt_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATIF_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATIF_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = !(stat & (BAT_THERM_OR_ID_MISSING_RT_STS_BIT
					| BAT_TERMINAL_MISSING_RT_STS_BIT));

	return rc;
}

/* HS60 add for SR-ZQL1695-01000000466 Provide sysFS node named /sys/class/power_supply/battery/online by gaochao at 2019/08/08 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
extern int g_sec_battery_cable_timeout;

void smblib_get_prop_batt_online_samsung(struct smb_charger *chg,
				union power_supply_propval *val)
{
	if (chg) {
		/* HS60 add for SR-ZQL1695-01000000466 Provide sysFS node named xxx/battery/online by gaochao at 2019/08/14 start */
		if (chg->real_charger_type == POWER_SUPPLY_TYPE_UNKNOWN)
			val->intval = POWER_SUPPLY_TYPE_BATTERY;
		else
			val->intval = chg->real_charger_type;

		smblib_dbg(chg, PR_MISC, "[ss]val->intval=%d, chg->real_charger_type=%d, g_sec_battery_cable_timeout=%d\n",
			val->intval, chg->real_charger_type, g_sec_battery_cable_timeout);
		/* HS60 add for SR-ZQL1695-01000000466 Provide sysFS node named xxx/battery/online by gaochao at 2019/08/14 end */
	} else
		smblib_err(chg, "[ss]chg is NULL\n");
}
#endif
/* HS60 add for SR-ZQL1695-01000000466 Provide sysFS node named /sys/class/power_supply/battery/online by gaochao at 2019/08/08 end */

int smblib_get_prop_batt_capacity(struct smb_charger *chg,
				  union power_supply_propval *val)
{
	int rc = -EINVAL;

	if (chg->fake_capacity >= 0) {
		val->intval = chg->fake_capacity;
		return 0;
	}

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CAPACITY, val);

	return rc;
}

/* HS60 add for SR-ZQL1695-01000000455 Provide sysFS node named /sys/class/power_supply/battery/batt_current_event by gaochao at 2019/08/08 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
/*HS60 add for P200214-01131 modify the threshold of current event by wangzikang at 2020/02/14 start*/
/*#define SS_BAT_LOW_TEMP_SWELLING		180
#define SS_BAT_HIGH_TEMP_SWELLING		410*/
#define SS_BAT_LOW_TEMP_SWELLING		120
#define SS_BAT_HIGH_TEMP_SWELLING		450
/*HS60 add for P200214-01131 modify the threshold of current event by wangzikang at 2020/02/14 end*/
extern int g_usb_connected_unconfigured;

void smblib_get_prop_batt_batt_current_event_samsung(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval prop = {0, };
	int battery_temperature = 0;
	int rc = 0;
	
	/* initial state */
	val->intval = SEC_BAT_CURRENT_EVENT_NONE;
	if (chg) {
		/* HS60 add for SR-ZQL1695-01000000455 Provide sysFS node named xxx/battery/batt_current_event by gaochao at 2019/08/14 start */
        	if (chg->real_charger_type != POWER_SUPPLY_TYPE_UNKNOWN) {
			/* obtain battery temperature */
			rc = power_supply_get_property(chg->batt_psy, POWER_SUPPLY_PROP_TEMP, &prop);
			if (rc < 0)
				pr_err("Failed to get battery temperature, rc=%d\n", rc);
			battery_temperature = prop.intval;

			if (battery_temperature <= SS_BAT_LOW_TEMP_SWELLING)
				val->intval |= SEC_BAT_CURRENT_EVENT_LOW_TEMP_SWELLING;
			if (battery_temperature >= SS_BAT_HIGH_TEMP_SWELLING)
				val->intval |= SEC_BAT_CURRENT_EVENT_HIGH_TEMP_SWELLING;
			/* HS60 add for SR-ZQL1695-01-358 Provide sysFS node named xxx/battery/batt_slate_mode by gaochao at 2019/08/29 start */
			if (chg->slate_mode == 1)
				val->intval |= SEC_BAT_CURRENT_EVENT_SLATE;
			/* HS60 add for SR-ZQL1695-01-358 Provide sysFS node named xxx/battery/batt_slate_mode by gaochao at 2019/08/29 end */
			if (g_usb_connected_unconfigured)
				val->intval |= SEC_BAT_CURRENT_EVENT_USB_100MA;
        	}
		/* HS60 add for SR-ZQL1695-01000000455 Provide sysFS node named xxx/battery/batt_current_event by gaochao at 2019/08/14 end */
		smblib_dbg(chg, PR_MISC, "[ss]val->intval=%d, battery_temperature=%d, g_usb_connected_unconfigured=%d, chg->slate_mode=%d\n",
			val->intval, battery_temperature, g_usb_connected_unconfigured, chg->slate_mode);
	}else
		smblib_err(chg, "[ss]chg is NULL\n");
	
}
#endif
/* HS60 add for SR-ZQL1695-01000000455 Provide sysFS node named /sys/class/power_supply/battery/batt_current_event by gaochao at 2019/08/08 end */

/* HS60 add for SR-ZQL1695-01000000460 Provide sysFS node named /sys/class/power_supply/battery/batt_misc_event by gaochao at 2019/08/11 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
void smblib_get_prop_batt_batt_misc_event_samsung(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = 0;
	if (chg) {
		/* BATT_MISC_EVENT_TIMEOUT_OPEN_TYPE */
		if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT || g_sec_battery_cable_timeout)
			val->intval = BATT_MISC_EVENT_TIMEOUT_OPEN_TYPE;
		val->intval  |= chg->battery_health << BATTERY_HEALTH_SHIFT;

		smblib_dbg(chg, PR_MISC, "[ss]val->intval=%d, g_sec_battery_cable_timeout=%d\n",
				val->intval, g_sec_battery_cable_timeout);
	} else
		smblib_err(chg, "[ss]chg is NULL\n");
}
#endif
/* HS60 add for SR-ZQL1695-01000000460 Provide sysFS node named /sys/class/power_supply/battery/batt_misc_event by gaochao at 2019/08/11 end */

/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
#define FLASH_ACTIVE_BATTERY_FULL_LEVEL	100
#endif
/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 end */
int smblib_get_prop_batt_status(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval = {0, };
	bool usb_online, dc_online;
	u8 stat;
	int rc, suspend = 0;

	/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	if (chg->dbc_usbov || chg->flash_active) {
	#else
	if (chg->dbc_usbov) {
	#endif
	/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 end */
		rc = smblib_get_prop_usb_present(chg, &pval);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb present prop rc=%d\n", rc);
			return rc;
		}

		rc = smblib_get_usb_suspend(chg, &suspend);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't get usb suspend rc=%d\n", rc);
			return rc;
		}

		/*
		 * Report charging as long as USBOV is not debounced and
		 * charging path is un-suspended.
		 */
		/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 start */
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		pr_debug("[%s]line=%d, flash_active=%d, dbc_usbov=%d, vbus=%d, suspend=%d, previous_charger_status=%d\n",
				__FUNCTION__, __LINE__, chg->flash_active, chg->dbc_usbov, pval.intval, suspend, chg->previous_charger_status);
		#endif
		/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 end */

		if (pval.intval && !suspend) {
			/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 start */
			#if !defined(HQ_FACTORY_BUILD)	//ss version
			rc = smblib_get_prop_batt_capacity(chg, &pval);
			if (rc < 0)
			{
				smblib_err(chg, "Couldn't get battery level rc=%d\n", rc);
			}

			if ((pval.intval == FLASH_ACTIVE_BATTERY_FULL_LEVEL) && (chg->previous_charger_status == POWER_SUPPLY_STATUS_FULL))
			{
				val->intval = POWER_SUPPLY_STATUS_FULL;
			}
			else
			{
				val->intval = POWER_SUPPLY_STATUS_CHARGING;
			}
			#else
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
			#endif
			/* HS60 add for P191025-06620 Charging popup is coming while camera takes a photo with flash light by gaochao at 2019/11/21 end */
			return 0;
		}
	}

	rc = smblib_get_prop_usb_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb online property rc=%d\n",
			rc);
		return rc;
	}
	usb_online = (bool)pval.intval;

	rc = smblib_get_prop_dc_online(chg, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get dc online property rc=%d\n",
			rc);
		return rc;
	}
	dc_online = (bool)pval.intval;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}
	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (!usb_online && !dc_online) {
		switch (stat) {
		case TERMINATE_CHARGE:
		case INHIBIT_CHARGE:
			val->intval = POWER_SUPPLY_STATUS_FULL;
			break;
		default:
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
			break;
		}
		return rc;
	}

	switch (stat) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
	case FULLON_CHARGE:
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_CHARGING;
		break;
	case TERMINATE_CHARGE:
	case INHIBIT_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_FULL;
		break;
	case DISABLE_CHARGE:
	case PAUSE_CHARGE:
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		val->intval = POWER_SUPPLY_STATUS_UNKNOWN;
		break;
	}

	/*
	 * If charge termination WA is active and has suspended charging, then
	 * continue reporting charging status as FULL.
	 */
	if (is_client_vote_enabled_locked(chg->usb_icl_votable,
						CHG_TERMINATION_VOTER)) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	if (val->intval != POWER_SUPPLY_STATUS_CHARGING)
		return 0;

	if (!usb_online && dc_online
		&& chg->fake_batt_status == POWER_SUPPLY_STATUS_FULL) {
		val->intval = POWER_SUPPLY_STATUS_FULL;
		return 0;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_5_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
			return rc;
	}

	stat &= ENABLE_TRICKLE_BIT | ENABLE_PRE_CHARGING_BIT |
						ENABLE_FULLON_MODE_BIT;

	if (!stat)
		val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;

	return 0;
}

/* HS60 add for SR-ZQL1695-01000000467 Provide sysFS node named xxx/battery/charge_type by gaochao at 2019/08/14 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
#define SLOW_CHARGING_CURRENT_STANDARD 400000
#define SLOW_CHARGING_COUNT  10
/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 start*/
/*HS60 add for P220315-05837 Slow Charging Optimize by gengyifei at 20220329 start*/
#if 0
#define SLOW_CHARGING_COUNT_POWERON  3
#define BOOT_MODE_STR_LEN 7
static char boot_mode_from_cmdline[BOOT_MODE_STR_LEN + 1];
static int __init boot_mode_setup(char *str)
{
	strlcpy(boot_mode_from_cmdline, str,
		ARRAY_SIZE(boot_mode_from_cmdline));

	return 1;
}
__setup("androidboot.mode=", boot_mode_setup);

bool boot_mode_is(char* str)
{
	return !strncmp(boot_mode_from_cmdline, str,
		BOOT_MODE_STR_LEN + 1);
}
#endif
/*HS60 add for P220315-05837 Slow Charging Optimize by gengyifei at 20220329 end*/
/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 end*/
#endif
/* HS60 add for SR-ZQL1695-01000000467 Provide sysFS node named xxx/battery/charge_type by gaochao at 2019/08/14 end */

int smblib_get_prop_batt_charge_type(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;
	/* HS60 add for SR-ZQL1695-01000000467 Provide sysFS node named xxx/battery/charge_type by gaochao at 2019/08/14 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	int settled_icl = 0;
	/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 start*/
	/*HS60 add for P220315-05837 Slow Charging Optimize by gengyifei at 20220329 start*/
	//int slow_charging_count = 0;
	/*HS60 add for P220315-05837 Slow Charging Optimize by gengyifei at 20220329 end*/
	/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 end*/
	#endif
	/* HS60 add for SR-ZQL1695-01000000467 Provide sysFS node named xxx/battery/charge_type by gaochao at 2019/08/14 end */

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	switch (stat & BATTERY_CHARGER_STATUS_MASK) {
	case TRICKLE_CHARGE:
	case PRE_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TRICKLE;
		break;
	case FULLON_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
		break;
	case TAPER_CHARGE:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_TAPER;
		break;
	default:
		val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
	}

	/* HS60 add for SR-ZQL1695-01000000467 Provide sysFS node named xxx/battery/charge_type by gaochao at 2019/08/14 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_icl);
	if (rc < 0)
	{
		smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
		return rc;
	}
	/* HS60 add for P191114-09571  by wangzikang at 2019/11/26 start */
	/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 start*/
	/*HS60 add for P220315-05837 Slow Charging Optimize by gengyifei at 20220329 start*/
	#if 0
	if(boot_mode_is("charger"))
		slow_charging_count = SLOW_CHARGING_COUNT;
	else
		slow_charging_count = SLOW_CHARGING_COUNT_POWERON;
	#endif
	/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 end*/
	/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 start*/
	/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 start*/
	if ((chg->slow_charging_count <= SLOW_CHARGING_COUNT) && (chg->real_charger_type != POWER_SUPPLY_TYPE_UNKNOWN))
	{
		chg->slow_charging_count++;
	}
	else
	{
		chg->slow_charging_count = 0;
	}

	pr_debug("[ss]slow charging off: settled_icl = %d, Type = %d, slow_charging_count = %d, chg->real_charger_type = %d \n",
			settled_icl, val->intval, chg->slow_charging_count, chg->real_charger_type);
	//if (settled_icl < SLOW_CHARGING_CURRENT_STANDARD  && val->intval != POWER_SUPPLY_CHARGE_TYPE_NONE)
	//if (((settled_icl < SLOW_CHARGING_CURRENT_STANDARD) && !chg->flash_active)  && (val->intval != POWER_SUPPLY_CHARGE_TYPE_NONE))
	if (((settled_icl < SLOW_CHARGING_CURRENT_STANDARD) && !chg->flash_active)  && (val->intval != POWER_SUPPLY_CHARGE_TYPE_NONE)
		&& (chg->slow_charging_count > SLOW_CHARGING_COUNT) && (chg->real_charger_type != POWER_SUPPLY_TYPE_UNKNOWN))
	/*HS60 add for P220315-05837 Slow Charging Optimize by gengyifei at 20220329 end*/
	/*HS50 add for P200213-04659 Slow Charging Optimize by wenyaqi at 20210301 end*/
	/* HS60 add for P191114-09571  by wangzikang at 2019/11/26 end */
	{
		chg->slow_charging_count = 0;
		val->intval = POWER_SUPPLY_CHARGE_TYPE_SLOW;
		smblib_dbg(chg, PR_MISC, "[ss]slow charging on: settled_icl = %d, Type = %d, slow_charging_count = %d, chg->real_charger_type = %d \n",
			settled_icl, val->intval, chg->slow_charging_count, chg->real_charger_type);
		/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 end*/
	}
	#endif
	/* HS60 add for SR-ZQL1695-01000000467 Provide sysFS node named xxx/battery/charge_type by gaochao at 2019/08/14 end */

	return rc;
}

int smblib_get_prop_batt_health(struct smb_charger *chg,
				union power_supply_propval *val)
{
	union power_supply_propval pval;
	int rc;
	int effective_fv_uv;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_2_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "BATTERY_CHARGER_STATUS_2 = 0x%02x\n",
		   stat);

	if (stat & CHARGER_ERROR_STATUS_BAT_OV_BIT) {
		rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_VOLTAGE_NOW, &pval);
		if (!rc) {
			/*
			 * If Vbatt is within 40mV above Vfloat, then don't
			 * treat it as overvoltage.
			 */
			effective_fv_uv = get_effective_result(chg->fv_votable);
			if (pval.intval >= effective_fv_uv + 40000) {
				val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
				smblib_err(chg, "battery over-voltage vbat_fg = %duV, fv = %duV\n",
						pval.intval, effective_fv_uv);
				goto done;
			}
		}
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
			rc);
		return rc;
	}
	if (stat & BAT_TEMP_STATUS_TOO_COLD_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COLD;
	else if (stat & BAT_TEMP_STATUS_TOO_HOT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
	else if (stat & BAT_TEMP_STATUS_COLD_SOFT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_COOL;
	else if (stat & BAT_TEMP_STATUS_HOT_SOFT_BIT)
		val->intval = POWER_SUPPLY_HEALTH_WARM;
	else
		val->intval = POWER_SUPPLY_HEALTH_GOOD;

done:
	return rc;
}

int smblib_get_prop_system_temp_level(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->system_temp_level;
	return 0;
}

int smblib_get_prop_system_temp_level_max(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->thermal_levels;
	return 0;
}

int smblib_get_prop_input_current_limited(struct smb_charger *chg,
				union power_supply_propval *val)
{
	u8 stat;
	int rc;

	if (chg->fake_input_current_limited >= 0) {
		val->intval = chg->fake_input_current_limited;
		return 0;
	}

	rc = smblib_read(chg, AICL_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
		return rc;
	}
	val->intval = (stat & SOFT_ILIMIT_BIT) || chg->is_hdc;
	return 0;
}

int smblib_get_prop_batt_charge_done(struct smb_charger *chg,
					union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
			rc);
		return rc;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;
	val->intval = (stat == TERMINATE_CHARGE);
	return 0;
}

/***********************
 * BATTERY PSY SETTERS *
 ***********************/

int smblib_set_prop_input_suspend(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	int rc;

	/* vote 0mA when suspended */
	rc = vote(chg->usb_icl_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s USB rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	rc = vote(chg->dc_suspend_votable, USER_VOTER, (bool)val->intval, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't vote to %s DC rc=%d\n",
			(bool)val->intval ? "suspend" : "resume", rc);
		return rc;
	}

	/*HS60 add for ZQL169XFAC-45 by wangzikang at 2019/11/30 start*/
	pr_err( "Input_Suspend is : %d.\n" , val->intval);
	/*HS60 add for ZQL169XFAC-45 by wangzikang at 2019/11/30 start*/

	power_supply_changed(chg->batt_psy);
	return rc;
}

int smblib_set_prop_batt_capacity(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	chg->fake_capacity = val->intval;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_batt_status(struct smb_charger *chg,
				  const union power_supply_propval *val)
{
	/* Faking battery full */
	if (val->intval == POWER_SUPPLY_STATUS_FULL)
		chg->fake_batt_status = val->intval;
	else
		chg->fake_batt_status = -EINVAL;

	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_system_temp_level(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	if (val->intval < 0)
		return -EINVAL;

	if (chg->thermal_levels <= 0)
		return -EINVAL;

	if (val->intval > chg->thermal_levels)
		return -EINVAL;

	chg->system_temp_level = val->intval;

	if (chg->system_temp_level == chg->thermal_levels)
		return vote(chg->chg_disable_votable,
			THERMAL_DAEMON_VOTER, true, 0);

	vote(chg->chg_disable_votable, THERMAL_DAEMON_VOTER, false, 0);
	if (chg->system_temp_level == 0)
		return vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, false, 0);

	vote(chg->fcc_votable, THERMAL_DAEMON_VOTER, true,
			chg->thermal_mitigation[chg->system_temp_level]);
	return 0;
}

int smblib_set_prop_input_current_limited(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	chg->fake_input_current_limited = val->intval;
	return 0;
}

int smblib_run_aicl(struct smb_charger *chg, int type)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
								rc);
		return rc;
	}

	/* USB is suspended so skip re-running AICL */
	if (stat & USBIN_SUSPEND_STS_BIT)
		return rc;

	smblib_dbg(chg, PR_MISC, "re-running AICL\n");

	stat = (type == RERUN_AICL) ? RERUN_AICL_BIT : RESTART_AICL_BIT;
	rc = smblib_masked_write(chg, AICL_CMD_REG, stat, stat);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to AICL_CMD_REG rc=%d\n",
				rc);
	return 0;
}

static int smblib_dp_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 increment */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_INCREMENT_BIT,
			SINGLE_INCREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

static int smblib_dm_pulse(struct smb_charger *chg)
{
	int rc;

	/* QC 3.0 decrement */
	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, SINGLE_DECREMENT_BIT,
			SINGLE_DECREMENT_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

int smblib_force_vbus_voltage(struct smb_charger *chg, u8 val)
{
	int rc;

	rc = smblib_masked_write(chg, CMD_HVDCP_2_REG, val, val);
	if (rc < 0)
		smblib_err(chg, "Couldn't write to CMD_HVDCP_2_REG rc=%d\n",
				rc);

	return rc;
}

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
static void smblib_hvdcp_set_fsw(struct smb_charger *chg, int bit)
{
	switch (bit) {
	case QC_5V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_5V);
		break;
	case QC_9V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_9V);
		break;
	case QC_12V_BIT:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_12V);
		break;
	default:
		smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_removal);
		break;
	}
}
#endif
#endif

int smblib_dp_dm(struct smb_charger *chg, int val)
{
	int target_icl_ua, rc = 0;
	union power_supply_propval pval;

	switch (val) {
	case POWER_SUPPLY_DP_DM_DP_PULSE:
		rc = smblib_dp_pulse(chg);
		if (!rc)
			chg->pulse_cnt++;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DP_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_DM_PULSE:
		rc = smblib_dm_pulse(chg);
		if (!rc && chg->pulse_cnt)
			chg->pulse_cnt--;
		smblib_dbg(chg, PR_PARALLEL, "DP_DM_DM_PULSE rc=%d cnt=%d\n",
				rc, chg->pulse_cnt);
		break;
	case POWER_SUPPLY_DP_DM_ICL_DOWN:
		target_icl_ua = get_effective_result(chg->usb_icl_votable);
		if (target_icl_ua < 0) {
			/* no client vote, get the ICL from charger */
			rc = power_supply_get_property(chg->usb_psy,
					POWER_SUPPLY_PROP_HW_CURRENT_MAX,
					&pval);
			if (rc < 0) {
				smblib_err(chg, "Couldn't get max curr rc=%d\n",
					rc);
				return rc;
			}
			target_icl_ua = pval.intval;
		}

		/*
		 * Check if any other voter voted on USB_ICL in case of
		 * voter other than SW_QC3_VOTER reset and restart reduction
		 * again.
		 */
		if (target_icl_ua != get_client_vote(chg->usb_icl_votable,
							SW_QC3_VOTER))
			chg->usb_icl_delta_ua = 0;

		chg->usb_icl_delta_ua += 100000;
		vote(chg->usb_icl_votable, SW_QC3_VOTER, true,
						target_icl_ua - 100000);
		smblib_dbg(chg, PR_PARALLEL, "ICL DOWN ICL=%d reduction=%d\n",
				target_icl_ua, chg->usb_icl_delta_ua);
		break;
	case POWER_SUPPLY_DP_DM_FORCE_5V:
		rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
		if (rc < 0)
			pr_err("Failed to force 5V\n");
		break;
	case POWER_SUPPLY_DP_DM_FORCE_9V:
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		#if defined(CONFIG_AFC)
		if (chg->hv_disable) {
			pr_err("%s: DP_DM_FORCE_9V but HV is DISABLE\n", __func__);
			return -EINVAL;
		}
		#endif
		#endif
		/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 start*/
		#ifdef CONFIG_ARCH_MSM8953
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
				smblib_err(chg, "Couldn't set 9V: unsupported\n");
				return -EINVAL;
		}
		#endif
		/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 end*/
		/* HS50 add for HS50-4045 resolve protocol conflictation between afc and qc by wenyaqi at 2020/11/02 start */
		#if defined(CONFIG_AFC)
		if(chg->afc_sts == AFC_FAIL)
			rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
		else
		#endif
		/* HS50 add for HS50-4045 resolve protocol conflictation between afc and qc by wenyaqi at 2020/11/02 end */
			rc = smblib_force_vbus_voltage(chg, FORCE_9V_BIT);
		if (rc < 0)
			pr_err("Failed to force 9V\n");
#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)	
		schedule_delayed_work(&chg->compliant_check_work,
			msecs_to_jiffies(2000));
#endif
#endif
		break;
	case POWER_SUPPLY_DP_DM_FORCE_12V:
		/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 start*/
		#ifdef CONFIG_ARCH_MSM8953
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_12V) {
				smblib_err(chg, "Couldn't set 12V: unsupported\n");
				return -EINVAL;
		}
		#endif
		/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 end*/
		rc = smblib_force_vbus_voltage(chg, FORCE_12V_BIT);
		if (rc < 0)
			pr_err("Failed to force 12V\n");
		break;
	case POWER_SUPPLY_DP_DM_ICL_UP:
	default:
		break;
	}

	return rc;
}

int smblib_disable_hw_jeita(struct smb_charger *chg, bool disable)
{
	int rc;
	u8 mask;

	/*
	 * Disable h/w base JEITA compensation if s/w JEITA is enabled
	 */
	mask = JEITA_EN_COLD_SL_FCV_BIT
		| JEITA_EN_HOT_SL_FCV_BIT
		| JEITA_EN_HOT_SL_CCC_BIT
		| JEITA_EN_COLD_SL_CCC_BIT,
	rc = smblib_masked_write(chg, JEITA_EN_CFG_REG, mask,
			disable ? 0 : mask);
	if (rc < 0) {
		dev_err(chg->dev, "Couldn't configure s/w jeita rc=%d\n",
				rc);
		return rc;
	}
	return 0;
}

int smblib_configure_wdog(struct smb_charger *chg, bool enable)
{
	int rc;
	u8 val = 0;

	if (enable)
		val = WDOG_TIMER_EN_ON_PLUGIN_BIT | BARK_WDOG_INT_EN_BIT;

	/* enable WD BARK and enable it on plugin */
	rc = smblib_masked_write(chg, WD_CFG_REG,
				WATCHDOG_TRIGGER_AFP_EN_BIT |
				WDOG_TIMER_EN_ON_PLUGIN_BIT |
				BARK_WDOG_INT_EN_BIT, val);
	if (rc < 0) {
		pr_err("Couldn't configue WD config rc=%d\n", rc);
		return rc;
	}

	return 0;
}

/*******************
 * DC PSY GETTERS *
 *******************/

int smblib_get_prop_dc_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, DCIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read DCIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & DCIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_dc_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (get_client_vote(chg->dc_suspend_votable, USER_VOTER)) {
		val->intval = false;
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_DCIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);

	return rc;
}

/*******************
 * USB PSY GETTERS *
 *******************/

int smblib_get_prop_usb_present(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USBIN_RT_STS rc=%d\n", rc);
		return rc;
	}

	val->intval = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	return 0;
}

int smblib_get_prop_usb_online(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	if (get_client_vote_locked(chg->usb_icl_votable, USER_VOTER) == 0) {
		val->intval = false;
		return rc;
	}

	/* HS50 add for HS50-1898 Import qcom patch to resolve deadlock by wenyaqi at 2020/9/27 start */
	if (is_client_vote_enabled_locked(chg->usb_icl_votable,
					CHG_TERMINATION_VOTER)) {
	/* HS50 add for HS50-1898 Import qcom patch to resolve deadlock by wenyaqi at 2020/9/27 end */
		rc = smblib_get_prop_usb_present(chg, val);
		return rc;
	}

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "POWER_PATH_STATUS = 0x%02x\n",
		   stat);

	val->intval = (stat & USE_USBIN_BIT) &&
		      (stat & VALID_INPUT_POWER_SOURCE_STS_BIT);
	return rc;
}

int smblib_get_prop_usb_voltage_max(struct smb_charger *chg,
				    union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
		/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 start*/
		#ifdef CONFIG_ARCH_MSM8953
		if (chg->qc2_unsupported_voltage == QC2_NON_COMPLIANT_9V) {
				val->intval = MICRO_5V;
				break;
		} else if (chg->qc2_unsupported_voltage ==
						QC2_NON_COMPLIANT_12V) {
				val->intval = MICRO_9V;
				break;
		}
		/* else, fallthrough */
		#endif
		/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 end*/
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		if (chg->smb_version == PMI632_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_max_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_usb_voltage_max_design(struct smb_charger *chg,
					union power_supply_propval *val)
{
	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP:
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
	case POWER_SUPPLY_TYPE_USB_PD:
		if (chg->smb_version == PMI632_SUBTYPE)
			val->intval = MICRO_9V;
		else
			val->intval = MICRO_12V;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_typec_cc_orientation(struct smb_charger *chg,
					 union power_supply_propval *val)
{
	int rc = 0;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_4 = 0x%02x\n", stat);

	if (stat & CC_ATTACHED_BIT)
		val->intval = (bool)(stat & CC_ORIENTATION_BIT) + 1;
	else
		val->intval = 0;

	return rc;
}

static const char * const smblib_typec_mode_name[] = {
	[POWER_SUPPLY_TYPEC_NONE]		  = "NONE",
	[POWER_SUPPLY_TYPEC_SOURCE_DEFAULT]	  = "SOURCE_DEFAULT",
	[POWER_SUPPLY_TYPEC_SOURCE_MEDIUM]	  = "SOURCE_MEDIUM",
	[POWER_SUPPLY_TYPEC_SOURCE_HIGH]	  = "SOURCE_HIGH",
	[POWER_SUPPLY_TYPEC_NON_COMPLIANT]	  = "NON_COMPLIANT",
	[POWER_SUPPLY_TYPEC_SINK]		  = "SINK",
	[POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE]   = "SINK_POWERED_CABLE",
	[POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY] = "SINK_DEBUG_ACCESSORY",
	[POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER]   = "SINK_AUDIO_ADAPTER",
	[POWER_SUPPLY_TYPEC_POWERED_CABLE_ONLY]   = "POWERED_CABLE_ONLY",
};

static int smblib_get_prop_ufp_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_SNK_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_1 rc=%d\n", rc);
		return POWER_SUPPLY_TYPEC_NONE;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_STATUS_1 = 0x%02x\n", stat);

	switch (stat & DETECTED_SRC_TYPE_MASK) {
	case SNK_RP_STD_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_DEFAULT;
	case SNK_RP_1P5_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_MEDIUM;
	case SNK_RP_3P0_BIT:
		return POWER_SUPPLY_TYPEC_SOURCE_HIGH;
	case SNK_RP_SHORT_BIT:
		return POWER_SUPPLY_TYPEC_NON_COMPLIANT;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_dfp_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_SRC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_SRC_STATUS_REG rc=%d\n",
				rc);
		return POWER_SUPPLY_TYPEC_NONE;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_SRC_STATUS_REG = 0x%02x\n", stat);

	switch (stat & DETECTED_SNK_TYPE_MASK) {
	case AUDIO_ACCESS_RA_RA_BIT:
		return POWER_SUPPLY_TYPEC_SINK_AUDIO_ADAPTER;
	case SRC_DEBUG_ACCESS_BIT:
		return POWER_SUPPLY_TYPEC_SINK_DEBUG_ACCESSORY;
	case SRC_RD_RA_VCONN_BIT:
		return POWER_SUPPLY_TYPEC_SINK_POWERED_CABLE;
	case SRC_RD_OPEN_BIT:
		return POWER_SUPPLY_TYPEC_SINK;
	default:
		break;
	}

	return POWER_SUPPLY_TYPEC_NONE;
}

static int smblib_get_prop_typec_mode(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
				rc);
		return 0;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_MISC_STATUS_REG = 0x%02x\n", stat);

	if (stat & SNK_SRC_MODE_BIT)
		return smblib_get_prop_dfp_mode(chg);
	else
		return smblib_get_prop_ufp_mode(chg);
}

int smblib_get_prop_typec_power_role(struct smb_charger *chg,
				     union power_supply_propval *val)
{
	int rc = 0;
	u8 ctrl;

	rc = smblib_read(chg, TYPE_C_MODE_CFG_REG, &ctrl);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_MODE_CFG_REG rc=%d\n",
			rc);
		return rc;
	}
	smblib_dbg(chg, PR_REGISTER, "TYPE_C_MODE_CFG_REG = 0x%02x\n",
		   ctrl);

	if (ctrl & TYPEC_DISABLE_CMD_BIT) {
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		return rc;
	}

	switch (ctrl & (EN_SRC_ONLY_BIT | EN_SNK_ONLY_BIT)) {
	case 0:
		val->intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		break;
	case EN_SRC_ONLY_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
		break;
	case EN_SNK_ONLY_BIT:
		val->intval = POWER_SUPPLY_TYPEC_PR_SINK;
		break;
	default:
		val->intval = POWER_SUPPLY_TYPEC_PR_NONE;
		smblib_err(chg, "unsupported power role 0x%02lx\n",
			ctrl & (EN_SRC_ONLY_BIT | EN_SNK_ONLY_BIT));
		return -EINVAL;
	}

	return rc;
}

int smblib_get_prop_input_current_settled(struct smb_charger *chg,
					  union power_supply_propval *val)
{
	return smblib_get_charge_param(chg, &chg->param.icl_stat, &val->intval);
}

#define HVDCP3_STEP_UV	200000
int smblib_get_prop_input_voltage_settled(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc, pulses;

	switch (chg->real_charger_type) {
	case POWER_SUPPLY_TYPE_USB_HVDCP_3:
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return 0;
		}
		val->intval = MICRO_5V + HVDCP3_STEP_UV * pulses;
		break;
	case POWER_SUPPLY_TYPE_USB_PD:
		val->intval = chg->voltage_min_uv;
		break;
	default:
		val->intval = MICRO_5V;
		break;
	}

	return 0;
}

int smblib_get_prop_pd_in_hard_reset(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->pd_hard_reset;
	return 0;
}

int smblib_get_pe_start(struct smb_charger *chg,
			       union power_supply_propval *val)
{
	val->intval = chg->ok_to_pd;
	return 0;
}

int smblib_get_prop_die_health(struct smb_charger *chg,
						union power_supply_propval *val)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, MISC_TEMP_RANGE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TEMP_RANGE_STATUS_REG rc=%d\n",
									rc);
		return rc;
	}

	/* TEMP_RANGE bits are mutually exclusive */
	switch (stat & TEMP_RANGE_MASK) {
	case TEMP_BELOW_RANGE_BIT:
		val->intval = POWER_SUPPLY_HEALTH_COOL;
		break;
	case TEMP_WITHIN_RANGE_BIT:
		val->intval = POWER_SUPPLY_HEALTH_WARM;
		break;
	case TEMP_ABOVE_RANGE_BIT:
		val->intval = POWER_SUPPLY_HEALTH_HOT;
		break;
	case ALERT_LEVEL_BIT:
		val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		break;
	default:
		val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
	}

	return 0;
}

static int get_rp_based_dcp_current(struct smb_charger *chg, int typec_mode)
{
	int rp_ua;

	switch (typec_mode) {
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		rp_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
	/* fall through */
	default:
		rp_ua = DCP_CURRENT_UA;
	}

	return rp_ua;
}

/*******************
 * USB PSY SETTERS *
 * *****************/

int smblib_set_prop_pd_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc;

	if (chg->pd_active)
		rc = vote(chg->usb_icl_votable, PD_VOTER, true, val->intval);
	else
		rc = -EPERM;

	return rc;
}

static int smblib_handle_usb_current(struct smb_charger *chg,
					int usb_current)
{
	int rc = 0, rp_ua, typec_mode;
	union power_supply_propval val = {0, };

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (usb_current == -ETIMEDOUT) {
			if ((chg->float_cfg & FLOAT_OPTIONS_MASK)
						== FORCE_FLOAT_SDP_CFG_BIT) {
				/*
				 * Confiugure USB500 mode if Float charger is
				 * configured for SDP.
				 */
				rc = set_sdp_current(chg, USBIN_500MA);
				if (rc < 0)
					smblib_err(chg,
						"Couldn't set SDP ICL rc=%d\n",
						rc);

				/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 start */
				rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
				if (rc < 0)
				{
					printk("[%s]line=%d: Couldn't vote USB_PSY_VOTER rc=%d\n", __FUNCTION__, __LINE__, rc);
				}

				rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, SDP_CURRENT_UA);
				if (rc < 0)
				{
					printk("[%s]line=%d: Couldn't vote SW_ICL_MAX_VOTER rc=%d\n", __FUNCTION__, __LINE__, rc);
				}
				/* HS70 add for HS70-565 Set float charger ICL as 500mA by gaochao at 2019/10/31 end */

				return rc;
			}

			if (chg->connector_type ==
					POWER_SUPPLY_CONNECTOR_TYPEC) {
				/*
				 * Valid FLOAT charger, report the current
				 * based of Rp.
				 */
				typec_mode = smblib_get_prop_typec_mode(chg);
				rp_ua = get_rp_based_dcp_current(chg,
								typec_mode);
				rc = vote(chg->usb_icl_votable,
					SW_ICL_MAX_VOTER, true, rp_ua);
				if (rc < 0)
					return rc;
			} else {
				rc = vote(chg->usb_icl_votable,
					SW_ICL_MAX_VOTER, true, DCP_CURRENT_UA);
				if (rc < 0)
					return rc;
			}
		} else {
			/*
			 * FLOAT charger detected as SDP by USB driver,
			 * charge with the requested current and update the
			 * real_charger_type
			 */
			chg->real_charger_type = POWER_SUPPLY_TYPE_USB;
			rc = vote(chg->usb_icl_votable, USB_PSY_VOTER,
						true, usb_current);
			if (rc < 0)
				return rc;
			rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER,
							false, 0);
			if (rc < 0)
				return rc;
		}
	} else {
		rc = smblib_get_prop_usb_present(chg, &val);
		if (!rc && !val.intval)
			return 0;

		/* if flash is active force 500mA */
		if ((usb_current < SDP_CURRENT_UA) && is_flash_active(chg))
			usb_current = SDP_CURRENT_UA;

		rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, true,
							usb_current);
		if (rc < 0) {
			pr_err("Couldn't vote ICL USB_PSY_VOTER rc=%d\n", rc);
			return rc;
		}

		rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		if (rc < 0) {
			pr_err("Couldn't remove SW_ICL_MAX vote rc=%d\n", rc);
			return rc;
		}

	}

	return 0;
}

int smblib_set_prop_sdp_current_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc = 0;

	if (!chg->pd_active) {
		rc = smblib_handle_usb_current(chg, val->intval);
	} else if (chg->system_suspend_supported) {
		if (val->intval <= USBIN_25MA)
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, true, val->intval);
		else
			rc = vote(chg->usb_icl_votable,
				PD_SUSPEND_SUPPORTED_VOTER, false, 0);
	}
	return rc;
}

int smblib_set_prop_boost_current(struct smb_charger *chg,
					const union power_supply_propval *val)
{
	int rc = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
				val->intval <= chg->boost_threshold_ua ?
				chg->chg_freq.freq_below_otg_threshold :
				chg->chg_freq.freq_above_otg_threshold);
	if (rc < 0) {
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);
		return rc;
	}

	chg->boost_current_ua = val->intval;
	return rc;
}

int smblib_set_prop_typec_power_role(struct smb_charger *chg,
				     const union power_supply_propval *val)
{
	int rc = 0;
	u8 power_role;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		return 0;
	smblib_err(chg, "set power role %d\n", val->intval);

	switch (val->intval) {
	case POWER_SUPPLY_TYPEC_PR_NONE:
		power_role = TYPEC_DISABLE_CMD_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_DUAL:
		power_role = 0;
		break;
	case POWER_SUPPLY_TYPEC_PR_SINK:
		power_role = EN_SNK_ONLY_BIT;
		break;
	case POWER_SUPPLY_TYPEC_PR_SOURCE:
		power_role = EN_SRC_ONLY_BIT;
		break;
	default:
		smblib_err(chg, "power role %d not supported\n", val->intval);
		return -EINVAL;
	}

	rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				TYPEC_POWER_ROLE_CMD_MASK | TYPEC_TRY_MODE_MASK,
				power_role);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write 0x%02x to TYPE_C_INTRPT_ENB_SOFTWARE_CTRL rc=%d\n",
			power_role, rc);
		return rc;
	}

	return rc;
}

int smblib_set_prop_pd_voltage_min(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, min_uv;

	min_uv = min(val->intval, chg->voltage_max_uv);
	rc = smblib_set_usb_pd_allowed_voltage(chg, min_uv,
					       chg->voltage_max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid max voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_min_uv = min_uv;
	power_supply_changed(chg->usb_main_psy);

	return rc;
}

int smblib_set_prop_pd_voltage_max(struct smb_charger *chg,
				    const union power_supply_propval *val)
{
	int rc, max_uv;

	max_uv = max(val->intval, chg->voltage_min_uv);
	rc = smblib_set_usb_pd_allowed_voltage(chg, chg->voltage_min_uv,
					       max_uv);
	if (rc < 0) {
		smblib_err(chg, "invalid min voltage %duV rc=%d\n",
			val->intval, rc);
		return rc;
	}

	chg->voltage_max_uv = max_uv;
	power_supply_changed(chg->usb_main_psy);

	return rc;
}

int smblib_set_prop_pd_active(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc = 0;

	chg->pd_active = val->intval;

	if (chg->pd_active) {
		vote(chg->usb_irq_enable_votable, PD_VOTER, true, 0);

		/*
		 * Enforce 500mA for PD until the real vote comes in later.
		 * It is guaranteed that pd_active is set prior to
		 * pd_current_max
		 */
		vote(chg->usb_icl_votable, PD_VOTER, true, USBIN_500MA);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
	} else {
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, SDP_100_MA);
		vote(chg->usb_icl_votable, PD_VOTER, false, 0);
		vote(chg->usb_irq_enable_votable, PD_VOTER, false, 0);

		/* PD hard resets failed, rerun apsd */
		if (chg->ok_to_pd) {
			chg->ok_to_pd = false;
			rc = smblib_configure_hvdcp_apsd(chg, true);
			if (rc < 0) {
				dev_err(chg->dev,
					"Couldn't enable APSD rc=%d\n", rc);
				return rc;
			}
			smblib_rerun_apsd_if_required(chg);
		}
	}

	smblib_update_usb_type(chg);
	power_supply_changed(chg->usb_psy);
	return rc;
}

int smblib_set_prop_ship_mode(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;

	smblib_dbg(chg, PR_MISC, "Set ship mode: %d!!\n", !!val->intval);

	rc = smblib_masked_write(chg, SHIP_MODE_REG, SHIP_MODE_EN_BIT,
			!!val->intval ? SHIP_MODE_EN_BIT : 0);
	if (rc < 0)
		dev_err(chg->dev, "Couldn't %s ship mode, rc=%d\n",
				!!val->intval ? "enable" : "disable", rc);

	return rc;
}

int smblib_set_prop_pd_in_hard_reset(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc = 0;

	if (chg->pd_hard_reset == val->intval)
		return rc;

	chg->pd_hard_reset = val->intval;
	rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
			EXIT_SNK_BASED_ON_CC_BIT,
			(chg->pd_hard_reset) ? EXIT_SNK_BASED_ON_CC_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set EXIT_SNK_BASED_ON_CC rc=%d\n",
				rc);

	return rc;
}

/* HS60 add for SR-ZQL1695-01-357 Import battery aging by gaochao at 2019/08/29 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
int smblib_set_prop_rechg_vbat_thresh(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	u32 temp = VBAT_TO_VRAW_ADC(val->intval);

	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg,
		CHGR_ADC_RECHARGE_THRESHOLD_MSB_REG, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg, "Couldn't write to ADC_RECHARGE_THRESHOLD REG rc=%d\n",
				rc);
		return rc;
	}

	chg->auto_recharge_vbat_mv = val->intval;
	smblib_dbg(chg, PR_MISC, "[ss][%s]line=%d: auto_recharge_vbat_mv=%d, val->intval=%d\n",
			__FUNCTION__, __LINE__, chg->auto_recharge_vbat_mv, val->intval);

	return rc;
}
#endif
/* HS60 add for SR-ZQL1695-01-357 Import battery aging by gaochao at 2019/08/29 end */

static int smblib_recover_from_soft_jeita(struct smb_charger *chg)
{
	u8 stat1, stat7;
	int rc;

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat1);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return rc;
	}

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_7_REG, &stat7);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_2 rc=%d\n",
				rc);
		return rc;
	}

	if ((chg->jeita_status && !(stat7 & BAT_TEMP_STATUS_SOFT_LIMIT_MASK) &&
		((stat1 & BATTERY_CHARGER_STATUS_MASK) == TERMINATE_CHARGE))) {
		/*
		 * We are moving from JEITA soft -> Normal and charging
		 * is terminated
		 */
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't disable charging rc=%d\n",
						rc);
			return rc;
		}
		rc = smblib_write(chg, CHARGING_ENABLE_CMD_REG,
						CHARGING_ENABLE_CMD_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't enable charging rc=%d\n",
						rc);
			return rc;
		}
	}

	chg->jeita_status = stat7 & BAT_TEMP_STATUS_SOFT_LIMIT_MASK;

	return 0;
}

/************************
 * USB MAIN PSY GETTERS *
 ************************/
int smblib_get_prop_fcc_delta(struct smb_charger *chg,
				union power_supply_propval *val)
{
	int rc, jeita_cc_delta_ua = 0;

	if (chg->sw_jeita_enabled) {
		val->intval = 0;
		return 0;
	}

	rc = smblib_get_jeita_cc_delta(chg, &jeita_cc_delta_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get jeita cc delta rc=%d\n", rc);
		jeita_cc_delta_ua = 0;
	}

	val->intval = jeita_cc_delta_ua;
	return 0;
}

/************************
 * USB MAIN PSY SETTERS *
 ************************/
int smblib_get_charge_current(struct smb_charger *chg,
				int *total_current_ua)
{
	const struct apsd_result *apsd_result = smblib_get_apsd_result(chg);
	union power_supply_propval val = {0, };
	int rc = 0, typec_source_rd, current_ua;
	bool non_compliant;
	u8 stat;

	if (chg->pd_active) {
		*total_current_ua =
			get_client_vote_locked(chg->usb_icl_votable, PD_VOTER);
		return rc;
	}

	rc = smblib_read(chg, LEGACY_CABLE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_5 rc=%d\n", rc);
		return rc;
	}
	non_compliant = stat & TYPEC_NONCOMP_LEGACY_CABLE_STATUS_BIT;

	/* get settled ICL */
	rc = smblib_get_prop_input_current_settled(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get settled ICL rc=%d\n", rc);
		return rc;
	}

	typec_source_rd = smblib_get_prop_ufp_mode(chg);

	/* QC 2.0/3.0 adapter */
	if (apsd_result->bit & (QC_3P0_BIT | QC_2P0_BIT)) {
		*total_current_ua = HVDCP_CURRENT_UA;
		return 0;
	}

	if (non_compliant) {
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = DCP_CURRENT_UA;
			break;
		default:
			current_ua = 0;
			break;
		}

		*total_current_ua = max(current_ua, val.intval);
		return 0;
	}

	switch (typec_source_rd) {
	case POWER_SUPPLY_TYPEC_SOURCE_DEFAULT:
		switch (apsd_result->bit) {
		case CDP_CHARGER_BIT:
			current_ua = CDP_CURRENT_UA;
			break;
		case DCP_CHARGER_BIT:
		case OCP_CHARGER_BIT:
		case FLOAT_CHARGER_BIT:
			current_ua = chg->default_icl_ua;
			break;
		default:
			current_ua = 0;
			break;
		}
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_MEDIUM:
		current_ua = TYPEC_MEDIUM_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_SOURCE_HIGH:
		current_ua = TYPEC_HIGH_CURRENT_UA;
		break;
	case POWER_SUPPLY_TYPEC_NON_COMPLIANT:
	case POWER_SUPPLY_TYPEC_NONE:
	default:
		current_ua = 0;
		break;
	}

	*total_current_ua = max(current_ua, val.intval);
	return 0;
}

/**********************
 * INTERRUPT HANDLERS *
 **********************/

irqreturn_t default_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	return IRQ_HANDLED;
}

#define CHG_TERM_WA_ENTRY_DELAY_MS		300000		/* 5 min */
#define CHG_TERM_WA_EXIT_DELAY_MS		60000		/* 1 min */
static void smblib_eval_chg_termination(struct smb_charger *chg, u8 batt_status)
{
	union power_supply_propval pval = {0, };
	int rc = 0;

	rc = smblib_get_prop_from_bms(chg,
				POWER_SUPPLY_PROP_REAL_CAPACITY, &pval);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SOC value, rc=%d\n", rc);
		return;
	}

	/*
	 * Post charge termination, switch to BSM mode triggers the risk of
	 * over charging as BATFET opening may take some time post the necessity
	 * of staying in supplemental mode, leading to unintended charging of
	 * battery. Trigger the charge termination WA once charging is completed
	 * to prevent overcharing.
	 */
	if ((batt_status == TERMINATE_CHARGE) && (pval.intval == 100)) {
		chg->cc_soc_ref = 0;
		chg->last_cc_soc = 0;
		alarm_start_relative(&chg->chg_termination_alarm,
			ms_to_ktime(CHG_TERM_WA_ENTRY_DELAY_MS));
	} else if (pval.intval < 100) {
		/*
		 * Reset CC_SOC reference value for charge termination WA once
		 * we exit the TERMINATE_CHARGE state and soc drops below 100%
		 */
		chg->cc_soc_ref = 0;
		chg->last_cc_soc = 0;
	}
}

irqreturn_t chg_state_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read BATTERY_CHARGER_STATUS_1 rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	stat = stat & BATTERY_CHARGER_STATUS_MASK;

	if (chg->wa_flags & CHG_TERMINATION_WA)
		smblib_eval_chg_termination(chg, stat);

	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t batt_temp_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	rc = smblib_recover_from_soft_jeita(chg);
	if (rc < 0) {
		smblib_err(chg, "Couldn't recover chg from soft jeita rc=%d\n",
				rc);
		return IRQ_HANDLED;
	}

	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

irqreturn_t batt_psy_changed_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);
	power_supply_changed(chg->batt_psy);
	return IRQ_HANDLED;
}

#define AICL_STEP_MV		200
#define MAX_AICL_THRESHOLD_MV	4800
irqreturn_t usbin_uv_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata;
	int rc;
	/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 start*/
	#ifdef CONFIG_ARCH_MSM8953
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);
	u8 stat = 0, max_pulses = 0;
	#endif
	/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 end*/

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	if ((chg->wa_flags & WEAK_ADAPTER_WA)
			&& is_storming(&irq_data->storm_data)) {

		if (chg->aicl_max_reached) {
			smblib_dbg(chg, PR_MISC,
					"USBIN_UV storm at max AICL threshold\n");
			return IRQ_HANDLED;
		}

		smblib_dbg(chg, PR_MISC, "USBIN_UV storm at threshold %d\n",
				chg->aicl_5v_threshold_mv);

		/* suspend USBIN before updating AICL threshold */
		vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER, true, 0);

		/* delay for VASHDN deglitch */
		msleep(20);

		if (chg->aicl_5v_threshold_mv > MAX_AICL_THRESHOLD_MV) {
			/* reached max AICL threshold */
			chg->aicl_max_reached = true;
			goto unsuspend_input;
		}

		/* Increase AICL threshold by 200mV */
		rc = smblib_set_charge_param(chg, &chg->param.aicl_5v_threshold,
				chg->aicl_5v_threshold_mv + AICL_STEP_MV);
		if (rc < 0)
			dev_err(chg->dev,
				"Error in setting AICL threshold rc=%d\n", rc);
		else
			chg->aicl_5v_threshold_mv += AICL_STEP_MV;

		rc = smblib_set_charge_param(chg,
				&chg->param.aicl_cont_threshold,
				chg->aicl_cont_threshold_mv + AICL_STEP_MV);
		if (rc < 0)
			dev_err(chg->dev,
				"Error in setting AICL threshold rc=%d\n", rc);
		else
			chg->aicl_cont_threshold_mv += AICL_STEP_MV;

unsuspend_input:
		if (chg->smb_version == PMI632_SUBTYPE)
			schgm_flash_torch_priority(chg, TORCH_BOOST_MODE);

		if (chg->aicl_max_reached) {
			smblib_dbg(chg, PR_MISC,
				"Reached max AICL threshold resctricting ICL to 100mA\n");
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					true, USBIN_100MA);
			smblib_run_aicl(chg, RESTART_AICL);
		} else {
			smblib_run_aicl(chg, RESTART_AICL);
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					false, 0);
		}

		wdata = &chg->irq_info[USBIN_UV_IRQ].irq_data->storm_data;
		reset_storm_count(wdata);
	}

	if (!chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data)
		return IRQ_HANDLED;

	wdata = &chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data->storm_data;
	reset_storm_count(wdata);

/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 start*/
#ifdef CONFIG_ARCH_MSM8953
	/* Workaround for non-QC2.0-compliant chargers follows */
	if (!chg->qc2_unsupported_voltage &&
			apsd->pst == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read CHANGE_STATUS_REG rc=%d\n", rc);

		if (stat & QC_5V_BIT)
			return IRQ_HANDLED;

		rc = smblib_read(chg, HVDCP_PULSE_COUNT_MAX_REG, &max_pulses);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read QC2 max pulses rc=%d\n", rc);

		chg->qc2_max_pulses = (max_pulses &
				HVDCP_PULSE_COUNT_MAX_QC2_MASK);

		if (stat & QC_12V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_12V;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 9V rc=%d\n",
						rc);

		} else if (stat & QC_9V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_9V;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 5V rc=%d\n",
						rc);

		}

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				0);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn off SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		smblib_rerun_apsd(chg);
	}
#endif
/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 end*/

	return IRQ_HANDLED;
}

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
void non_compliant_chg_WA(struct smb_charger *chg){
	int rc;
	u8 stat = 0, max_pulses = 0;
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	pr_info("%s: APSD=%s\n", __func__, apsd->name);

	if (!chg->qc2_unsupported_voltage &&
			apsd->pst == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read CHANGE_STATUS_REG rc=%d\n", rc);

		if (stat & QC_5V_BIT)
			return;

		rc = smblib_read(chg, HVDCP_PULSE_COUNT_MAX_REG, &max_pulses);
		if (rc < 0)
			smblib_err(chg,
				"Couldn't read QC2 max pulses rc=%d\n", rc);

		chg->qc2_max_pulses = (max_pulses &
				HVDCP_PULSE_COUNT_MAX_QC2_MASK);

		if (stat & QC_12V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_12V;
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 9V rc=%d\n",
						rc);

		} else if (stat & QC_9V_BIT) {
			chg->qc2_unsupported_voltage = QC2_NON_COMPLIANT_9V;
			pr_info("%s: qc2_unsupported_voltage(%d)\n", __func__, chg->qc2_unsupported_voltage);
			rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
					HVDCP_PULSE_COUNT_MAX_QC2_MASK,
					HVDCP_PULSE_COUNT_MAX_QC2_5V);
			if (rc < 0)
				smblib_err(chg, "Couldn't force max pulses to 5V rc=%d\n",
						rc);

		}

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				0);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn off SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		pr_info("%s: qc2_unsupported_voltage : %d \n", __func__, chg->qc2_unsupported_voltage );
		smblib_rerun_apsd(chg);
	}
}
#endif
#endif

#define USB_WEAK_INPUT_UA	1400000
#define ICL_CHANGE_DELAY_MS	1000
irqreturn_t icl_change_irq_handler(int irq, void *data)
{
	u8 stat;
	int rc, settled_ua, delay = ICL_CHANGE_DELAY_MS;
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->mode == PARALLEL_MASTER) {
		/*
		 * Ignore if change in ICL is due to DIE temp mitigation.
		 * This is to prevent any further ICL split.
		 */
		if (chg->hw_die_temp_mitigation) {
			rc = smblib_read(chg, MISC_DIE_TEMP_STATUS_REG, &stat);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't read DIE_TEMP rc=%d\n", rc);
				return IRQ_HANDLED;
			}
			if (stat & (DIE_TEMP_UB_BIT | DIE_TEMP_LB_BIT)) {
				smblib_dbg(chg, PR_PARALLEL,
					"skip ICL change DIE_TEMP %x\n", stat);
				return IRQ_HANDLED;
			}
		}

		rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n",
					rc);
			return IRQ_HANDLED;
		}

		rc = smblib_get_charge_param(chg, &chg->param.icl_stat,
					&settled_ua);
		if (rc < 0) {
			smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
			return IRQ_HANDLED;
		}

		/* If AICL settled then schedule work now */
		if (settled_ua == get_effective_result(chg->usb_icl_votable))
			delay = 0;

		cancel_delayed_work_sync(&chg->icl_change_work);
		schedule_delayed_work(&chg->icl_change_work,
						msecs_to_jiffies(delay));
	}

	return IRQ_HANDLED;
}

static void smblib_micro_usb_plugin(struct smb_charger *chg, bool vbus_rising)
{
	if (!vbus_rising) {
		smblib_update_usb_type(chg);
		smblib_notify_device_mode(chg, false);
		smblib_uusb_removal(chg);
	}
}

void smblib_usb_plugin_hard_reset_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	if (vbus_rising) {
		/* Remove FCC_STEPPER 1.5A init vote to allow FCC ramp up */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);
	} else {
		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		/* Force 1500mA FCC on USB removal if fcc stepper is enabled */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
							true, 1500000);
	}

	power_supply_changed(chg->usb_psy);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

/* Huaqin add for P200731-01593 Enable charging while TYPE-C is  default mode by gaochao at 2020/08/10 start */
enum HS70_NA {
	HS70_HQ_PCBA_AT_T = 0x5,
	HS70_HQ_PCBA_Canada = 0x8,
};

#define PCB_MASK_HQ		0xFF0
#define PCB_SHIFT_HQ		4
/* Huaqin add for P200731-01593 Enable charging while TYPE-C is  default mode by gaochao at 2020/08/10 end */

#define PL_DELAY_MS	30000
void smblib_usb_plugin_locked(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	bool vbus_rising;
	struct smb_irq_data *data;
	struct storm_watch *wdata;
	/* Huaqin add for P200731-01593 enable charging while Rp-Rp on both CC Pins by gaochao at 2020/08/10 start */
	u32 pcba_config = 0;
	pcba_config = hq_get_huaqin_pcba_config();
	/* Huaqin add for P200731-01593 enable charging while Rp-Rp on both CC Pins by gaochao at 2020/08/10 end */

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		return;
	}

	vbus_rising = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
	smblib_set_opt_switcher_freq(chg, vbus_rising ? chg->chg_freq.freq_5V :
						chg->chg_freq.freq_removal);

	if (vbus_rising) {
		rc = smblib_request_dpdm(chg, true);
		if (rc < 0)
			smblib_err(chg, "Couldn't to enable DPDM rc=%d\n", rc);

		/* Remove FCC_STEPPER 1.5A init vote to allow FCC ramp up */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER, false, 0);

		/* Schedule work to enable parallel charger */
		vote(chg->awake_votable, PL_DELAY_VOTER, true, 0);
		schedule_delayed_work(&chg->pl_enable_work,
					msecs_to_jiffies(PL_DELAY_MS));
	} else {
#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
		detach_afc();  //related with afc driver
#endif
#endif
		if (chg->wa_flags & BOOST_BACK_WA) {
			data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				update_storm_count(wdata,
						WEAK_CHG_STORM_COUNT);
				vote(chg->usb_icl_votable, BOOST_BACK_VOTER,
						false, 0);
				vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
						false, 0);
			}
		}

		if (chg->wa_flags & WEAK_ADAPTER_WA) {
			chg->aicl_5v_threshold_mv =
					chg->default_aicl_5v_threshold_mv;
			chg->aicl_cont_threshold_mv =
					chg->default_aicl_cont_threshold_mv;

			smblib_set_charge_param(chg,
					&chg->param.aicl_5v_threshold,
					chg->aicl_5v_threshold_mv);
			smblib_set_charge_param(chg,
					&chg->param.aicl_cont_threshold,
					chg->aicl_cont_threshold_mv);
			chg->aicl_max_reached = false;

			if (chg->smb_version == PMI632_SUBTYPE)
				schgm_flash_torch_priority(chg,
						TORCH_BUCK_MODE);

			data = chg->irq_info[USBIN_UV_IRQ].irq_data;
			if (data) {
				wdata = &data->storm_data;
				reset_storm_count(wdata);
			}
			vote(chg->usb_icl_votable, AICL_THRESHOLD_VOTER,
					false, 0);
		}

		/* Force 1500mA FCC on removal if fcc stepper is enabled */
		if (chg->fcc_stepper_enable)
			vote(chg->fcc_votable, FCC_STEPPER_VOTER,
							true, 1500000);

		rc = smblib_request_dpdm(chg, false);
		if (rc < 0)
			smblib_err(chg, "Couldn't disable DPDM rc=%d\n", rc);

		smblib_update_usb_type(chg);
		/* Huaqin add for P200731-01593 Enable charging while TYPE-C is default mode by gaochao at 2020/08/10 start */
		if (chg->distinguish_sdm439_sdm450_others == DETECT_SDM450_PLATFORM)		//HS70
		{
			if ((((pcba_config & PCB_MASK_HQ) >> PCB_SHIFT_HQ) == HS70_HQ_PCBA_AT_T)
				|| (((pcba_config & PCB_MASK_HQ) >> PCB_SHIFT_HQ) == HS70_HQ_PCBA_Canada))
			{
				pr_info("[%s]line=%d: ignore A11 NA\n", __FUNCTION__, __LINE__);
			}
			else
			{
				if (chg->use_extcon)
					smblib_notify_device_mode(chg, false);
				vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0); //remove USB_PSY voting when plugin detach
			}
		}
		else
		{
			/*Huaqin add for Enable Charge while TypeC mode detected as DEFAULT by wangzikang at 2020/07/14 start*/
			if (chg->use_extcon)
				smblib_notify_device_mode(chg, false);
			rc = vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0); //remove USB_PSY voting when plugin detach
			if (rc < 0)
			{
			    pr_err("line=%d: Couldn't vote USB_PSY_VOTER rc=%d\n", __LINE__, rc);
			}
			/*Huaqin add for Enable Charge while TypeC mode detected as DEFAULT by wangzikang at 2020/07/14 start*/
		}
		/* Huaqin add for P200731-01593 Enable charging while TYPE-C is default mode by gaochao at 2020/08/10 end */
		/* QL3095 add for P210204-02894 Set ICL 500ma when usb plugin detach by shixuanxuan at 2020/02/14 start */
			rc = vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, SDP_CURRENT_UA);
			if (rc < 0)
			{
			    pr_err("line=%d: Couldn't vote SW_ICL_MAX_VOTER rc=%d\n", __LINE__, rc);
			}
		/* QL3095 add for P210204-02894 Set ICL 500ma when usb plugin detach by shixuanxuan at 2020/02/14 end */
	}

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		smblib_micro_usb_plugin(chg, vbus_rising);

	/* HS60 add for HS60-163 Set usb thermal by gaochao at 2019/07/30 start */
	pr_info("[%s]line=%d, vbus_rising=%d\n", __FUNCTION__, __LINE__, vbus_rising);
	//ss_vbus_control_gpio_init(chg);
	/* HS60 add for HS60-163 Set usb thermal by gaochao at 2019/07/30 end */
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 start */
	if(chg->store_mode_ss == STORE_MODE_ENABLE_SS){
		if (vbus_rising)
		{
			pr_info("[%s]line=%d, start retail_app_status_change_work after %d ms\n",
					__FUNCTION__, __LINE__, RETAIL_APP_START_DETECT_TIME);

			schedule_delayed_work(&chg->retail_app_status_change_work, msecs_to_jiffies(RETAIL_APP_START_DETECT_TIME));
		}
		else
		{
			pr_info("[%s]line=%d, cancel retail_app_status_change_work\n",
					__FUNCTION__, __LINE__);

			cancel_delayed_work_sync(&chg->retail_app_status_change_work);
			/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 start*/
			rc = vote(chg->fcc_votable,STORE_MODE_VOTER,false,STORE_MODE_FCC);
			if(rc < 0)
				pr_err("Failed to vote STORE_MODE_VOTER, rc=%d\n", rc);
			/*Huaqin added for SR-ZQL1871-01-248 & SR-ZQL1695-01-486 by wangzikang at 2019/10/18 end*/
		}
	}
	/* HS60 add for SR-ZQL1695-01-495 by wangzikang at 2019/10/28 end */
	#endif
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 end */

	/* HS60 add for HS60-811 Set float charger by gaochao at 2019/08/27 start */
	if (vbus_rising)
	{
		/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 start*/
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		chg->slow_charging_count = 0;
		#endif
		/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 end*/
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 start */
		pr_info("[%s]line=%d, start float_charger_detect_work after %d ms, rerun_apsd_work after %d ms\n",
				__FUNCTION__, __LINE__, FLOAT_CHARGER_START_DETECT_TIME, FLOAT_CHARGER_START_DETECT_TIME + RERUN_APSD_DETECT_TIME);
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 end */

		/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 start */
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		/* HS70 add for HS70-565 remove float_charger_redetect_work by qianyingdong at 2019/1/22 start */
		/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 start */
		if (chg->boot_to_detect_charger == BOOT_TO_DETECT_INIT)
		{
			schedule_delayed_work(&chg->float_charger_detect_work, msecs_to_jiffies(BOOT_TO_DETECT_FLOAT_CHARGER_START_DETECT_TIME));
		}
		else
		{
			schedule_delayed_work(&chg->float_charger_detect_work, msecs_to_jiffies(FLOAT_CHARGER_START_DETECT_TIME));
		}

		pr_info("[%s]line=%d, boot_to_detect_charger=%d\n",
				__FUNCTION__, __LINE__, chg->boot_to_detect_charger);
		/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 end */
		/* HS70 add for HS70-565 remove float_charger_redetect_work by qianyingdong at 2019/1/22 end */
		#else	//factory
		schedule_delayed_work(&chg->float_charger_detect_work, msecs_to_jiffies(FLOAT_CHARGER_START_DETECT_TIME));
		#endif
		/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 end */
	}
	else
	{
		/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 start*/
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		chg->slow_charging_count = 0;
		#endif
		/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 end*/
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 start */
		pr_info("[%s]line=%d, cancel float_charger_detect_work, cancel rerun_apsd_work\n",
				__FUNCTION__, __LINE__);
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 end */

		cancel_delayed_work_sync(&chg->float_charger_detect_work);
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 start */
		cancel_delayed_work_sync(&chg->rerun_apsd_work);
		/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 end */
	}
	/* HS60 add for HS60-811 Set float charger by gaochao at 2019/08/27 end */
	/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	chg->boot_to_detect_charger += BOOT_TO_DETECT_STEP;
	if (chg->boot_to_detect_charger > BOOT_TO_DETECT_MAX)
	{
		chg->boot_to_detect_charger = BOOT_TO_DETECT_MAX;
	}

	pr_info("[%s]line=%d, boot_to_detect_charger=%d\n",
			__FUNCTION__, __LINE__, chg->boot_to_detect_charger);
	#endif
	/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 end */

	power_supply_changed(chg->usb_psy);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: usbin-plugin %s\n",
					vbus_rising ? "attached" : "detached");
}

irqreturn_t usb_plugin_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->pd_hard_reset)
		smblib_usb_plugin_hard_reset_locked(chg);
	else
		smblib_usb_plugin_locked(chg);

	return IRQ_HANDLED;
}

static void smblib_handle_slow_plugin_timeout(struct smb_charger *chg,
					      bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: slow-plugin-timeout %s\n",
		   rising ? "rising" : "falling");
}

static void smblib_handle_sdp_enumeration_done(struct smb_charger *chg,
					       bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: sdp-enumeration-done %s\n",
		   rising ? "rising" : "falling");
}

#define QC3_PULSES_FOR_6V	5
#define QC3_PULSES_FOR_9V	20
#define QC3_PULSES_FOR_12V	35
static void smblib_hvdcp_adaptive_voltage_change(struct smb_charger *chg)
{
	int rc;
	u8 stat;
	int pulses;

	power_supply_changed(chg->usb_main_psy);
	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP) {
		rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_CHANGE_STATUS rc=%d\n", rc);
			return;
		}

		switch (stat & QC_2P0_STATUS_MASK) {
		case QC_5V_BIT:
			smblib_set_opt_switcher_freq(chg,
					chg->chg_freq.freq_5V);
			break;
		case QC_9V_BIT:
			smblib_set_opt_switcher_freq(chg,
					chg->chg_freq.freq_9V);
			break;
		case QC_12V_BIT:
			smblib_set_opt_switcher_freq(chg,
					chg->chg_freq.freq_12V);
			break;
		default:
			smblib_set_opt_switcher_freq(chg,
					chg->chg_freq.freq_removal);
			break;
		}
	}

	if (chg->real_charger_type == POWER_SUPPLY_TYPE_USB_HVDCP_3) {
		rc = smblib_get_pulse_cnt(chg, &pulses);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't read QC_PULSE_COUNT rc=%d\n", rc);
			return;
		}

		if (pulses < QC3_PULSES_FOR_6V)
			smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_5V);
		else if (pulses < QC3_PULSES_FOR_9V)
			smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_6V_8V);
		else if (pulses < QC3_PULSES_FOR_12V)
			smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_9V);
		else
			smblib_set_opt_switcher_freq(chg,
				chg->chg_freq.freq_12V);
	}
}

/* triggers when HVDCP 3.0 authentication has finished */
static void smblib_handle_hvdcp_3p0_auth_done(struct smb_charger *chg,
					      bool rising)
{
	const struct apsd_result *apsd_result;

	if (!rising)
		return;

	if (chg->mode == PARALLEL_MASTER)
		vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, true, 0);

	/* the APSD done handler will set the USB supply type */
	apsd_result = smblib_get_apsd_result(chg);
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-3p0-auth-done rising; %s detected\n",
		   apsd_result->name);
}

static void smblib_handle_hvdcp_check_timeout(struct smb_charger *chg,
					      bool rising, bool qc_charger)
{
	if (rising) {
		
#if defined(CONFIG_AFC)
		if (qc_charger && !chg->qc2_unsupported_voltage) {
#else
		if (qc_charger) {
#endif
			/* enable HDC and ICL irq for QC2/3 charger */
			vote(chg->usb_irq_enable_votable, QC_VOTER, true, 0);
			vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
				HVDCP_CURRENT_UA);
		} else {
			/* A plain DCP, enforce DCP ICL if specified */
			vote(chg->usb_icl_votable, DCP_VOTER,
				chg->dcp_icl_ua != -EINVAL, chg->dcp_icl_ua);
		}
	}

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s %s\n", __func__,
		   rising ? "rising" : "falling");
}

/* triggers when HVDCP is detected */
static void smblib_handle_hvdcp_detect_done(struct smb_charger *chg,
					    bool rising)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: hvdcp-detect-done %s\n",
		   rising ? "rising" : "falling");
}

static void update_sw_icl_max(struct smb_charger *chg, int pst)
{
	/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 start */
	int typec_mode;
	int rp_ua;
	/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 end */

	/* HS50 add for SR-QL3095-01-67 Import default charger profile by wenyaqi at 2020/08/03 start */
	chg->is_dcp = false;
#if defined(CONFIG_AFC)
	if (pst == POWER_SUPPLY_TYPE_USB_DCP && chg->real_charger_type != POWER_SUPPLY_TYPE_AFC)
#else
	if (pst == POWER_SUPPLY_TYPE_USB_DCP)
#endif
	{
		chg->is_dcp = true;
	}
	pr_debug("is_dcp=%d\n", chg->is_dcp);
	/* HS50 add for SR-QL3095-01-67 Import default charger profile by wenyaqi at 2020/08/03 end */

	/* while PD is active it should have complete ICL control */
	if (chg->pd_active)
		return;

	/*
	 * HVDCP 2/3, handled separately
	 * For UNKNOWN(input not present) return without updating ICL
	 */
	if (pst == POWER_SUPPLY_TYPE_USB_HVDCP
			|| pst == POWER_SUPPLY_TYPE_USB_HVDCP_3
#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
			|| (chg->real_charger_type == POWER_SUPPLY_TYPE_AFC)
#endif
#endif
			|| pst == POWER_SUPPLY_TYPE_UNKNOWN)
		return;

	/* TypeC rp med or high, use rp value */
	/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 start */
	typec_mode = smblib_get_prop_typec_mode(chg);
	if (typec_rp_med_high(chg, typec_mode)) {
		rp_ua = get_rp_based_dcp_current(chg, typec_mode);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true, rp_ua);
		return;
	}
	/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 end */
	/* rp-std or legacy, USB BC 1.2 */
	switch (pst) {
	case POWER_SUPPLY_TYPE_USB:
		/*
		 * USB_PSY will vote to increase the current to 500/900mA once
		 * enumeration is done.
		 */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
						USB_PSY_VOTER)) {
			/* if flash is active force 500mA */
			vote(chg->usb_icl_votable, USB_PSY_VOTER, true,
					is_flash_active(chg) ?
					SDP_CURRENT_UA : SDP_100_MA);
		}
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		break;
	case POWER_SUPPLY_TYPE_USB_CDP:
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					CDP_CURRENT_UA);
		break;
	case POWER_SUPPLY_TYPE_USB_DCP:
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					DCP_CURRENT_UA);
		break;
	case POWER_SUPPLY_TYPE_USB_FLOAT:
		/*
		 * limit ICL to 100mA, the USB driver will enumerate to check
		 * if this is a SDP and appropriately set the current
		 */
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					SDP_100_MA);
		break;
	default:
		smblib_err(chg, "Unknown APSD %d; forcing 500mA\n", pst);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
					SDP_CURRENT_UA);
		break;
	}
}

static void smblib_handle_apsd_done(struct smb_charger *chg, bool rising)
{
	const struct apsd_result *apsd_result;
#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	int typec_mode, rc = 0;
#endif
#endif

	if (!rising)
		return;

	apsd_result = smblib_update_usb_type(chg);

	update_sw_icl_max(chg, apsd_result->pst);

	switch (apsd_result->bit) {
	case SDP_CHARGER_BIT:
	case CDP_CHARGER_BIT:
	case FLOAT_CHARGER_BIT:
		if ((chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
				|| chg->use_extcon)
			smblib_notify_device_mode(chg, true);
		break;
	case OCP_CHARGER_BIT:
	case DCP_CHARGER_BIT:
		break;
	default:
		break;
	}

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	typec_mode = smblib_get_prop_typec_mode(chg);

	if(apsd_result->bit == DCP_CHARGER_BIT && typec_mode == POWER_SUPPLY_TYPEC_SOURCE_DEFAULT) {
		if ((chg->afc_sts == AFC_INIT) && (!chg->hv_disable)) {
			/* AFC function call */
			smblib_dbg(chg, PR_MISC, "Start AFC!!!\n");
			vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, 500000);   /* 500mA for AFC communication  */
			rc = smblib_set_adapter_allowance(chg,
					USBIN_ADAPTER_ALLOW_5V_TO_9V);
			if (rc < 0)
				smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_TO_9V rc=%d\n",
					rc);

			if(chg->flash_active)
				afc_set_voltage(SET_5V);
			else
				afc_set_voltage(SET_9V);
		} else {
			pr_err("%s: Do not start AFC, afc_sts(%d), hv_disable(%d)\n",
					__func__, chg->afc_sts, chg->hv_disable);
			/* afc_sts enum 
			 * AFC_INIT = 0, NOT_AFC = 1, AFC_FAIL = 2, AFC_DISABLE = 3 */
		}
	}
#endif
#endif

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: apsd-done rising; %s detected\n",
		   apsd_result->name);
}

irqreturn_t usb_source_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc = 0;
	u8 stat;

	/*
	 * Prepared to run PD or PD is active. At this moment, APSD is disabled,
	 * but there still can be irq on apsd_done from previously unfinished
	 * APSD run, skip it.
	 */
	if (chg->ok_to_pd)
		return IRQ_HANDLED;

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", stat);

/* HS70 add for P200302-05335 Remove re-running APSD when micro-usb cable plugged by qianyingdong at 2020/03/02 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
	smblib_dbg(chg, PR_INTERRUPT, "[%s]skip re-runing APSD\n", __func__);
#else
	if ((chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB)
		&& (stat & APSD_DTC_STATUS_DONE_BIT)
		&& !chg->uusb_apsd_rerun_done) {
		/*
		 * Force re-run APSD to handle slow insertion related
		 * charger-mis-detection.
		 */
		chg->uusb_apsd_rerun_done = true;
		smblib_rerun_apsd_if_required(chg);
		return IRQ_HANDLED;
	}
#endif
/* HS70 add for P200302-05335 Remove re-running APSD when micro-usb cable plugged by qianyingdong at 2020/03/02 start */

	smblib_handle_apsd_done(chg,
		(bool)(stat & APSD_DTC_STATUS_DONE_BIT));

	smblib_handle_hvdcp_detect_done(chg,
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_check_timeout(chg,
		(bool)(stat & HVDCP_CHECK_TIMEOUT_BIT),
		(bool)(stat & QC_CHARGER_BIT));

	smblib_handle_hvdcp_3p0_auth_done(chg,
		(bool)(stat & QC_AUTH_DONE_STATUS_BIT));

	smblib_handle_sdp_enumeration_done(chg,
		(bool)(stat & ENUMERATION_DONE_BIT));

	smblib_handle_slow_plugin_timeout(chg,
		(bool)(stat & SLOW_PLUGIN_TIMEOUT_BIT));

	smblib_hvdcp_adaptive_voltage_change(chg);

	power_supply_changed(chg->usb_psy);

	rc = smblib_read(chg, APSD_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read APSD_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}
	smblib_dbg(chg, PR_REGISTER, "APSD_STATUS = 0x%02x\n", stat);

	return IRQ_HANDLED;
}

static void typec_sink_insertion(struct smb_charger *chg)
{
	vote(chg->usb_icl_votable, OTG_VOTER, true, 0);

	if (chg->use_extcon) {
		smblib_notify_usb_host(chg, true);
		chg->otg_present = true;
	}

	if (!chg->pr_swap_in_progress)
		chg->ok_to_pd = (!(*chg->pd_disabled) || chg->early_usb_attach)
					&& !chg->pd_not_supported;
}

static void typec_src_insertion(struct smb_charger *chg)
{
	int rc = 0;
	u8 stat;

	if (chg->pr_swap_in_progress)
		return;

	rc = smblib_read(chg, LEGACY_CABLE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATE_MACHINE_STATUS_REG rc=%d\n",
			rc);
		return;
	}

	chg->typec_legacy = stat & TYPEC_LEGACY_CABLE_STATUS_BIT;
	chg->ok_to_pd = (!(chg->typec_legacy || *chg->pd_disabled)
			|| chg->early_usb_attach) && !chg->pd_not_supported;
	/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 start */
	if (!chg->ok_to_pd) {
		rc = smblib_configure_hvdcp_apsd(chg, true);
		if (rc < 0) {
			dev_err(chg->dev,
				"Couldn't enable APSD rc=%d\n", rc);
			return;
		}
		smblib_rerun_apsd_if_required(chg);
	}
	/* HS70 add for HS60-5436 by wangzikang at 2020/03/24 end */
}

static void typec_sink_removal(struct smb_charger *chg)
{
	vote(chg->usb_icl_votable, OTG_VOTER, false, 0);

	if (chg->use_extcon) {
		if (chg->otg_present)
			smblib_notify_usb_host(chg, false);
		chg->otg_present = false;
	}
}

static void typec_src_removal(struct smb_charger *chg)
{
	int rc;
	struct smb_irq_data *data;
	struct storm_watch *wdata;

	/* disable apsd */
	rc = smblib_configure_hvdcp_apsd(chg, false);
	if (rc < 0)
		smblib_err(chg, "Couldn't disable APSD rc=%d\n", rc);

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	smblib_hvdcp_detect_enable(chg, false);
#endif
#endif
	smblib_update_usb_type(chg);

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	chg->afc_sts = AFC_INIT;
	vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
	vote(chg->fcc_votable, SEC_BATTERY_AFC_VOTER, false, 0);
#endif
#endif

	if (chg->wa_flags & BOOST_BACK_WA) {
		data = chg->irq_info[SWITCHER_POWER_OK_IRQ].irq_data;
		if (data) {
			wdata = &data->storm_data;
			update_storm_count(wdata, WEAK_CHG_STORM_COUNT);
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					false, 0);
		}
	}

	cancel_delayed_work_sync(&chg->pl_enable_work);
#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	cancel_delayed_work_sync(&chg->compliant_check_work);
#endif
#endif

	if (chg->wa_flags & CHG_TERMINATION_WA)
		alarm_cancel(&chg->chg_termination_alarm);

	/* reset input current limit voters */
	vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, true,
			is_flash_active(chg) ? SDP_CURRENT_UA : SDP_100_MA);
	vote(chg->usb_icl_votable, PD_VOTER, false, 0);
	vote(chg->usb_icl_votable, USB_PSY_VOTER, false, 0);
	vote(chg->usb_icl_votable, DCP_VOTER, false, 0);
	vote(chg->usb_icl_votable, PL_USBIN_USBIN_VOTER, false, 0);
	vote(chg->usb_icl_votable, SW_QC3_VOTER, false, 0);
	vote(chg->usb_icl_votable, OTG_VOTER, false, 0);
	vote(chg->usb_icl_votable, CTM_VOTER, false, 0);
	vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);

	/* reset usb irq voters */
	vote(chg->usb_irq_enable_votable, PD_VOTER, false, 0);
	vote(chg->usb_irq_enable_votable, QC_VOTER, false, 0);

	/* reset parallel voters */
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);
	vote(chg->pl_disable_votable, PL_FCC_LOW_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_I_VOTER, false, 0);
	vote(chg->pl_enable_votable_indirect, USBIN_V_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);

	/* reset USBOV votes and cancel work */
	cancel_delayed_work_sync(&chg->usbov_dbc_work);
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
	chg->dbc_usbov = false;

	chg->pulse_cnt = 0;
	chg->usb_icl_delta_ua = 0;
	chg->voltage_min_uv = MICRO_5V;
	chg->voltage_max_uv = MICRO_5V;

	/* write back the default FLOAT charger configuration */
	rc = smblib_masked_write(chg, USBIN_OPTIONS_2_CFG_REG,
				(u8)FLOAT_OPTIONS_MASK, chg->float_cfg);
	if (rc < 0)
		smblib_err(chg, "Couldn't write float charger options rc=%d\n",
			rc);

	/* reconfigure allowed voltage for HVDCP */
	rc = smblib_set_adapter_allowance(chg,
			USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V);
	if (rc < 0)
		smblib_err(chg, "Couldn't set USBIN_ADAPTER_ALLOW_5V_OR_9V_TO_12V rc=%d\n",
			rc);

/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 start*/
#ifdef CONFIG_ARCH_MSM8953
	/*
	 * if non-compliant charger caused UV, restore original max pulses
	 * and turn SUSPEND_ON_COLLAPSE_USBIN_BIT back on.
	 */
	if (chg->qc2_unsupported_voltage) {
		rc = smblib_masked_write(chg, HVDCP_PULSE_COUNT_MAX_REG,
				HVDCP_PULSE_COUNT_MAX_QC2_MASK,
				chg->qc2_max_pulses);
		if (rc < 0)
			smblib_err(chg, "Couldn't restore max pulses rc=%d\n",
					rc);

		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
				SUSPEND_ON_COLLAPSE_USBIN_BIT,
				SUSPEND_ON_COLLAPSE_USBIN_BIT);
		if (rc < 0)
			smblib_err(chg, "Couldn't turn on SUSPEND_ON_COLLAPSE_USBIN_BIT rc=%d\n",
					rc);

		chg->qc2_unsupported_voltage = QC2_COMPLIANT;
	}
#endif
/*HS70 add for HS70-919 import Handle QC2.0 charger collapse patch by qianyingdong at 2019/11/18 end*/

	if (chg->use_extcon)
		smblib_notify_device_mode(chg, false);

	chg->typec_legacy = false;
}

static void smblib_handle_rp_change(struct smb_charger *chg, int typec_mode)
{
	const struct apsd_result *apsd = smblib_get_apsd_result(chg);

	/*
	 * We want the ICL vote @ 100mA for a FLOAT charger
	 * until the detection by the USB stack is complete.
	 * Ignore the Rp changes unless there is a
	 * pre-existing valid vote or FLOAT is configured for
	 * SDP current.
	 */
	if (apsd->pst == POWER_SUPPLY_TYPE_USB_FLOAT) {
		if (get_client_vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER)
					<= USBIN_100MA
			|| (chg->float_cfg & FLOAT_OPTIONS_MASK)
					== FORCE_FLOAT_SDP_CFG_BIT)
		return;
	}

	update_sw_icl_max(chg, apsd->pst);

	smblib_dbg(chg, PR_MISC, "CC change old_mode=%d new_mode=%d\n",
						chg->typec_mode, typec_mode);
}

irqreturn_t typec_or_rid_detection_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		if (chg->moisture_protection_enabled &&
				(chg->wa_flags & MOISTURE_PROTECTION_WA)) {
			/*
			 * Adding pm_stay_awake as because pm_relax is called
			 * on exit path from the work routine.
			 */
			pm_stay_awake(chg->dev);
			schedule_work(&chg->moisture_protection_work);
		}

		cancel_delayed_work_sync(&chg->uusb_otg_work);
		/*
		 * Skip OTG enablement if RID interrupt triggers with moisture
		 * protection still enabled.
		 */
		if (!chg->moisture_present) {
			vote(chg->awake_votable, OTG_DELAY_VOTER, true, 0);
			smblib_dbg(chg, PR_INTERRUPT, "Scheduling OTG work\n");
			schedule_delayed_work(&chg->uusb_otg_work,
					msecs_to_jiffies(chg->otg_delay_ms));
		}

		return IRQ_HANDLED;
	}

	return IRQ_HANDLED;
}

irqreturn_t typec_state_change_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int typec_mode;

	if (chg->connector_type == POWER_SUPPLY_CONNECTOR_MICRO_USB) {
		smblib_dbg(chg, PR_INTERRUPT,
				"Ignoring for micro USB\n");
		return IRQ_HANDLED;
	}

	typec_mode = smblib_get_prop_typec_mode(chg);
	if (chg->sink_src_mode != UNATTACHED_MODE
			&& (typec_mode != chg->typec_mode))
		smblib_handle_rp_change(chg, typec_mode);
	chg->typec_mode = typec_mode;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: cc-state-change; Type-C %s detected\n",
				smblib_typec_mode_name[chg->typec_mode]);

	power_supply_changed(chg->usb_psy);

	return IRQ_HANDLED;
}

irqreturn_t typec_attach_detach_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;
#if defined(CONFIG_TYPEC)
	struct typec_partner_desc desc;
	enum typec_pwr_opmode mode = TYPEC_PWR_MODE_USB;
	union power_supply_propval val = {0};
#endif

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_read(chg, TYPE_C_STATE_MACHINE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATE_MACHINE_STATUS_REG rc=%d\n",
			rc);
		return IRQ_HANDLED;
	}

	if (stat & TYPEC_ATTACH_DETACH_STATE_BIT) {
		rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_MISC_STATUS_REG rc=%d\n",
				rc);
			return IRQ_HANDLED;
		}

		if (stat & SNK_SRC_MODE_BIT) {
			chg->sink_src_mode = SRC_MODE;
			typec_sink_insertion(chg);
#if defined(CONFIG_TYPEC)
			if (chg->port && (chg->partner == NULL)) {
				chg->typec_power_role = TYPEC_SOURCE;
				chg->typec_data_role = TYPEC_HOST;
				chg->pwr_opmode = TYPEC_PWR_MODE_USB;
				typec_set_pwr_opmode(chg->port, chg->pwr_opmode);
				desc.usb_pd = mode == TYPEC_PWR_MODE_PD;
				desc.accessory = TYPEC_ACCESSORY_NONE; /* XXX: handle accessories */
				desc.identity = NULL;
				typec_set_pwr_role(chg->port, chg->typec_power_role);
				typec_set_data_role(chg->port, chg->typec_data_role);
				pr_info("%s: typec_register_partner", __func__);
				chg->partner = typec_register_partner(chg->port, &desc);
			}
#endif
		} else {
			chg->sink_src_mode = SINK_MODE;
			typec_src_insertion(chg);
#if defined(CONFIG_TYPEC)
			if (chg->port && (chg->partner == NULL)) {
				chg->typec_power_role = TYPEC_SINK;
				chg->typec_data_role = TYPEC_DEVICE;
				chg->pwr_opmode = TYPEC_PWR_MODE_USB;
				typec_set_pwr_opmode(chg->port, chg->pwr_opmode);
				desc.usb_pd = mode == TYPEC_PWR_MODE_PD;
				desc.accessory = TYPEC_ACCESSORY_NONE; /* XXX: handle accessories */
				desc.identity = NULL;
				typec_set_pwr_role(chg->port, chg->typec_power_role);
				typec_set_data_role(chg->port, chg->typec_data_role);
				pr_info("%s: typec_register_partner", __func__);
				chg->partner = typec_register_partner(chg->port, &desc);
			}
#endif
		}

	} else {
		switch (chg->sink_src_mode) {
		case SRC_MODE:
			typec_sink_removal(chg);
			break;
		case SINK_MODE:
			typec_src_removal(chg);
			break;
		default:
			break;
		}

		if (!chg->pr_swap_in_progress) {
			chg->ok_to_pd = false;
			chg->sink_src_mode = UNATTACHED_MODE;
			chg->early_usb_attach = false;
		}
#if defined(CONFIG_TYPEC)
		if (chg->typec_try_state_change == TRY_ROLE_SWAP_TYPE) {
			if (chg->requested_port_type == TYPEC_PORT_DFP)
				val.intval = POWER_SUPPLY_TYPEC_PR_SOURCE;
			else if (chg->requested_port_type == TYPEC_PORT_UFP)
				val.intval = POWER_SUPPLY_TYPEC_PR_SINK;
			else
				val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		} else {
			val.intval = POWER_SUPPLY_TYPEC_PR_DUAL;
		}
		pr_info("%s: typec_try_state_change = %d, requested_port_type = %d, val.intval = %d", __func__,
			chg->typec_try_state_change, chg->requested_port_type, val.intval);
		smblib_set_prop_typec_power_role(chg, &val);

		chg->typec_try_state_change = TRY_ROLE_SWAP_NONE;

		if (chg->partner) {
			pr_info("%s: typec_unregister_partner", __func__);
			if (!IS_ERR(chg->partner))
				typec_unregister_partner(chg->partner);
			chg->typec_power_role = TYPEC_SINK;
			chg->typec_data_role = TYPEC_DEVICE;
			chg->partner = NULL;
		}
#endif
	}

	power_supply_changed(chg->usb_psy);

	return IRQ_HANDLED;
}

irqreturn_t dc_plugin_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	power_supply_changed(chg->dc_psy);
	return IRQ_HANDLED;
}

irqreturn_t high_duty_cycle_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;

	chg->is_hdc = true;
	/*
	 * Disable usb IRQs after the flag set and re-enable IRQs after
	 * the flag cleared in the delayed work queue, to avoid any IRQ
	 * storming during the delays
	 */
	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		disable_irq_nosync(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);

	schedule_delayed_work(&chg->clear_hdc_work, msecs_to_jiffies(60));

	return IRQ_HANDLED;
}

static void smblib_bb_removal_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bb_removal_work.work);

	vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
	vote(chg->awake_votable, BOOST_BACK_VOTER, false, 0);
}

#define BOOST_BACK_UNVOTE_DELAY_MS		750
#define BOOST_BACK_STORM_COUNT			3
#define WEAK_CHG_STORM_COUNT			8
irqreturn_t switcher_power_ok_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	struct storm_watch *wdata = &irq_data->storm_data;
	int rc, usb_icl;
	u8 stat;

	if (!(chg->wa_flags & BOOST_BACK_WA))
		return IRQ_HANDLED;

	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return IRQ_HANDLED;
	}

	/* skip suspending input if its already suspended by some other voter */
	usb_icl = get_effective_result(chg->usb_icl_votable);
	if ((stat & USE_USBIN_BIT) && usb_icl >= 0 && usb_icl <= USBIN_25MA)
		return IRQ_HANDLED;

	if (stat & USE_DCIN_BIT)
		return IRQ_HANDLED;

	if (is_storming(&irq_data->storm_data)) {
		/* This could be a weak charger reduce ICL */
		if (!is_client_vote_enabled(chg->usb_icl_votable,
						WEAK_CHARGER_VOTER)) {
			smblib_err(chg,
				"Weak charger detected: voting %dmA ICL\n",
				*chg->weak_chg_icl_ua / 1000);
			vote(chg->usb_icl_votable, WEAK_CHARGER_VOTER,
					true, *chg->weak_chg_icl_ua);
			/*
			 * reset storm data and set the storm threshold
			 * to 3 for reverse boost detection.
			 */
			update_storm_count(wdata, BOOST_BACK_STORM_COUNT);
		} else {
			smblib_err(chg,
				"Reverse boost detected: voting 0mA to suspend input\n");
			vote(chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
			vote(chg->awake_votable, BOOST_BACK_VOTER, true, 0);
			/*
			 * Remove the boost-back vote after a delay, to avoid
			 * permanently suspending the input if the boost-back
			 * condition is unintentionally hit.
			 */
			schedule_delayed_work(&chg->bb_removal_work,
				msecs_to_jiffies(BOOST_BACK_UNVOTE_DELAY_MS));
		}
	}

	return IRQ_HANDLED;
}

irqreturn_t wdog_bark_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	int rc;

	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_data->name);

	rc = smblib_write(chg, BARK_BITE_WDOG_PET_REG, BARK_BITE_WDOG_PET_BIT);
	if (rc < 0)
		smblib_err(chg, "Couldn't pet the dog rc=%d\n", rc);

	if (chg->step_chg_enabled || chg->sw_jeita_enabled)
		power_supply_changed(chg->batt_psy);

	return IRQ_HANDLED;
}

static void smblib_usbov_dbc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						usbov_dbc_work.work);

	smblib_dbg(chg, PR_MISC, "Resetting USBOV debounce\n");
	chg->dbc_usbov = false;
	vote(chg->awake_votable, USBOV_DBC_VOTER, false, 0);
}

irqreturn_t usbin_ov_irq_handler(int irq, void *data)
{
	struct smb_irq_data *irq_data = data;
	struct smb_charger *chg = irq_data->parent_data;
	u8 stat;
	int rc;

	if (!(chg->wa_flags & USBIN_OV_WA))
		goto out;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		goto out;
	}
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s stat=%x\n", irq_data->name,
				!!stat);

	if (stat & USBIN_OV_RT_STS_BIT) {
		chg->dbc_usbov = true;
		vote(chg->awake_votable, USBOV_DBC_VOTER, true, 0);
		schedule_delayed_work(&chg->usbov_dbc_work,
				msecs_to_jiffies(1000));
	} else {
		cancel_delayed_work_sync(&chg->usbov_dbc_work);
		chg->dbc_usbov = false;
		vote(chg->awake_votable, USBOV_DBC_VOTER, true, 0);
	}

out:
	smblib_dbg(chg, PR_MISC, "USBOV debounce status %d\n",
				chg->dbc_usbov);
	return IRQ_HANDLED;
}

/**************
 * Additional USB PSY getters/setters
 * that call interrupt functions
 ***************/

int smblib_get_prop_pr_swap_in_progress(struct smb_charger *chg,
				union power_supply_propval *val)
{
	val->intval = chg->pr_swap_in_progress;
	return 0;
}

int smblib_set_prop_pr_swap_in_progress(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	int rc;
	u8 stat = 0, orientation;

	chg->pr_swap_in_progress = val->intval;

	rc = smblib_masked_write(chg, TYPE_C_DEBOUNCE_OPTION_REG,
			REDUCE_TCCDEBOUNCE_TO_2MS_BIT,
			val->intval ? REDUCE_TCCDEBOUNCE_TO_2MS_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set tCC debounce rc=%d\n", rc);

	rc = smblib_masked_write(chg, TYPE_C_EXIT_STATE_CFG_REG,
			BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT,
			val->intval ? BYPASS_VSAFE0V_DURING_ROLE_SWAP_BIT : 0);
	if (rc < 0)
		smblib_err(chg, "Couldn't set exit state cfg rc=%d\n", rc);

	if (chg->pr_swap_in_progress) {
		rc = smblib_read(chg, TYPE_C_MISC_STATUS_REG, &stat);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_STATUS_4 rc=%d\n",
				rc);
		}

		orientation =
			stat & CC_ORIENTATION_BIT ? TYPEC_CCOUT_VALUE_BIT : 0;
		rc = smblib_masked_write(chg, TYPE_C_CCOUT_CONTROL_REG,
			TYPEC_CCOUT_SRC_BIT | TYPEC_CCOUT_BUFFER_EN_BIT
					| TYPEC_CCOUT_VALUE_BIT,
			TYPEC_CCOUT_SRC_BIT | TYPEC_CCOUT_BUFFER_EN_BIT
					| orientation);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
				rc);
		}
	} else {
		rc = smblib_masked_write(chg, TYPE_C_CCOUT_CONTROL_REG,
			TYPEC_CCOUT_SRC_BIT, 0);
		if (rc < 0) {
			smblib_err(chg, "Couldn't read TYPE_C_CCOUT_CONTROL_REG rc=%d\n",
				rc);
		}

		/* enable DRP */
		rc = smblib_masked_write(chg, TYPE_C_MODE_CFG_REG,
				 TYPEC_POWER_ROLE_CMD_MASK, 0);
		if (rc < 0)
			smblib_err(chg, "Couldn't enable DRP rc=%d\n", rc);
	}

	return 0;
}

/***************
 * Work Queues *
 ***************/
#ifdef CONFIG_USB_NOTIFY_LAYER
static void smblib_microb_otg_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						microb_otg_work.work);


	struct otg_notify *o_notify = get_otg_notify();

	if (!o_notify) {
		smblib_dbg(chg, PR_MISC, "microb_otg_work again for otg_notify\n");

		schedule_delayed_work(&chg->microb_otg_work,
					msecs_to_jiffies(1000));
		return;
	}
	smblib_dbg(chg, PR_MISC, "enable=%d\n", chg->otg_enable);

	if (chg->otg_enable)
		smblib_notify_extcon_props(chg, EXTCON_USB_HOST);

	extcon_set_state_sync(chg->extcon, EXTCON_USB_HOST, chg->otg_enable);

	send_otg_notify(o_notify, NOTIFY_EVENT_HOST, chg->otg_enable);
}
#endif

static void smblib_uusb_otg_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						uusb_otg_work.work);
	int rc;
	u8 stat;
	bool otg;
#ifdef CONFIG_USB_NOTIFY_LAYER
	struct otg_notify *o_notify = get_otg_notify();

	if (!o_notify) {
		smblib_dbg(chg, PR_MISC, "uusb_otg_work again for otg_notify\n");

		schedule_delayed_work(&chg->uusb_otg_work,
					msecs_to_jiffies(500));
		return;
	}
#endif

	rc = smblib_read(chg, TYPEC_U_USB_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_STATUS_3 rc=%d\n", rc);
		goto out;
	}
	otg = !!(stat & U_USB_GROUND_NOVBUS_BIT);
#ifdef CONFIG_USB_NOTIFY_LAYER
	smblib_dbg(chg, PR_MISC, "chg->otg_present=%d, otg=%d\n",
		chg->otg_present, otg);
#endif
	if (chg->otg_present != otg)
		smblib_notify_usb_host(chg, otg);
	else
		goto out;

	chg->otg_present = otg;
	if (!otg)
		chg->boost_current_ua = 0;

	rc = smblib_set_charge_param(chg, &chg->param.freq_switcher,
				otg ? chg->chg_freq.freq_below_otg_threshold
					: chg->chg_freq.freq_removal);
	if (rc < 0)
		dev_err(chg->dev, "Error in setting freq_boost rc=%d\n", rc);

	smblib_dbg(chg, PR_REGISTER, "TYPE_C_U_USB_STATUS = 0x%02x OTG=%d\n",
			stat, otg);
	power_supply_changed(chg->usb_psy);

out:
	vote(chg->awake_votable, OTG_DELAY_VOTER, false, 0);
}

static void bms_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						bms_update_work);

	smblib_suspend_on_debug_battery(chg);

	if (chg->batt_psy)
		power_supply_changed(chg->batt_psy);
}

static void pl_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						pl_update_work);

	smblib_stat_sw_override_cfg(chg, false);
}

static void clear_hdc_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						clear_hdc_work.work);

	chg->is_hdc = 0;
	if (chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq)
		enable_irq(chg->irq_info[HIGH_DUTY_CYCLE_IRQ].irq);
}

static void smblib_icl_change_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							icl_change_work.work);
	int rc, settled_ua;

	rc = smblib_get_charge_param(chg, &chg->param.icl_stat, &settled_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get ICL status rc=%d\n", rc);
		return;
	}

	power_supply_changed(chg->usb_main_psy);

	smblib_dbg(chg, PR_INTERRUPT, "icl_settled=%d\n", settled_ua);
}

static void smblib_pl_enable_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							pl_enable_work.work);

	smblib_dbg(chg, PR_PARALLEL, "timer expired, enabling parallel\n");
	vote(chg->pl_disable_votable, PL_DELAY_VOTER, false, 0);
	vote(chg->awake_votable, PL_DELAY_VOTER, false, 0);
}

#define MOISTURE_PROTECTION_CHECK_DELAY_MS 300000		/* 5 mins */
static void smblib_moisture_protection_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						moisture_protection_work);
	int rc;
	bool usb_plugged_in;
	u8 stat;

	/*
	 * Hold awake votable to prevent pm_relax being called prior to
	 * completion of this work.
	 */
	vote(chg->awake_votable, MOISTURE_VOTER, true, 0);

	/*
	 * Disable 1% duty cycle on CC_ID pin and enable uUSB factory mode
	 * detection to track any change on RID, as interrupts are disable.
	 */
	rc = smblib_write(chg, TYPEC_U_USB_WATER_PROTECTION_CFG_REG, 0);
	if (rc < 0) {
		smblib_err(chg, "Couldn't disable periodic monitoring of CC_ID rc=%d\n",
			rc);
		goto out;
	}

	rc = smblib_masked_write(chg, TYPEC_U_USB_CFG_REG,
					EN_MICRO_USB_FACTORY_MODE_BIT,
					EN_MICRO_USB_FACTORY_MODE_BIT);
	if (rc < 0) {
		smblib_err(chg, "Couldn't enable uUSB factory mode detection rc=%d\n",
			rc);
		goto out;
	}

	/*
	 * Add a delay of 100ms to allow change in rid to reflect on
	 * status registers.
	 */
	msleep(100);

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read USB_INT_RT_STS rc=%d\n", rc);
		goto out;
	}
	usb_plugged_in = (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);

	/* Check uUSB status for moisture presence */
	rc = smblib_read(chg, TYPEC_U_USB_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read TYPE_C_U_USB_STATUS_REG rc=%d\n",
				rc);
		goto out;
	}

	/*
	 * Factory mode detection happens in case of USB plugged-in by using
	 * a different current source of 2uA which can hamper moisture
	 * detection. Since factory mode is not supported in kernel, factory
	 * mode detection can be considered as equivalent to presence of
	 * moisture.
	 */
	if (stat == U_USB_STATUS_WATER_PRESENT || stat == U_USB_FMB1_BIT ||
			stat == U_USB_FMB2_BIT || (usb_plugged_in &&
			stat == U_USB_FLOAT1_BIT)) {
		smblib_set_moisture_protection(chg, true);
		alarm_start_relative(&chg->moisture_protection_alarm,
			ms_to_ktime(MOISTURE_PROTECTION_CHECK_DELAY_MS));
	} else {
		smblib_set_moisture_protection(chg, false);
		rc = alarm_cancel(&chg->moisture_protection_alarm);
		if (rc < 0)
			smblib_err(chg, "Couldn't cancel moisture protection alarm\n");
	}

out:
	vote(chg->awake_votable, MOISTURE_VOTER, false, 0);
	pm_relax(chg->dev);
}

static enum alarmtimer_restart moisture_protection_alarm_cb(struct alarm *alarm,
							ktime_t now)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
					moisture_protection_alarm);

	smblib_dbg(chg, PR_MISC, "moisture Protection Alarm Triggered %lld\n",
			ktime_to_ms(now));

	/* Atomic context, cannot use voter */
	pm_stay_awake(chg->dev);
	schedule_work(&chg->moisture_protection_work);

	return ALARMTIMER_NORESTART;
}

static void smblib_chg_termination_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						chg_termination_work);
	union power_supply_propval pval;
	int rc, delay = CHG_TERM_WA_ENTRY_DELAY_MS;

	/*
	 * Hold awake votable to prevent pm_relax being called prior to
	 * completion of this work.
	 */
	vote(chg->awake_votable, CHG_TERMINATION_VOTER, true, 0);

	rc = smblib_get_prop_usb_present(chg, &pval);
	if (rc < 0 || !pval.intval)
		goto out;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_REAL_CAPACITY, &pval);
	if (rc < 0 || (pval.intval < 100)) {
		vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
		goto out;
	}

	rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CHARGE_FULL,
					&pval);
	if (rc < 0)
		goto out;

	/*
	 * On change in the value of learned capacity, re-initialize the
	 * reference cc_soc value due to change in cc_soc characteristic value
	 * at full capacity. Also, in case cc_soc_ref value is reset,
	 * re-initialize it.
	 */
	if ((pval.intval != chg->charge_full_cc) || !chg->cc_soc_ref) {
		chg->charge_full_cc = pval.intval;
		rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CC_SOC,
					&pval);
		if (rc < 0)
			goto out;

		chg->cc_soc_ref = pval.intval;
	} else {
		rc = smblib_get_prop_from_bms(chg, POWER_SUPPLY_PROP_CC_SOC,
					&pval);
		if (rc < 0)
			goto out;
	}

	/*
	 * In BSM a sudden jump in CC_SOC is not expected. If seen, its a
	 * good_ocv or updated capacity, reject it.
	 */
	if (chg->last_cc_soc && pval.intval > (chg->last_cc_soc + 100)) {
		/* CC_SOC has increased by 1% from last time */
		chg->cc_soc_ref = pval.intval;
		smblib_dbg(chg, PR_MISC, "cc_soc jumped(%d->%d), reset cc_soc_ref\n",
				chg->last_cc_soc, pval.intval);
	}
	chg->last_cc_soc = pval.intval;

	/*
	 * Suspend/Unsuspend USB input to keep cc_soc within the 0.5% to 0.75%
	 * overshoot range of the cc_soc value at termination, to prevent
	 * overcharging.
	 */
	if (pval.intval < DIV_ROUND_CLOSEST(chg->cc_soc_ref * 10050, 10000)) {
		vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, false, 0);
		delay = CHG_TERM_WA_ENTRY_DELAY_MS;
	} else if (pval.intval > DIV_ROUND_CLOSEST(chg->cc_soc_ref * 10075,
								10000)) {
		vote(chg->usb_icl_votable, CHG_TERMINATION_VOTER, true, 0);
		delay = CHG_TERM_WA_EXIT_DELAY_MS;
	}

	smblib_dbg(chg, PR_MISC, "Chg Term WA readings: cc_soc: %d, cc_soc_ref: %d, delay: %d\n",
			pval.intval, chg->cc_soc_ref, delay);
	alarm_start_relative(&chg->chg_termination_alarm, ms_to_ktime(delay));
out:
	vote(chg->awake_votable, CHG_TERMINATION_VOTER, false, 0);
	pm_relax(chg->dev);
}

static enum alarmtimer_restart chg_termination_alarm_cb(struct alarm *alarm,
							ktime_t now)
{
	struct smb_charger *chg = container_of(alarm, struct smb_charger,
						chg_termination_alarm);

	smblib_dbg(chg, PR_MISC, "Charge termination WA alarm triggered %lld\n",
			ktime_to_ms(now));

	/* Atomic context, cannot use voter */
	pm_stay_awake(chg->dev);
	schedule_work(&chg->chg_termination_work);

	return ALARMTIMER_NORESTART;
}

#define JEITA_SOFT			0
#define JEITA_HARD			1
/* HS70 add for HS71-21 Optimize the stop and resume charging of battery temperature by gaochao at 2019/11/29 start */
#if !defined(HQ_FACTORY_BUILD)	//ss version
int smblib_update_jeita(struct smb_charger *chg, u32 *thresholds,
								int type)
#else
static int smblib_update_jeita(struct smb_charger *chg, u32 *thresholds,
								int type)
#endif
/* HS70 add for HS71-21 Optimize the stop and resume charging of battery temperature by gaochao at 2019/11/29 end */
{
	int rc;
	u16 temp, base;

	base = CHGR_JEITA_THRESHOLD_BASE_REG(type);

	temp = thresholds[1] & 0xFFFF;
	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg, base, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't configure Jeita %s hot threshold rc=%d\n",
			(type == JEITA_SOFT) ? "Soft" : "Hard", rc);
		return rc;
	}

	temp = thresholds[0] & 0xFFFF;
	temp = ((temp & 0xFF00) >> 8) | ((temp & 0xFF) << 8);
	rc = smblib_batch_write(chg, base + 2, (u8 *)&temp, 2);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't configure Jeita %s cold threshold rc=%d\n",
			(type == JEITA_SOFT) ? "Soft" : "Hard", rc);
		return rc;
	}

	smblib_dbg(chg, PR_MISC, "%s Jeita threshold configured\n",
				(type == JEITA_SOFT) ? "Soft" : "Hard");

	return 0;
}

static void jeita_update_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
						jeita_update_work);
	struct device_node *node = chg->dev->of_node;
	struct device_node *batt_node, *pnode;
	union power_supply_propval val;
	int rc;
	u32 jeita_thresholds[2];

	batt_node = of_find_node_by_name(node, "qcom,battery-data");
	if (!batt_node) {
		smblib_err(chg, "Batterydata not available\n");
		goto out;
	}

	/* if BMS is not ready, defer the work */
	if (!chg->bms_psy)
		return;

	rc = smblib_get_prop_from_bms(chg,
			POWER_SUPPLY_PROP_RESISTANCE_ID, &val);
	if (rc < 0) {
		smblib_err(chg, "Failed to get batt-id rc=%d\n", rc);
		goto out;
	}

	/* if BMS hasn't read out the batt_id yet, defer the work */
	if (val.intval <= 0)
		return;

	pnode = of_batterydata_get_best_profile(batt_node,
					val.intval / 1000, NULL);
	if (IS_ERR(pnode)) {
		rc = PTR_ERR(pnode);
		smblib_err(chg, "Failed to detect valid battery profile %d\n",
				rc);
		goto out;
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-hard-thresholds",
				jeita_thresholds, 2);
	if (!rc) {
		rc = smblib_update_jeita(chg, jeita_thresholds, JEITA_HARD);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure Hard Jeita rc=%d\n",
					rc);
			goto out;
		}
	}

	rc = of_property_read_u32_array(pnode, "qcom,jeita-soft-thresholds",
				jeita_thresholds, 2);
	if (!rc) {
		rc = smblib_update_jeita(chg, jeita_thresholds, JEITA_SOFT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't configure Soft Jeita rc=%d\n",
					rc);
			goto out;
		}
	}

out:
	chg->jeita_configured = true;
}

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
static void smblib_compliant_check_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work, struct smb_charger,
							compliant_check_work.work);
	int rc;
	u8 stat;

	rc = smblib_read(chg, QC_CHANGE_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read QC_CHANGE_STATUS_REG rc=%d\n",
					rc);
		return;
	}

	if (stat & QC_9V_BIT) {
		 rc = smblib_read(chg, AICL_STATUS_REG, &stat);
		 if (rc < 0) {
			 smblib_err(chg, "Couldn't read AICL_STATUS rc=%d\n", rc);
			 return;
		 }
		
		 if( (stat & USBIN_CH_COLLAPSE) && (stat & ICL_IMIN) && (!chg->qc2_unsupported_voltage)) 
		{
			non_compliant_chg_WA(chg);
			smblib_run_aicl(chg, RESTART_AICL);
			smblib_hvdcp_set_fsw(chg, QC_5V_BIT);
			rc = smblib_force_vbus_voltage(chg, FORCE_5V_BIT);
			if (rc < 0)
				pr_err("Failed to force 5V\n");
		}
	}
}
#endif
#endif

static int smblib_create_votables(struct smb_charger *chg)
{
	int rc = 0;

	chg->fcc_votable = find_votable("FCC");
	if (chg->fcc_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FCC votable rc=%d\n", rc);
		return rc;
	}

	chg->fv_votable = find_votable("FV");
	if (chg->fv_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find FV votable rc=%d\n", rc);
		return rc;
	}

	chg->usb_icl_votable = find_votable("USB_ICL");
	if (chg->usb_icl_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find USB_ICL votable rc=%d\n", rc);
		return rc;
	}

	chg->pl_disable_votable = find_votable("PL_DISABLE");
	if (chg->pl_disable_votable == NULL) {
		rc = -EINVAL;
		smblib_err(chg, "Couldn't find votable PL_DISABLE rc=%d\n", rc);
		return rc;
	}

	chg->pl_enable_votable_indirect = find_votable("PL_ENABLE_INDIRECT");
	if (chg->pl_enable_votable_indirect == NULL) {
		rc = -EINVAL;
		smblib_err(chg,
			"Couldn't find votable PL_ENABLE_INDIRECT rc=%d\n",
			rc);
		return rc;
	}

	vote(chg->pl_disable_votable, PL_DELAY_VOTER, true, 0);

	chg->dc_suspend_votable = create_votable("DC_SUSPEND", VOTE_SET_ANY,
					smblib_dc_suspend_vote_callback,
					chg);
	if (IS_ERR(chg->dc_suspend_votable)) {
		rc = PTR_ERR(chg->dc_suspend_votable);
		chg->dc_suspend_votable = NULL;
		return rc;
	}

	chg->awake_votable = create_votable("AWAKE", VOTE_SET_ANY,
					smblib_awake_vote_callback,
					chg);
	if (IS_ERR(chg->awake_votable)) {
		rc = PTR_ERR(chg->awake_votable);
		chg->awake_votable = NULL;
		return rc;
	}

	chg->chg_disable_votable = create_votable("CHG_DISABLE", VOTE_SET_ANY,
					smblib_chg_disable_vote_callback,
					chg);
	if (IS_ERR(chg->chg_disable_votable)) {
		rc = PTR_ERR(chg->chg_disable_votable);
		chg->chg_disable_votable = NULL;
		return rc;
	}

	chg->usb_irq_enable_votable = create_votable("USB_IRQ_DISABLE",
					VOTE_SET_ANY,
					smblib_usb_irq_enable_vote_callback,
					chg);
	if (IS_ERR(chg->usb_irq_enable_votable)) {
		rc = PTR_ERR(chg->usb_irq_enable_votable);
		chg->usb_irq_enable_votable = NULL;
		return rc;
	}

	return rc;
}

static void smblib_destroy_votables(struct smb_charger *chg)
{
	if (chg->dc_suspend_votable)
		destroy_votable(chg->dc_suspend_votable);
	if (chg->usb_icl_votable)
		destroy_votable(chg->usb_icl_votable);
	if (chg->awake_votable)
		destroy_votable(chg->awake_votable);
	if (chg->chg_disable_votable)
		destroy_votable(chg->chg_disable_votable);
}

int smblib_init(struct smb_charger *chg)
{
	int rc = 0;

	mutex_init(&chg->lock);
	INIT_WORK(&chg->bms_update_work, bms_update_work);
	INIT_WORK(&chg->pl_update_work, pl_update_work);
	INIT_WORK(&chg->jeita_update_work, jeita_update_work);
	INIT_DELAYED_WORK(&chg->clear_hdc_work, clear_hdc_work);
	INIT_DELAYED_WORK(&chg->icl_change_work, smblib_icl_change_work);
	INIT_DELAYED_WORK(&chg->pl_enable_work, smblib_pl_enable_work);
#ifdef CONFIG_USB_NOTIFY_LAYER
	INIT_DELAYED_WORK(&chg->microb_otg_work, smblib_microb_otg_work);
#endif
	INIT_DELAYED_WORK(&chg->uusb_otg_work, smblib_uusb_otg_work);
	INIT_DELAYED_WORK(&chg->bb_removal_work, smblib_bb_removal_work);
	INIT_DELAYED_WORK(&chg->usbov_dbc_work, smblib_usbov_dbc_work);
#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
	INIT_DELAYED_WORK(&chg->compliant_check_work, smblib_compliant_check_work);
#endif
#endif
	/* HS60 add for HS60-163 Set usb thermal by gaochao at 2019/07/30 start */
	INIT_DELAYED_WORK(&chg->usb_thermal_status_change_work, ss_usb_thermal_status_change_work);
	hq_usb_thermal_work_init_config(chg);
	/* HS60 add for HS60-163 Set usb thermal by gaochao at 2019/07/30 end */
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	INIT_DELAYED_WORK(&chg->retail_app_status_change_work, ss_retail_app_status_change_work);
	#endif
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 end */
	/* HS60 add for HS60-811 Set float charger by gaochao at 2019/08/27 start */
	INIT_DELAYED_WORK(&chg->float_charger_detect_work, hq_float_charger_detect_work);
	/* HS60 add for HS60-811 Set float charger by gaochao at 2019/08/27 end */
	/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 start */
	INIT_DELAYED_WORK(&chg->rerun_apsd_work, hq_rerun_apsd_work);
	/* HS60 add for ZQL1693-8 rerun apsd to identify charger type by gaochao at 2019/09/04 end */

	if (chg->wa_flags & CHG_TERMINATION_WA) {
		INIT_WORK(&chg->chg_termination_work,
					smblib_chg_termination_work);

		if (alarmtimer_get_rtcdev()) {
			alarm_init(&chg->chg_termination_alarm, ALARM_BOOTTIME,
						chg_termination_alarm_cb);
		} else {
			smblib_err(chg, "Couldn't get rtc device\n");
			return -ENODEV;
		}
	}

	if (chg->moisture_protection_enabled &&
				(chg->wa_flags & MOISTURE_PROTECTION_WA)) {
		INIT_WORK(&chg->moisture_protection_work,
					smblib_moisture_protection_work);

		if (alarmtimer_get_rtcdev()) {
			alarm_init(&chg->moisture_protection_alarm,
				ALARM_BOOTTIME, moisture_protection_alarm_cb);
		} else {
			smblib_err(chg, "Failed to initialize moisture protection alarm\n");
			return -ENODEV;
		}
	}

	/* HS60 add for SR-ZQL1695-01-358 Provide sysFS node named xxx/battery/batt_slate_mode by gaochao at 2019/08/29 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	chg->slate_mode = 0;
	#endif
	/* HS60 add for SR-ZQL1695-01-358 Provide sysFS node named xxx/battery/batt_slate_mode by gaochao at 2019/08/29 end */
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	chg->store_mode_ss = STORE_MODE_DISABLE_SS;
	#endif
	/* HS60 add for SR-ZQL1695-01-315 Provide sysFS node named /sys/class/power_supply/battery/store_mode for retail APP by gaochao at 2019/08/18 end */
	/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 start */
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	chg->boot_to_detect_charger = BOOT_TO_DETECT_INIT;
	#endif
	/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 end */
	/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 start*/
	#if !defined(HQ_FACTORY_BUILD)	//ss version
	chg->slow_charging_count = 0;
	#endif
	/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 end*/
	chg->fake_capacity = -EINVAL;
	chg->fake_input_current_limited = -EINVAL;
	chg->fake_batt_status = -EINVAL;
	chg->jeita_configured = false;
	chg->sink_src_mode = UNATTACHED_MODE;

	switch (chg->mode) {
	case PARALLEL_MASTER:
		rc = qcom_batt_init(chg->smb_version);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_batt_init rc=%d\n",
				rc);
			return rc;
		}

		rc = qcom_step_chg_init(chg->dev, chg->step_chg_enabled,
						chg->sw_jeita_enabled);
		if (rc < 0) {
			smblib_err(chg, "Couldn't init qcom_step_chg_init rc=%d\n",
				rc);
			return rc;
		}

		rc = smblib_create_votables(chg);
		if (rc < 0) {
			smblib_err(chg, "Couldn't create votables rc=%d\n",
				rc);
			return rc;
		}

		chg->bms_psy = power_supply_get_by_name("bms");
		chg->pl.psy = power_supply_get_by_name("parallel");
		if (chg->pl.psy) {
			rc = smblib_stat_sw_override_cfg(chg, false);
			if (rc < 0) {
				smblib_err(chg,
					"Couldn't config stat sw rc=%d\n", rc);
				return rc;
			}
		}
		rc = smblib_register_notifier(chg);
		if (rc < 0) {
			smblib_err(chg,
				"Couldn't register notifier rc=%d\n", rc);
			return rc;
		}
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	return rc;
}

int smblib_deinit(struct smb_charger *chg)
{
	switch (chg->mode) {
	case PARALLEL_MASTER:
		if (chg->moisture_protection_enabled &&
				(chg->wa_flags & MOISTURE_PROTECTION_WA)) {
			alarm_cancel(&chg->moisture_protection_alarm);
			cancel_work_sync(&chg->moisture_protection_work);
		}
		if (chg->wa_flags & CHG_TERMINATION_WA) {
			alarm_cancel(&chg->chg_termination_alarm);
			cancel_work_sync(&chg->chg_termination_work);
		}
		cancel_work_sync(&chg->bms_update_work);
		cancel_work_sync(&chg->jeita_update_work);
		cancel_work_sync(&chg->pl_update_work);
		cancel_delayed_work_sync(&chg->clear_hdc_work);
		cancel_delayed_work_sync(&chg->icl_change_work);
		cancel_delayed_work_sync(&chg->pl_enable_work);
#ifdef CONFIG_USB_NOTIFY_LAYER
		cancel_delayed_work_sync(&chg->microb_otg_work);
#endif
		cancel_delayed_work_sync(&chg->uusb_otg_work);
		cancel_delayed_work_sync(&chg->bb_removal_work);
		cancel_delayed_work_sync(&chg->usbov_dbc_work);
		/* HS70 add for HS70-135 Distinguish HS60 and HS70 charging by gaochao at 2019/10/08 start */
		cancel_delayed_work_sync(&chg->usb_thermal_status_change_work);
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		cancel_delayed_work_sync(&chg->retail_app_status_change_work);
		#endif
		cancel_delayed_work_sync(&chg->float_charger_detect_work);
		cancel_delayed_work_sync(&chg->rerun_apsd_work);
		/* HS70 add for HS70-135 Distinguish HS60 and HS70 charging by gaochao at 2019/10/08 end */
		/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 start */
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		chg->boot_to_detect_charger = BOOT_TO_DETECT_INIT;
		#endif
		/* HS70 add for HS70-565 Set ICL of float charger as 500mA by gaochao at 2019/12/20 end */
		/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 start*/
		#if !defined(HQ_FACTORY_BUILD)	//ss version
		chg->slow_charging_count = 0;
		#endif
		/*HS60 add for P200213-04659 Slow Charging Optimize by wangzikang at 2020/02/14 end*/
		power_supply_unreg_notifier(&chg->nb);
		smblib_destroy_votables(chg);
		qcom_step_chg_deinit();
		qcom_batt_deinit();
		break;
	case PARALLEL_SLAVE:
		break;
	default:
		smblib_err(chg, "Unsupported mode %d\n", chg->mode);
		return -EINVAL;
	}

	return 0;
}

#if !defined(HQ_FACTORY_BUILD)	//ss version
#if defined(CONFIG_AFC)
int is_afc_result(struct smb_charger *chg,int result)
{
	if (chg->real_charger_type != POWER_SUPPLY_TYPE_USB_DCP
			&& chg->real_charger_type != POWER_SUPPLY_TYPE_AFC) {
		smblib_err(chg, "cable is not DCP OR AFC %d\n", result);
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
		return 0;
	}
	smblib_err(chg, "is_afc_result = %d, before afc_sts(%d)\n", result, chg->afc_sts);
	chg->afc_sts = result;
	
	if ((result == NOT_AFC) || (result == AFC_FAIL))  {
		if(chg->real_charger_type == POWER_SUPPLY_TYPE_AFC){
			smblib_err(chg, "afc_set_voltage() failed\n");
			vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
		}
		else{
			smblib_err(chg, "AFC failed, re-enabling HVDCP\n");
			smblib_hvdcp_detect_enable(chg, true);
			vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
		}
	} else if (result == AFC_5V) {
		smblib_err(chg, "afc set to 5V\n");
		smblib_hvdcp_set_fsw(chg, QC_5V_BIT);
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, DCP_CURRENT_UA);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
	} else if (result == AFC_9V) {
		smblib_err(chg, "afc set to 9V\n");
		smblib_hvdcp_set_fsw(chg, QC_9V_BIT);
//		vote(chg->usb_icl_votable, HVDCP2_ICL_VOTER, false, 0);
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, true, AFC_CURRENT_UA);
		vote(chg->fcc_votable, SEC_BATTERY_AFC_VOTER, true, 2700000);
		vote(chg->usb_icl_votable, SW_ICL_MAX_VOTER, false, 0);
		/* HS50 add for SR-QL3095-01-67 Import default charger profile by wenyaqi at 2020/08/10 start */
		chg->is_dcp = false;
		pr_debug("is_dcp=%d\n", chg->is_dcp);
		/* HS50 add for SR-QL3095-01-67 Import default charger profile by wenyaqi at 2020/08/10 end */
	} else if (result == AFC_DISABLE) {
		smblib_err(chg, "afc disable\n");
		vote(chg->usb_icl_votable, SEC_BATTERY_AFC_VOTER, false, 0);
	}

	if (result >= AFC_5V)
		chg->real_charger_type = POWER_SUPPLY_TYPE_AFC;

	sec_bat_monitor_work(chg);
	
	return 0;
}
#endif
#endif
