#include "densitometer.h"

#define LOG_TAG "densitometer"

#include <math.h>
#include <elog.h>

#include "settings.h"
#include "sensor.h"
#include "light.h"
#include "util.h"

float densitometer_reflection_d = NAN;
float densitometer_transmission_d = NAN;

#define CAL_READ_ITERATIONS 5

densitometer_result_t densitometer_reflection_measure()
{
    float cal_lo_d;
    float cal_lo_value;
    float cal_hi_d;
    float cal_hi_value;
    char numbuf1[16];
    char numbuf2[16];

    /* Get the current calibration values */
    settings_get_cal_reflection_lo(&cal_lo_d, &cal_lo_value);
    settings_get_cal_reflection_hi(&cal_hi_d, &cal_hi_value);

    /* Check that the calibration values are usable */
    if (cal_lo_d < 0.0F || cal_hi_d <= cal_lo_d
        || cal_lo_value < 0.0F || cal_hi_value >= cal_lo_value) {

        log_w("Invalid calibration values");

        float_to_str(cal_lo_d, numbuf1, 2);
        float_to_str(cal_lo_value, numbuf2, 6);
        log_w("CAL-LO: D=%s, VALUE=%s", numbuf1, numbuf2);

        float_to_str(cal_hi_d, numbuf1, 2);
        float_to_str(cal_hi_value, numbuf2, 6);
        log_w("CAL-HI: D=%s, VALUE=%s", numbuf1, numbuf2);

        return DENSITOMETER_CAL_ERROR;
    }

    /* Set light intensity for reading */
    light_set_reflection(128);
    light_set_transmission(0);

    /* Perform sensor read */
    float ch0_basic;
    float ch1_basic;
    if (sensor_read(2, &ch0_basic, &ch1_basic) != HAL_OK) {
        log_w("Sensor read error");
        light_set_reflection(LIGHT_REFLECTION_IDLE);
        return DENSITOMETER_SENSOR_ERROR;
    }

    /* Make sure the two channels don't overlap the wrong way */
    if (ch1_basic >= ch0_basic) { ch1_basic = 0; }

    /* Convert all values into log units */
    float meas_value = ch0_basic - ch1_basic;
    float meas_ll = log10f(meas_value);
    float cal_hi_ll = log10f(cal_hi_value);
    float cal_lo_ll = log10f(cal_lo_value);

    /* Calculate the slope of the line */
    float m = (cal_hi_d - cal_lo_d) / (cal_hi_ll - cal_lo_ll);

    /* Calculate the measured density */
    float meas_d = (m * (meas_ll - cal_lo_ll)) + cal_lo_d;

    float_to_str(meas_d, numbuf1, 2);
    float_to_str(meas_value, numbuf2, 6);
    log_i("D=%s, VALUE=%s", numbuf1, numbuf2);

    densitometer_reflection_d = meas_d;

    /* Set light back to idle */
    light_set_reflection(LIGHT_REFLECTION_IDLE);

    return DENSITOMETER_OK;
}

float densitometer_reflection_get_last_reading()
{
    return densitometer_reflection_d;
}

densitometer_result_t densitometer_transmission_measure()
{
    float cal_zero_value;
    float cal_hi_d;
    float cal_hi_value;
    char numbuf1[16];
    char numbuf2[16];

    /* Get the current calibration values */
    settings_get_cal_transmission_zero(&cal_zero_value);
    settings_get_cal_transmission_hi(&cal_hi_d, &cal_hi_value);

    /* Check that the calibration values are usable */
    if (cal_zero_value <= 0.0F || cal_hi_d <= 0.0F || cal_hi_value <= 0.0F
        || cal_hi_value >= cal_zero_value) {

        log_w("Invalid calibration values");

        float_to_str(cal_zero_value, numbuf1, 6);
        log_w("CAL-ZERO: VALUE=%s", numbuf1);

        float_to_str(cal_hi_d, numbuf1, 2);
        float_to_str(cal_hi_value, numbuf2, 6);
        log_w("CAL-HI: D=%s, VALUE=%s", numbuf1, numbuf2);

        return DENSITOMETER_CAL_ERROR;
    }

    /* Set light intensity for reading */
    light_set_reflection(0);
    light_set_transmission(128);

    /* Perform sensor read */
    float ch0_basic;
    float ch1_basic;
    if (sensor_read(2, &ch0_basic, &ch1_basic) != HAL_OK) {
        log_w("Sensor read error");
        light_set_transmission(LIGHT_TRANSMISSION_IDLE);
        return DENSITOMETER_SENSOR_ERROR;
    }

    if (ch1_basic >= ch0_basic) { ch1_basic = 0; }
    float meas_value = ch0_basic - ch1_basic;

    /* Calculate the measured CAL-HI density relative to the zero value */
    float cal_hi_meas_d = -1.0F * log10f(cal_hi_value / cal_zero_value);

    /* Calculate the measured target density relative to the zero value */
    float meas_d = -1.0F * log10f(meas_value / cal_zero_value);

    /* Calculate the adjustment factor */
    float adj_factor = cal_hi_d / cal_hi_meas_d;

    /* Calculate the calibration corrected density */
    float corr_d = meas_d * adj_factor;

    float_to_str(corr_d, numbuf1, 2);
    float_to_str(meas_value, numbuf2, 6);
    log_i("D=%s, VALUE=%s", numbuf1, numbuf2);

    densitometer_transmission_d = corr_d;

    /* Set light back to idle */
    light_set_transmission(LIGHT_TRANSMISSION_IDLE);

    return DENSITOMETER_OK;
}

float densitometer_transmission_get_last_reading()
{
    return densitometer_transmission_d;
}

void densitometer_calibrate_gain()
{
    //TODO Currently in the sensor module. Not sure if it should be there or here.
}

densitometer_result_t densitometer_calibrate_reflection_lo(float cal_lo_d)
{
    float ch0_basic;
    float ch1_basic;

    /* Make sure the argument is within a reasonable range */
    if (cal_lo_d < 0.00F || cal_lo_d > 2.50F) {
        return DENSITOMETER_CAL_ERROR;
    }

    /* Set light intensity for reading */
    light_set_reflection(128);
    light_set_transmission(0);

    /* Perform sensor read */
    if (sensor_read(CAL_READ_ITERATIONS, &ch0_basic, &ch1_basic) != HAL_OK) {
        log_w("Sensor read error");
        light_set_reflection(LIGHT_REFLECTION_IDLE);
        return DENSITOMETER_SENSOR_ERROR;
    }

    if (ch1_basic >= ch0_basic) { ch1_basic = 0; }
    float meas_value = ch0_basic - ch1_basic;

    if (meas_value < 0.01F) {
        return DENSITOMETER_CAL_ERROR;
    }

    /* Save the calibration value */
    settings_set_cal_reflection_lo(cal_lo_d, meas_value);

    /* Set light back to idle */
    light_set_reflection(LIGHT_REFLECTION_IDLE);

    return DENSITOMETER_OK;
}

densitometer_result_t densitometer_calibrate_reflection_hi(float cal_hi_d)
{
    float ch0_basic;
    float ch1_basic;

    /* Make sure the argument is within a reasonable range */
    if (cal_hi_d < 0.00F || cal_hi_d > 2.50F) {
        return DENSITOMETER_CAL_ERROR;
    }

    /* Set light intensity for reading */
    light_set_reflection(128);
    light_set_transmission(0);

    /* Perform sensor read */
    if (sensor_read(CAL_READ_ITERATIONS, &ch0_basic, &ch1_basic) != HAL_OK) {
        log_w("Sensor read error");
        light_set_reflection(LIGHT_REFLECTION_IDLE);
        return DENSITOMETER_SENSOR_ERROR;
    }

    if (ch1_basic >= ch0_basic) { ch1_basic = 0; }
    float meas_value = ch0_basic - ch1_basic;

    if (meas_value < 0.01F) {
        return DENSITOMETER_CAL_ERROR;
    }

    /* Save the calibration value */
    settings_set_cal_reflection_hi(cal_hi_d, meas_value);

    /* Set light back to idle */
    light_set_reflection(LIGHT_REFLECTION_IDLE);

    return DENSITOMETER_OK;
}

densitometer_result_t densitometer_calibrate_transmission_zero()
{
    float ch0_basic;
    float ch1_basic;

    /* Set light intensity for reading */
    light_set_reflection(0);
    light_set_transmission(128);

    /* Perform sensor read */
    if (sensor_read(CAL_READ_ITERATIONS, &ch0_basic, &ch1_basic) != HAL_OK) {
        log_w("Sensor read error");
        light_set_transmission(LIGHT_TRANSMISSION_IDLE);
        return DENSITOMETER_SENSOR_ERROR;
    }

    if (ch1_basic >= ch0_basic) { ch1_basic = 0; }
    float meas_value = ch0_basic - ch1_basic;

    if (meas_value < 0.01F) {
        return DENSITOMETER_CAL_ERROR;
    }

    /* Save the calibration value */
    settings_set_cal_transmission_zero(meas_value);

    /* Set light back to idle */
    light_set_transmission(LIGHT_TRANSMISSION_IDLE);

    return DENSITOMETER_OK;
}

densitometer_result_t densitometer_calibrate_transmission_hi(float cal_hi_d)
{
    float ch0_basic;
    float ch1_basic;

    /* Make sure the argument is within a reasonable range */
    if (cal_hi_d < 0.00F || cal_hi_d > 4.00F) {
        return DENSITOMETER_CAL_ERROR;
    }

    /* Set light intensity for reading */
    light_set_reflection(0);
    light_set_transmission(128);

    /* Perform sensor read */
    if (sensor_read(CAL_READ_ITERATIONS, &ch0_basic, &ch1_basic) != HAL_OK) {
        log_w("Sensor read error");
        light_set_transmission(LIGHT_TRANSMISSION_IDLE);
        return DENSITOMETER_SENSOR_ERROR;
    }

    if (ch1_basic >= ch0_basic) { ch1_basic = 0; }
    float meas_value = ch0_basic - ch1_basic;

    if (meas_value < 0.01F) {
        return DENSITOMETER_CAL_ERROR;
    }

    /* Save the calibration value */
    settings_set_cal_transmission_hi(cal_hi_d, meas_value);

    /* Set light back to idle */
    light_set_transmission(LIGHT_TRANSMISSION_IDLE);

    return DENSITOMETER_OK;
}