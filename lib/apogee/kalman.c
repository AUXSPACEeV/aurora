/**
 * @file kalman.c
 * @brief Kalman filter for apogee detection.
 *
 * Implements a 2-state (altitude, vertical velocity) Kalman filter that
 * tracks barometric altitude and detects apogee via a velocity zero-crossing.
 *
 * Copyright (c) 2026, Auxspace e.V.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <math.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <aurora/lib/filter.h>

LOG_MODULE_REGISTER(kalman, CONFIG_AURORA_APOGEE_DETECTION_LOG_LEVEL);

/* filter_init – see filter.h */
int filter_init(struct filter *filter)
{
    if (filter == NULL)
        return -EINVAL;

    const float q_alt =
        ((float)CONFIG_FILTER_Q_ALT_MILLISCALE) / FILTER_SCALE_DIVISOR;

    const float q_vel =
        ((float)CONFIG_FILTER_Q_VEL_MILLISCALE) / FILTER_SCALE_DIVISOR;

    const float r_meas =
        ((float)CONFIG_FILTER_R_MILLISCALE) / FILTER_SCALE_DIVISOR;

    filter->state[0] = 0.0f;
    filter->state[1] = 0.0f;

    filter->covariance[0][0] = 10.0f;
    filter->covariance[0][1] = 0.0f;
    filter->covariance[1][0] = 0.0f;
    filter->covariance[1][1] = 10.0f;

    filter->noise_p[0][0] = q_alt;
    filter->noise_p[0][1] = 0.0f;
    filter->noise_p[1][0] = 0.0f;
    filter->noise_p[1][1] = q_vel;

    filter->noise_m = r_meas;

    return 0;
}

/* filter_predict – see filter.h */
int filter_predict(struct filter *filter, int64_t dt)
{
    if (filter == NULL || dt <= 0)
        return -EINVAL;

    float dt_ms = dt / 1000000;

    /* Clamp dt to prevent filter explosion */
    if (dt_ms > 1)
        dt_ms = 1;

    /* State prediction */
    const float altitude = filter->state[0];
    const float velocity = filter->state[1];

    filter->state[0] = altitude + velocity * dt_ms;
    filter->state[1] = velocity;

    /* Scale process noise with dt */
    const float Q00 = filter->noise_p[0][0] * dt_ms;
    const float Q11 = filter->noise_p[1][1] * dt_ms;

    /* Covariance prediction */
    const float P00 = filter->covariance[0][0];
    const float P01 = filter->covariance[0][1];
    const float P10 = filter->covariance[1][0];
    const float P11 = filter->covariance[1][1];

    filter->covariance[0][0] = P00 + dt_ms*(P10 + P01) + dt_ms*dt_ms*P11 + Q00;
    filter->covariance[0][1] = P01 + dt_ms*P11;
    filter->covariance[1][0] = P10 + dt_ms*P11;
    filter->covariance[1][1] = P11 + Q11;

    return 0;
}

/* filter_update – see filter.h */
int filter_update(struct filter *filter, float z)
{
    if (filter == NULL)
        return -EINVAL;

    /* Innovation */
    float y = z - filter->state[0];

    /* Innovation covariance */
    float S = filter->covariance[0][0] + filter->noise_m;

    /* Kalman gain */
    float K0 = filter->covariance[0][0] / S;
    float K1 = filter->covariance[1][0] / S;

    /* State update */
    filter->state[0] += K0 * y;
    filter->state[1] += K1 * y;

    /* Covariance update */
    float P00 = filter->covariance[0][0];
    float P01 = filter->covariance[0][1];
    float P10 = filter->covariance[1][0];
    float P11 = filter->covariance[1][1];

    filter->covariance[0][0] = P00 - K0 * P00;
    filter->covariance[0][1] = P01 - K0 * P01;
    filter->covariance[1][0] = P10 - K1 * P00;
    filter->covariance[1][1] = P11 - K1 * P01;

    return 0;
}

/* filter_detect_apogee – see filter.h */
int filter_detect_apogee(struct filter *filter)
{
    if (filter == NULL)
        return -EINVAL;

    static float prev_velocity = 0.0f;
    float current_velocity = filter->state[1];

    if (prev_velocity > 0.0f && current_velocity <= 0.0f)
    {
        prev_velocity = current_velocity;
        return 1;
    }

    prev_velocity = current_velocity;
    return 0;
}
