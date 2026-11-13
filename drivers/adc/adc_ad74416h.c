/*
 * Copyright (c) 2026 Terraflow Energy Europe
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AD74416H ADC: Zephyr ADC API on top of the core driver.
 *
 * ADC channels 0..3 convert I/O channels A..D (24-bit codes, range, rate and
 * input set by the channel configuration). ADC channels 4..7 convert
 * diagnostic slots 0..3 (16-bit codes). All channels in a sequence are
 * converted in one AD74416H conversion sequence.
 *
 * Samples are raw codes. With resolution 24 every sample takes 32 bits; with
 * resolution 16 only diagnostic channels are allowed and samples take 16
 * bits.
 */

#define DT_DRV_COMPAT adi_ad74416h_adc

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zephyr/drivers/mfd/ad74416h.h>

#define ADC_CONTEXT_USES_KERNEL_TIMER
#include "adc_context.h"

BUILD_ASSERT(CONFIG_ADC_AD74416H_INIT_PRIORITY > CONFIG_MFD_INIT_PRIORITY,
	     "AD74416H ADC must init after the MFD core");

LOG_MODULE_REGISTER(adc_ad74416h, CONFIG_ADC_LOG_LEVEL);

#define NUM_ADC_CHANNELS (AD74416H_NUM_CHANNELS + AD74416H_NUM_DIAGS)
#define CH_MASK          GENMASK(AD74416H_NUM_CHANNELS - 1, 0)
#define DIAG_MASK        GENMASK(NUM_ADC_CHANNELS - 1, AD74416H_NUM_CHANNELS)

/* Worst case: four channels at 10 SPS plus four diagnostics at 20 SPS */
#define SEQUENCE_TIMEOUT K_MSEC(1000)

/* The ADC measures against a 2.5 V reference */
#define AD74416H_ADC_REF_MV 2500

struct ad74416h_adc_config {
	const struct device *parent;
	k_thread_stack_t *stack;
};

struct ad74416h_adc_data {
	const struct device *dev;
	struct adc_context ctx;
	struct k_sem acq_sem;
	struct k_thread thread;
	uint8_t configured;
	uint8_t resolution;
	uint32_t channels;
	void *buffer;
	void *repeat_buffer;
};

static int ad74416h_adc_channel_setup(const struct device *dev, const struct adc_channel_cfg *cfg)
{
	struct ad74416h_adc_data *data = dev->data;

	if (cfg->channel_id >= NUM_ADC_CHANNELS) {
		return -EINVAL;
	}

	if (cfg->gain != ADC_GAIN_1 || cfg->reference != ADC_REF_INTERNAL ||
	    cfg->acquisition_time != ADC_ACQ_TIME_DEFAULT) {
		/* Range and rate come from the channel configuration */
		LOG_ERR("Channel %u: use ADC_GAIN_1, ADC_REF_INTERNAL, ADC_ACQ_TIME_DEFAULT",
			cfg->channel_id);
		return -ENOTSUP;
	}

	data->configured |= BIT(cfg->channel_id);
	return 0;
}

static int validate_sequence(const struct device *dev, const struct adc_sequence *seq)
{
	struct ad74416h_adc_data *data = dev->data;
	size_t sample_size;
	size_t needed;

	if (seq->channels == 0 || (seq->channels & ~data->configured) != 0) {
		return -EINVAL;
	}

	if (seq->oversampling != 0 || seq->calibrate) {
		return -ENOTSUP;
	}

	if (seq->resolution == 24) {
		sample_size = sizeof(uint32_t);
	} else if (seq->resolution == 16 && (seq->channels & CH_MASK) == 0) {
		sample_size = sizeof(uint16_t);
	} else {
		return -EINVAL;
	}

	needed = POPCOUNT(seq->channels) * sample_size;
	if (seq->options != NULL) {
		needed *= 1 + seq->options->extra_samplings;
	}
	if (seq->buffer_size < needed) {
		return -ENOMEM;
	}

	return 0;
}

static void adc_context_start_sampling(struct adc_context *ctx)
{
	struct ad74416h_adc_data *data = CONTAINER_OF(ctx, struct ad74416h_adc_data, ctx);

	data->channels = ctx->sequence.channels;
	data->repeat_buffer = data->buffer;
	k_sem_give(&data->acq_sem);
}

static void adc_context_update_buffer_pointer(struct adc_context *ctx, bool repeat_sampling)
{
	struct ad74416h_adc_data *data = CONTAINER_OF(ctx, struct ad74416h_adc_data, ctx);

	if (repeat_sampling) {
		data->buffer = data->repeat_buffer;
	}
}

static int start_read(const struct device *dev, const struct adc_sequence *seq)
{
	struct ad74416h_adc_data *data = dev->data;
	int ret;

	ret = validate_sequence(dev, seq);
	if (ret != 0) {
		return ret;
	}

	data->buffer = seq->buffer;
	data->resolution = seq->resolution;
	adc_context_start_read(&data->ctx, seq);

	return adc_context_wait_for_completion(&data->ctx);
}

static int ad74416h_adc_read(const struct device *dev, const struct adc_sequence *seq)
{
	struct ad74416h_adc_data *data = dev->data;
	int ret;

	adc_context_lock(&data->ctx, false, NULL);
	ret = start_read(dev, seq);
	adc_context_release(&data->ctx, ret);

	return ret;
}

#ifdef CONFIG_ADC_ASYNC
static int ad74416h_adc_read_async(const struct device *dev, const struct adc_sequence *seq,
				   struct k_poll_signal *async)
{
	struct ad74416h_adc_data *data = dev->data;
	int ret;

	adc_context_lock(&data->ctx, true, async);
	ret = start_read(dev, seq);
	adc_context_release(&data->ctx, ret);

	return ret;
}
#endif

static void acquisition_thread(void *p1, void *p2, void *p3)
{
	const struct device *dev = p1;
	const struct ad74416h_adc_config *config = dev->config;
	struct ad74416h_adc_data *data = dev->data;
	uint32_t results[NUM_ADC_CHANNELS];
	int ret;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (true) {
		k_sem_take(&data->acq_sem, K_FOREVER);

		ret = ad74416h_adc_convert(config->parent, data->channels & CH_MASK,
					   (data->channels & DIAG_MASK) >> AD74416H_NUM_CHANNELS,
					   results, SEQUENCE_TIMEOUT);
		if (ret != 0) {
			adc_context_complete(&data->ctx, ret);
			continue;
		}

		/* Samples in channel order, as the ADC API requires */
		for (int i = 0; i < NUM_ADC_CHANNELS; i++) {
			if ((data->channels & BIT(i)) == 0) {
				continue;
			}
			if (data->resolution == 24) {
				*(uint32_t *)data->buffer = results[i];
				data->buffer = (uint32_t *)data->buffer + 1;
			} else {
				*(uint16_t *)data->buffer = results[i];
				data->buffer = (uint16_t *)data->buffer + 1;
			}
		}

		adc_context_on_sampling_done(&data->ctx, dev);
	}
}

static int ad74416h_adc_init(const struct device *dev)
{
	const struct ad74416h_adc_config *config = dev->config;
	struct ad74416h_adc_data *data = dev->data;
	k_tid_t tid;

	if (!device_is_ready(config->parent)) {
		return -ENODEV;
	}

	data->dev = dev;
	k_sem_init(&data->acq_sem, 0, 1);
	adc_context_init(&data->ctx);

	tid = k_thread_create(&data->thread, config->stack,
			      CONFIG_ADC_AD74416H_ACQUISITION_THREAD_STACK_SIZE, acquisition_thread,
			      (void *)dev, NULL, NULL, CONFIG_ADC_AD74416H_ACQUISITION_THREAD_PRIO,
			      0, K_NO_WAIT);
	k_thread_name_set(tid, dev->name);

	adc_context_unlock_unconditionally(&data->ctx);
	return 0;
}

static DEVICE_API(adc, ad74416h_adc_api) = {
	.channel_setup = ad74416h_adc_channel_setup,
	.read = ad74416h_adc_read,
#ifdef CONFIG_ADC_ASYNC
	.read_async = ad74416h_adc_read_async,
#endif
	.ref_internal = AD74416H_ADC_REF_MV,
};

#define AD74416H_ADC_INIT(n)                                                                       \
	static K_KERNEL_STACK_DEFINE(ad74416h_adc_stack_##n,                                       \
				     CONFIG_ADC_AD74416H_ACQUISITION_THREAD_STACK_SIZE);           \
                                                                                                   \
	static const struct ad74416h_adc_config ad74416h_adc_config_##n = {                        \
		.parent = DEVICE_DT_GET(DT_INST_PARENT(n)),                                        \
		.stack = ad74416h_adc_stack_##n,                                                   \
	};                                                                                         \
                                                                                                   \
	static struct ad74416h_adc_data ad74416h_adc_data_##n;                                     \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(n, ad74416h_adc_init, NULL, &ad74416h_adc_data_##n,                  \
			      &ad74416h_adc_config_##n, POST_KERNEL,                               \
			      CONFIG_ADC_AD74416H_INIT_PRIORITY, &ad74416h_adc_api);

DT_INST_FOREACH_STATUS_OKAY(AD74416H_ADC_INIT)
