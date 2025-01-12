#include "sensor.h"

#define LOG_TAG "sensor"
#define LOG_LVL  4
#include <elog.h>

#include <math.h>
#include <limits.h>
#include <cmsis_os.h>
#include <FreeRTOS.h>
#include <queue.h>

#include "stm32l0xx_hal.h"
#include "settings.h"
#include "task_sensor.h"
#include "tsl2591.h"
#include "light.h"
#include "util.h"

#define SENSOR_TARGET_READ_ITERATIONS 2
#define SENSOR_GAIN_CAL_READ_ITERATIONS 5
#define SENSOR_GAIN_LED_CHECK_READ_ITERATIONS 2

/* These constants are for the matte white stage plate */
#define GAIN_CAL_BRIGHTNESS_LOW_MED   128
#define GAIN_CAL_BRIGHTNESS_MED_HIGH  128  /* actual value determined dynamically */
#define GAIN_CAL_BRIGHTNESS_HIGH_MAX  8    /* actual value determined dynamically */

#define LIGHT_CAL_CH0_TARGET_FACTOR   (0.98F)
#define GAIN_CAL_CH0_TARGET_FACTOR    (0.75F)

/* Number of iterations to use for light source calibration */
#define LIGHT_CAL_ITERATIONS 600

static osStatus_t sensor_gain_calibration_loop(
    tsl2591_gain_t gain0, tsl2591_gain_t gain1, tsl2591_time_t time,
    uint8_t led_brightness,
    float *gain_ch0, float *gain_ch1,
    sensor_gain_calibration_status_t callback_status,
    sensor_gain_calibration_callback_t callback, void *user_data);
static bool sensor_gain_calibration_cooldown(sensor_gain_calibration_callback_t callback, void *user_data);
static osStatus_t sensor_find_gain_brightness(uint8_t *led_brightness,
    tsl2591_gain_t gain, tsl2591_time_t time,
    uint8_t start_brightness, uint8_t end_brightness,
    float target_factor,
    sensor_gain_calibration_callback_t callback, void *user_data);
static bool gain_status_callback(
    sensor_gain_calibration_callback_t callback,
    sensor_gain_calibration_status_t status, int param,
    void *user_data);
static osStatus_t sensor_raw_read_loop(uint8_t count, float *ch0_avg, float *ch1_avg);
static uint8_t sensor_get_read_brightness(sensor_light_t light_source);

osStatus_t sensor_gain_calibration(sensor_gain_calibration_callback_t callback, void *user_data)
{
    /*
     * The sensor gain calibration process currently uses hand-picked
     * values for the integration time and transmission LED brightness.
     * Eventually, some mechanism for auto-ranging on the ideal LED
     * brightness for each step should be implemented, but the current
     * approach is likely good enough for now.
     */

    osStatus_t ret = osOK;
    uint8_t measurement_led_brightness = 0;
    uint8_t max_gain_led_brightness = 0;
    settings_cal_light_t cal_light = {0};
    float gain_med_ch0 = NAN;
    float gain_med_ch1 = NAN;
    float gain_high_ch0 = NAN;
    float gain_high_ch1 = NAN;
    float gain_max_ch0 = NAN;
    float gain_max_ch1 = NAN;

    settings_get_cal_light(&cal_light);

    log_i("Starting gain calibration");

    if (!gain_status_callback(callback, SENSOR_GAIN_CALIBRATION_STATUS_INIT, 0, user_data)) { return osError; }

    /* Set lights to initial state */
    sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

    do {
        /* Put the sensor into a known initial state */
        ret = sensor_set_config(TSL2591_GAIN_MAXIMUM, TSL2591_TIME_100MS);
        if (ret != osOK) { break; }
        ret = sensor_start();
        if (ret != osOK) { break; }

        /* Wait for things to stabilize */
        osDelay(1000);

        /* Find the ideal measurement brightness, which should not saturate at high gain */
        ret = sensor_find_gain_brightness(&measurement_led_brightness,
            TSL2591_GAIN_HIGH, TSL2591_TIME_200MS,
            128, 64, LIGHT_CAL_CH0_TARGET_FACTOR,
            callback, user_data);
        if (ret != osOK || measurement_led_brightness == 0) { break; }

        /* Wait for LED cool down */
        if (!sensor_gain_calibration_cooldown(callback, user_data)) {
            ret = osError;
            break;
        }

        /* Calibrate the value for medium gain */
        log_i("Medium gain calibration");
        ret = sensor_gain_calibration_loop(
            TSL2591_GAIN_LOW, TSL2591_GAIN_MEDIUM, TSL2591_TIME_600MS,
            GAIN_CAL_BRIGHTNESS_LOW_MED, &gain_med_ch0, &gain_med_ch1,
            SENSOR_GAIN_CALIBRATION_STATUS_MEDIUM, callback, user_data);
        if (ret != osOK) { break; }

        log_i("Medium gain: CH0=%dx, CH1=%dx", lroundf(gain_med_ch0), lroundf(gain_med_ch1));

        if (gain_med_ch0 < TSL2591_GAIN_MEDIUM_MIN || gain_med_ch0 > TSL2591_GAIN_MEDIUM_MAX) {
            log_w("Medium CH0 gain out of range!");
            gain_med_ch0 = TSL2591_GAIN_MEDIUM_TYP;
        }
        if (gain_med_ch1 < TSL2591_GAIN_MEDIUM_MIN || gain_med_ch1 > TSL2591_GAIN_MEDIUM_MAX) {
            log_w("Medium CH1 gain out of range!");
            gain_med_ch1 = TSL2591_GAIN_MEDIUM_TYP;
        }

        /* Wait for LED cool down */
        if (!sensor_gain_calibration_cooldown(callback, user_data)) {
            ret = osError;
            break;
        }

        /* Calibrate the value for high gain, using the calibrated measurement brightness */
        log_i("High gain calibration");
        ret = sensor_gain_calibration_loop(
            TSL2591_GAIN_MEDIUM, TSL2591_GAIN_HIGH, TSL2591_TIME_200MS,
            measurement_led_brightness, &gain_high_ch0, &gain_high_ch1,
            SENSOR_GAIN_CALIBRATION_STATUS_HIGH, callback, user_data);
        if (ret != osOK) { break; }

        gain_high_ch0 *= gain_med_ch0;
        gain_high_ch1 *= gain_med_ch1;

        log_i("High gain: CH0=%dx, CH1=%dx", lroundf(gain_high_ch0), lroundf(gain_high_ch1));

        if (gain_high_ch0 < TSL2591_GAIN_HIGH_MIN || gain_high_ch0 > TSL2591_GAIN_HIGH_MAX) {
            log_w("High CH0 gain out of range!");
            gain_high_ch0 = TSL2591_GAIN_HIGH_TYP;
        }
        if (gain_high_ch1 < TSL2591_GAIN_HIGH_MIN || gain_high_ch1 > TSL2591_GAIN_HIGH_MAX) {
            log_w("High CH1 gain out of range!");
            gain_high_ch1 = TSL2591_GAIN_HIGH_TYP;
        }

        /* Wait for LED cool down */
        if (!sensor_gain_calibration_cooldown(callback, user_data)) {
            ret = osError;
            break;
        }

        /* Find the ideal brightness for testing maximum gain */
        ret = sensor_find_gain_brightness(&max_gain_led_brightness,
            TSL2591_GAIN_MAXIMUM, TSL2591_TIME_200MS,
            4, 16, GAIN_CAL_CH0_TARGET_FACTOR,
            callback, user_data);
        if (ret != osOK || max_gain_led_brightness == 0) { break; }

        /* Wait for LED cool down */
        if (!sensor_gain_calibration_cooldown(callback, user_data)) {
            ret = osError;
            break;
        }

        /* Calibrate the value for maximum gain */
        log_i("Maximum gain calibration");
        ret = sensor_gain_calibration_loop(
            TSL2591_GAIN_HIGH, TSL2591_GAIN_MAXIMUM, TSL2591_TIME_200MS,
            max_gain_led_brightness, &gain_max_ch0, &gain_max_ch1,
            SENSOR_GAIN_CALIBRATION_STATUS_MAXIMUM, callback, user_data);
        if (ret != osOK) { break; }

        gain_max_ch0 *= gain_high_ch0;
        gain_max_ch1 *= gain_high_ch1;

        log_i("Maximum gain: CH0=%dx, CH1=%dx", lroundf(gain_max_ch0), lroundf(gain_max_ch1));

        if (gain_max_ch0 < TSL2591_GAIN_MAXIMUM_CH0_MIN || gain_max_ch0 > TSL2591_GAIN_MAXIMUM_CH0_MAX) {
            log_w("Maximum CH0 gain out of range!");
            gain_max_ch0 = TSL2591_GAIN_MAXIMUM_CH0_TYP;
        }
        if (gain_max_ch1 < TSL2591_GAIN_MAXIMUM_CH1_MIN || gain_max_ch1 > TSL2591_GAIN_MAXIMUM_CH1_MAX) {
            log_w("Maximum CH1 gain out of range!");
            gain_max_ch1 = TSL2591_GAIN_MAXIMUM_CH1_TYP;
        }
    } while (0);

    if (ret == osOK) {
        if (!gain_status_callback(callback, SENSOR_GAIN_CALIBRATION_STATUS_DONE, 0, user_data)) { ret = osError; }
    } else {
        if (!gain_status_callback(callback, SENSOR_GAIN_CALIBRATION_STATUS_FAILED, 0, user_data)) { ret = osError; }
    }

    /* Turn off the sensor */
    sensor_stop();

    /* Turn off the lights */
    sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

    if (ret == osOK) {
        log_i("Gain calibration complete");

        log_d("Measurement light -> %d / 128", measurement_led_brightness);
        log_d("Low -> 1.000000 1.000000");
        log_d("Med -> %f %f", gain_med_ch0, gain_med_ch1);
        log_d("High -> %f %f", gain_high_ch0, gain_high_ch1);
        log_d("Max -> %f %f", gain_max_ch0, gain_max_ch1);

        settings_cal_light_t cal_light = {0};
        cal_light.reflection = 128;
        cal_light.transmission = measurement_led_brightness;
        if (settings_set_cal_light(&cal_light)) {
            log_i("Measurement light calibration saved");
        }

        settings_cal_gain_t cal_gain = {0};
        cal_gain.ch0_medium = gain_med_ch0;
        cal_gain.ch1_medium = gain_med_ch1;
        cal_gain.ch0_high = gain_high_ch0;
        cal_gain.ch1_high = gain_high_ch1;
        cal_gain.ch0_maximum = gain_max_ch0;
        cal_gain.ch1_maximum = gain_max_ch1;
        if (settings_set_cal_gain(&cal_gain)) {
            log_i("Gain calibration saved");
        }
    } else {
        log_e("Gain calibration failed");
    }

    return ret;
}

#ifdef TEST_LIGHT_CAL
osStatus_t sensor_light_calibration(sensor_light_t light_source, sensor_light_calibration_callback_t callback, void *user_data)
{
    osStatus_t ret = osOK;
    sensor_reading_t reading;
    uint32_t ticks_start;

    /*
     * Variables used for regression.
     * This is all being done with doubles so we don't need
     * to worry about sums on sensor reading calculations
     * overflowing.
     */
    const double n_real = (double)(LIGHT_CAL_ITERATIONS);
    double sum_x = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    double sum_y = 0.0;
    double sum_yy = 0.0;
    double denominator = 0.0;
    double slope = 0.0;
    double intercept = 0.0;
    double drop_factor = 0.0;

    /* Parameter validation */
    if (light_source != SENSOR_LIGHT_REFLECTION && light_source != SENSOR_LIGHT_TRANSMISSION) {
        return osErrorParameter;
    }

    log_i("Starting LED brightness calibration");

    do {
        /* Set lights to initial off state */
        ret = sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);
        if (ret != osOK) { break; }

        /* Rough delay for things to settle */
        osDelay(1000);

        /* Start the sensor */
        ret = sensor_set_config(TSL2591_GAIN_HIGH, TSL2591_TIME_200MS);
        if (ret != osOK) { break; }
        ret = sensor_start();
        if (ret != osOK) { break; }

        /* Swallow the first reading */
        ret = sensor_get_next_reading(&reading, 2000);
        if (ret != osOK) { break; }

        /* Set LED to full brightness at the next cycle */
        ret = sensor_set_light_mode(light_source, /*next_cycle*/true, 128);
        if (ret != osOK) { break; }

        /* Wait for another cycle which will trigger the LED on */
        ret = sensor_get_next_reading(&reading, 2000);
        if (ret != osOK) { break; }

        ticks_start = reading.reading_ticks;

        if (callback) {
            if (!callback(0, user_data)) { ret = osError; break; }
        }

        /* Iterate over 2 minutes of readings and accumulate regression data */
        log_d("Starting read loop");
        for (int i = 0; i < LIGHT_CAL_ITERATIONS; i++) {
            ret = sensor_get_next_reading(&reading, 1000);
            if (ret != osOK) { break; }

            double x = log((double)(reading.reading_ticks - ticks_start));

            sum_x += x;
            sum_xx += x * x;
            sum_xy += x * (double)reading.ch0_val;
            sum_y += (double)reading.ch0_val;
            sum_yy += (double)reading.ch0_val * (double)reading.ch0_val;

            uint8_t progress = lroundf((i / n_real) * 100.0F);
            if (callback) {
                if (!callback(progress, user_data)) { ret = osError; break; }
            }
        }
        log_d("Finished read loop");
    } while (0);

    /* Turn LED off */
    sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

    /* Stop the sensor */
    sensor_stop();

    if (callback) {
        if (!callback(100, user_data)) { ret = osError; }
    }

    osDelay(500);

    if (ret != osOK) {
        log_e("Light source calibration failed: %d", ret);
        return ret;
    }

    denominator = n_real * sum_xx - sum_x * sum_x;
    if (denominator <= 0.0) {
        log_e("Denominator calculation error: %f", denominator);
        return osError;
    }

    slope = (n_real * sum_xy - sum_x * sum_y) / denominator;
    intercept = (sum_y - slope * sum_x) / n_real;
    drop_factor = slope / intercept;

    /* The drop factor is supposed to be negative */
    if (drop_factor >= 0.0) {
        log_e("Drop factor calculation error: %f", drop_factor);
        return osError;
    }

    log_i("LED calibration run complete");

    log_d("Slope = %f", slope);
    log_d("Intercept = %f", intercept);
    log_d("Drop factor = %f", drop_factor);

    /*
     * Correction formula is:
     * ch_val - (ch_val * (drop_factor * log(elapsed_ticks)))
     */

    return os_to_hal_status(ret);
}
#endif

osStatus_t sensor_read_target(sensor_light_t light_source,
    float *ch0_result, float *ch1_result,
    sensor_read_callback_t callback, void *user_data)
{
    osStatus_t ret = osOK;
    uint8_t light_value = 0;
    sensor_reading_t reading;
    tsl2591_gain_t target_read_gain;
    float ch0_sum = 0;
    float ch1_sum = 0;
    float ch0_avg = NAN;
    float ch1_avg = NAN;

    if (light_source != SENSOR_LIGHT_REFLECTION && light_source != SENSOR_LIGHT_TRANSMISSION) {
        return osErrorParameter;
    }

    light_value = sensor_get_read_brightness(light_source);

    log_i("Starting sensor target read (light=%d)", light_value);

    do {
        /* Put the sensor and light into a known initial state, with maximum gain */
        ret = sensor_set_config(TSL2591_GAIN_MAXIMUM, TSL2591_TIME_100MS);
        if (ret != osOK) { break; }

        /* Activate light source synchronized with sensor cycle */
        ret = sensor_set_light_mode(light_source, /*next_cycle*/true, light_value);
        if (ret != osOK) { break; }

        /* Start the sensor */
        ret = sensor_start();
        if (ret != osOK) { break; }

        /* Do initial read to detect gain */
        ret = sensor_get_next_reading(&reading, 1000);
        if (ret != osOK) { break; }
        log_v("TSL2591[%d]: CH0=%d, CH1=%d", reading.reading_count, reading.ch0_val, reading.ch1_val);

        /* Invoke the progress callback */
        if (callback) { callback(user_data); }

        /*
         * Pick target gain based on previous result.
         * Since the measurement integration time will be approximately
         * double the initialization integration time, this detection
         * needs to be at a point slightly less than half the saturation
         * point for measurement readings.
         * The regular saturation detection won't work here, because
         * the 100ms saturation point is slightly greater than half-way.
         */
        if (reading.ch0_val > 32700 || reading.ch1_val > 32700) {
            target_read_gain = TSL2591_GAIN_HIGH;
        } else {
            target_read_gain = TSL2591_GAIN_MAXIMUM;
        }

        /* Switch to the target read gain and integration time */
        ret = sensor_set_config(target_read_gain, TSL2591_TIME_200MS);
        if (ret != osOK) { break; }

        /* Take the actual target measurement readings */
        for (int i = 0; i < SENSOR_TARGET_READ_ITERATIONS; i++) {
            float ch0_basic = 0;
            float ch1_basic = 0;

            ret = sensor_get_next_reading(&reading, 500);
            if (ret != osOK) { break; }
            log_v("TSL2591[%d]: CH0=%d, CH1=%d", reading.reading_count, reading.ch0_val, reading.ch1_val);

            /* Invoke the progress callback */
            if (callback) { callback(user_data); }

            /* Make sure we're consistent with our read cycles */
            if (reading.reading_count != i + 4) {
                log_e("Unexpected read cycle count: %d", reading.reading_count);
                ret = osError;
                break;
            }

            /* Make sure we didn't unexpectedly saturate */
            if (sensor_is_reading_saturated(&reading)) {
                log_e("Unexpected sensor saturation");
                ret = osError;
                break;
            }

            sensor_convert_to_basic_counts(&reading, &ch0_basic, &ch1_basic);
            ch0_sum += ch0_basic;
            ch1_sum += ch1_basic;
        }
        if (ret != osOK) { break; }

        ch0_avg = (ch0_sum / (float)SENSOR_TARGET_READ_ITERATIONS);
        ch1_avg = (ch1_sum / (float)SENSOR_TARGET_READ_ITERATIONS);
    } while (0);

    /* Turn off the sensor */
    sensor_stop();
    sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

    if (ret == osOK) {
        log_i("Sensor read complete");
        if (ch0_result) { *ch0_result = ch0_avg; }
        if (ch1_result) { *ch1_result = ch1_avg; }
    } else {
        log_e("Sensor read failed: ret=%d", ret);
        if (ret == osOK) {
            ret = osError;
        }
    }
    return ret;
}

osStatus_t sensor_read_target_raw(sensor_light_t light_source,
    tsl2591_gain_t gain, tsl2591_time_t time,
    uint16_t *ch0_result, uint16_t *ch1_result)
{
    osStatus_t ret = osOK;
    uint8_t light_value = 0;
    sensor_reading_t reading;
    float ch0_sum = 0;
    float ch1_sum = 0;
    float ch0_avg = NAN;
    float ch1_avg = NAN;
    bool saturated = false;

    if (light_source != SENSOR_LIGHT_OFF
        && light_source != SENSOR_LIGHT_REFLECTION
        && light_source != SENSOR_LIGHT_TRANSMISSION) {
        return osErrorParameter;
    }
    if (gain < TSL2591_GAIN_LOW || gain > TSL2591_GAIN_MAXIMUM) {
        return osErrorParameter;
    }
    if (time < TSL2591_TIME_100MS || time > TSL2591_TIME_600MS) {
        return osErrorParameter;
    }

    light_value = sensor_get_read_brightness(light_source);

    log_i("Starting sensor raw target read (light=%d)", light_value);

    do {
        /* Put the sensor into the configured state */
        ret = sensor_set_config(gain, time);
        if (ret != osOK) { break; }

        /* Activate light source synchronized with sensor cycle */
        ret = sensor_set_light_mode(light_source, /*next_cycle*/true, light_value);
        if (ret != osOK) { break; }

        /* Start the sensor */
        ret = sensor_start();
        if (ret != osOK) { break; }

        /* Take the target measurement readings */
        for (int i = 0; i < SENSOR_TARGET_READ_ITERATIONS; i++) {
            ret = sensor_get_next_reading(&reading, 2000);
            if (ret != osOK) { break; }
            log_v("TSL2591[%d]: CH0=%d, CH1=%d", reading.reading_count, reading.ch0_val, reading.ch1_val);

            /* Make sure we're consistent with our read cycles */
            if (reading.reading_count != i + 2) {
                log_e("Unexpected read cycle count: %d", reading.reading_count);
                ret = osError;
                break;
            }

            /* Abort if the sensor is saturated */
            if (sensor_is_reading_saturated(&reading)) {
                log_w("Aborting due to sensor saturation");
                saturated = true;
                break;
            }

            /* Accumulate the results */
            ch0_sum += (float)reading.ch0_val;
            ch1_sum += (float)reading.ch1_val;
        }
        if (ret != osOK) { break; }

        ch0_avg = (ch0_sum / (float)SENSOR_TARGET_READ_ITERATIONS);
        ch1_avg = (ch1_sum / (float)SENSOR_TARGET_READ_ITERATIONS);
    } while (0);

    /* Turn off the sensor */
    sensor_stop();
    sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

    if (ret == osOK) {
        log_i("Sensor read complete");
        if (saturated) {
            if (ch0_result) { *ch0_result = USHRT_MAX; }
            if (ch1_result) { *ch1_result = USHRT_MAX; }
        } else {
            if (ch0_result) { *ch0_result = (uint16_t)lroundf(ch0_avg); }
            if (ch1_result) { *ch1_result = (uint16_t)lroundf(ch1_avg); }
        }
    } else {
        log_e("Sensor read failed: ret=%d", ret);
        if (ret == osOK) {
            ret = osError;
        }
    }
    return ret;
}

/**
 * Sensor read loop used for internal calibration purposes.
 *
 * Assumes the sensor is already running and configured, and returns the
 * geometric mean of a series of raw sensor readings.
 * No corrections are performed, so the results from this function should
 * only be compared to results from a similar run under the same conditions.
 *
 * @param count Number of values to average
 * @param ch0_avg Average reading of Channel 0
 * @param ch1_avg Average reading of Channel 1
 */
osStatus_t sensor_raw_read_loop(uint8_t count, float *ch0_avg, float *ch1_avg)
{
    osStatus_t ret = osOK;
    sensor_reading_t reading;
    float ch0_sum = 0;
    float ch1_sum = 0;
    bool saturation = false;

    if (count == 0) {
        return osErrorParameter;
    }

    /* Loop over measurements */
    for (uint8_t i = 0; i < count; i++) {
        /* Wait for the next reading */
        ret = sensor_get_next_reading(&reading, 2000);
        if (ret != osOK) {
            log_e("sensor_get_next_reading error: %d", ret);
            break;
        }

        /* Accumulate the results */
        log_v("TSL2591[%d]: CH0=%d, CH1=%d", i, reading.ch0_val, reading.ch1_val);
        if (sensor_is_reading_saturated(&reading)) {
            log_w("Sensor value indicates saturation");
            saturation = true;
            break;
        }
        ch0_sum += logf(reading.ch0_val);
        ch1_sum += logf(reading.ch1_val);
    }

    if (ret == osOK) {
        if (ch0_avg) {
            *ch0_avg = saturation ? NAN : expf(ch0_sum / (float)count);
        }
        if (ch1_avg) {
            *ch1_avg = saturation ? NAN : expf(ch1_sum / (float)count);
        }
    } else {
        log_e("Sensor error during read loop: %d", ret);
    }

    return ret;
}

/**
 * Calibrate the relationship between two gain values.
 *
 * @param gain_low Low gain value
 * @param gain_high High gain value
 * @param time Integration time for measurements
 * @param led_brightness LED brightness value for measurements
 * @param gain_ch0 Measured gain for Channel 0
 * @param gain_ch0 Measured gain for Channel 1
 */
osStatus_t sensor_gain_calibration_loop(
    tsl2591_gain_t gain_low, tsl2591_gain_t gain_high, tsl2591_time_t time,
    uint8_t led_brightness,
    float *gain_ch0, float *gain_ch1,
    sensor_gain_calibration_status_t callback_status,
    sensor_gain_calibration_callback_t callback, void *user_data)
{
    osStatus_t ret = osOK;
    sensor_reading_t discard_reading;
    float ch0_avg_high;
    float ch1_avg_high;
    float ch0_avg_low;
    float ch1_avg_low;

    if (gain_low >= gain_high) {
        return osErrorParameter;
    }

    do {
        if (!gain_status_callback(callback, callback_status, 0, user_data)) { ret = osError; break; }

        /* Setup for high gain measurement */
        ret = sensor_set_config(gain_high, time);
        if (ret != osOK) { break; }

        /* Wait for the first reading at the new settings to come through */
        ret = sensor_get_next_reading(&discard_reading, 2000);
        if (ret != osOK) { break; }

        /* Set the LED to target brightness on the next cycle */
        sensor_set_light_mode(SENSOR_LIGHT_TRANSMISSION, /*next_cycle*/true, led_brightness);

        /* Wait for the next cycle which will turn the LED on */
        ret = sensor_get_next_reading(&discard_reading, 2000);
        if (ret != osOK) { break; }

        /* Do the high gain read loop */
        log_d("Higher gain loop...");
        ret = sensor_raw_read_loop(SENSOR_GAIN_CAL_READ_ITERATIONS, &ch0_avg_high, &ch1_avg_high);
        if (ret != osOK) { break; }

        log_d("TSL2591[Higher]: CH0=%d, CH1=%d", lroundf(ch0_avg_high), lroundf(ch1_avg_high));

        /* Turn off the LED and wait for it to cool down */
        sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);
        if (!sensor_gain_calibration_cooldown(callback, user_data)) {
            ret = osError;
            break;
        }

        if (!gain_status_callback(callback, callback_status, 1, user_data)) { ret = osError; break; }

        /* Setup for low gain measurement */
        ret = sensor_set_config(gain_low, time);
        if (ret != osOK) { break; }

        /* Wait for the first reading at the new settings to come through */
        ret = sensor_get_next_reading(&discard_reading, 2000);
        if (ret != osOK) { break; }

        /* Set the LED to target brightness on the next cycle */
        sensor_set_light_mode(SENSOR_LIGHT_TRANSMISSION, /*next_cycle*/true, led_brightness);

        /* Wait for the next cycle which will turn the LED on */
        ret = sensor_get_next_reading(&discard_reading, 2000);
        if (ret != osOK) { break; }

        /* Do the low gain read loop */
        log_d("Lower gain loop...");
        ret = sensor_raw_read_loop(SENSOR_GAIN_CAL_READ_ITERATIONS, &ch0_avg_low, &ch1_avg_low);
        if (ret != osOK) { break; }

        /* Turn off the LED */
        sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

        log_d("TSL2591[Lower]: CH0=%d, CH1=%d", lroundf(ch0_avg_low), lroundf(ch1_avg_low));
    } while (0);

    if (ret == osOK) {
        if (gain_ch0) {
            if (ch0_avg_high <= 0 || ch0_avg_low <= 0) {
                *gain_ch0 = 0;
            } else {
                *gain_ch0 = ch0_avg_high / ch0_avg_low;
            }
        }
        if (gain_ch1) {
            if (ch1_avg_high <= 0 || ch1_avg_low <= 0) {
                *gain_ch1 = 0;
            } else {
                *gain_ch1 = ch1_avg_high / ch1_avg_low;
            }
        }
    }

    return ret;
}

bool sensor_gain_calibration_cooldown(sensor_gain_calibration_callback_t callback, void *user_data)
{
    log_i("Waiting for cool down");
    for (int i = 0; i < 5; i++) {
        if (!gain_status_callback(callback, SENSOR_GAIN_CALIBRATION_STATUS_COOLDOWN, i, user_data)) { return false; }

        osDelay(1000);
    }
    return true;
}

bool gain_status_callback(
    sensor_gain_calibration_callback_t callback,
    sensor_gain_calibration_status_t status, int param,
    void *user_data)
{
    if (callback) {
        return callback(status, param, user_data);
    } else {
        return true;
    }
}

/**
 * Find the ideal LED brightness for measuring gain at a particular gain setting.
 *
 * This routine counts upward and is intended to select a brightness near
 * the bottom of the brightness range without coming too close to saturation.
 *
 * @param led_brightness Brightness to use for further measurements
 * @param gain Gain setting for measurements
 * @param time Integration time for measurements
 * @param start_brightness Starting brightness value, inclusive
 * @param end_brightness Ending brightness value, inclusive
 * @param target_factor Multiplier to determine how close to saturation is allowed
 */
static osStatus_t sensor_find_gain_brightness(uint8_t *led_brightness,
    tsl2591_gain_t gain, tsl2591_time_t time,
    uint8_t start_brightness, uint8_t end_brightness,
    float target_factor,
    sensor_gain_calibration_callback_t callback, void *user_data)
{
    osStatus_t ret = osOK;
    bool count_upward;
    sensor_reading_t discard_reading;
    float target_ch0;
    float ch0_avg;
    float closest_ch0 = NAN;
    uint8_t closest_led = 0;

    /* Basic parameter validation */
    if (start_brightness == 0 || start_brightness == end_brightness
        || isnanf(target_factor) || target_factor < 0.1F || target_factor > 1.0F) {
        return osErrorParameter;
    }

    /* Determine the target reading */
    if (time == TSL2591_TIME_100MS) {
        target_ch0 = (float)TSL2591_ANALOG_SATURATION * target_factor;
    } else {
        target_ch0 = (float)TSL2591_DIGITAL_SATURATION * target_factor;
    }
    log_d("Target reading: %f", target_ch0);

    count_upward = start_brightness < end_brightness;

    if (count_upward) {
        log_d("Counting upward from %d to %d", start_brightness, end_brightness);
    } else {
        log_d("Counting downward from %d to %d", start_brightness, end_brightness);
    }

    if (!gain_status_callback(callback, SENSOR_GAIN_CALIBRATION_STATUS_LED, 0, user_data)) { return osError; }

    do {
        /* Setup for sensor configuration */
        ret = sensor_set_config(gain, time);
        if (ret != osOK) { break; }

        /* Wait for the first reading at the new settings to come through */
        ret = sensor_get_next_reading(&discard_reading, 2000);
        if (ret != osOK) { break; }

        if (!gain_status_callback(callback, SENSOR_GAIN_CALIBRATION_STATUS_LED, start_brightness, user_data)) { return osError; }

        uint8_t i = start_brightness;
        while (i != end_brightness) {
            log_d("Testing brightness: %d", i);

            /* Set the LED to target brightness on the next cycle */
            sensor_set_light_mode(SENSOR_LIGHT_TRANSMISSION, /*next_cycle*/true, i);

            /* Wait for the next cycle which will turn the LED on */
            ret = sensor_get_next_reading(&discard_reading, 2000);
            if (ret != osOK) { break; }

            /* Wait for the next cycle which will turn the LED on */
            ret = sensor_get_next_reading(&discard_reading, 2000);
            if (ret != osOK) { break; }

            ret = sensor_raw_read_loop(SENSOR_GAIN_LED_CHECK_READ_ITERATIONS, &ch0_avg, NULL);
            if (ret != osOK) { break; }
            log_d("Value: %f", ch0_avg);

            if (count_upward) {
                /* Break if we've saturated */
                if (isnanf(ch0_avg)) { break; }

                if (closest_led == 0) {
                    closest_ch0 = ch0_avg;
                    closest_led = i;
                } else {
                    float cur_diff = fabsf(target_ch0 - ch0_avg);
                    float last_diff = fabsf(target_ch0 - closest_ch0);
                    if (cur_diff < last_diff) {
                        closest_ch0 = ch0_avg;
                        closest_led = i;
                    } else {
                        break;
                    }
                }
                i++;
            } else {
                /* Check if we got a usable reading */
                if (!isnanf(ch0_avg) && ch0_avg <= target_ch0) {
                    closest_ch0 = ch0_avg;
                    closest_led = i;
                    break;
                }
                i--;
            }

            /* Turn off the LED and wait for a minimal cooldown period */
            sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

            if (!gain_status_callback(callback, SENSOR_GAIN_CALIBRATION_STATUS_LED, i, user_data)) { return osError; }

            osDelay((i < 64) ? 1000 : 2000);
        }
    } while (0);

    /* Turn off the LED */
    sensor_set_light_mode(SENSOR_LIGHT_OFF, false, 0);

    if (ret == osOK) {
        if (led_brightness) {
            *led_brightness = closest_led;
        }
        log_d("Selected brightness: %d (%f)", closest_led, closest_ch0);
    }

    return ret;
}

bool sensor_is_reading_saturated(const sensor_reading_t *reading)
{
    if (!reading) {
        return false;
    }

    uint16_t limit;
    if (reading->time == TSL2591_TIME_100MS) {
        limit = TSL2591_ANALOG_SATURATION;
    } else {
        limit = TSL2591_DIGITAL_SATURATION;
    }
    if (reading->ch0_val >= limit || reading->ch1_val >= limit) {
        return true;
    } else {
        return false;
    }
}

void sensor_convert_to_basic_counts(const sensor_reading_t *reading, float *ch0_basic, float *ch1_basic)
{
    settings_cal_gain_t cal_gain;
    float ch0_gain;
    float ch1_gain;
    float atime_ms;

    if (!reading) {
        if (ch0_basic) { *ch0_basic = NAN; }
        if (ch1_basic) { *ch1_basic = NAN; }
        return;
    }

    /* Get the gain value from sensor calibration */
    settings_get_cal_gain(&cal_gain);
    settings_get_cal_gain_fields(&cal_gain, reading->gain, &ch0_gain, &ch1_gain);

    /*
     * Integration time is uncalibrated, due to the assumption that all
     * target measurements will be done at the same setting.
     */
    atime_ms = tsl2591_get_time_value_ms(reading->time);

    float ch0_cpl = (atime_ms * ch0_gain) / (TSL2591_LUX_GA * TSL2591_LUX_DF);
    float ch1_cpl = (atime_ms * ch1_gain) / (TSL2591_LUX_GA * TSL2591_LUX_DF);

    if (ch0_basic) {
        *ch0_basic = (float)reading->ch0_val / ch0_cpl;
    }
    if (ch1_basic) {
        *ch1_basic = (float)reading->ch1_val / ch1_cpl;
    }
}

float sensor_apply_slope_calibration(float basic_reading)
{
    settings_cal_slope_t cal_slope;

    bool valid = settings_get_cal_slope(&cal_slope);

    if (isnanf(basic_reading) || isinff(basic_reading) || basic_reading <= 0.0F) {
        log_w("Cannot apply slope correction to invalid reading: %f", basic_reading);
        return basic_reading;
    }

    if (!valid) {
        log_w("Invalid slope calibration values");
        return basic_reading;
    }

    float l_reading = log10f(basic_reading);
    float l_expected = cal_slope.b0 + (cal_slope.b1 * l_reading) + (cal_slope.b2 * powf(l_reading, 2.0F));
    float corr_reading = powf(10.0F, l_expected);

    return corr_reading;
}

uint8_t sensor_get_read_brightness(sensor_light_t light_source)
{
    settings_cal_light_t cal_light = {0};

    if (!settings_get_cal_light(&cal_light)) {
        log_w("Using default light values due to invalid calibration");
    }

    switch (light_source) {
    case SENSOR_LIGHT_REFLECTION:
        return cal_light.reflection;
    case SENSOR_LIGHT_TRANSMISSION:
        return cal_light.transmission;
    case SENSOR_LIGHT_OFF:
        return 0;
    default:
        return 128;
    }
}
