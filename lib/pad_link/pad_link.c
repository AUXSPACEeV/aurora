/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/zbus/zbus.h>

#include <aurora/lib/pad_link.h>
#include <aurora/lib/state/state.h>

#if defined(CONFIG_IMU)
#include <aurora/lib/imu.h>
#endif
#if defined(CONFIG_BARO)
#include <aurora/lib/baro.h>
#endif

#include "pad_link_wire.h"

LOG_MODULE_REGISTER(pad_link, CONFIG_AURORA_PAD_LINK_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* UUIDs                                                               */
/* ------------------------------------------------------------------ */
/* One 128-bit base; low 32 bits identify the characteristic.
 * e8a591xx-7c0e-4b5b-9a4c-1f1b6f7c4d70
 *   xx = 00 service,    01 board,      02 sm_state,
 *        03 raw sensor, 04 computed,   05 sm_type
 *        a0 boardcap,   a1 baro,       a2 accel,
 *        a3 gyro,       a4 6-DoF IMU,  a5 9-DoF IMU (planned),
 *        a6 GPS/GNSS (planned),        a7 inner_temp,
 *        a8 motor_temp (planned),      a9 hull_temp (planned)
 */
#define PL_UUID_SVC_VAL \
	BT_UUID_128_ENCODE(0xe8a59100, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_BOARD_VAL \
	BT_UUID_128_ENCODE(0xe8a59101, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_STATE_VAL \
	BT_UUID_128_ENCODE(0xe8a59102, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_RAW_VAL \
	BT_UUID_128_ENCODE(0xe8a59103, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_COMP_VAL \
	BT_UUID_128_ENCODE(0xe8a59104, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_SMTYPE_VAL \
	BT_UUID_128_ENCODE(0xe8a59105, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)

#define PL_UUID_BOARDCAP_VAL \
	BT_UUID_128_ENCODE(0xe8a591a0, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_BARO_VAL \
	BT_UUID_128_ENCODE(0xe8a591a1, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_ACCEL_VAL \
	BT_UUID_128_ENCODE(0xe8a591a2, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_GYRO_VAL \
	BT_UUID_128_ENCODE(0xe8a591a3, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_IMU6_VAL \
	BT_UUID_128_ENCODE(0xe8a591a4, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_INNER_TEMP_VAL \
	BT_UUID_128_ENCODE(0xe8a591a7, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)

/* Planned — not yet implemented: */
#define PL_UUID_IMU9_VAL \
	BT_UUID_128_ENCODE(0xe8a591a5, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_GPS_VAL \
	BT_UUID_128_ENCODE(0xe8a591a6, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_MOTOR_TEMP_VAL \
	BT_UUID_128_ENCODE(0xe8a591a8, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)
#define PL_UUID_HULL_TEMP_VAL \
	BT_UUID_128_ENCODE(0xe8a591a9, 0x7c0e, 0x4b5b, 0x9a4c, 0x1f1b6f7c4d70)

static const struct bt_uuid_128 pl_uuid_svc =
	BT_UUID_INIT_128(PL_UUID_SVC_VAL);
static const struct bt_uuid_128 pl_uuid_board =
	BT_UUID_INIT_128(PL_UUID_BOARD_VAL);
static const struct bt_uuid_128 pl_uuid_state =
	BT_UUID_INIT_128(PL_UUID_STATE_VAL);
static const struct bt_uuid_128 pl_uuid_raw =
	BT_UUID_INIT_128(PL_UUID_RAW_VAL);
static const struct bt_uuid_128 pl_uuid_comp =
	BT_UUID_INIT_128(PL_UUID_COMP_VAL);
static const struct bt_uuid_128 pl_uuid_smtype =
	BT_UUID_INIT_128(PL_UUID_SMTYPE_VAL);

static const struct bt_uuid_128 pl_uuid_boardcap =
	BT_UUID_INIT_128(PL_UUID_BOARDCAP_VAL);
static const struct bt_uuid_128 pl_uuid_baro =
	BT_UUID_INIT_128(PL_UUID_BARO_VAL);
static const struct bt_uuid_128 pl_uuid_accel =
	BT_UUID_INIT_128(PL_UUID_ACCEL_VAL);
static const struct bt_uuid_128 pl_uuid_gyro =
	BT_UUID_INIT_128(PL_UUID_GYRO_VAL);
static const struct bt_uuid_128 pl_uuid_imu6 =
	BT_UUID_INIT_128(PL_UUID_IMU6_VAL);
static const struct bt_uuid_128 pl_uuid_inner_temp =
	BT_UUID_INIT_128(PL_UUID_INNER_TEMP_VAL);

/* Wire-format payloads (pl_raw_payload, pl_computed_payload) live in
 * pad_link_wire.h so the unit tests can include them directly.
 */

static struct {
	struct k_spinlock lock;
	uint8_t sm_type;
	uint8_t sm_state;
	struct pl_raw_payload raw;
	struct pl_computed_payload comp;
	uint32_t boardcap;
	struct pl_baro_payload baro;
	struct pl_accel_payload accel;
	struct pl_gyro_payload gyro;
	struct pl_imu6_payload imu6;
	struct pl_inner_temp_payload inner_temp;
} snap;

static const char board_id[] = CONFIG_AURORA_PAD_LINK_BOARD_ID;

/* Single-central peripheral. */
static struct bt_conn *current_conn;

static bool sm_state_notify_enabled;
static bool raw_notify_enabled;
static bool comp_notify_enabled;
static bool baro_notify_enabled;
static bool accel_notify_enabled;
static bool gyro_notify_enabled;
static bool imu6_notify_enabled;
static bool inner_temp_notify_enabled;

/* Back-off gate for bt_gatt_notify. The LL link can die (timeout, RF
 * loss) well before disconnected() fires; in that gap conn is
 * non-NULL, bt_conn_get_info still reports CONNECTED, but ATT has no
 * bearer. Calling bt_gatt_notify in that state makes the host log
 * "No ATT channel for MTU N" at every SM tick (100 Hz). When a notify
 * fails we sit out for NOTIFY_BACKOFF_MS before retrying;
 * disconnected() clears the timer on the next real teardown so a
 * fresh connection isn't penalised.
 */
#define NOTIFY_BACKOFF_MS 1000
static int64_t notify_backoff_until_ms;

/* ------------------------------------------------------------------ */
/* GATT read handlers                                                  */
/* ------------------------------------------------------------------ */

static ssize_t read_board(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 board_id, sizeof(board_id) - 1);
}

static ssize_t read_smtype(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t v;
	K_SPINLOCK(&snap.lock) {
		v = snap.sm_type;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_state(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	uint8_t v;
	K_SPINLOCK(&snap.lock) {
		v = snap.sm_state;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_raw(struct bt_conn *conn,
			const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	struct pl_raw_payload v;
	K_SPINLOCK(&snap.lock) {
		v = snap.raw;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_comp(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	struct pl_computed_payload v;
	K_SPINLOCK(&snap.lock) {
		v = snap.comp;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_boardcap(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	uint32_t v;
	K_SPINLOCK(&snap.lock) {
		v = snap.boardcap;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_baro(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	struct pl_baro_payload v;
	K_SPINLOCK(&snap.lock) {
		v = snap.baro;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_accel(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	struct pl_accel_payload v;
	K_SPINLOCK(&snap.lock) {
		v = snap.accel;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_gyro(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	struct pl_gyro_payload v;
	K_SPINLOCK(&snap.lock) {
		v = snap.gyro;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_imu6(struct bt_conn *conn,
			 const struct bt_gatt_attr *attr,
			 void *buf, uint16_t len, uint16_t offset)
{
	struct pl_imu6_payload v;
	K_SPINLOCK(&snap.lock) {
		v = snap.imu6;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static ssize_t read_inner_temp(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       void *buf, uint16_t len, uint16_t offset)
{
	struct pl_inner_temp_payload v;
	K_SPINLOCK(&snap.lock) {
		v = snap.inner_temp;
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &v, sizeof(v));
}

static void state_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	sm_state_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void raw_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	raw_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void comp_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	comp_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void baro_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	baro_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void accel_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	accel_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void gyro_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	gyro_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void imu6_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	imu6_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

static void inner_temp_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	inner_temp_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
}

/* Service layout. Keep the value-attribute indices in sync with
 * the BT_GATT_SERVICE_DEFINE entries below; they're used by
 * bt_gatt_notify().
 *   [0]  primary service
 *   [1]  board declaration         [2]  board value
 *   [3]  sm_type declaration       [4]  sm_type value
 *   [5]  state declaration         [6]  state value     [7]  state CCC
 *   [8]  raw declaration           [9]  raw value      [10]  raw CCC
 *   [11] computed declaration     [12] computed val    [13]  computed CCC
 *   [14] boardcap declaration     [15] boardcap value
 *   [16] baro declaration         [17] baro value      [18]  baro CCC
 *   [19] accel declaration        [20] accel value     [21]  accel CCC
 *   [22] gyro declaration         [23] gyro value      [24]  gyro CCC
 *   [25] imu6 declaration         [26] imu6 value      [27]  imu6 CCC
 *   [ -] imu9 (planned, a5)       [ -] imu9 value      [ -]  imu9 CCC
 *   [ -] gps  (planned, a6)       [ -] gps value       [ -]  gps CCC
 *   [28] inner_temp declaration   [29] inner_temp val  [30]  inner_temp CCC
 *   [ -] motor_temp (planned, a8) [ -] motor_temp val  [ -]  motor_temp CCC
 *   [ -] hull_temp  (planned, a9) [ -] hull_temp val   [ -]  hull_temp CCC
 */
#define PL_ATTR_STATE_VALUE      6
#define PL_ATTR_RAW_VALUE        9
#define PL_ATTR_COMP_VALUE      12
#define PL_ATTR_BARO_VALUE      17
#define PL_ATTR_ACCEL_VALUE     20
#define PL_ATTR_GYRO_VALUE      23
#define PL_ATTR_IMU6_VALUE      26
#define PL_ATTR_INNER_TEMP_VALUE 29

BT_GATT_SERVICE_DEFINE(pad_link_svc,
	BT_GATT_PRIMARY_SERVICE(&pl_uuid_svc),

	BT_GATT_CHARACTERISTIC(&pl_uuid_board.uuid,
		BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ,
		read_board, NULL, NULL),

	BT_GATT_CHARACTERISTIC(&pl_uuid_smtype.uuid,
		BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ,
		read_smtype, NULL, NULL),

	BT_GATT_CHARACTERISTIC(&pl_uuid_state.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_state, NULL, NULL),
	BT_GATT_CCC(state_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&pl_uuid_raw.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_raw, NULL, NULL),
	BT_GATT_CCC(raw_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&pl_uuid_comp.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_comp, NULL, NULL),
	BT_GATT_CCC(comp_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&pl_uuid_boardcap.uuid,
		BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ,
		read_boardcap, NULL, NULL),

	BT_GATT_CHARACTERISTIC(&pl_uuid_baro.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_baro, NULL, NULL),
	BT_GATT_CCC(baro_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&pl_uuid_accel.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_accel, NULL, NULL),
	BT_GATT_CCC(accel_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&pl_uuid_gyro.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_gyro, NULL, NULL),
	BT_GATT_CCC(gyro_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(&pl_uuid_imu6.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_imu6, NULL, NULL),
	BT_GATT_CCC(imu6_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* TODO: a5 imu9, a6 gps — add read handler, snap field and CCC when source exists. */

	BT_GATT_CHARACTERISTIC(&pl_uuid_inner_temp.uuid,
		BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ,
		read_inner_temp, NULL, NULL),
	BT_GATT_CCC(inner_temp_ccc_cfg,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* TODO: a8 motor_temp, a9 hull_temp — add read handler, snap field and CCC when source exists. */
);

/* ------------------------------------------------------------------ */
/* ZBUS listeners that copy raw sensor snapshots                      */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_IMU)
static void on_imu(const struct zbus_channel *chan)
{
	const struct imu_data *d = zbus_chan_const_msg(chan);
	uint32_t now = k_uptime_get_32();

	K_SPINLOCK(&snap.lock) {
		snap.raw.uptime_ms = now;
		for (int i = 0; i < 3; i++) {
			snap.raw.accel_val1[i] = d->accel[i].val1;
			snap.raw.accel_val2[i] = d->accel[i].val2;
			snap.raw.gyro_val1[i]  = d->gyro[i].val1;
			snap.raw.gyro_val2[i]  = d->gyro[i].val2;
		}

		snap.accel.uptime_ms = now;
		snap.gyro.uptime_ms  = now;
		snap.imu6.uptime_ms  = now;
		for (int i = 0; i < 3; i++) {
			int64_t accel_us = sv_to_i64(&d->accel[i]);
			int64_t gyro_us  = sv_to_i64(&d->gyro[i]);
			snap.accel.accel_us[i] = accel_us;
			snap.gyro.gyro_us[i]   = gyro_us;
			snap.imu6.accel_us[i]  = accel_us;
			snap.imu6.gyro_us[i]   = gyro_us;
		}
	}
}
ZBUS_LISTENER_DEFINE(pl_imu_lis, on_imu);
ZBUS_CHAN_ADD_OBS(imu_data_chan, pl_imu_lis, 4);
#endif /* CONFIG_IMU */

#if defined(CONFIG_BARO)
static void on_baro(const struct zbus_channel *chan)
{
	const struct baro_data *d = zbus_chan_const_msg(chan);
	uint32_t now = k_uptime_get_32();
	int64_t temp_us = sv_to_i64(&d->temperature);
	int64_t press_us = sv_to_i64(&d->pressure);

	K_SPINLOCK(&snap.lock) {
		snap.raw.uptime_ms   = now;
		snap.raw.temp_val1   = d->temperature.val1;
		snap.raw.temp_val2   = d->temperature.val2;
		snap.raw.press_val1  = d->pressure.val1;
		snap.raw.press_val2  = d->pressure.val2;

		snap.baro.uptime_ms       = now;
		snap.baro.temp_us         = temp_us;
		snap.baro.press_us        = press_us;
		snap.inner_temp.uptime_ms = now;
		snap.inner_temp.temp_us   = temp_us;
	}
}
ZBUS_LISTENER_DEFINE(pl_baro_lis, on_baro);
ZBUS_CHAN_ADD_OBS(baro_data_chan, pl_baro_lis, 4);
#endif /* CONFIG_BARO */

/* ------------------------------------------------------------------ */
/* Advertising / connection                                            */
/* ------------------------------------------------------------------ */

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE,
		CONFIG_AURORA_PAD_LINK_DEVICE_NAME,
		sizeof(CONFIG_AURORA_PAD_LINK_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, PL_UUID_SVC_VAL),
};

static void adv_start(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1,
				  ad, ARRAY_SIZE(ad),
				  sd, ARRAY_SIZE(sd));
	if (err == -EALREADY) {
		return;
	}
	if (err) {
		LOG_WRN("adv_start failed (%d)", err);
		return;
	}
	LOG_INF("advertising as %s",
		CONFIG_AURORA_PAD_LINK_DEVICE_NAME);
}

static void adv_start_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	adv_start();
}
static K_WORK_DEFINE(adv_start_work, adv_start_work_handler);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connect err=0x%02x. Restarting adv", err);
		k_work_submit(&adv_start_work);
		return;
	}
	if (!current_conn) {
		current_conn = bt_conn_ref(conn);
	}
	LOG_INF("central connected");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("central disconnected (reason 0x%02x). Restarting adv",
		reason);
	if (current_conn == conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	sm_state_notify_enabled  = false;
	raw_notify_enabled       = false;
	comp_notify_enabled      = false;
	baro_notify_enabled      = false;
	accel_notify_enabled     = false;
	gyro_notify_enabled      = false;
	imu6_notify_enabled      = false;
	inner_temp_notify_enabled = false;
	notify_backoff_until_ms  = 0;

	k_work_submit(&adv_start_work);
}

BT_CONN_CB_DEFINE(pad_link_conn_cb) = {
	.connected    = connected,
	.disconnected = disconnected,
};

static void get_boardcap(void)
{
	uint32_t cap = 0;
#if defined(CONFIG_IMU)
	cap |= PL_BOARDCAP_IMU;
#endif
#if defined(CONFIG_BARO)
	cap |= PL_BOARDCAP_BARO;
#endif
	K_SPINLOCK(&snap.lock) {
		snap.boardcap = cap;
	}
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int pad_link_init(void)
{
	int err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (%d). Pad link disabled", err);
		return err;
	}

	get_boardcap();

	LOG_INF("BLE host up; board_id=\"%s\"", board_id);
	adv_start();
	return 0;
}

void pad_link_publish_sm(enum sm_state state, enum sm_type type,
			 const struct sm_inputs *inputs)
{
	if (!inputs) {
		return;
	}

	uint8_t s = (uint8_t)state;
	uint8_t t = (uint8_t)type;
	struct pl_computed_payload comp = {
		.uptime_ms  = k_uptime_get_32(),
		.altitude   = (float)inputs->altitude,
		.velocity   = (float)inputs->velocity,
		.yaw        = (float)inputs->orientation[0],
		.pitch      = (float)inputs->orientation[1],
		.roll       = (float)inputs->orientation[2],
		.accel_vert = (float)inputs->accel_vert,
	};

	K_SPINLOCK(&snap.lock) {
		snap.sm_type  = t;
		snap.sm_state = s;
		snap.comp     = comp;
	}

	/* Take a real reference for the notify window so disconnect()
	 * can't free the conn underneath us. Stop talking the moment any
	 * notify reports a teardown — bt_gatt_notify returns -ENOTCONN
	 * once the LL link is down, well before disconnected() fires.
	 * Silently dropping the rest avoids spamming the host with
	 * "No ATT channel" warnings at the SM tick rate.
	 */
	struct bt_conn *conn = current_conn;
	if (!conn) {
		return;
	}
	conn = bt_conn_ref(conn);
	if (!conn) {
		return;
	}

	/* Back-off gate, see notify_backoff_until_ms file-scope decl. */
	int64_t now_ms = k_uptime_get();
	if (now_ms < notify_backoff_until_ms) {
		goto out;
	}

	int rc;
	if (sm_state_notify_enabled) {
		rc = bt_gatt_notify(conn,
				    &pad_link_svc.attrs[PL_ATTR_STATE_VALUE],
				    &s, sizeof(s));
		if (rc != 0) {
			LOG_WRN("notify state rc=%d len=%u", rc, sizeof(s));
			notify_backoff_until_ms = now_ms + NOTIFY_BACKOFF_MS;
			goto out;
		}
	}
	if (comp_notify_enabled) {
		rc = bt_gatt_notify(conn,
				    &pad_link_svc.attrs[PL_ATTR_COMP_VALUE],
				    &comp, sizeof(comp));
		if (rc != 0) {
			LOG_WRN("notify comp rc=%d len=%u", rc, sizeof(comp));
			notify_backoff_until_ms = now_ms + NOTIFY_BACKOFF_MS;
			goto out;
		}
	}
	if (raw_notify_enabled) {
		struct pl_raw_payload raw;
		K_SPINLOCK(&snap.lock) {
			raw = snap.raw;
		}
		(void)bt_gatt_notify(conn,
				     &pad_link_svc.attrs[PL_ATTR_RAW_VALUE],
				     &raw, sizeof(raw));
	}
	if (baro_notify_enabled) {
		struct pl_baro_payload baro;
		K_SPINLOCK(&snap.lock) {
			baro = snap.baro;
		}
		rc = bt_gatt_notify(conn,
				    &pad_link_svc.attrs[PL_ATTR_BARO_VALUE],
				    &baro, sizeof(baro));
		if (rc != 0) {
			LOG_WRN("notify baro rc=%d", rc);
			notify_backoff_until_ms = now_ms + NOTIFY_BACKOFF_MS;
			goto out;
		}
	}
	if (accel_notify_enabled) {
		struct pl_accel_payload accel;
		K_SPINLOCK(&snap.lock) {
			accel = snap.accel;
		}
		rc = bt_gatt_notify(conn,
				    &pad_link_svc.attrs[PL_ATTR_ACCEL_VALUE],
				    &accel, sizeof(accel));
		if (rc != 0) {
			LOG_WRN("notify accel rc=%d", rc);
			notify_backoff_until_ms = now_ms + NOTIFY_BACKOFF_MS;
			goto out;
		}
	}
	if (gyro_notify_enabled) {
		struct pl_gyro_payload gyro;
		K_SPINLOCK(&snap.lock) {
			gyro = snap.gyro;
		}
		rc = bt_gatt_notify(conn,
				    &pad_link_svc.attrs[PL_ATTR_GYRO_VALUE],
				    &gyro, sizeof(gyro));
		if (rc != 0) {
			LOG_WRN("notify gyro rc=%d", rc);
			notify_backoff_until_ms = now_ms + NOTIFY_BACKOFF_MS;
			goto out;
		}
	}
	if (imu6_notify_enabled) {
		struct pl_imu6_payload imu6;
		K_SPINLOCK(&snap.lock) {
			imu6 = snap.imu6;
		}
		rc = bt_gatt_notify(conn,
				    &pad_link_svc.attrs[PL_ATTR_IMU6_VALUE],
				    &imu6, sizeof(imu6));
		if (rc != 0) {
			LOG_WRN("notify imu6 rc=%d", rc);
			notify_backoff_until_ms = now_ms + NOTIFY_BACKOFF_MS;
			goto out;
		}
	}
	if (inner_temp_notify_enabled) {
		struct pl_inner_temp_payload inner_temp;
		K_SPINLOCK(&snap.lock) {
			inner_temp = snap.inner_temp;
		}
		(void)bt_gatt_notify(conn,
				     &pad_link_svc.attrs[PL_ATTR_INNER_TEMP_VALUE],
				     &inner_temp, sizeof(inner_temp));
	}

out:
	bt_conn_unref(conn);
}

#if defined(CONFIG_ZTEST)
void pad_link_test_get_snapshot(uint8_t *sm_type,
				uint8_t *sm_state,
				struct pl_raw_payload *raw,
				struct pl_computed_payload *comp,
				uint32_t *boardcap,
				struct pl_baro_payload *baro,
				struct pl_accel_payload *accel,
				struct pl_gyro_payload *gyro,
				struct pl_imu6_payload *imu6,
				struct pl_inner_temp_payload *inner_temp)
{
	K_SPINLOCK(&snap.lock) {
		if (sm_type) {
			*sm_type = snap.sm_type;
		}
		if (sm_state) {
			*sm_state = snap.sm_state;
		}
		if (raw) {
			*raw = snap.raw;
		}
		if (comp) {
			*comp = snap.comp;
		}
		if (boardcap) {
			*boardcap = snap.boardcap;
		}
		if (baro) {
			*baro = snap.baro;
		}
		if (accel) {
			*accel = snap.accel;
		}
		if (gyro) {
			*gyro = snap.gyro;
		}
		if (imu6) {
			*imu6 = snap.imu6;
		}
		if (inner_temp) {
			*inner_temp = snap.inner_temp;
		}
	}
}

/* get_boardcap() normally only runs inside pad_link_init(), which this
 * suite never calls (see file header). Trigger it directly so the
 * Kconfig→flag mapping can be tested without bringing up the BT host.
 */
void pad_link_test_trigger_boardcap(void)
{
	get_boardcap();
}
#endif
