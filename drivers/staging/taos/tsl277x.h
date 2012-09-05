/* drivers/staging/taos/tsl277x.h
 *
 * Copyright (C) 2011 Sony Ericsson Mobile Communications AB.
 *
 * Author: Aleksej Makarov <aleksej.makarov@sonyericsson.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2, as
 * published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */


#ifndef __TSL277X_H
#define __TSL277X_H

struct device;

enum tsl2772_pwr_state {
	POWER_ON,
	POWER_OFF,
	POWER_STANDBY,
};

enum taos_ctrl_reg {
	AGAIN_1        = (0 << 0),
	AGAIN_8        = (1 << 0),
	AGAIN_16       = (2 << 0),
	AGAIN_120      = (3 << 0),
	PGAIN_1        = (0 << 2),
	PGAIN_2        = (1 << 2),
	PGAIN_4        = (2 << 2),
	PGAIN_8        = (3 << 2),
	PDIOD_NO       = (0 << 4),
	PDIOD_CH0      = (1 << 4),
	PDIOD_CH1      = (2 << 4),
	PDIOD_DONT_USE = (3 << 4),
	PDRIVE_120MA   = (0 << 6),
	PDRIVE_60MA    = (1 << 6),
	PDRIVE_30MA    = (2 << 6),
	PDRIVE_15MA    = (3 << 6),
};

#define PRX_PERSIST(p) (((p) & 0xf) << 4)
#define ALS_PERSIST(p) (((p) & 0xf) << 0)

struct taos_raw_settings {
	u8 als_time;
	u8 als_gain;
	u8 prx_time;
	u8 wait_time;
	u8 persist;
	u8 cfg_reg;
	u8 prox_pulse_cnt;
	u8 ctrl_reg;
	u8 prox_offs;
};

struct taos_parameters {
	u16 prox_th_min;
	u16 prox_th_max;
	u16 als_gate;
	u16 als_gain;
};

struct lux_segment {
	u32 ratio;
	u32 k0;
	u32 k1;
};

/**
 * struct taos_platform_data - platform data for the TAOS tsl2772 driver
 * platform dependent callbacks.
*/
struct tsl2772_platform_data {
	/* The following callback for power events received and handled by
	   the driver.  Currently only for SUSPEND and RESUME */
	int (*platform_power)(struct device *dev, enum tsl2772_pwr_state state);
	int (*platform_init)(struct device *dev);
	void (*platform_teardown)(struct device *dev);
	char const *prox_name;
	char const *als_name;
	struct taos_parameters parameters;
	struct taos_raw_settings const *raw_settings;
	bool proximity_can_wake;
	bool als_can_wake;
	struct lux_segment *segment;
	int segment_num;
};

#endif /* __TSL277X_H */
