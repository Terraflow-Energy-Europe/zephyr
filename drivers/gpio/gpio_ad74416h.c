/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* AD74416H GPIO controller on top of the core driver.
 *
 *   Lines 0..5   GPIO_A..GPIO_F pins (GPIO_CONFIGn), input or output.
 *   Lines 6..9   Debounced digital input of channels A..D (DIN_COMP_OUT),
 *                input only. The channel needs a digital input function.
 *   Lines 10..13 External FET digital output of channels A..D
 *                (DO_EXT_CONFIGn), output only, SPI controlled.
 */

#define DT_DRV_COMPAT adi_ad74416h_gpio

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_utils.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/mfd/ad74416h.h>

BUILD_ASSERT(CONFIG_GPIO_AD74416H_INIT_PRIORITY > CONFIG_MFD_INIT_PRIORITY,
	     "AD74416H GPIO must init after the MFD core");

LOG_MODULE_REGISTER(gpio_ad74416h, CONFIG_GPIO_LOG_LEVEL);

#define LINE_DIN_FIRST AD74416H_GPIO_DIN(0)
#define LINE_DO_FIRST  AD74416H_GPIO_DO(0)
#define NUM_LINES      (LINE_DO_FIRST + AD74416H_NUM_CHANNELS)

#define DIN_MASK GENMASK(LINE_DO_FIRST - 1, LINE_DIN_FIRST)

struct ad74416h_gpio_config {
	struct gpio_driver_config common;
	const struct device *parent;
};

struct ad74416h_gpio_data {
	struct gpio_driver_data common;
};

static int configure_pin(const struct device *parent, gpio_pin_t pin, gpio_flags_t flags)
{
	uint16_t val = FIELD_PREP(AD74416H_GPIO_GP_WK_PD_EN, (flags & GPIO_PULL_DOWN) != 0);

	if ((flags & GPIO_OUTPUT) != 0) {
		/* Output mode keeps the input buffer on, so input also works */
		val |= FIELD_PREP(AD74416H_GPIO_SELECT, AD74416H_GPIO_SEL_GPO) |
		       FIELD_PREP(AD74416H_GPIO_GPO_DATA, (flags & GPIO_OUTPUT_INIT_HIGH) != 0);
	} else if ((flags & GPIO_INPUT) != 0) {
		val |= FIELD_PREP(AD74416H_GPIO_SELECT, AD74416H_GPIO_SEL_GPI);
	} else {
		val |= FIELD_PREP(AD74416H_GPIO_SELECT, AD74416H_GPIO_SEL_HIGH_Z);
	}

	return ad74416h_reg_write(parent, AD74416H_REG_GPIO_CONFIG(pin), val);
}

static int configure_do(const struct device *parent, uint8_t ch, gpio_flags_t flags)
{
	uint16_t val = 0;

	if ((flags & GPIO_INPUT) != 0) {
		return -ENOTSUP;
	}

	if ((flags & GPIO_OUTPUT) != 0) {
		val = AD74416H_DO_MODE |
		      FIELD_PREP(AD74416H_DO_DATA, (flags & GPIO_OUTPUT_INIT_HIGH) != 0);
	}

	/* SPI control (DO_SRC_SEL = 0), keep the T1 and T2 timers */
	return ad74416h_reg_update(parent, AD74416H_REG_DO_EXT_CONFIG(ch),
				   AD74416H_DO_MODE | AD74416H_DO_DATA | AD74416H_DO_SRC_SEL, val);
}

static int ad74416h_gpio_pin_configure(const struct device *dev, gpio_pin_t pin, gpio_flags_t flags)
{
	const struct ad74416h_gpio_config *config = dev->config;

	if ((flags & (GPIO_PULL_UP | GPIO_SINGLE_ENDED)) != 0) {
		return -ENOTSUP;
	}

	if (pin < AD74416H_NUM_GPIOS) {
		return configure_pin(config->parent, pin, flags);
	}

	if (pin < LINE_DO_FIRST) {
		/* Digital input: read only, configured by the channel function */
		return (flags & GPIO_OUTPUT) != 0 ? -ENOTSUP : 0;
	}

	if (pin < NUM_LINES) {
		if ((flags & GPIO_PULL_DOWN) != 0) {
			return -ENOTSUP;
		}
		return configure_do(config->parent, pin - LINE_DO_FIRST, flags);
	}

	return -EINVAL;
}

static int ad74416h_gpio_port_get_raw(const struct device *dev, gpio_port_value_t *value)
{
	const struct ad74416h_gpio_config *config = dev->config;
	uint8_t regs[AD74416H_NUM_GPIOS + 1 + AD74416H_NUM_CHANNELS];
	uint16_t vals[ARRAY_SIZE(regs)];
	gpio_port_value_t v = 0;
	int ret;

	for (int i = 0; i < AD74416H_NUM_GPIOS; i++) {
		regs[i] = AD74416H_REG_GPIO_CONFIG(i);
	}
	regs[AD74416H_NUM_GPIOS] = AD74416H_REG_DIN_COMP_OUT;
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		regs[AD74416H_NUM_GPIOS + 1 + ch] = AD74416H_REG_DO_EXT_CONFIG(ch);
	}

	ret = ad74416h_reg_read_multi(config->parent, regs, vals, ARRAY_SIZE(regs));
	if (ret != 0) {
		return ret;
	}

	for (int i = 0; i < AD74416H_NUM_GPIOS; i++) {
		if ((vals[i] & AD74416H_GPIO_GPI_DATA) != 0) {
			v |= BIT(i);
		}
	}
	v |= (gpio_port_value_t)(vals[AD74416H_NUM_GPIOS] & 0xF) << LINE_DIN_FIRST;
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		if ((vals[AD74416H_NUM_GPIOS + 1 + ch] & AD74416H_DO_DATA) != 0) {
			v |= BIT(LINE_DO_FIRST + ch);
		}
	}

	*value = v;
	return 0;
}

static int ad74416h_gpio_port_set_masked_raw(const struct device *dev, gpio_port_pins_t mask,
					     gpio_port_value_t value)
{
	const struct ad74416h_gpio_config *config = dev->config;
	int ret = 0;

	if ((mask & DIN_MASK) != 0) {
		return -ENOTSUP;
	}

	ad74416h_lock(config->parent);

	for (int i = 0; ret == 0 && i < NUM_LINES; i++) {
		if ((mask & BIT(i)) == 0) {
			continue;
		}

		if (i < AD74416H_NUM_GPIOS) {
			ret = ad74416h_reg_update(config->parent, AD74416H_REG_GPIO_CONFIG(i),
						  AD74416H_GPIO_GPO_DATA,
						  (value & BIT(i)) ? AD74416H_GPIO_GPO_DATA : 0);
		} else if (i >= LINE_DO_FIRST) {
			ret = ad74416h_do_set(config->parent, i - LINE_DO_FIRST,
					      (value & BIT(i)) != 0);
		}
	}

	ad74416h_unlock(config->parent);
	return ret;
}

static int ad74416h_gpio_port_set_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return ad74416h_gpio_port_set_masked_raw(dev, pins, pins);
}

static int ad74416h_gpio_port_clear_bits_raw(const struct device *dev, gpio_port_pins_t pins)
{
	return ad74416h_gpio_port_set_masked_raw(dev, pins, 0);
}

/* Output state, from GPO_DATA and DO_DATA */
static int output_state_get(const struct device *dev, gpio_port_value_t *value)
{
	const struct ad74416h_gpio_config *config = dev->config;
	uint8_t regs[AD74416H_NUM_GPIOS + AD74416H_NUM_CHANNELS];
	uint16_t vals[ARRAY_SIZE(regs)];
	gpio_port_value_t v = 0;
	int ret;

	for (int i = 0; i < AD74416H_NUM_GPIOS; i++) {
		regs[i] = AD74416H_REG_GPIO_CONFIG(i);
	}
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		regs[AD74416H_NUM_GPIOS + ch] = AD74416H_REG_DO_EXT_CONFIG(ch);
	}

	ret = ad74416h_reg_read_multi(config->parent, regs, vals, ARRAY_SIZE(regs));
	if (ret != 0) {
		return ret;
	}

	for (int i = 0; i < AD74416H_NUM_GPIOS; i++) {
		if ((vals[i] & AD74416H_GPIO_GPO_DATA) != 0) {
			v |= BIT(i);
		}
	}
	for (int ch = 0; ch < AD74416H_NUM_CHANNELS; ch++) {
		if ((vals[AD74416H_NUM_GPIOS + ch] & AD74416H_DO_DATA) != 0) {
			v |= BIT(LINE_DO_FIRST + ch);
		}
	}

	*value = v;
	return 0;
}

static int ad74416h_gpio_port_toggle_bits(const struct device *dev, gpio_port_pins_t pins)
{
	const struct ad74416h_gpio_config *config = dev->config;
	gpio_port_value_t cur;
	int ret;

	ad74416h_lock(config->parent);

	ret = output_state_get(dev, &cur);
	if (ret == 0) {
		ret = ad74416h_gpio_port_set_masked_raw(dev, pins, ~cur);
	}

	ad74416h_unlock(config->parent);
	return ret;
}

static int ad74416h_gpio_pin_interrupt_configure(const struct device *dev, gpio_pin_t pin,
						 enum gpio_int_mode mode, enum gpio_int_trig trig)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(pin);
	ARG_UNUSED(trig);

	return mode == GPIO_INT_MODE_DISABLED ? 0 : -ENOTSUP;
}

static int ad74416h_gpio_init(const struct device *dev)
{
	const struct ad74416h_gpio_config *config = dev->config;

	return device_is_ready(config->parent) ? 0 : -ENODEV;
}

static DEVICE_API(gpio, ad74416h_gpio_api) = {
	.pin_configure = ad74416h_gpio_pin_configure,
	.port_get_raw = ad74416h_gpio_port_get_raw,
	.port_set_masked_raw = ad74416h_gpio_port_set_masked_raw,
	.port_set_bits_raw = ad74416h_gpio_port_set_bits_raw,
	.port_clear_bits_raw = ad74416h_gpio_port_clear_bits_raw,
	.port_toggle_bits = ad74416h_gpio_port_toggle_bits,
	.pin_interrupt_configure = ad74416h_gpio_pin_interrupt_configure,
};

#define AD74416H_GPIO_INIT(n)                                                                      \
	static const struct ad74416h_gpio_config ad74416h_gpio_config_##n = {                      \
		.common = {.port_pin_mask = GPIO_PORT_PIN_MASK_FROM_DT_INST(n) &                   \
					    GENMASK(NUM_LINES - 1, 0)},                            \
		.parent = DEVICE_DT_GET(DT_INST_PARENT(n)),                                        \
	};                                                                                         \
                                                                                                   \
	static struct ad74416h_gpio_data ad74416h_gpio_data_##n;                                   \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, ad74416h_gpio_init, NULL, &ad74416h_gpio_data_##n,                \
			      &ad74416h_gpio_config_##n, POST_KERNEL,                              \
			      CONFIG_GPIO_AD74416H_INIT_PRIORITY, &ad74416h_gpio_api);

DT_INST_FOREACH_STATUS_OKAY(AD74416H_GPIO_INIT)
