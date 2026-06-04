/*
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

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

LOG_MODULE_REGISTER(pad_link, CONFIG_AURORA_PAD_LINK_LOG_LEVEL);

/* ------------------------------------------------------------------ */
/* UUIDs                                                               */
/* ------------------------------------------------------------------ */
/* One 128-bit base; low 32 bits identify the characteristic.
 * e8a591xx-7c0e-4b5b-9a4c-1f1b6f7c4d70
 *   xx = 00 service, 01 board, 02 state, 03 raw, 04 computed
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

/* ------------------------------------------------------------------ */
/* Wire formats (packed, little-endian by Zephyr build convention)     */
/* ------------------------------------------------------------------ */

/* Raw sensor snapshot. sensor_value (val1.val2) preserved so the
 * central reconstructs full precision; uptime_ms is the time of the
 * most recent contributing publish (IMU or baro), not both. */
struct __packed pl_raw_payload {
	uint32_t uptime_ms;
	int32_t  accel_val1[3];
	int32_t  accel_val2[3];
	int32_t  gyro_val1[3];
	int32_t  gyro_val2[3];
	int32_t  temp_val1;
	int32_t  temp_val2;
	int32_t  press_val1;
	int32_t  press_val2;
};

/* Computed kinematics. Doubles narrowed to float, enough for status. */
struct __packed pl_computed_payload {
	uint32_t uptime_ms;
	float    altitude;
	float    velocity;
	float    yaw;
	float    pitch;
	float    roll;
	float    accel_vert;
};

static struct {
	struct k_spinlock lock;
	uint8_t sm_state;
	struct pl_raw_payload raw;
	struct pl_computed_payload comp;
} snap;

static const char board_id[] = CONFIG_AURORA_PAD_LINK_BOARD_ID;

/* Single-central peripheral. */
static struct bt_conn *current_conn;

static bool sm_state_notify_enabled;
static bool raw_notify_enabled;
static bool comp_notify_enabled;

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

/* Service layout. Keep the value-attribute indices in sync with
 * the BT_GATT_SERVICE_DEFINE entries below; they're used by
 * bt_gatt_notify().
 *   [0] primary service
 *   [1] board declaration         [2] board value
 *   [3] state declaration         [4] state value   [5] state CCC
 *   [6] raw declaration           [7] raw value     [8] raw CCC
 *   [9] computed declaration     [10] computed val [11] computed CCC
 */
#define PL_ATTR_STATE_VALUE 4
#define PL_ATTR_RAW_VALUE   7
#define PL_ATTR_COMP_VALUE  10

BT_GATT_SERVICE_DEFINE(pad_link_svc,
	BT_GATT_PRIMARY_SERVICE(&pl_uuid_svc),

	BT_GATT_CHARACTERISTIC(&pl_uuid_board.uuid,
		BT_GATT_CHRC_READ,
		BT_GATT_PERM_READ,
		read_board, NULL, NULL),

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

	K_SPINLOCK(&snap.lock) {
		snap.raw.uptime_ms   = now;
		snap.raw.temp_val1   = d->temperature.val1;
		snap.raw.temp_val2   = d->temperature.val2;
		snap.raw.press_val1  = d->pressure.val1;
		snap.raw.press_val2  = d->pressure.val2;
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
	sm_state_notify_enabled = false;
	raw_notify_enabled      = false;
	comp_notify_enabled     = false;

	k_work_submit(&adv_start_work);
}

BT_CONN_CB_DEFINE(pad_link_conn_cb) = {
	.connected    = connected,
	.disconnected = disconnected,
};

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
	LOG_INF("BLE host up; board_id=\"%s\"", board_id);
	adv_start();
	return 0;
}

void pad_link_publish_sm(enum sm_state state,
			 const struct sm_inputs *inputs)
{
	if (!inputs) {
		return;
	}

	uint8_t s = (uint8_t)state;
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
		snap.sm_state = s;
		snap.comp     = comp;
	}

	struct bt_conn *conn = current_conn;
	if (!conn) {
		return;
	}

	if (sm_state_notify_enabled) {
		(void)bt_gatt_notify(conn,
				     &pad_link_svc.attrs[PL_ATTR_STATE_VALUE],
				     &s, sizeof(s));
	}
	if (comp_notify_enabled) {
		(void)bt_gatt_notify(conn,
				     &pad_link_svc.attrs[PL_ATTR_COMP_VALUE],
				     &comp, sizeof(comp));
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
}
