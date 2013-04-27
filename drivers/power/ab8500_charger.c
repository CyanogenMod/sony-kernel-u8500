/*
 * Copyright (C) ST-Ericsson SA 2010
 * Copyright (C) 2012 Sony Mobile Communications AB.
 *
 * Charger driver for AB8500
 *
 * License Terms: GNU General Public License v2
 * Author: Johan Palsson <johan.palsson@stericsson.com>
 * Author: Karl Komierowski <karl.komierowski@stericsson.com>
 * Author: Arun R Murthy <arun.murthy@stericsson.com>
 * Author: Imre Sunyi <Imre.Sunyi@sonymobile.com>
 * Author: Sergii Kriachko <sergii.kriachko@sonymobile.com>
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/completion.h>
#include <linux/regulator/consumer.h>
#include <linux/err.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/mfd/ab8500.h>
#include <linux/mfd/abx500.h>
#include <linux/mfd/abx500/ab8500-bm.h>
#include <linux/mfd/abx500/ab8500-gpadc.h>
#include <linux/mfd/abx500/ux500_chargalg.h>
#include <linux/usb/otg.h>

/* Charger constants */
#define NO_PW_CONN			0
#define AC_PW_CONN			1
#define USB_PW_CONN			2

#define MAIN_WDOG_ENA			0x01
#define MAIN_WDOG_KICK			0x02
#define MAIN_WDOG_DIS			0x00
#define CHARG_WD_KICK			0x01
#define MAIN_CH_ENA			0x01
#define MAIN_CH_NO_OVERSHOOT_ENA_N	0x02
#define USB_CH_ENA			0x01
#define USB_CHG_NO_OVERSHOOT_ENA_N	0x02
#define MAIN_CH_DET			0x01
#define MAIN_CH_CV_ON			0x04
#define USB_CH_CV_ON			0x08
#define VBUS_DET_DBNC100		0x02
#define VBUS_DET_DBNC1			0x01
#define OTP_ENABLE_WD			0x01

#define MAIN_CH_INPUT_CURR_SHIFT	4
#define VBUS_IN_CURR_LIM_SHIFT		4
#define AUTO_VBUS_IN_CURR_LIM_SHIFT	4
#define VBUS_IN_CURR_LIM_RETRY_SET_TIME 30 /* seconds */
#define VBUS_IN_CURR_LIM_RETRY_MAX_TIME 3840 /* seconds */

#define LED_INDICATOR_PWM_ENA		0x01
#define LED_INDICATOR_PWM_DIS		0x00
#define LED_IND_CUR_5MA			0x04
#define LED_INDICATOR_PWM_DUTY_252_256	0xBF

/* HW failure constants */
#define MAIN_CH_TH_PROT			0x02
#define VBUS_CH_NOK			0x08
#define USB_CH_TH_PROT			0x02
#define VBUS_OVV_TH			0x01
#define MAIN_CH_NOK			0x01
#define VBUS_DET			0x80

/* UsbLineStatus register bit masks */
#define AB8500_USB_LINK_STATUS		0x78
#define AB8500_STD_HOST_SUSP		0x18

/* Watchdog timeout constant */
#define WD_TIMER			0x30 /* 4min */
#define WD_KICK_INTERVAL		(60 * HZ)

/* Lowest charger voltage is 3.39V -> 0x4E */
#define LOW_VOLT_REG			0x4E

/* Step up/down delay in ms */
#define STEP_MDELAY			1

/* Wait for enumeration before charing in us */
#define WAIT_ACA_RID_ENUMERATION	(5 * 1000)

/* This declaration should be in ab8599-bm.h
 - but put here just to be able to cherry-pick */
#define AB8500_OTP_NO_OF_REGS	0x10


/* UsbLineStatus register - usb types */
enum ab8500_charger_link_status {
	USB_STAT_NOT_CONFIGURED,
	USB_STAT_STD_HOST_NC,
	USB_STAT_STD_HOST_C_NS,
	USB_STAT_STD_HOST_C_S,
	USB_STAT_HOST_CHG_NM,
	USB_STAT_HOST_CHG_HS,
	USB_STAT_HOST_CHG_HS_CHIRP,
	USB_STAT_DEDICATED_CHG,
	USB_STAT_ACA_RID_A,
	USB_STAT_ACA_RID_B,
	USB_STAT_ACA_RID_C_NM,
	USB_STAT_ACA_RID_C_HS,
	USB_STAT_ACA_RID_C_HS_CHIRP,
	USB_STAT_HM_IDGND,
	USB_STAT_RESERVED,
	USB_STAT_NOT_VALID_LINK,
};

enum ab8500_usb_state {
	AB8500_BM_USB_STATE_RESET_HS,	/* HighSpeed Reset */
	AB8500_BM_USB_STATE_RESET_FS,	/* FullSpeed/LowSpeed Reset */
	AB8500_BM_USB_STATE_CONFIGURED,
	AB8500_BM_USB_STATE_SUSPEND,
	AB8500_BM_USB_STATE_RESUME,
	AB8500_BM_USB_STATE_MAX,
};

/* VBUS input current limits supported in AB8500 in mA */
#define USB_CH_IP_CUR_LVL_0P05		50
#define USB_CH_IP_CUR_LVL_0P09		98
#define USB_CH_IP_CUR_LVL_0P19		193
#define USB_CH_IP_CUR_LVL_0P29		290
#define USB_CH_IP_CUR_LVL_0P38		380
#define USB_CH_IP_CUR_LVL_0P45		450
#define USB_CH_IP_CUR_LVL_0P5		500
#define USB_CH_IP_CUR_LVL_0P6		600
#define USB_CH_IP_CUR_LVL_0P7		700
#define USB_CH_IP_CUR_LVL_0P8		800
#define USB_CH_IP_CUR_LVL_0P9		900
#define USB_CH_IP_CUR_LVL_1P0		1000
#define USB_CH_IP_CUR_LVL_1P1		1100
#define USB_CH_IP_CUR_LVL_1P3		1300
#define USB_CH_IP_CUR_LVL_1P4		1400
#define USB_CH_IP_CUR_LVL_1P5		1500

#define VBAT_TRESH_IP_CUR_RED		3800

#define to_ab8500_charger_usb_device_info(x) container_of((x), \
	struct ab8500_charger, usb_chg)
#define to_ab8500_charger_ac_device_info(x) container_of((x), \
	struct ab8500_charger, ac_chg)

/**
 * struct ab8500_charger_interrupts - ab8500 interupts
 * @name:	name of the interrupt
 * @isr		function pointer to the isr
 */
struct ab8500_charger_interrupts {
	char *name;
	irqreturn_t (*isr)(int irq, void *data);
};

struct ab8500_charger_info {
	int charger_connected;
	int charger_online;
	int charger_voltage;
	int cv_active;
	bool wd_expired;
};

struct ab8500_charger_event_flags {
	bool mainextchnotok;
	bool main_thermal_prot;
	bool usb_thermal_prot;
	bool vbus_ovv;
	bool usbchargernotok;
	bool chgwdexp;
	bool vbus_collapse;
	bool vbus_drop_end;
	bool report_charger_no_charge;
};

struct ab8500_charger_usb_state {
	int usb_current;
	int usb_current_tmp;
	enum ab8500_usb_state state;
	enum ab8500_usb_state state_tmp;
	spinlock_t usb_lock;
};

/**
 * struct ab8500_vbus_drop - ab8500 VBUS drop handling
 * @real_max_usb_in_curr:	The real maximum USB charger input current
 * @retry_current_time:		Time to retry to set current to maximum (secs)
 * @work_expire:		When work is about to expire (jiffies)
 * @end_work:			Work for detecting VBUS drop end
 */
struct ab8500_vbus_drop {
	int real_max_usb_in_curr[2];
	unsigned int retry_current_time;
	unsigned long work_expire;
	struct delayed_work end_work;
};

/**
 * struct ab8500_charger - ab8500 Charger device information
 * @dev:		Pointer to the structure device
 * @cpu:		The cpu to get the time from
 * @max_usb_in_curr:	Max USB charger input current
 * @vbus_detected:	VBUS detected
 * @vbus_detected_start:
 *			VBUS detected during startup
 * @ac_conn:		This will be true when the AC charger has been plugged
 * @vddadc_en_ac:	Indicate if VDD ADC supply is enabled because AC
 *			charger is enabled
 * @vddadc_en_usb:	Indicate if VDD ADC supply is enabled because USB
 *			charger is enabled
 * @vbat		Battery voltage
 * @old_vbat		Previously measured battery voltage
 * @autopower		Indicate if we should have automatic pwron after pwrloss
 * @invalid_charger_detect_state:
			State when forcing AB to use invalid charger
 * @is_usb_host:	Indicate if last detected USB type is host
 * @is_aca_rid:		Incicate if accessory is ACA type
 * @current_stepping_sessions:
 *			Counter for current stepping sessions
 * @parent:		Pointer to the struct ab8500
 * @gpadc:		Pointer to the struct gpadc
 * @pdata:		Pointer to the ab8500_charger platform data
 * @bat:		Pointer to the ab8500_bm platform data
 * @flags:		Structure for information about events triggered
 * @usb_state:		Structure for usb stack information
 * @ac_chg:		AC charger power supply
 * @usb_chg:		USB charger power supply
 * @ac:			Structure that holds the AC charger properties
 * @usb:		Structure that holds the USB charger properties
 * @vbus_drop:		Structure that holds the VBUS drop properties
 * @regu:		Pointer to the struct regulator
 * @charger_wq:		Work queue for the IRQs and checking HW state
 * @usb_ipt_crnt_lock:	Lock to protect VBUS input current setting from mutuals
 * @current_stepping_sessions_lock:
 *			Lock to protect current stepping session counter
 * @pm_lock:		Lock to prevent system to suspend
 * @check_vbat_work	Work for checking vbat threshold to adjust vbus current
 * @check_hw_failure_work:	Work for checking HW state
 * @check_usbchgnotok_work:	Work for checking USB charger not ok status
 * @kick_wd_work:		Work for kicking the charger watchdog in case
 *				of ABB rev 1.* due to the watchog logic bug
 * @ac_work:			Work for checking AC charger connection
 * @detect_usb_type_work:	Work for detecting the USB type connected
 * @usb_link_status_work:	Work for checking the new USB link status
 * @usb_state_changed_work:	Work for checking USB state
 * @attach_work:		Work for detecting USB type
 * @check_main_thermal_prot_work:
 *				Work for checking Main thermal status
 * @check_usb_thermal_prot_work:
 *				Work for checking USB thermal status
 */
struct ab8500_charger {
	struct device *dev;
	int cpu;
	int max_usb_in_curr;
	bool vbus_detected;
	bool vbus_detected_start;
	bool ac_conn;
	bool vddadc_en_ac;
	bool vddadc_en_usb;
	int vbat;
	int old_vbat;
	bool autopower;
	int invalid_charger_detect_state;
	bool is_usb_host;
	int is_aca_rid;
	int current_stepping_sessions;
	struct ab8500 *parent;
	struct ab8500_gpadc *gpadc;
	struct ab8500_charger_platform_data *pdata;
	struct ab8500_bm_data *bat;
	struct ab8500_charger_event_flags flags;
	struct ab8500_charger_usb_state usb_state;
	struct ux500_charger ac_chg;
	struct ux500_charger usb_chg;
	struct ab8500_charger_info ac;
	struct ab8500_charger_info usb;
	struct ab8500_vbus_drop vbus_drop;
	struct regulator *regu;
	struct workqueue_struct *charger_wq;
	struct mutex usb_ipt_crnt_lock;
	struct mutex current_stepping_sessions_lock;
	struct wake_lock pm_lock;
	struct delayed_work check_vbat_work;
	struct delayed_work check_hw_failure_work;
	struct delayed_work check_usbchgnotok_work;
	struct delayed_work kick_wd_work;
	struct delayed_work usb_state_changed_work;
	struct delayed_work attach_work;
	struct work_struct ac_work;
	struct work_struct detect_usb_type_work;
	struct work_struct usb_link_status_work;
	struct work_struct check_main_thermal_prot_work;
	struct work_struct check_usb_thermal_prot_work;
	struct otg_transceiver *otg;
	struct notifier_block nb;
};


/* USB properties */
static enum power_supply_property ab8500_charger_usb_props[] = {
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
};

static struct timespec ab8500_charger_get_time(struct ab8500_charger *di)
{
	unsigned long long ctime;
	struct timespec time;

	ctime = cpu_clock(di->cpu);
	time.tv_nsec = sector_div(ctime, NSEC_PER_SEC);
	time.tv_sec = (__kernel_time_t)ctime;
	return time;
}

/*
 * Function for enabling and disabling sw fallback mode
 * should always be disabled when no charger is connected.
 */
static void ab8500_enable_disable_sw_fallback(struct ab8500_charger *di,
		bool fallback)
{
	u8 reg;
	int ret;

	dev_dbg(di->dev, "SW Fallback: %d\n", fallback);

	/* read the register containing fallback bit */
	ret = abx500_get_register_interruptible(di->dev, 0x15, 0x00, &reg);
	if (ret) {
		dev_err(di->dev, "%d write failed\n", __LINE__);
		return;
	}

	/* enable the OPT emulation registers */
	ret = abx500_set_register_interruptible(di->dev, 0x11, 0x00, 0x2);
	if (ret) {
		dev_err(di->dev, "%d write failed\n", __LINE__);
		return;
	}

	if (fallback)
		reg |= 0x8;
	else
		reg &= ~0x8;

	/* write back the changed fallback bit value to register */
	ret = abx500_set_register_interruptible(di->dev, 0x15, 0x00, reg);
	if (ret) {
		dev_err(di->dev, "%d write failed\n", __LINE__);
		return;
	}

	/* disable the set OTP registers again */
	ret = abx500_set_register_interruptible(di->dev, 0x11, 0x00, 0x0);
	if (ret) {
		dev_err(di->dev, "%d write failed\n", __LINE__);
		return;
	}
}

/**
 * ab8500_power_supply_changed - a wrapper with local extentions for
 * power_supply_changed
 * @di:	  pointer to the ab8500_charger structure
 * @psy:  pointer to power_supply_that have changed.
 *
 */
static void ab8500_power_supply_changed(struct ab8500_charger *di,
					struct power_supply *psy)
{
	if (di->pdata->autopower_cfg) {
		if (!di->usb.charger_connected &&
		    !di->ac.charger_connected &&
		    di->autopower) {
			di->autopower = false;
			ab8500_enable_disable_sw_fallback(di, false);
		} else if (!di->autopower &&
			   (di->ac.charger_connected ||
			    di->usb.charger_connected)) {
			di->autopower = true;
			ab8500_enable_disable_sw_fallback(di, true);
		}
	}
	power_supply_changed(psy);
}

static void ab8500_charger_set_usb_connected(struct ab8500_charger *di,
	bool connected)
{
	if (connected != di->usb.charger_connected) {
		di->usb.charger_connected = connected;
		dev_dbg(di->dev, "%s connected %x\n", __func__, connected);

		if (!connected) {
			memset(di->vbus_drop.real_max_usb_in_curr, 0,
			       sizeof(di->vbus_drop.real_max_usb_in_curr));
			di->flags.vbus_drop_end = false;
			di->vbus_drop.retry_current_time =
				VBUS_IN_CURR_LIM_RETRY_SET_TIME;
			di->is_usb_host = false;
			di->is_aca_rid = 0;
			di->flags.report_charger_no_charge = false;
		}

		if (di->is_usb_host)
			sysfs_notify(&di->usb_chg.psy.dev->kobj, NULL,
				     "present");
		else
			sysfs_notify(&di->ac_chg.psy.dev->kobj, NULL,
				     "present");
	}
}

static void ab8500_charger_psy_changed(struct ab8500_charger *di)
{
	if (di->is_usb_host)
		power_supply_changed(&di->usb_chg.psy);
	else
		power_supply_changed(&di->ac_chg.psy);
}

/**
 * ab8500_charger_get_vbus_voltage() - get vbus voltage
 * @di:		pointer to the ab8500_charger structure
 *
 * This function returns the vbus voltage.
 * Returns vbus voltage (on success)
 */
static int ab8500_charger_get_vbus_voltage(struct ab8500_charger *di)
{
	int vch;

	/* Only measure voltage if the charger is connected */
	if (di->usb.charger_connected) {
		vch = ab8500_gpadc_convert(di->gpadc, VBUS_V);
		if (vch < 0)
			dev_err(di->dev, "%s gpadc conv failed\n", __func__);
	} else {
		vch = 0;
	}
	return vch;
}

/**
 * ab8500_charger_get_usb_current() - get usb charger current
 * @di:		pointer to the ab8500_charger structure
 *
 * This function returns the usb charger current.
 * Returns usb current (on success) and error code on failure
 */
static int ab8500_charger_get_usb_current(struct ab8500_charger *di)
{
	int ich;

	/* Only measure current if the charger is online */
	if (di->usb.charger_online) {
		ich = ab8500_gpadc_convert(di->gpadc, USB_CHARGER_C);
		if (ich < 0)
			dev_err(di->dev, "%s gpadc conv failed\n", __func__);
	} else {
		ich = 0;
	}
	return ich;
}


/**
 * ab8500_charger_usb_cv() - check if the usb charger is in CV mode
 * @di:		pointer to the ab8500_charger structure
 *
 * Returns ac charger CV mode (on success) else error code
 */
static int ab8500_charger_usb_cv(struct ab8500_charger *di)
{
	int ret;
	u8 val;

	/* Only check CV mode if the charger is online */
	if (di->usb.charger_online) {
		ret = abx500_get_register_interruptible(di->dev, AB8500_CHARGER,
			AB8500_CH_USBCH_STAT1_REG, &val);
		if (ret < 0) {
			dev_err(di->dev, "%s ab8500 read failed\n", __func__);
			return 0;
		}

		if (val & USB_CH_CV_ON)
			ret = 1;
		else
			ret = 0;
	} else {
		ret = 0;
	}

	return ret;
}

/**
 * ab8500_charger_detect_chargers() - Detect the connected chargers
 * @di:		pointer to the ab8500_charger structure
 * @probe:	if probe, don't delay and wait for HW
 *
 * Returns the type of charger connected.
 * For USB it will not mean we can actually charge from it
 * but that there is a USB cable connected that we have to
 * identify. This is used during startup when we don't get
 * interrupts of the charger detection
 *
 * Returns an integer value, that means,
 * NO_PW_CONN  no power supply is connected
 * AC_PW_CONN  if the AC power supply is connected
 * USB_PW_CONN  if the USB power supply is connected
 * AC_PW_CONN + USB_PW_CONN if USB and AC power supplies are both connected
 */
static int ab8500_charger_detect_chargers(struct ab8500_charger *di, bool probe)
{
	int result = NO_PW_CONN;
	int ret;
	u8 val;

	if (!probe) {
		/* AB8500 says VBUS_DET_DBNC1 & VBUS_DET_DBNC100
		 * when disconnecting ACA even though no
		 * charger was connected. Try waiting a little
		 * longer than the 100 ms of VBUS_DET_DBNC100...
		 */
		msleep(110);
	}
	/* Check for USB charger */
	ret = abx500_get_register_interruptible(di->dev, AB8500_CHARGER,
		AB8500_CH_USBCH_STAT1_REG, &val);
	if (ret < 0) {
		dev_err(di->dev, "%s ab8500 read failed\n", __func__);
		return ret;
	}
	dev_dbg(di->dev,
		"%s AB8500_CH_USBCH_STAT1_REG %x\n", __func__,
		val);
	if ((val & VBUS_DET_DBNC1) && (val & VBUS_DET_DBNC100))
		result |= USB_PW_CONN;

	return result;
}


/**
 * ab8500_charger_max_usb_curr() - get the max curr for the USB type
 * @di:			pointer to the ab8500_charger structure
 * @link_status:	the identified USB type
 *
 * Get the maximum current that is allowed to be drawn from the host
 * based on the USB type.
 * Returns error code in case of failure else 0 on success
 */
static int ab8500_charger_max_usb_curr(struct ab8500_charger *di,
	enum ab8500_charger_link_status link_status)
{
	int ret = 0;
	int vbusv;
	u8 val;

	/* Platform only supports USB 2.0.
	 * This means that charging current from USB source
	 * is maximum 500 mA. Every occurence of USB_STAT_*_HOST_*
	 * should set USB_CH_IP_CUR_LVL_0P5.
	 */

	switch (link_status) {
	case USB_STAT_STD_HOST_NC:
	case USB_STAT_STD_HOST_C_NS:
	case USB_STAT_STD_HOST_C_S:
		dev_dbg(di->dev, "USB Type - Standard host is "
			"detected through USB driver\n");
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P5;
		di->is_usb_host = true;
		di->is_aca_rid = 0;
		break;
	case USB_STAT_HOST_CHG_HS_CHIRP:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P5;
		di->is_usb_host = true;
		di->is_aca_rid = 0;
		break;
	case USB_STAT_HOST_CHG_HS:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P5;
		di->is_usb_host = true;
		di->is_aca_rid = 0;
		break;
	case USB_STAT_ACA_RID_C_HS:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P9;
		di->is_usb_host = false;
		di->is_aca_rid = 0;
		break;
	case USB_STAT_ACA_RID_A:
		/*
		 * Dedicated charger level minus maximum current accessory
		 * can consume (900mA). Closest level is 500mA
		 */
		dev_dbg(di->dev, "USB_STAT_ACA_RID_A detected\n");
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P5;
		di->is_usb_host = false;
		di->is_aca_rid = 1;
		break;
	case USB_STAT_ACA_RID_B:
		/*
		 * Dedicated charger level minus 120mA (20mA for ACA and
		 * 100mA for potential accessory). Closest level is 1300mA
		 */
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_1P3;
		di->is_usb_host = false;
		di->is_aca_rid = 1;
		break;
	case USB_STAT_HOST_CHG_NM:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P5;
		di->is_usb_host = true;
		di->is_aca_rid = 0;
		break;
	case USB_STAT_DEDICATED_CHG:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_1P5;
		di->is_usb_host = false;
		di->is_aca_rid = 0;
		break;
	case USB_STAT_ACA_RID_C_HS_CHIRP:
	case USB_STAT_ACA_RID_C_NM:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_1P5;
		di->is_usb_host = false;
		di->is_aca_rid = 1;
		break;
	case USB_STAT_RESERVED:
		/*
		 * This state is used to indicate that VBUS has dropped below
		 * the detection level 4 times in a row. This is due to the
		 * charger output current is set to high making the charger
		 * voltage collapse. This have to be propagated through to
		 * chargalg. This is done using the property
		 * POWER_SUPPLY_PROP_CURRENT_AVG = 1
		 */

		vbusv =  ab8500_charger_get_vbus_voltage(di);
		dev_dbg(di->dev, "Vbus collapsed, measuring vbus"
			" voltage %d mV\n", vbusv);

		ret = abx500_get_register_interruptible(di->dev,
			0x02, AB8500_MAIN_WDOG_CTRL_REG, &val);
		dev_dbg(di->dev, "Read reg 0x0201 0x%02x [ret: %d]\n",
			val, ret);

		/* Disable the charger by SW: @0x0BC0 0x02 */
		ret = abx500_mask_and_set_register_interruptible(
			di->dev, AB8500_CHARGER, AB8500_USBCH_CTRL1_REG,
			0x03, 0x02);
		dev_dbg(di->dev, "Disabling charger [ret: %d]\n", ret);

		/* Reset the drop counter: @0x0B56 0x01 */
		ret = abx500_set_register_interruptible(di->dev,
			AB8500_CHARGER, AB8500_CHARGER_CTRL, 0x01);
		dev_dbg(di->dev, "Resetting drop counter [ret: %d]\n",
			ret);

		/* Re enable charger  by SW: @0x0BC0 0x03 */
		ret = abx500_mask_and_set_register_interruptible(
			di->dev, AB8500_CHARGER, AB8500_USBCH_CTRL1_REG,
			0x03, 0x03);
		dev_dbg(di->dev, "Re-enabling charger [ret: %d]\n",
			ret);

		/* Check @0x0B02 charger is ON */
		ret = abx500_get_register_interruptible(di->dev,
			AB8500_CHARGER, 0x02, &val);
		dev_dbg(di->dev, "Check if charger is on: "
			" 0x%02x [ret: %d]\n", val, ret);

		if (val & 0x04) {
			dev_dbg(di->dev, "Successfully recovered from"
			" VBUS collapse 0x%02x [ret: %d]\n", val, ret);
			di->flags.vbus_collapse = false;
			ret = 0;
		} else {
			di->flags.vbus_collapse = true;
			dev_dbg(di->dev, "USB Type - USB_STAT_RESERVED "
				"VBUS has collapsed\n");
			ret = -EBUSY;
		}
		break;
	case USB_STAT_NOT_VALID_LINK:
		dev_err(di->dev, "USB Type invalid - try charging anyway\n");
		/* Intentional fall through */
	case USB_STAT_NOT_CONFIGURED:
		/*
		 * USB chargers with out-of-spec D+D- resistance can be
		 * supported by setting the maximum allowed current
		 * for not configured chargers
		 */
		if (di->bat->chg_params->usb_curr_max_nc) {
			di->max_usb_in_curr =
				di->bat->chg_params->usb_curr_max_nc;
			break;
		}
		/* Intentional fallthrogh */
	case USB_STAT_HM_IDGND:
		dev_err(di->dev, "USB Type - Charging not allowed\n");
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P05;
		ret = -ENXIO;
		break;
	default:
		dev_err(di->dev, "USB Type - Unknown\n");
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P05;
		ret = -ENXIO;
		break;
	};

	di->vbus_drop.real_max_usb_in_curr[0] = di->max_usb_in_curr;
	dev_dbg(di->dev, "USB Type - 0x%02x MaxCurr: %d",
		link_status, di->max_usb_in_curr);

	return ret;
}

/**
 * ab8500_charger_read_usb_type() - read the type of usb connected
 * @di:		pointer to the ab8500_charger structure
 *
 * Detect the type of the plugged USB
 * Returns error code in case of failure else 0 on success
 */
static int ab8500_charger_read_usb_type(struct ab8500_charger *di)
{
	int ret;
	u8 val;

	ret = abx500_get_register_interruptible(di->dev,
		AB8500_INTERRUPT, AB8500_IT_SOURCE21_REG, &val);
	if (ret < 0) {
		dev_err(di->dev, "%s ab8500 read failed\n", __func__);
		return ret;
	}
	ret = abx500_get_register_interruptible(di->dev, AB8500_USB,
		AB8500_USB_LINE_STAT_REG, &val);
	if (ret < 0) {
		dev_err(di->dev, "%s ab8500 read failed\n", __func__);
		return ret;
	}

	/* get the USB type */
	val = (val & AB8500_USB_LINK_STATUS) >> 3;
	ret = ab8500_charger_max_usb_curr(di,
		(enum ab8500_charger_link_status) val);

	return ret;
}

/**
 * ab8500_charger_detect_usb_type() - get the type of usb connected
 * @di:		pointer to the ab8500_charger structure
 *
 * Detect the type of the plugged USB
 * Returns error code in case of failure else 0 on success
 */
static int ab8500_charger_detect_usb_type(struct ab8500_charger *di)
{
	int i, ret;
	u8 val;

	/*
	 * On getting the VBUS rising edge detect interrupt there
	 * is a 250ms delay after which the register UsbLineStatus
	 * is filled with valid data.
	 */
	for (i = 0; i < 10; i++) {
		msleep(250);
		ret = abx500_get_register_interruptible(di->dev,
			AB8500_INTERRUPT, AB8500_IT_SOURCE21_REG,
			&val);
		dev_dbg(di->dev, "%s AB8500_IT_SOURCE21_REG %x\n",
			__func__, val);
		if (ret < 0) {
			dev_err(di->dev, "%s ab8500 read failed\n", __func__);
			return ret;
		}
		ret = abx500_get_register_interruptible(di->dev, AB8500_USB,
			AB8500_USB_LINE_STAT_REG, &val);
		if (ret < 0) {
			dev_err(di->dev, "%s ab8500 read failed\n", __func__);
			return ret;
		}
		dev_dbg(di->dev, "%s AB8500_USB_LINE_STAT_REG %x\n", __func__,
			val);
		/*
		 * Until the IT source register is read the UsbLineStatus
		 * register is not updated, hence doing the same
		 * Revisit this:
		 */

		/* get the USB type */
		val = (val & AB8500_USB_LINK_STATUS) >> 3;
		if (val)
			break;
	}
	ret = ab8500_charger_max_usb_curr(di,
		(enum ab8500_charger_link_status) val);

	return ret;
}

/*
 * This array maps the raw hex value to charger voltage used by the AB8500
 * Values taken from the UM0836
 */
static int ab8500_charger_voltage_map[] = {
	3500 ,
	3525 ,
	3550 ,
	3575 ,
	3600 ,
	3625 ,
	3650 ,
	3675 ,
	3700 ,
	3725 ,
	3750 ,
	3775 ,
	3800 ,
	3825 ,
	3850 ,
	3875 ,
	3900 ,
	3925 ,
	3950 ,
	3975 ,
	4000 ,
	4025 ,
	4050 ,
	4060 ,
	4070 ,
	4080 ,
	4090 ,
	4100 ,
	4110 ,
	4120 ,
	4130 ,
	4140 ,
	4150 ,
	4160 ,
	4170 ,
	4180 ,
	4190 ,
	4200 ,
	4210 ,
	4220 ,
	4230 ,
	4240 ,
	4250 ,
	4260 ,
	4270 ,
	4280 ,
	4290 ,
	4300 ,
	4310 ,
	4320 ,
	4330 ,
	4340 ,
	4350 ,
	4360 ,
	4370 ,
	4380 ,
	4390 ,
	4400 ,
	4410 ,
	4420 ,
	4430 ,
	4440 ,
	4450 ,
	4460 ,
	4470 ,
	4480 ,
	4490 ,
	4500 ,
	4510 ,
	4520 ,
	4530 ,
	4540 ,
	4550 ,
	4560 ,
	4570 ,
	4580 ,
	4590 ,
	4600 ,
};

/*
 * This array maps the raw hex value to charger current used by the AB8500
 * Values taken from the UM0836
 */
static int ab8500_charger_current_map[] = {
	100 ,
	200 ,
	300 ,
	400 ,
	500 ,
	600 ,
	700 ,
	800 ,
	900 ,
	1000 ,
	1100 ,
	1200 ,
	1300 ,
	1400 ,
	1500 ,
};

/*
 * This array maps the raw hex value to VBUS input current used by the AB8500
 * Values taken from the UM0836
 */
static int ab8500_charger_vbus_in_curr_map[] = {
	USB_CH_IP_CUR_LVL_0P05,
	USB_CH_IP_CUR_LVL_0P09,
	USB_CH_IP_CUR_LVL_0P19,
	USB_CH_IP_CUR_LVL_0P29,
	USB_CH_IP_CUR_LVL_0P38,
	USB_CH_IP_CUR_LVL_0P45,
	USB_CH_IP_CUR_LVL_0P5,
	USB_CH_IP_CUR_LVL_0P6,
	USB_CH_IP_CUR_LVL_0P7,
	USB_CH_IP_CUR_LVL_0P8,
	USB_CH_IP_CUR_LVL_0P9,
	USB_CH_IP_CUR_LVL_1P0,
	USB_CH_IP_CUR_LVL_1P1,
	USB_CH_IP_CUR_LVL_1P3,
	USB_CH_IP_CUR_LVL_1P4,
	USB_CH_IP_CUR_LVL_1P5,
};

static int ab8500_voltage_to_regval(int voltage)
{
	int i;

	/* Special case for voltage below 3.5V */
	if (voltage < ab8500_charger_voltage_map[0])
		return LOW_VOLT_REG;

	for (i = 1; i < ARRAY_SIZE(ab8500_charger_voltage_map); i++) {
		if (voltage < ab8500_charger_voltage_map[i])
			return i - 1;
	}

	/* If not last element, return error */
	i = ARRAY_SIZE(ab8500_charger_voltage_map) - 1;
	if (voltage == ab8500_charger_voltage_map[i])
		return i;
	else
		return -1;
}

static int ab8500_current_to_regval(int curr)
{
	int i;

	if (curr < ab8500_charger_current_map[0])
		return 0;

	for (i = 0; i < ARRAY_SIZE(ab8500_charger_current_map); i++) {
		if (curr < ab8500_charger_current_map[i])
			return i - 1;
	}

	/* If not last element, return error */
	i = ARRAY_SIZE(ab8500_charger_current_map) - 1;
	if (curr == ab8500_charger_current_map[i])
		return i;
	else
		return -1;
}

static int ab8500_vbus_in_curr_to_regval(int curr)
{
	int i;

	if (curr < ab8500_charger_vbus_in_curr_map[0])
		return 0;

	for (i = 0; i < ARRAY_SIZE(ab8500_charger_vbus_in_curr_map); i++) {
		if (curr < ab8500_charger_vbus_in_curr_map[i])
			return i - 1;
	}

	/* If not last element, return error */
	i = ARRAY_SIZE(ab8500_charger_vbus_in_curr_map) - 1;
	if (curr == ab8500_charger_vbus_in_curr_map[i])
		return i;
	else
		return -1;
}

/**
 * ab8500_charger_get_usb_cur() - get usb current
 * @di:		pointer to the ab8500_charger structre
 *
 * The usb stack provides the maximum current that can be drawn from
 * the standard usb host. This will be in mA.
 * This function converts current in mA to a value that can be written
 * to the register. Returns -1 if charging is not allowed
 */
static int ab8500_charger_get_usb_cur(struct ab8500_charger *di)
{
	int ret = 0;

	switch (di->usb_state.usb_current) {
	case 100:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P09;
		break;
	case 200:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P19;
		break;
	case 300:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P29;
		break;
	case 400:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P38;
		break;
	case 500:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P5;
		break;
	default:
		di->max_usb_in_curr = USB_CH_IP_CUR_LVL_0P05;
		ret = -EPERM;
		break;
	};

	di->vbus_drop.real_max_usb_in_curr[0] = di->max_usb_in_curr;
	return ret;
}

/**
 * ab8500_charger_check_continue_stepping() - Check to allow stepping
 * @di:		pointer to the ab8500_charger structure
 * @reg:	select what charger register to check
 *
 * Check if current stepping should be allowed to continue.
 * Checks if charger source has not collapsed. If it has, further stepping
 * is not allowed.
 */
static bool ab8500_charger_check_continue_stepping(struct ab8500_charger *di,
						   int reg)
{
	bool allow = true;

	switch (reg) {
	case AB8500_USBCH_IPT_CRNTLVL_REG:
		allow = !di->flags.vbus_drop_end;
		break;
	default:
		break;
	}

	return allow;
}

/**
 * ab8500_charger_set_current() - set charger current
 * @di:		pointer to the ab8500_charger structure
 * @ich:	charger current, in mA
 * @reg:	select what charger register to set
 *
 * Set charger current.
 * There is no state machine in the AB to step up/down the charger
 * current to avoid dips and spikes on MAIN, VBUS and VBAT when
 * charging is started. Instead we need to implement
 * this charger current step-up/down here.
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_set_current(struct ab8500_charger *di,
	int ich, int reg)
{
	int ret = 0;
	int curr_index, prev_curr_index, shift_value, i;
	u8 reg_value;
	u32 step_mdelay;
	bool no_stepping = false;

	mutex_lock(&di->current_stepping_sessions_lock);
	di->current_stepping_sessions++;
	mutex_unlock(&di->current_stepping_sessions_lock);

	ret = abx500_get_register_interruptible(di->dev, AB8500_CHARGER,
		reg, &reg_value);
	if (ret < 0) {
		dev_err(di->dev, "%s read failed\n", __func__);
		goto exit_set_current;
	}

	switch (reg) {
	case AB8500_MCH_IPT_CURLVL_REG:
		shift_value = MAIN_CH_INPUT_CURR_SHIFT;
		prev_curr_index = (reg_value >> shift_value);
		curr_index = ab8500_current_to_regval(ich);
		step_mdelay = STEP_MDELAY;
		if (!di->ac.charger_connected)
			no_stepping = true;
		break;
	case AB8500_USBCH_IPT_CRNTLVL_REG:
		shift_value = VBUS_IN_CURR_LIM_SHIFT;
		prev_curr_index = (reg_value >> shift_value);
		curr_index = ab8500_vbus_in_curr_to_regval(ich);
		step_mdelay = STEP_MDELAY * 10;

		ret = abx500_get_register_interruptible(di->dev, AB8500_CHARGER,
					AB8500_CH_USBCH_STAT2_REG, &reg_value);
		if (ret < 0) {
			dev_err(di->dev, "%s read failed\n", __func__);
			goto exit_set_current;
		} else {
			reg_value >>= AUTO_VBUS_IN_CURR_LIM_SHIFT;

			dev_dbg(di->dev, "%s Auto VBUS curr is %d mA\n",
				__func__,
			ab8500_charger_vbus_in_curr_map[reg_value]);

			prev_curr_index =
				min_t(int, prev_curr_index, reg_value);
		}

		if (!di->usb.charger_connected)
			no_stepping = true;
		break;
	case AB8500_CH_OPT_CRNTLVL_REG:
		shift_value = 0;
		prev_curr_index = (reg_value >> shift_value);
		curr_index = ab8500_current_to_regval(ich);
		if (curr_index == 0)
			step_mdelay = STEP_MDELAY;
		else if ((curr_index - prev_curr_index) > 1)
			step_mdelay = STEP_MDELAY * 10;
		else
			step_mdelay = STEP_MDELAY;

		if (!di->usb.charger_connected && !di->ac.charger_connected)
			no_stepping = true;

		break;
	default:
		dev_err(di->dev, "%s current register not valid\n", __func__);
		ret = -ENXIO;
		goto exit_set_current;
	}

	if (curr_index < 0) {
		dev_err(di->dev, "requested current limit out-of-range\n");
		ret = -ENXIO;
		goto exit_set_current;
	}

	/* only update current if it's been changed */
	if (prev_curr_index == curr_index) {
		dev_dbg(di->dev, "%s current not changed for reg: 0x%02x\n",
			__func__, reg);
		ret = 0;
		goto exit_set_current;
	}

	dev_dbg(di->dev, "%s set charger current: %d mA for reg: 0x%02x\n",
		__func__, ich, reg);

	if (no_stepping) {
		ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
					reg, (u8) curr_index << shift_value);
		if (ret)
			dev_err(di->dev, "%s write failed\n", __func__);
	} else if (prev_curr_index > curr_index) {
		for (i = prev_curr_index - 1; i >= curr_index; i--) {
			dev_dbg(di->dev, "curr change_1 to: %x for 0x%02x\n",
				(u8) i << shift_value, reg);
			ret = abx500_set_register_interruptible(di->dev,
				AB8500_CHARGER, reg, (u8) i << shift_value);
			if (ret) {
				dev_err(di->dev, "%s write failed\n", __func__);
				goto exit_set_current;
			}
			if (i != curr_index)
				msleep(step_mdelay);
		}
	} else {
		bool allow = true;
		for (i = prev_curr_index + 1; i <= curr_index && allow; i++) {
			dev_dbg(di->dev, "curr change_2 to: %x for 0x%02x\n",
				(u8) i << shift_value, reg);
			ret = abx500_set_register_interruptible(di->dev,
				AB8500_CHARGER, reg, (u8) i << shift_value);
			if (ret) {
				dev_err(di->dev, "%s write failed\n", __func__);
				goto exit_set_current;
			}
			if (i != curr_index)
				msleep(step_mdelay);

			allow = ab8500_charger_check_continue_stepping(di, reg);
		}
	}

exit_set_current:
	mutex_lock(&di->current_stepping_sessions_lock);
	di->current_stepping_sessions--;
	mutex_unlock(&di->current_stepping_sessions_lock);

	return ret;
}

/**
 * ab8500_charger_set_vbus_in_curr() - set VBUS input current limit
 * @di:		pointer to the ab8500_charger structure
 * @ich_in:	charger input current limit
 *
 * Sets the current that can be drawn from the USB host
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_set_vbus_in_curr(struct ab8500_charger *di,
		int ich_in)
{
	int min_value;
	int ret;

	/* We should always use to lowest current limit */
	min_value = min(di->bat->chg_params->usb_curr_max, ich_in);
	if (di->vbus_drop.real_max_usb_in_curr[0] > 0)
		min_value =
			min(di->vbus_drop.real_max_usb_in_curr[0], min_value);

	if (di->usb_state.usb_current >= 100)
		min_value = min(di->usb_state.usb_current, min_value);

	switch (min_value) {
	case 100:
		if (di->vbat < VBAT_TRESH_IP_CUR_RED)
			min_value = USB_CH_IP_CUR_LVL_0P05;
		break;
	case 500:
		if (di->vbat < VBAT_TRESH_IP_CUR_RED)
			min_value = USB_CH_IP_CUR_LVL_0P45;
		break;
	default:
		break;
	}

	dev_info(di->dev, "VBUS input current limit set to %d mA\n", min_value);

	mutex_lock(&di->usb_ipt_crnt_lock);
	ret = ab8500_charger_set_current(di, min_value,
		AB8500_USBCH_IPT_CRNTLVL_REG);
	mutex_unlock(&di->usb_ipt_crnt_lock);

	return ret;
}

/**
 * ab8500_charger_set_output_curr() - set charger output current
 * @di:		pointer to the ab8500_charger structure
 * @ich_out:	output charger current, in mA
 *
 * Set charger output current.
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_set_output_curr(struct ab8500_charger *di,
	int ich_out)
{
	return ab8500_charger_set_current(di, ich_out,
		AB8500_CH_OPT_CRNTLVL_REG);
}

/**
 * ab8500_charger_led_en() - turn on/off chargign led
 * @di:		pointer to the ab8500_charger structure
 * @on:		flag to turn on/off the chargign led
 *
 * Power ON/OFF charging LED indication
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_led_en(struct ab8500_charger *di, int on)
{
	int ret;

	if (on) {
		/* Power ON charging LED indicator, set LED current to 5mA */
		ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
			AB8500_LED_INDICATOR_PWM_CTRL,
			(LED_IND_CUR_5MA | LED_INDICATOR_PWM_ENA));
		if (ret) {
			dev_err(di->dev, "Power ON LED failed\n");
			return ret;
		}
		/* LED indicator PWM duty cycle 252/256 */
		ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
			AB8500_LED_INDICATOR_PWM_DUTY,
			LED_INDICATOR_PWM_DUTY_252_256);
		if (ret) {
			dev_err(di->dev, "Set LED PWM duty cycle failed\n");
			return ret;
		}
	} else {
		/* Power off charging LED indicator */
		ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
			AB8500_LED_INDICATOR_PWM_CTRL,
			LED_INDICATOR_PWM_DIS);
		if (ret) {
			dev_err(di->dev, "Power-off LED failed\n");
			return ret;
		}
	}

	return ret;
}


/**
 * ab8500_charger_usb_en() - enable usb charging
 * @di:		pointer to the ab8500_charger structure
 * @enable:	enable/disable flag
 * @vset:	charging voltage
 * @ich_out:	charger output current
 *
 * Enable/Disable USB charging and turns on/off the charging led respectively.
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_usb_en(struct ux500_charger *charger,
	int enable, int vset, int ich_out)
{
	int ret;
	int volt_index;
	int curr_index;
	u8 overshoot = 0;
	struct ab8500_charger *di;

	if (charger->psy.type == POWER_SUPPLY_TYPE_MAINS)
		di = to_ab8500_charger_ac_device_info(charger);
	else if (charger->psy.type == POWER_SUPPLY_TYPE_USB)
		di = to_ab8500_charger_usb_device_info(charger);
	else
		return -ENXIO;

	if (enable) {
		/* Check if USB is connected */
		if (!di->usb.charger_connected) {
			dev_err(di->dev, "USB charger not connected\n");
			return -ENXIO;
		}

		/*
		 * Due to a bug in AB8500, BTEMP_HIGH/LOW interrupts
		 * will be triggered everytime we enable the VDD ADC supply.
		 * This will turn off charging for a short while.
		 * It can be avoided by having the supply on when
		 * there is a charger enabled. Normally the VDD ADC supply
		 * is enabled everytime a GPADC conversion is triggered. We will
		 * force it to be enabled from this driver to have
		 * the GPADC module independant of the AB8500 chargers
		 */
		if (!di->vddadc_en_usb) {
			regulator_enable(di->regu);
			di->vddadc_en_usb = true;
		}

		/* Enable USB charging */
		dev_info(di->dev, "Enable USB: %dmV %dmA\n", vset, ich_out);

		/* Check if the requested voltage or current is valid */
		volt_index = ab8500_voltage_to_regval(vset);
		curr_index = ab8500_current_to_regval(ich_out);
		if (volt_index < 0 || curr_index < 0) {
			dev_err(di->dev,
				"Charger voltage or current too high, "
				"charging not started\n");
			return -ENXIO;
		}

		/* ChVoltLevel: max voltage upto which battery can be charged */
		ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
			AB8500_CH_VOLT_LVL_REG, (u8) volt_index);
		if (ret) {
			dev_err(di->dev, "%s write failed\n", __func__);
			return ret;
		}

		/* Check if VBAT overshoot control should be enabled */
		if (!di->bat->enable_overshoot)
			overshoot = USB_CHG_NO_OVERSHOOT_ENA_N;

		/* Enable USB Charger */
		dev_dbg(di->dev,
			"Enabling USB with write to AB8500_USBCH_CTRL1_REG\n");
		ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
			AB8500_USBCH_CTRL1_REG, USB_CH_ENA | overshoot);
		if (ret) {
			dev_err(di->dev, "%s write failed\n", __func__);
			return ret;
		}

#ifdef CONFIG_AB8500_BM_ENABLE_CONTROL_CHARGING_LED
		/* If success power on charging LED indication */
		ret = ab8500_charger_led_en(di, true);
		if (ret < 0)
			dev_err(di->dev, "failed to enable LED\n");
#endif

		di->usb.charger_online = 1;

		/* USBChInputCurr: current that can be drawn from the usb */
		ret = ab8500_charger_set_vbus_in_curr(di, di->max_usb_in_curr);
		if (ret) {
			dev_err(di->dev, "setting USBChInputCurr failed\n");
			return ret;
		}

		/* ChOutputCurentLevel: protected output current */
		ret = ab8500_charger_set_output_curr(di, ich_out);
		if (ret) {
			dev_err(di->dev, "%s "
				"Failed to set ChOutputCurentLevel\n",
				__func__);
			return ret;
		}

		queue_delayed_work(di->charger_wq, &di->check_vbat_work, HZ);

	} else {
		/* Disable USB charging */
		dev_dbg(di->dev, "%s Disabled USB charging\n", __func__);
		ret = abx500_set_register_interruptible(di->dev,
			AB8500_CHARGER,
			AB8500_USBCH_CTRL1_REG, 0);
		if (ret) {
			dev_err(di->dev,
				"%s write failed\n", __func__);
			return ret;
		}

#ifdef CONFIG_AB8500_BM_ENABLE_CONTROL_CHARGING_LED
		ret = ab8500_charger_led_en(di, false);
		if (ret < 0)
			dev_err(di->dev, "failed to disable LED\n");
#endif
		/* USBChInputCurr: current that can be drawn from the usb */
		ret = ab8500_charger_set_vbus_in_curr(di, 0);
		if (ret) {
			dev_err(di->dev, "setting USBChInputCurr failed\n");
			return ret;
		}

		/* ChOutputCurentLevel: protected output current */
		ret = ab8500_charger_set_output_curr(di, 0);
		if (ret) {
			dev_err(di->dev, "%s "
				"Failed to reset ChOutputCurentLevel\n",
				__func__);
			return ret;
		}
		di->usb.charger_online = 0;
		di->usb.wd_expired = false;

		/* Disable regulator if enabled */
		if (di->vddadc_en_usb) {
			regulator_disable(di->regu);
			di->vddadc_en_usb = false;
		}

		dev_dbg(di->dev, "%s Disabled USB charging\n", __func__);

		/* Cancel any pending Vbat check work */
		if (delayed_work_pending(&di->check_vbat_work))
			cancel_delayed_work(&di->check_vbat_work);

	}
	ab8500_charger_psy_changed(di);

	return ret;
}

/**
 * ab8500_charger_watchdog_kick() - kick charger watchdog
 * @di:		pointer to the ab8500_charger structure
 *
 * Kick charger watchdog
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_watchdog_kick(struct ux500_charger *charger)
{
	int ret;
	struct ab8500_charger *di;

	if (charger->psy.type == POWER_SUPPLY_TYPE_MAINS)
		di = to_ab8500_charger_ac_device_info(charger);
	else if (charger->psy.type == POWER_SUPPLY_TYPE_USB)
		di = to_ab8500_charger_usb_device_info(charger);
	else
		return -ENXIO;

	ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
		AB8500_CHARG_WD_CTRL, CHARG_WD_KICK);
	if (ret)
		dev_err(di->dev, "Failed to kick WD!\n");

	return ret;
}

/**
 * ab8500_charger_update_charger_current() - update charger current
 * @di:		pointer to the ab8500_charger structure
 *
 * Update the charger output current for the specified charger
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_update_charger_current(struct ux500_charger *charger,
		int ich_out)
{
	int ret;
	struct ab8500_charger *di;

	if (charger->psy.type == POWER_SUPPLY_TYPE_MAINS)
		di = to_ab8500_charger_ac_device_info(charger);
	else if (charger->psy.type == POWER_SUPPLY_TYPE_USB)
		di = to_ab8500_charger_usb_device_info(charger);
	else
		return -ENXIO;

	ret = ab8500_charger_set_output_curr(di, ich_out);
	if (ret) {
		dev_err(di->dev, "%s "
			"Failed to set ChOutputCurentLevel\n",
			__func__);
		return ret;
	}

	/* Reset the main and usb drop input current measurement counter */
	ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
				AB8500_CHARGER_CTRL,
				0x1);
	if (ret) {
		dev_err(di->dev, "%s write failed\n", __func__);
		return ret;
	}

	return ret;
}

static int ab8500_charger_get_ext_psy_data(struct device *dev, void *data)
{
	struct power_supply *psy;
	struct power_supply *ext;
	struct ab8500_charger *di;
	union power_supply_propval ret;
	int i, j;
	bool psy_found = false;
	struct ux500_charger *usb_chg;

	usb_chg = (struct ux500_charger *)data;
	psy = &usb_chg->psy;

	di = to_ab8500_charger_usb_device_info(usb_chg);

	ext = dev_get_drvdata(dev);

	/* For all psy where the driver name appears in any supplied_to */
	for (i = 0; i < ext->num_supplicants; i++) {
		if (!strcmp(ext->supplied_to[i], psy->name))
			psy_found = true;
	}

	if (!psy_found)
		return 0;

	/* Go through all properties for the psy */
	for (j = 0; j < ext->num_properties; j++) {
		enum power_supply_property prop;
		prop = ext->properties[j];

		if (ext->get_property(ext, prop, &ret))
			continue;

		switch (prop) {
		case POWER_SUPPLY_PROP_VOLTAGE_NOW:
			switch (ext->type) {
			case POWER_SUPPLY_TYPE_BATTERY:
				di->vbat = ret.intval / 1000;
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
	}
	return 0;
}

/**
 * ab8500_charger_check_vbat_work() - keep vbus current within spec
 * @work	pointer to the work_struct structure
 *
 * Due to a asic bug it is necessary to lower the input current to the vbus
 * charger when charging with at some specific levels. This issue is only valid
 * for below a certain battery voltage. This function makes sure that the
 * the allowed current limit isn't exceeded.
 */
static void ab8500_charger_check_vbat_work(struct work_struct *work)
{
	int t = 10;
	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, check_vbat_work.work);

	class_for_each_device(power_supply_class, NULL,
		&di->usb_chg.psy, ab8500_charger_get_ext_psy_data);

	/* First run old_vbat is 0. */
	if (di->old_vbat == 0)
		di->old_vbat = di->vbat;

	if (!((di->old_vbat <= VBAT_TRESH_IP_CUR_RED &&
		di->vbat <= VBAT_TRESH_IP_CUR_RED) ||
		(di->old_vbat > VBAT_TRESH_IP_CUR_RED &&
		di->vbat > VBAT_TRESH_IP_CUR_RED))) {

		dev_dbg(di->dev, "Vbat did cross threshold, curr: %d, new: %d,"
			" old: %d\n", di->max_usb_in_curr, di->vbat,
			di->old_vbat);
		ab8500_charger_set_vbus_in_curr(di, di->max_usb_in_curr);
		power_supply_changed(&di->usb_chg.psy);
	}

	di->old_vbat = di->vbat;

	/*
	 * No need to check the battery voltage every second when not close to
	 * the threshold.
	 */
	if (di->vbat < (VBAT_TRESH_IP_CUR_RED + 100) &&
		(di->vbat > (VBAT_TRESH_IP_CUR_RED - 100)))
			t = 1;

	queue_delayed_work(di->charger_wq, &di->check_vbat_work, t * HZ);
}

/**
 * ab8500_charger_check_hw_failure_work() - check main charger failure
 * @work:	pointer to the work_struct structure
 *
 * Work queue function for checking the main charger status
 */
static void ab8500_charger_check_hw_failure_work(struct work_struct *work)
{
	int ret;
	u8 reg_value;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, check_hw_failure_work.work);

	if (di->flags.vbus_ovv) {
		ret = abx500_get_register_interruptible(di->dev,
			AB8500_CHARGER, AB8500_CH_USBCH_STAT2_REG,
			&reg_value);
		if (ret < 0) {
			dev_err(di->dev, "%s ab8500 read failed\n", __func__);
			return;
		}
		if (!(reg_value & VBUS_OVV_TH)) {
			di->flags.vbus_ovv = false;
			ab8500_power_supply_changed(di, &di->usb_chg.psy);
		}
	}
	/* If we still have a failure, schedule a new check */
	if (di->flags.vbus_ovv) {
		queue_delayed_work(di->charger_wq,
			&di->check_hw_failure_work, round_jiffies(HZ));
	}
}

/**
 * ab8500_charger_kick_watchdog_work() - kick the watchdog
 * @work:	pointer to the work_struct structure
 *
 * Work queue function for kicking the charger watchdog.
 *
 * For ABB revision 1.0 and 1.1 there is a bug in the watchdog
 * logic. That means we have to continously kick the charger
 * watchdog even when no charger is connected. This is only
 * valid once the AC charger has been enabled. This is
 * a bug that is not handled by the algorithm and the
 * watchdog have to be kicked by the charger driver
 * when the AC charger is disabled
 */
static void ab8500_charger_kick_watchdog_work(struct work_struct *work)
{
	int ret;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, kick_wd_work.work);

	ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
		AB8500_CHARG_WD_CTRL, CHARG_WD_KICK);
	if (ret)
		dev_err(di->dev, "Failed to kick WD!\n");

	/* Schedule a new watchdog kick */
	queue_delayed_work(di->charger_wq,
		&di->kick_wd_work, round_jiffies(WD_KICK_INTERVAL));
}


/**
 * ab8500_charger_detect_usb_type_work() - work to detect USB type
 * @work:	Pointer to the work_struct structure
 *
 * Detect the type of USB plugged
 */
void ab8500_charger_detect_usb_type_work(struct work_struct *work)
{
	int ret;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, detect_usb_type_work);

	/*
	 * Since we can't be sure that the events are received
	 * synchronously, we have the check if is
	 * connected by reading the status register
	 */
	ret = ab8500_charger_detect_chargers(di, false);
	if (ret < 0)
		return;

	if (!(ret & USB_PW_CONN)) {
		dev_dbg(di->dev, "%s di->vbus_detected = false\n", __func__);
		di->vbus_detected = false;
		ab8500_charger_set_usb_connected(di, false);
		ab8500_charger_psy_changed(di);
	} else {
		dev_dbg(di->dev, "%s di->vbus_detected = true\n", __func__);
		di->vbus_detected = true;
		if (is_ab8500_1p1_or_earlier(di->parent)) {
			ret = ab8500_charger_detect_usb_type(di);
			if (!ret) {
				ab8500_charger_set_usb_connected(di, true);
				ab8500_charger_psy_changed(di);
			}
		} else {
			/* For ABB cut2.0 and onwards we have an IRQ,
			 * USB_LINK_STATUS that will be triggered when the USB
			 * link status changes. The exception is USB connected
			 * during startup. Then we don't get a
			 * USB_LINK_STATUS IRQ
			 */
			if (di->vbus_detected_start) {
				di->vbus_detected_start = false;
				ret = ab8500_charger_detect_usb_type(di);
				if (!ret) {
					ab8500_charger_set_usb_connected(di,
						true);
					ab8500_charger_psy_changed(di);
				}
			}
		}
	}
}

/**
 * ab8500_charger_usb_link_attach_work() - work to detect USB type
 * @work:	pointer to the work_struct structure
 *
 * Detect the type of USB plugged
 */
static void ab8500_charger_usb_link_attach_work(struct work_struct *work)
{
	int ret;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, attach_work.work);

	/* Update maximum input current */
	if (di->usb.charger_online) {
		ret = ab8500_charger_set_vbus_in_curr(di, di->max_usb_in_curr);
		if (ret)
			return;
	}

	ab8500_charger_set_usb_connected(di, true);
	ab8500_charger_psy_changed(di);
}

/**
 * ab8500_charger_usb_link_status_work() - work to detect USB type
 * @work:	pointer to the work_struct structure
 *
 * Detect the type of USB plugged
 */
static void ab8500_charger_usb_link_status_work(struct work_struct *work)
{
	int detected_chargers;
	int ret;
	u8 val;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, usb_link_status_work);

	/*
	 * Since we can't be sure that the events are received
	 * synchronously, we have the check if  is
	 * connected by reading the status register
	 */
	detected_chargers = ab8500_charger_detect_chargers(di, false);
	if (detected_chargers < 0)
		return;

	/*
	 * Some chargers that breaks the USB spec is
	 * identified as invalid by AB8500 and it refuse
	 * to start the charging process. But by jumping
	 * through a few hoops it can be forced to start.
	 */
	if (detected_chargers & USB_PW_CONN) {
		ret = abx500_get_register_interruptible(di->dev, AB8500_USB,
						AB8500_USB_LINE_STAT_REG, &val);
		dev_dbg(di->dev,
			"%s: err %d, UsbLineStatus register = 0x%02x\n",
			__func__, ret >= 0 ? 0 : ret, val);

		if (ret >= 0 && ((val & AB8500_USB_LINK_STATUS) >> 3) ==
			USB_STAT_NOT_VALID_LINK &&
			di->invalid_charger_detect_state == 0) {
			dev_dbg(di->dev, "Invalid charger detected, state=0\n");
			/* Enable charger */
			abx500_mask_and_set_register_interruptible(di->dev,
				AB8500_CHARGER, AB8500_USBCH_CTRL1_REG,
				USB_CH_ENA, USB_CH_ENA);
			/* Enable USB charger detection */
			abx500_mask_and_set_register_interruptible(di->dev,
			AB8500_USB, AB8500_USB_LINE_CTRL2_REG, 0x01, 0x01);
			di->invalid_charger_detect_state = 1;
			/* Exit and wait for new link status interrupt. */
			return;

		}
		if (di->invalid_charger_detect_state == 1) {
			dev_dbg(di->dev, "Invalid charger detected, state=1\n");
			/* Disable USB charger detection */
			abx500_mask_and_set_register_interruptible(di->dev,
			AB8500_USB, AB8500_USB_LINE_CTRL2_REG, 0x01, 0x00);
			di->invalid_charger_detect_state = 2;
		}
	} else {
		di->invalid_charger_detect_state = 0;
	}

	if (!(detected_chargers & USB_PW_CONN)) {
		dev_dbg(di->dev, "%s di->vbus_detected = false\n", __func__);
		di->vbus_detected = false;
		ab8500_charger_set_usb_connected(di, false);
		ab8500_charger_psy_changed(di);
		dev_dbg(di->dev,
			"%s cancel_delayed_work_sync(&di->attach_work)...\n",
			__func__);
		cancel_delayed_work_sync(&di->attach_work);
	} else {
		dev_dbg(di->dev, "%s di->vbus_detected = true\n", __func__);
		di->vbus_detected = true;
		ret = ab8500_charger_read_usb_type(di);
		if (!ret) {
			if (di->is_aca_rid == 1) {
				/* Only wait once */
				di->is_aca_rid++;
				dev_dbg(di->dev,
				"%s Wait %d msec for USB enum to finish\n",
				__func__, WAIT_ACA_RID_ENUMERATION);
				queue_delayed_work(di->charger_wq,
				&di->attach_work,
				msecs_to_jiffies(WAIT_ACA_RID_ENUMERATION));
			} else {
				queue_delayed_work(di->charger_wq,
					&di->attach_work,
					0);
			}
		} else if (ret == -ENXIO) {
			/* No valid charger type detected */
			di->flags.report_charger_no_charge = true;
			di->is_usb_host = true;
			ab8500_charger_set_usb_connected(di, true);
			ab8500_charger_psy_changed(di);
		}
	}
}

static void ab8500_charger_usb_state_changed_work(struct work_struct *work)
{
	int ret;
	unsigned long flags;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, usb_state_changed_work.work);

	if (!di->vbus_detected) {
		dev_dbg(di->dev, "%s !di->vbus_detected\n", __func__);
		return;
	}

	spin_lock_irqsave(&di->usb_state.usb_lock, flags);
	di->usb_state.state = di->usb_state.state_tmp;
	di->usb_state.usb_current = di->usb_state.usb_current_tmp;
	spin_unlock_irqrestore(&di->usb_state.usb_lock, flags);

	dev_dbg(di->dev, "%s USB state: 0x%02x mA: %d\n",
		__func__, di->usb_state.state, di->usb_state.usb_current);

	switch (di->usb_state.state) {
	case AB8500_BM_USB_STATE_RESET_HS:
	case AB8500_BM_USB_STATE_RESET_FS:
	case AB8500_BM_USB_STATE_SUSPEND:
	case AB8500_BM_USB_STATE_MAX:
		ab8500_charger_set_usb_connected(di, false);
		ab8500_charger_psy_changed(di);
		break;

	case AB8500_BM_USB_STATE_RESUME:
		/*
		 * when suspend->resume there should be delay
		 * of 1sec for enabling charging
		 */
		msleep(1000);
		/* Intentional fall through */
	case AB8500_BM_USB_STATE_CONFIGURED:
		/*
		 * USB is configured, enable charging with the charging
		 * input current obtained from USB driver
		 */
		if (!ab8500_charger_get_usb_cur(di)) {
			/* Update maximum input current */
			ret = ab8500_charger_set_vbus_in_curr(di,
					di->max_usb_in_curr);
			if (ret)
				return;

			ab8500_charger_set_usb_connected(di, true);
			ab8500_charger_psy_changed(di);
		}
		break;

	default:
		break;
	};
}

/**
 * ab8500_charger_check_usbchargernotok_work() - check USB chg not ok status
 * @work:	pointer to the work_struct structure
 *
 * Work queue function for checking the USB charger Not OK status
 */
static void ab8500_charger_check_usbchargernotok_work(struct work_struct *work)
{
	int ret;
	u8 reg_value;
	bool prev_status;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, check_usbchgnotok_work.work);

	/* Check if the status bit for usbchargernotok is still active */
	ret = abx500_get_register_interruptible(di->dev,
		AB8500_CHARGER, AB8500_CH_USBCH_STAT2_REG, &reg_value);
	if (ret < 0) {
		dev_err(di->dev, "%s ab8500 read failed\n", __func__);
		return;
	}
	prev_status = di->flags.usbchargernotok;

	if (reg_value & VBUS_CH_NOK) {
		di->flags.usbchargernotok = true;
		/* Check again in 1sec */
		queue_delayed_work(di->charger_wq,
			&di->check_usbchgnotok_work, HZ);
	} else {
		di->flags.usbchargernotok = false;
		di->flags.vbus_collapse = false;
	}

	if (prev_status != di->flags.usbchargernotok)
		ab8500_charger_psy_changed(di);
}

/**
 * ab8500_charger_check_usb_thermal_prot_work() - check usb thermal status
 * @work:	pointer to the work_struct structure
 *
 * Work queue function for checking the USB thermal prot status
 */
static void ab8500_charger_check_usb_thermal_prot_work(
	struct work_struct *work)
{
	int ret;
	u8 reg_value;

	struct ab8500_charger *di = container_of(work,
		struct ab8500_charger, check_usb_thermal_prot_work);

	/* Check if the status bit for usb_thermal_prot is still active */
	ret = abx500_get_register_interruptible(di->dev,
		AB8500_CHARGER, AB8500_CH_USBCH_STAT2_REG, &reg_value);
	if (ret < 0) {
		dev_err(di->dev, "%s ab8500 read failed\n", __func__);
		return;
	}
	if (reg_value & USB_CH_TH_PROT)
		di->flags.usb_thermal_prot = true;
	else
		di->flags.usb_thermal_prot = false;

	ab8500_charger_psy_changed(di);
}

static void ab8500_charger_vbus_drop_end_work(struct work_struct *work)
{
	struct ab8500_vbus_drop *vd =
		container_of(work, struct ab8500_vbus_drop, end_work.work);
	struct ab8500_charger *di =
		container_of(vd, struct ab8500_charger, vbus_drop);
	int ret;
	u8 reg_value;

	di->flags.vbus_drop_end = false;

	/* Reset the drop counter */
	abx500_set_register_interruptible(di->dev,
				  AB8500_CHARGER, AB8500_CHARGER_CTRL, 0x01);

	ret = abx500_get_register_interruptible(di->dev, AB8500_CHARGER,
						AB8500_CH_USBCH_STAT2_REG,
						&reg_value);
	if (ret < 0) {
		dev_err(di->dev, "%s ab8500 read failed\n", __func__);
	} else {
		int curr = ab8500_charger_vbus_in_curr_map[
			reg_value >> AUTO_VBUS_IN_CURR_LIM_SHIFT];

		if (vd->real_max_usb_in_curr[1] != curr) {
			/* USB source is collapsing */
			vd->real_max_usb_in_curr[1] = curr;
			vd->retry_current_time =
				VBUS_IN_CURR_LIM_RETRY_SET_TIME;
			dev_info(di->dev,
				 "VBUS input current limiting to %d mA."
				 " Retry set %d mA\n",
				 vd->real_max_usb_in_curr[1],
				 di->max_usb_in_curr);
		} else {
			/* USB source can not give more than this amount.
			 * Taking more will collapse the source.
			 */
			int new_time = vd->retry_current_time << 1;
			if (new_time > VBUS_IN_CURR_LIM_RETRY_MAX_TIME) {
				vd->real_max_usb_in_curr[0] =
					vd->real_max_usb_in_curr[1];
				dev_info(di->dev,
					 "VBUS input current limited to"
					 " %d mA. No more retry to set %d mA\n",
					 vd->real_max_usb_in_curr[0],
					 di->max_usb_in_curr);
				return;
			} else {
				dev_info(di->dev,
					 "VBUS input current still limiting to"
					 " %d mA. Retry set %d mA\n",
					 vd->real_max_usb_in_curr[1],
					 di->max_usb_in_curr);
				vd->retry_current_time = new_time;
			}
		}
	}

	if (di->usb.charger_connected)
		ab8500_charger_set_vbus_in_curr(di, di->max_usb_in_curr);
}

/**
 * ab8500_charger_vbusdetf_handler() - VBUS falling detected
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_vbusdetf_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	di->vbus_detected = false;
	dev_dbg(di->dev, "VBUS falling detected\n");
	queue_work(di->charger_wq, &di->detect_usb_type_work);

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_vbusdetr_handler() - VBUS rising detected
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_vbusdetr_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	di->vbus_detected = true;
	dev_dbg(di->dev, "VBUS rising detected\n");

	/* When already called suspend handler we can not guarantee that
	 * USB detect type work is able to run complete.
	 * Need to wake lock with timeout to make sure work is starting to
	 * execute. Upon suspend and work is not complete it will be handled
	 * by flushing the work.
	 */
	wake_lock_timeout(&di->pm_lock, HZ / 2);

	queue_work(di->charger_wq, &di->detect_usb_type_work);

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_usblinkstatus_handler() - USB link status has changed
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_usblinkstatus_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	dev_dbg(di->dev, "USB link status changed\n");

	queue_work(di->charger_wq, &di->usb_link_status_work);

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_usbchthprotr_handler() - Die temp is above usb charger
 * thermal protection threshold
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_usbchthprotr_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	dev_dbg(di->dev,
		"Die temp above USB charger thermal protection threshold\n");
	queue_work(di->charger_wq, &di->check_usb_thermal_prot_work);

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_usbchthprotf_handler() - Die temp is below usb charger
 * thermal protection threshold
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_usbchthprotf_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	dev_dbg(di->dev,
		"Die temp ok for USB charger thermal protection threshold\n");
	queue_work(di->charger_wq, &di->check_usb_thermal_prot_work);

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_usbchargernotokr_handler() - USB charger not ok detected
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_usbchargernotokr_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	dev_dbg(di->dev, "Not allowed USB charger detected\n");
	queue_delayed_work(di->charger_wq, &di->check_usbchgnotok_work, 0);

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_chwdexp_handler() - Charger watchdog expired
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_chwdexp_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	dev_dbg(di->dev, "Charger watchdog expired\n");

	/*
	 * The charger that was online when the watchdog expired
	 * needs to be restarted for charging to start again
	 */
	if (di->ac.charger_online) {
		di->ac.wd_expired = true;
		ab8500_charger_psy_changed(di);
	}
	if (di->usb.charger_online) {
		di->usb.wd_expired = true;
		ab8500_charger_psy_changed(di);
	}

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_vbuschdropend_handler() - VBUS drop removed
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_vbuschdropend_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	dev_dbg(di->dev, "VBUS charger drop ended\n");
	di->flags.vbus_drop_end = true;
	/* VBUS might have dropped due to bad connection.
	 * Schedule a new input limit set to the value SW requests.
	 */
	queue_delayed_work(di->charger_wq, &di->vbus_drop.end_work,
		   round_jiffies(di->vbus_drop.retry_current_time * HZ));

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_vbusovv_handler() - VBUS overvoltage detected
 * @irq:       interrupt number
 * @_di:       pointer to the ab8500_charger structure
 *
 * Returns IRQ status(IRQ_HANDLED)
 */
static irqreturn_t ab8500_charger_vbusovv_handler(int irq, void *_di)
{
	struct ab8500_charger *di = _di;

	dev_dbg(di->dev, "VBUS overvoltage detected\n");
	di->flags.vbus_ovv = true;
	ab8500_charger_psy_changed(di);

	/* Schedule a new HW failure check */
	queue_delayed_work(di->charger_wq, &di->check_hw_failure_work, 0);

	return IRQ_HANDLED;
}

/**
 * ab8500_charger_usb_get_property() - get the usb properties
 * @psy:        pointer to the power_supply structure
 * @psp:        pointer to the power_supply_property structure
 * @val:        pointer to the power_supply_propval union
 *
 * This function gets called when an application tries to get the usb
 * properties by reading the sysfs files.
 * USB properties are online, present and voltage.
 * online:     usb charging is in progress or not
 * present:    presence of the usb
 * voltage:    vbus voltage
 * Returns error code in case of failure else 0(on success)
 */
static int ab8500_charger_usb_get_property(struct power_supply *psy,
	enum power_supply_property psp,
	union power_supply_propval *val)
{
	struct ab8500_charger *di;
	struct ux500_charger *chg = psy_to_ux500_charger(psy);

	if (psy->type == POWER_SUPPLY_TYPE_MAINS)
		di = to_ab8500_charger_ac_device_info(chg);
	else if (psy->type == POWER_SUPPLY_TYPE_USB)
		di = to_ab8500_charger_usb_device_info(chg);
	else
		return -ENXIO;

	switch (psp) {
	case POWER_SUPPLY_PROP_HEALTH:
		if (di->flags.report_charger_no_charge)
			val->intval = POWER_SUPPLY_HEALTH_UNKNOWN;
		else if (di->flags.usbchargernotok)
			val->intval = POWER_SUPPLY_HEALTH_UNSPEC_FAILURE;
		else if (di->ac.wd_expired || di->usb.wd_expired)
			val->intval = POWER_SUPPLY_HEALTH_DEAD;
		else if (di->flags.usb_thermal_prot)
			val->intval = POWER_SUPPLY_HEALTH_OVERHEAT;
		else if (di->flags.vbus_ovv)
			val->intval = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
		else
			val->intval = POWER_SUPPLY_HEALTH_GOOD;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		/* Android is interpreting charger connected as 'ONLINE'
		 * but expects result as reported in 'PRESENT'.
		 */
#ifndef CONFIG_ANDROID
#endif
	case POWER_SUPPLY_PROP_PRESENT:
		if (psy->type == POWER_SUPPLY_TYPE_MAINS) {
			if (di->is_usb_host)
				val->intval = 0;
			else
				val->intval = di->usb.charger_connected;
		} else if (psy->type == POWER_SUPPLY_TYPE_USB) {
			if (di->is_usb_host)
				val->intval = di->usb.charger_connected;
			else
				val->intval = 0;
		}
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		di->usb.charger_voltage = ab8500_charger_get_vbus_voltage(di);
		val->intval = di->usb.charger_voltage * 1000;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		/*
		 * This property is used to indicate when CV mode is entered
		 * for the USB charger
		 */
		di->usb.cv_active = ab8500_charger_usb_cv(di);
		val->intval = di->usb.cv_active;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = ab8500_charger_get_usb_current(di) * 1000;
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		/*
		 * This property is used to indicate when VBUS has collapsed
		 * due to too high output current from the USB charger
		 */
		if (di->flags.vbus_collapse)
			val->intval = 1;
		else
			val->intval = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/*
 * Function for enabling main watch dog in OTP to configure
 * for restart in case PRCMU FW gets stuck
 */
static void ab8500_enable_otp_emulation_of_main_wd(struct ab8500_charger *di,
						   u8 wdog_reg)
{
	int ret;
	u8 otp_regs[AB8500_OTP_NO_OF_REGS];
	u8 otp_wd;
	u8 i;

	dev_dbg(di->dev, "OTP emulation, real watch dog: 0x%02x\n", wdog_reg);

	dev_dbg(di->dev, "Enable the OTP emulation register...\n");
	/* enable the OTP emulation registers */
	ret = abx500_set_register_interruptible(di->dev,
						AB8500_DEVELOPMENT,
						0x00,
						0x2);
	if (ret) {
		dev_err(di->dev, "%s %d write failed\n", __func__, __LINE__);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(otp_regs); i++) {
		/* read the register containing fallback bit */
		ret = abx500_get_register_interruptible(di->dev,
							AB8500_OTP_EMUL,
							i,
							&otp_regs[i]);
		dev_dbg(di->dev, "OTP reg 0x%02x: 0x%02x\n", i, otp_regs[i]);
	}

	/* Clear OTP wd bit */
	otp_wd = otp_regs[AB8500_OTP_CONF_15] & 0xFE;

	/* Set again if was enabled by SW */
	if (wdog_reg & MAIN_WDOG_ENA)
		otp_wd |=  MAIN_WDOG_ENA;

	otp_regs[AB8500_OTP_CONF_15] = otp_wd;

	dev_dbg(di->dev, "Set up to read emulation contents...\n");
	/* Set up to read emulation contents */
	ret = abx500_set_register_interruptible(di->dev,
						AB8500_STE_TEST,
						0xB1,
						0x2);
	if (ret) {
		dev_err(di->dev, "%s %d write failed\n", __func__, __LINE__);
		return;
	}

	for (i = 0; i < ARRAY_SIZE(otp_regs); i++) {
		dev_dbg(di->dev, "About to write OTP reg 0x%02x: 0x%02x\n",
			i, otp_regs[i]);

		/* write back the changed wd bit value to register */
		ret = abx500_set_register_interruptible(di->dev,
							AB8500_OTP_EMUL,
							i,
							otp_regs[i]);
		if (ret) {
			dev_err(di->dev, "%s %d write failed\n",
				__func__, __LINE__);
			return;
		}
	}

	/* Set up chip control by emulation registers  */
	ret = abx500_set_register_interruptible(di->dev,
						AB8500_STE_TEST,
						0xB1,
						0x3);
	if (ret) {
		dev_err(di->dev, "%s %d write failed\n", __func__, __LINE__);
		return;
	}
}

/**
 * ab8500_charger_init_hw_registers() - Set up charger related registers
 * @di:		pointer to the ab8500_charger structure
 *
 * Set up charger OVV, watchdog and maximum voltage registers as well as
 * charging of the backup battery
 */
static int ab8500_charger_init_hw_registers(struct ab8500_charger *di)
{
	int ret = 0;
	u8 save_val;

	/* Setup maximum charger current and voltage for ABB cut2.0 */
	if (!is_ab8500_1p1_or_earlier(di->parent)) {
		ret = abx500_set_register_interruptible(di->dev,
			AB8500_CHARGER,
			AB8500_CH_VOLT_LVL_MAX_REG, CH_VOL_LVL_4P6);
		if (ret) {
			dev_err(di->dev,
				"failed to set CH_VOLT_LVL_MAX_REG\n");
			goto out;
		}

		ret = abx500_set_register_interruptible(di->dev,
			AB8500_CHARGER,
			AB8500_CH_OPT_CRNTLVL_MAX_REG, CH_OP_CUR_LVL_1P6);
		if (ret) {
			dev_err(di->dev,
				"failed to set CH_OPT_CRNTLVL_MAX_REG\n");
			goto out;
		}
	}

	/* VBUS OVV set to 6.3V and enable automatic current limitiation */
	ret = abx500_set_register_interruptible(di->dev,
		AB8500_CHARGER,
		AB8500_USBCH_CTRL2_REG,
		VBUS_OVV_SELECT_6P3V | VBUS_AUTO_IN_CURR_LIM_ENA);
	if (ret) {
		dev_err(di->dev, "failed to set VBUS OVV\n");
		goto out;
	}

	ret = abx500_get_register_interruptible(di->dev,
						AB8500_SYS_CTRL2_BLOCK,
						AB8500_MAIN_WDOG_CTRL_REG,
						&save_val);
	if (ret < 0) {
		dev_err(di->dev, "%s ab8500 read failed\n", __func__);
		return 0;
	}

	/* Make sure OTP emulation has same main WD setting as was
	 * set in soc_settings
	 */
	ab8500_enable_otp_emulation_of_main_wd(di, save_val);


	/* Write enable bit to main watchdog to signal SW
	 * taking over charging control from HW
	 */
	ret = abx500_set_register_interruptible(di->dev,
		AB8500_SYS_CTRL2_BLOCK,
		AB8500_MAIN_WDOG_CTRL_REG,
		(save_val | MAIN_WDOG_ENA | MAIN_WDOG_KICK));
	if (ret) {
		dev_err(di->dev, "failed to enable main watchdog\n");
		goto out;
	}

	/*
	 * Due to internal synchronisation, Enable and Kick watchdog bits
	 * cannot be enabled in a single write.
	 * A minimum delay of 2*32 kHz period (62.5�s) must be inserted
	 * between writing Enable then Kick bits.
	 */
	udelay(63);

	/* Kick main watchdog */
	ret = abx500_set_register_interruptible(di->dev,
		AB8500_SYS_CTRL2_BLOCK,
		AB8500_MAIN_WDOG_CTRL_REG,
		(MAIN_WDOG_ENA | MAIN_WDOG_KICK));
	if (ret) {
		dev_err(di->dev, "failed to kick main watchdog\n");
		goto out;
	}

	/* Restore watchdog */
	ret = abx500_set_register_interruptible(di->dev,
		AB8500_SYS_CTRL2_BLOCK,
		AB8500_MAIN_WDOG_CTRL_REG,
		save_val);
	if (ret) {
		dev_err(di->dev, "failed to restore main watchdog\n");
		goto out;
	}

	/* Set charger watchdog timeout */
	ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
		AB8500_CH_WD_TIMER_REG, WD_TIMER);
	if (ret) {
		dev_err(di->dev, "failed to set charger watchdog timeout\n");
		goto out;
	}

#ifndef CONFIG_AB8500_BM_ENABLE_CONTROL_CHARGING_LED
	ret = ab8500_charger_led_en(di, false);
	if (ret < 0) {
		dev_err(di->dev, "failed to disable LED\n");
		goto out;
	}
#endif

#ifdef CONFIG_AB8500_BM_ENABLE_BACKUP_CHARGER
	/* Backup battery voltage and current */
	ret = abx500_set_register_interruptible(di->dev,
		AB8500_RTC,
		AB8500_RTC_BACKUP_CHG_REG,
		di->bat->bkup_bat_v |
		di->bat->bkup_bat_i);
	if (ret) {
		dev_err(di->dev, "failed to setup backup battery charging\n");
		goto out;
	}

	/* Enable backup battery charging */
	abx500_mask_and_set_register_interruptible(di->dev,
		AB8500_RTC, AB8500_RTC_CTRL_REG,
		RTC_BUP_CH_ENA, RTC_BUP_CH_ENA);
	if (ret < 0)
		dev_err(di->dev, "%s mask and set failed\n", __func__);
#endif

out:
	return ret;
}

/*
 * ab8500 charger driver interrupts and their respective isr
 */
static struct ab8500_charger_interrupts ab8500_charger_irq[] = {
	{"VBUS_DET_F", ab8500_charger_vbusdetf_handler},
	{"VBUS_DET_R", ab8500_charger_vbusdetr_handler},
	{"USB_LINK_STATUS", ab8500_charger_usblinkstatus_handler},
	{"USB_CH_TH_PROT_R", ab8500_charger_usbchthprotr_handler},
	{"USB_CH_TH_PROT_F", ab8500_charger_usbchthprotf_handler},
	{"USB_CHARGER_NOT_OKR", ab8500_charger_usbchargernotokr_handler},
	{"VBUS_OVV", ab8500_charger_vbusovv_handler},
	{"CH_WD_EXP", ab8500_charger_chwdexp_handler},
	{"VBUS_CH_DROP_END", ab8500_charger_vbuschdropend_handler},
};

static int ab8500_charger_usb_notifier_call(struct notifier_block *nb,
		unsigned long event, void *power)
{
	struct ab8500_charger *di =
		container_of(nb, struct ab8500_charger, nb);
	enum ab8500_usb_state bm_usb_state;
	unsigned mA = *((unsigned *)power);

	if (event != USB_EVENT_VBUS) {
		dev_dbg(di->dev, "not a standard host, returning\n");
		return NOTIFY_DONE;
	}

	if (di == NULL)
		return NOTIFY_DONE;

	/* TODO: State is fabricate  here. See if charger really needs USB
	 * state or if mA is enough
	 */
	if ((di->usb_state.usb_current == 2) && (mA > 2))
		bm_usb_state = AB8500_BM_USB_STATE_RESUME;
	else if (mA == 0)
		bm_usb_state = AB8500_BM_USB_STATE_RESET_HS;
	else if (mA == 2)
		bm_usb_state = AB8500_BM_USB_STATE_SUSPEND;
	else if (mA >= 8) /* 8, 100, 500 */
		bm_usb_state = AB8500_BM_USB_STATE_CONFIGURED;
	else /* Should never occur */
		bm_usb_state = AB8500_BM_USB_STATE_RESET_FS;

	dev_dbg(di->dev, "%s usb_state: 0x%02x mA: %d\n",
		__func__, bm_usb_state, mA);

	spin_lock(&di->usb_state.usb_lock);
	di->usb_state.state_tmp = bm_usb_state;
	di->usb_state.usb_current_tmp = mA;
	spin_unlock(&di->usb_state.usb_lock);

	/*
	 * wait for some time until you get updates from the usb stack
	 * and negotiations are completed
	 */
	queue_delayed_work(di->charger_wq, &di->usb_state_changed_work, HZ/2);

	return NOTIFY_OK;
}

#if defined(CONFIG_PM)
static int ab8500_charger_resume(struct platform_device *pdev)
{
	int ret;
	struct ab8500_charger *di = platform_get_drvdata(pdev);

	/*
	 * For ABB revision 1.0 and 1.1 there is a bug in the watchdog
	 * logic. That means we have to continously kick the charger
	 * watchdog even when no charger is connected. This is only
	 * valid once the AC charger has been enabled. This is
	 * a bug that is not handled by the algorithm and the
	 * watchdog have to be kicked by the charger driver
	 * when the AC charger is disabled
	 */
	if (di->ac_conn && is_ab8500_1p1_or_earlier(di->parent)) {
		ret = abx500_set_register_interruptible(di->dev, AB8500_CHARGER,
			AB8500_CHARG_WD_CTRL, CHARG_WD_KICK);
		if (ret)
			dev_err(di->dev, "Failed to kick WD!\n");

		/* If not already pending start a new timer */
		if (!delayed_work_pending(
			&di->kick_wd_work)) {
			queue_delayed_work(di->charger_wq, &di->kick_wd_work,
				round_jiffies(WD_KICK_INTERVAL));
		}
	}

	/* If we still have a HW failure, schedule a new check */
	if (di->flags.mainextchnotok || di->flags.vbus_ovv) {
		queue_delayed_work(di->charger_wq,
			&di->check_hw_failure_work, 0);
	}

	if (di->flags.vbus_drop_end) {
		struct timespec now = ab8500_charger_get_time(di);
		unsigned long jiffies_now =
			timespec_to_jiffies(&now);

		if (jiffies_now < di->vbus_drop.work_expire)
			di->vbus_drop.work_expire -= jiffies_now;
		else
			di->vbus_drop.work_expire = 0;

		queue_delayed_work(di->charger_wq, &di->vbus_drop.end_work,
				   round_jiffies(di->vbus_drop.work_expire));
	}

	return 0;
}

static int ab8500_charger_suspend(struct platform_device *pdev,
	pm_message_t state)
{
	struct ab8500_charger *di = platform_get_drvdata(pdev);
	int ret = 0;

	/* Cancel any pending HW failure check */
	if (delayed_work_pending(&di->check_hw_failure_work))
		cancel_delayed_work(&di->check_hw_failure_work);

	if (delayed_work_pending(&di->vbus_drop.end_work)) {
		struct timespec t = ab8500_charger_get_time(di);
		/* 'jiffies' does not increment during suspend. Remove that time
		 * base and add to one that does increment monotonically during
		 * suspend.
		 */
		di->vbus_drop.work_expire =
			di->vbus_drop.end_work.timer.expires - jiffies +
			timespec_to_jiffies(&t);

		cancel_delayed_work(&di->vbus_drop.end_work);
	}
	/*
	 * if the job is in progress, it has to be finished
	 * before entering to suspend mode, otherwise USB
	 * status link may not be changed in time.
	 */
	(void) flush_work(&di->detect_usb_type_work);

	if (mutex_is_locked(&di->current_stepping_sessions_lock) ||
	    di->current_stepping_sessions)
		ret = -EAGAIN;

	return ret;
}
#else
#define ab8500_charger_suspend      NULL
#define ab8500_charger_resume       NULL
#endif

static int __devexit ab8500_charger_remove(struct platform_device *pdev)
{
	struct ab8500_charger *di = platform_get_drvdata(pdev);
	int i, irq;

	/* Disable USB charging */
	ab8500_charger_usb_en(&di->usb_chg, false, 0, 0);

	/* Disable interrupts */
	for (i = 0; i < ARRAY_SIZE(ab8500_charger_irq); i++) {
		irq = platform_get_irq_byname(pdev, ab8500_charger_irq[i].name);
		free_irq(irq, di);
	}

	/* disable the regulator */
	regulator_put(di->regu);

#ifdef CONFIG_AB8500_BM_ENABLE_BACKUP_CHARGER
	/* Backup battery voltage and current disable */
	if (abx500_mask_and_set_register_interruptible(di->dev,
	       AB8500_RTC, AB8500_RTC_CTRL_REG, RTC_BUP_CH_ENA, 0) < 0)
		dev_err(di->dev, "%s mask and set failed\n", __func__);
#endif

	otg_unregister_notifier(di->otg, &di->nb);
	otg_put_transceiver(di->otg);

	/* Delete the work queue */
	destroy_workqueue(di->charger_wq);

	flush_scheduled_work();
	wake_lock_destroy(&di->pm_lock);
	power_supply_unregister(&di->usb_chg.psy);
	power_supply_unregister(&di->ac_chg.psy);
	platform_set_drvdata(pdev, NULL);
	kfree(di);

	return 0;
}

static int __devinit ab8500_charger_probe(struct platform_device *pdev)
{
	int irq, i, charger_status, ret = 0;
	struct ab8500_platform_data *plat;

	struct ab8500_charger *di =
		kzalloc(sizeof(struct ab8500_charger), GFP_KERNEL);
	if (!di)
		return -ENOMEM;

	/* get parent data */
	di->dev = &pdev->dev;
	di->parent = dev_get_drvdata(pdev->dev.parent);
	di->gpadc = ab8500_gpadc_get();

	/* initialize lock */
	spin_lock_init(&di->usb_state.usb_lock);
	mutex_init(&di->usb_ipt_crnt_lock);
	mutex_init(&di->current_stepping_sessions_lock);
	wake_lock_init(&di->pm_lock, WAKE_LOCK_SUSPEND, "ab8500-charger");

	plat = dev_get_platdata(di->parent->dev);

	/* get charger specific platform data */
	if (!plat->charger) {
		dev_err(di->dev, "no charger platform data supplied\n");
		ret = -EINVAL;
		goto free_device_info;
	}
	di->pdata = plat->charger;

	/* get battery specific platform data */
	if (!plat->battery) {
		dev_err(di->dev, "no battery platform data supplied\n");
		ret = -EINVAL;
		goto free_device_info;
	}

	di->cpu = smp_processor_id();
	if (di->cpu < 0) {
		dev_err(di->dev, "Could not get CPU id\n");
		ret = -EINVAL;
		goto free_device_info;
	}

	di->bat = plat->battery;
	di->autopower = false;

	/* USB AC supply */
	/* power_supply base class */
	di->ac_chg.psy.name = "ab8500_ac";
	di->ac_chg.psy.type = POWER_SUPPLY_TYPE_MAINS;
	di->ac_chg.psy.properties = ab8500_charger_usb_props;
	di->ac_chg.psy.num_properties = ARRAY_SIZE(ab8500_charger_usb_props);
	di->ac_chg.psy.get_property = ab8500_charger_usb_get_property;
	di->ac_chg.psy.supplied_to = di->pdata->supplied_to;
	di->ac_chg.psy.num_supplicants = di->pdata->num_supplicants;
	/* ux500_charger sub-class */
	di->ac_chg.ops.enable = &ab8500_charger_usb_en;
	di->ac_chg.ops.kick_wd = &ab8500_charger_watchdog_kick;
	di->ac_chg.ops.update_curr = &ab8500_charger_update_charger_current;
	di->ac_chg.max_out_volt = ab8500_charger_voltage_map[
		ARRAY_SIZE(ab8500_charger_voltage_map) - 1];
	di->ac_chg.max_out_curr = ab8500_charger_current_map[
		ARRAY_SIZE(ab8500_charger_current_map) - 1];

	/* USB supply */
	/* power_supply base class */
	di->usb_chg.psy.name = "ab8500_usb";
	di->usb_chg.psy.type = POWER_SUPPLY_TYPE_USB;
	di->usb_chg.psy.properties = ab8500_charger_usb_props;
	di->usb_chg.psy.num_properties = ARRAY_SIZE(ab8500_charger_usb_props);
	di->usb_chg.psy.get_property = ab8500_charger_usb_get_property;
	di->usb_chg.psy.supplied_to = di->pdata->supplied_to;
	di->usb_chg.psy.num_supplicants = di->pdata->num_supplicants;
	/* ux500_charger sub-class */
	di->usb_chg.ops.enable = &ab8500_charger_usb_en;
	di->usb_chg.ops.kick_wd = &ab8500_charger_watchdog_kick;
	di->usb_chg.ops.update_curr = &ab8500_charger_update_charger_current;
	di->usb_chg.max_out_volt = ab8500_charger_voltage_map[
		ARRAY_SIZE(ab8500_charger_voltage_map) - 1];
	di->usb_chg.max_out_curr = ab8500_charger_current_map[
		ARRAY_SIZE(ab8500_charger_current_map) - 1];
	di->usb_state.usb_current = -1;

	/* Create a work queue for the charger */
	di->charger_wq =
		create_singlethread_workqueue("ab8500_charger_wq");
	if (di->charger_wq == NULL) {
		dev_err(di->dev, "failed to create work queue\n");
		goto free_device_info;
	}

	/* Init work for HW failure check */
	INIT_DELAYED_WORK_DEFERRABLE(&di->check_hw_failure_work,
		ab8500_charger_check_hw_failure_work);
	INIT_DELAYED_WORK_DEFERRABLE(&di->check_usbchgnotok_work,
		ab8500_charger_check_usbchargernotok_work);

	/*
	 * For ABB revision 1.0 and 1.1 there is a bug in the watchdog
	 * logic. That means we have to continously kick the charger
	 * watchdog even when no charger is connected. This is only
	 * valid once the AC charger has been enabled. This is
	 * a bug that is not handled by the algorithm and the
	 * watchdog have to be kicked by the charger driver
	 * when the AC charger is disabled
	 */
	INIT_DELAYED_WORK_DEFERRABLE(&di->kick_wd_work,
		ab8500_charger_kick_watchdog_work);

	INIT_DELAYED_WORK_DEFERRABLE(&di->attach_work,
		ab8500_charger_usb_link_attach_work);

	INIT_DELAYED_WORK_DEFERRABLE(&di->check_vbat_work,
		ab8500_charger_check_vbat_work);

	INIT_DELAYED_WORK_DEFERRABLE(&di->usb_state_changed_work,
		ab8500_charger_usb_state_changed_work);

	INIT_DELAYED_WORK_DEFERRABLE(&di->vbus_drop.end_work,
		ab8500_charger_vbus_drop_end_work);

	/* Init work for charger detection */
	INIT_WORK(&di->usb_link_status_work,
		ab8500_charger_usb_link_status_work);
	INIT_WORK(&di->detect_usb_type_work,
		ab8500_charger_detect_usb_type_work);

	/* Init work for checking HW status */
	INIT_WORK(&di->check_usb_thermal_prot_work,
		ab8500_charger_check_usb_thermal_prot_work);

	/*
	 * VDD ADC supply needs to be enabled from this driver when there
	 * is a charger connected to avoid erroneous BTEMP_HIGH/LOW
	 * interrupts during charging
	 */
	di->regu = regulator_get(di->dev, "vddadc");
	if (IS_ERR(di->regu)) {
		ret = PTR_ERR(di->regu);
		dev_err(di->dev, "failed to get vddadc regulator\n");
		goto free_charger_wq;
	}


	/* Initialize OVV, and other registers */
	ret = ab8500_charger_init_hw_registers(di);
	if (ret) {
		dev_err(di->dev, "failed to initialize ABB registers\n");
		goto free_regulator;
	}

	/* Register AC charger class */
	ret = power_supply_register(di->dev, &di->ac_chg.psy);
	if (ret) {
		dev_err(di->dev, "failed to register AC charger\n");
		goto free_regulator;
	}

	/* Register USB charger class */
	ret = power_supply_register(di->dev, &di->usb_chg.psy);
	if (ret) {
		dev_err(di->dev, "failed to register USB charger\n");
		goto free_ac;
	}

	di->otg = otg_get_transceiver();
	if (!di->otg) {
		dev_err(di->dev, "failed to get otg transceiver\n");
		ret = -EINVAL;
		goto free_usb;
	}
	di->nb.notifier_call = ab8500_charger_usb_notifier_call;
	ret = otg_register_notifier(di->otg, &di->nb);
	if (ret) {
		dev_err(di->dev, "failed to register otg notifier\n");
		goto put_otg_transceiver;
	}

	/* Identify the connected charger types during startup */
	charger_status = ab8500_charger_detect_chargers(di, true);
	if (charger_status & AC_PW_CONN) {
		di->ac.charger_connected = 1;
		di->ac_conn = true;
		ab8500_power_supply_changed(di, &di->ac_chg.psy);
		sysfs_notify(&di->ac_chg.psy.dev->kobj, NULL, "present");
	}

	if (charger_status & USB_PW_CONN) {
		dev_dbg(di->dev, "VBUS Detect during startup\n");
		di->vbus_detected = true;
		di->vbus_detected_start = true;
		queue_work(di->charger_wq,
			&di->detect_usb_type_work);
	}

	/* Register interrupts */
	for (i = 0; i < ARRAY_SIZE(ab8500_charger_irq); i++) {
		irq = platform_get_irq_byname(pdev, ab8500_charger_irq[i].name);
		ret = request_threaded_irq(irq, NULL, ab8500_charger_irq[i].isr,
			IRQF_SHARED | IRQF_NO_SUSPEND,
			ab8500_charger_irq[i].name, di);

		if (ret != 0) {
			dev_err(di->dev, "failed to request %s IRQ %d: %d\n"
				, ab8500_charger_irq[i].name, irq, ret);
			goto free_irq;
		}
		dev_dbg(di->dev, "Requested %s IRQ %d: %d\n",
			ab8500_charger_irq[i].name, irq, ret);
	}

	platform_set_drvdata(pdev, di);

	return ret;

free_irq:
	otg_unregister_notifier(di->otg, &di->nb);

	/* We also have to free all successfully registered irqs */
	for (i = i - 1; i >= 0; i--) {
		irq = platform_get_irq_byname(pdev, ab8500_charger_irq[i].name);
		free_irq(irq, di);
	}
put_otg_transceiver:
	otg_put_transceiver(di->otg);
free_usb:
	power_supply_unregister(&di->usb_chg.psy);
free_ac:
	power_supply_unregister(&di->ac_chg.psy);
free_regulator:
	regulator_put(di->regu);
free_charger_wq:
	destroy_workqueue(di->charger_wq);
free_device_info:
	kfree(di);

	return ret;
}

static struct platform_driver ab8500_charger_driver = {
	.probe = ab8500_charger_probe,
	.remove = __devexit_p(ab8500_charger_remove),
	.suspend = ab8500_charger_suspend,
	.resume = ab8500_charger_resume,
	.driver = {
		.name = "ab8500-charger",
		.owner = THIS_MODULE,
	},
};

static int __init ab8500_charger_init(void)
{
	return platform_driver_register(&ab8500_charger_driver);
}

static void __exit ab8500_charger_exit(void)
{
	platform_driver_unregister(&ab8500_charger_driver);
}

subsys_initcall_sync(ab8500_charger_init);
module_exit(ab8500_charger_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Johan Palsson, Karl Komierowski, Arun R Murthy");
MODULE_ALIAS("platform:ab8500-charger");
MODULE_DESCRIPTION("AB8500 charger management driver");
