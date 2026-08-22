/**
 * @file ground_state.h
 * @brief Shared ground-station snapshot.
 *
 * One mutex-protected record that decouples the producers (sensor sampler,
 * GNSS callback, HC-12 receiver) from the single consumer (the LVGL display
 * thread). Each producer owns one sub-struct and writes it whole; the display
 * copies the entire snapshot once per frame. No producer ever blocks on
 * another, and the display never sees a half-updated field.
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GROUND_SUPPORT_GROUND_STATE_H_
#define GROUND_SUPPORT_GROUND_STATE_H_

#include <stdbool.h>
#include <stdint.h>

/** @brief Attitude/environment of the ground unit itself (onboard sensors). */
struct gs_local {
	bool imu_ok;       /**< IMU produced at least one sample. */
	bool baro_ok;      /**< Barometer produced at least one sample. */
	double roll_deg;   /**< Bank about the forward axis (deg). */
	double pitch_deg;  /**< Elevation of the forward axis from horizontal (deg). */
	double heading_deg;/**< Tilt-compensated magnetic heading, 0 = N, CW (deg). */
	double accel_g;    /**< Total specific force magnitude (g). */
	double altitude_m; /**< Barometric altitude above the boot reference (m). */
};

/** @brief GNSS fix of the ground unit (from the NEO-6M). */
struct gs_gnss {
	bool valid;        /**< A fix has been reported at least once. */
	uint8_t fix;       /**< enum gnss_fix_status. */
	uint16_t sats;     /**< Satellites tracked. */
	double lat_deg;    /**< Latitude (deg, +N). */
	double lon_deg;    /**< Longitude (deg, +E). */
	double alt_m;      /**< Altitude above MSL (m). */
	double speed_ms;   /**< Ground speed (m/s). */
};

/** @brief Latest rocket telemetry decoded from the HC-12 downlink. */
struct gs_telemetry {
	bool valid;             /**< At least one valid frame decoded. */
	int64_t rx_uptime_ms;   /**< Local uptime when the last frame arrived. */
	uint32_t timestamp_ms;  /**< Rocket-side timestamp from the frame. */
	uint8_t state;          /**< Flight state (enum sm_state of @ref type). */
	uint8_t type;           /**< State-machine type (enum sm_type). */
	uint8_t armed;          /**< Non-zero if the rocket reports armed. */
	double altitude_m;      /**< Rocket altitude (m). */
	double accel_ms2;       /**< Rocket total acceleration (m/s^2). */
	double accel_vert_ms2;  /**< Rocket vertical acceleration (m/s^2, up +). */
	double velocity_ms;     /**< Rocket vertical velocity (m/s). */
	double orientation[3];  /**< Rocket orientation (yaw, pitch, roll) (deg). */
};

/** @brief Full ground-station snapshot. */
struct gs_snapshot {
	struct gs_local local;
	struct gs_gnss gnss;
	struct gs_telemetry tlm;
	bool pad_link_valid;    /**< BLE pad-link connected (not yet implemented). */
};

/**
 * @brief Initialise the shared snapshot (zeroed) and its lock.
 *
 * Call once before any producer or the display thread runs.
 */
void gs_init(void);

/** @brief Publish the onboard-attitude sub-record. */
void gs_set_local(const struct gs_local *local);

/** @brief Publish the GNSS sub-record. */
void gs_set_gnss(const struct gs_gnss *gnss);

/** @brief Publish the rocket-telemetry sub-record. */
void gs_set_telemetry(const struct gs_telemetry *tlm);

/**
 * @brief Copy the whole snapshot out under the lock.
 *
 * @param out Destination, must be non-NULL.
 */
void gs_get(struct gs_snapshot *out);

#endif /* GROUND_SUPPORT_GROUND_STATE_H_ */
