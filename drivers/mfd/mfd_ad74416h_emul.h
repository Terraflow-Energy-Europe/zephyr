/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Backdoor API of the AD74416H emulator, for tests.
 */

#ifndef ZEPHYR_DRIVERS_MFD_MFD_AD74416H_EMUL_H_
#define ZEPHYR_DRIVERS_MFD_MFD_AD74416H_EMUL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/drivers/emul.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Number of channel function writes kept per channel */
#define AD74416H_EMUL_FUNC_HISTORY 16

/** What the emulator observed on the SPI bus */
struct ad74416h_emul_stats {
	/** Valid frames received */
	uint32_t frames;
	/** Frames with a bad CRC or length */
	uint32_t bad_frames;
	/** Device resets (software keys) */
	uint32_t resets;
	/**
	 * Data sheet sequence violations: a function change without high
	 * impedance in between, a function held for less than the channel
	 * initialization time, or VOUT_RANGE/VIOUT_DRV_EN_DLY changed outside
	 * high impedance.
	 */
	uint32_t violations;
	/** CH_FUNC values written per channel, oldest first */
	uint8_t func_history[4][AD74416H_EMUL_FUNC_HISTORY];
	uint8_t func_history_len[4];
};

/** Read a register without side effects. */
uint16_t ad74416h_emul_reg_get(const struct emul *target, uint8_t reg);

/** Write a register without side effects. */
void ad74416h_emul_reg_set(const struct emul *target, uint8_t reg, uint16_t val);

/** Code the next conversion returns. Index 0..3 channels, 4..7 diagnostics. */
void ad74416h_emul_adc_code_set(const struct emul *target, uint8_t index, uint32_t code);

/** When set, conversions never complete. */
void ad74416h_emul_adc_hang(const struct emul *target, bool hang);

/** Level on a GPIO pin (0..5) configured as input. */
void ad74416h_emul_gpi_set(const struct emul *target, uint8_t pin, bool level);

/** Debounced digital input states, bit n is channel n. */
void ad74416h_emul_din_set(const struct emul *target, uint8_t state);

/** Value of a digital input counter. */
void ad74416h_emul_din_counter_set(const struct emul *target, uint8_t ch, uint32_t count);

/** Finish linear DAC slewing on a channel (DAC_ACTIVE = DAC_CODE). */
void ad74416h_emul_dac_settle(const struct emul *target, uint8_t ch);

/** Set channel alert bits (CHANNEL_ALERT_STATUSn) and update ALERT. */
void ad74416h_emul_channel_alert_set(const struct emul *target, uint8_t ch, uint16_t bits);

/** Set supply alert bits (SUPPLY_ALERT_STATUS) and update ALERT. */
void ad74416h_emul_supply_alert_set(const struct emul *target, uint16_t bits);

/** Make CAL_MEM_ERR stick, as with a faulty calibration memory. */
void ad74416h_emul_cal_mem_fault(const struct emul *target, bool fault);

/** Get the bus statistics. */
const struct ad74416h_emul_stats *ad74416h_emul_stats_get(const struct emul *target);

/** Clear the bus statistics. */
void ad74416h_emul_stats_clear(const struct emul *target);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_MFD_MFD_AD74416H_EMUL_H_ */
