/*
 * Copyright (c) 2021 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * This is a simplified version of battery_voltage_divider.c which always reads
 * the VDDHDIV5 channel of the &adc node and multiplies it by 5.
 */

#define DT_DRV_COMPAT zmk_battery_nrf_vddh

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include <drivers/sensor/battery/battery_charging.h>
#include "battery_common.h"

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define VDDHDIV (5)

static const struct device *adc = DEVICE_DT_GET(DT_NODELABEL(adc));

struct vddh_config {
    struct gpio_dt_spec chg;
};

struct vddh_data {
    struct adc_channel_cfg acc;
    struct adc_sequence as;
    struct battery_value value;
};

static int vddh_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    // Make sure selected channel is supported
    if (chan != SENSOR_CHAN_GAUGE_VOLTAGE && chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE &&
        (enum sensor_channel_bvd)chan != SENSOR_CHAN_CHARGING && chan != SENSOR_CHAN_ALL) {
        LOG_DBG("Selected channel is not supported: %d.", chan);
        return -ENOTSUP;
    }

    struct vddh_data *drv_data = dev->data;
    const struct vddh_config *drv_cfg = dev->config;
    struct adc_sequence *as = &drv_data->as;

    int rc = adc_read(adc, as);
    as->calibrate = false;

    if (rc != 0) {
        LOG_ERR("Failed to read ADC: %d", rc);
        return rc;
    }

    int32_t val = drv_data->value.adc_raw;
    rc = adc_raw_to_millivolts(adc_ref_internal(adc), drv_data->acc.gain, as->resolution, &val);
    if (rc != 0) {
        LOG_ERR("Failed to convert raw ADC to mV: %d", rc);
        return rc;
    }

    drv_data->value.millivolts = val * VDDHDIV;
    drv_data->value.state_of_charge = lithium_ion_mv_to_pct(drv_data->value.millivolts);

    LOG_DBG("ADC raw %d ~ %d mV => %d%%", drv_data->value.adc_raw, drv_data->value.millivolts,
            drv_data->value.state_of_charge);

#if DT_INST_NODE_HAS_PROP(0, chg_gpios)
    int raw = gpio_pin_get_dt(&drv_cfg->chg);
    if (raw == -EIO || raw == -EWOULDBLOCK) {
        LOG_DBG("Failed to read chg status: %d", raw);
        return raw;
    } else {
        bool charging = raw;
        LOG_DBG("Charging state: %d", raw);
        drv_data->value.charging = charging;
    }
#endif

    return rc;
}

static int vddh_channel_get(const struct device *dev, enum sensor_channel chan,
                            struct sensor_value *val) {
    struct vddh_data const *drv_data = dev->data;
    return battery_channel_get(&drv_data->value, chan, val);
}

static const struct sensor_driver_api vddh_api = {
    .sample_fetch = vddh_sample_fetch,
    .channel_get = vddh_channel_get,
};

static int vddh_init(const struct device *dev) {
    struct vddh_data *drv_data = dev->data;
    const struct vddh_config *drv_cfg = dev->config;

    if (!device_is_ready(adc)) {
        LOG_ERR("ADC device is not ready %s", adc->name);
        return -ENODEV;
    }

    drv_data->as = (struct adc_sequence){
        .channels = BIT(0),
        .buffer = &drv_data->value.adc_raw,
        .buffer_size = sizeof(drv_data->value.adc_raw),
        .oversampling = 4,
        .calibrate = true,
    };

#ifdef CONFIG_ADC_NRFX_SAADC
    drv_data->acc = (struct adc_channel_cfg){
        .gain = ADC_GAIN_1_2,
        .reference = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 40),
        .input_positive = SAADC_CH_PSELN_PSELN_VDDHDIV5,
    };

    drv_data->as.resolution = 12;
#else
#error Unsupported ADC
#endif

    int rc = adc_channel_setup(adc, &drv_data->acc);
    LOG_DBG("VDDHDIV5 setup returned %d", rc);

#if DT_INST_NODE_HAS_PROP(0, chg_gpios)
    if (!device_is_ready(drv_cfg->chg.port)) {
        LOG_ERR("GPIO port for chg reading is not ready");
        return -ENODEV;
    }
    rc = gpio_pin_configure_dt(&drv_cfg->chg, GPIO_INPUT);
    if (rc != 0) {
        LOG_ERR("Failed to set chg feed %u: %d", drv_cfg->chg.pin, rc);
        return rc;
    }
#endif // DT_INST_NODE_HAS_PROP(0, chg_gpios)

    return rc;
}

static struct vddh_data vddh_data;

static const struct vddh_config vddh_cfg = {
#if DT_INST_NODE_HAS_PROP(0, chg_gpios)
    .chg = GPIO_DT_SPEC_INST_GET(0, chg_gpios),
#endif
};

DEVICE_DT_INST_DEFINE(0, &vddh_init, NULL, &vddh_data, &vddh_cfg, POST_KERNEL,
                      CONFIG_SENSOR_INIT_PRIORITY, &vddh_api);
