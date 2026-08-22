/**
 * @file display.c
 * @brief ST7789 / LVGL ground-station display.
 *
 * Renders an artificial-horizon gauge from the ground unit's own attitude,
 * plus text panels for heading, pitch, GNSS fix, the rocket's downlinked
 * telemetry and (placeholder) pad-link status. Everything LVGL runs on this
 * one thread; producers only ever touch the shared snapshot.
 *
 * Copyright (c) 2026 Auxspace e.V.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "display.h"
#include "ground_state.h"

#include <math.h>
#include <stdio.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <lvgl.h>

LOG_MODULE_REGISTER(display, CONFIG_GROUND_SUPPORT_LOG_LEVEL);

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DISPLAY_NODE DT_CHOSEN(zephyr_display)
BUILD_ASSERT(DT_NODE_HAS_STATUS(DISPLAY_NODE, okay),
	     "chosen 'zephyr,display' must point at an enabled display");
#define DISPLAY_DEV DEVICE_DT_GET(DISPLAY_NODE)

/** UI refresh period. The sensors run faster; the eye does not care. */
#define DISPLAY_REFRESH_MS 33

/** Telemetry older than this is shown as stale. */
#define TELEMETRY_STALE_MS 3000

/* Artificial-horizon geometry, in the bezel's local coordinates. */
#define GAUGE_D 108
#define GAUGE_R 46
#define GAUGE_C (GAUGE_D / 2)
#define PITCH_PX_PER_DEG ((double)GAUGE_R / 45.0)

/* Palette (RGB888, converted to the panel's RGB565 by LVGL). */
#define COL_BG      0x0a0e12
#define COL_TITLE   0x33ccff
#define COL_TEXT    0xe6e6e6
#define COL_DIM     0x60707c
#define COL_OK      0x50d080
#define COL_RKT     0xffb020
#define COL_HORIZON 0x33ccff
#define COL_CRAFT   0xffcc00

static lv_obj_t *horizon_line;
static lv_obj_t *lbl_hdg;
static lv_obj_t *lbl_gnss1;
static lv_obj_t *lbl_gnss2;
static lv_obj_t *lbl_rkt1;
static lv_obj_t *lbl_rkt2;
static lv_obj_t *lbl_pad;

static lv_point_precise_t horizon_pts[2];

/* Mirrors the simple state-machine backend (enum sm_state, SM_TYPE_SIMPLE).
 * Kept local so the ground station does not pull in the whole flight state
 * machine just to name a state.
 */
static const char *const sm_simple_names[] = {
	"IDLE", "ARMED", "BOOST", "BURNOUT", "APOGEE",
	"MAIN", "REDUNDANT", "LANDED", "ERROR",
};

static const char *state_name(uint8_t type, uint8_t state)
{
	if (type == 0 /* SM_TYPE_SIMPLE */ && state < ARRAY_SIZE(sm_simple_names)) {
		return sm_simple_names[state];
	}
	return "?";
}

static const char *fix_name(uint8_t fix)
{
	switch (fix) {
	case 1: return "FIX";
	case 2: return "DGPS";
	case 3: return "EST";
	default: return "NO FIX";
	}
}

static void set_text_color(lv_obj_t *lbl, uint32_t rgb)
{
	lv_obj_set_style_text_color(lbl, lv_color_hex(rgb), LV_PART_MAIN);
}

static lv_obj_t *make_label(int x, int y, uint32_t rgb)
{
	lv_obj_t *lbl = lv_label_create(lv_screen_active());

	lv_obj_set_pos(lbl, x, y);
	set_text_color(lbl, rgb);
	lv_label_set_text(lbl, "");
	return lbl;
}

/** Static wing/dot mark fixed at the gauge centre. */
static void build_center_mark(lv_obj_t *bezel)
{
	static const lv_point_precise_t left[]  = {{GAUGE_C - 16, GAUGE_C}, {GAUGE_C - 5, GAUGE_C}};
	static const lv_point_precise_t right[] = {{GAUGE_C + 5, GAUGE_C}, {GAUGE_C + 16, GAUGE_C}};
	static const lv_point_precise_t dot[]   = {{GAUGE_C - 1, GAUGE_C}, {GAUGE_C + 1, GAUGE_C}};

	const lv_point_precise_t *marks[] = {left, right, dot};

	for (int i = 0; i < 3; i++) {
		lv_obj_t *l = lv_line_create(bezel);

		lv_line_set_points(l, marks[i], 2);
		lv_obj_set_style_line_width(l, 2, LV_PART_MAIN);
		lv_obj_set_style_line_color(l, lv_color_hex(COL_CRAFT), LV_PART_MAIN);
		lv_obj_set_style_line_rounded(l, true, LV_PART_MAIN);
	}
}

static void build_ui(void)
{
	lv_obj_t *scr = lv_screen_active();

	lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
	/* No input device: nothing scrolls, so keep the scrollbars off-screen. */
	lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

	lv_obj_t *title = lv_label_create(scr);

	lv_label_set_text(title, "GROUND SUPPORT");
	set_text_color(title, COL_TITLE);
	lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 2);

	/* Circular bezel; children (the horizon and centre mark) are clipped to
	 * the circle because the radius rounds the clip region.
	 */
	lv_obj_t *bezel = lv_obj_create(scr);

	lv_obj_set_size(bezel, GAUGE_D, GAUGE_D);
	lv_obj_align(bezel, LV_ALIGN_TOP_MID, 0, 18);
	lv_obj_set_style_radius(bezel, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_pad_all(bezel, 0, LV_PART_MAIN);
	lv_obj_set_style_bg_color(bezel, lv_color_hex(0x122028), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(bezel, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(bezel, 2, LV_PART_MAIN);
	lv_obj_set_style_border_color(bezel, lv_color_hex(COL_DIM), LV_PART_MAIN);
	lv_obj_set_scrollbar_mode(bezel, LV_SCROLLBAR_MODE_OFF);

	horizon_line = lv_line_create(bezel);
	lv_obj_set_style_line_width(horizon_line, 3, LV_PART_MAIN);
	lv_obj_set_style_line_color(horizon_line, lv_color_hex(COL_HORIZON), LV_PART_MAIN);
	lv_obj_set_style_line_rounded(horizon_line, true, LV_PART_MAIN);

	build_center_mark(bezel);

	lbl_hdg   = make_label(6, 128, COL_TEXT);
	lbl_gnss1 = make_label(6, 148, COL_DIM);
	lbl_gnss2 = make_label(6, 166, COL_DIM);
	lbl_rkt1  = make_label(6, 186, COL_DIM);
	lbl_rkt2  = make_label(6, 204, COL_DIM);
	lbl_pad   = make_label(6, 222, COL_DIM);
}

static void update_horizon(double pitch_deg, double roll_deg)
{
	double dy = pitch_deg * PITCH_PX_PER_DEG;

	if (dy > GAUGE_R) {
		dy = GAUGE_R;
	} else if (dy < -GAUGE_R) {
		dy = -GAUGE_R;
	}

	const double r = roll_deg * (M_PI / 180.0);
	const double cs = cos(r), sn = sin(r);

	/* Horizontal chord at y = centre + pitch offset, rotated by roll about
	 * the gauge centre (screen Y points down, so +roll banks right).
	 */
	const double x0 = -GAUGE_R, x1 = GAUGE_R, y = dy;

	horizon_pts[0].x = (lv_value_precise_t)(GAUGE_C + (x0 * cs - y * sn));
	horizon_pts[0].y = (lv_value_precise_t)(GAUGE_C + (x0 * sn + y * cs));
	horizon_pts[1].x = (lv_value_precise_t)(GAUGE_C + (x1 * cs - y * sn));
	horizon_pts[1].y = (lv_value_precise_t)(GAUGE_C + (x1 * sn + y * cs));

	lv_line_set_points(horizon_line, horizon_pts, 2);
}

static void update_ui(const struct gs_snapshot *s)
{
	char buf[48];

	if (s->local.imu_ok) {
		update_horizon(s->local.pitch_deg, s->local.roll_deg);
	}

	/* Heading / pitch-from-ground / altitude. */
	if (s->local.imu_ok) {
		snprintf(buf, sizeof(buf), "HDG %3.0f  PITCH %+3.0f  ALT %.0fm",
			 s->local.heading_deg, s->local.pitch_deg,
			 s->local.baro_ok ? s->local.altitude_m : 0.0);
		set_text_color(lbl_hdg, COL_TEXT);
	} else {
		snprintf(buf, sizeof(buf), "HDG --  PITCH --");
		set_text_color(lbl_hdg, COL_DIM);
	}
	lv_label_set_text(lbl_hdg, buf);

	/* GNSS. */
	snprintf(buf, sizeof(buf), "GPS: %s  %u sat", fix_name(s->gnss.fix), s->gnss.sats);
	lv_label_set_text(lbl_gnss1, buf);
	set_text_color(lbl_gnss1, s->gnss.valid ? COL_OK : COL_DIM);

	if (s->gnss.valid) {
		snprintf(buf, sizeof(buf), "%+.5f %+.5f", s->gnss.lat_deg, s->gnss.lon_deg);
		set_text_color(lbl_gnss2, COL_OK);
	} else {
		snprintf(buf, sizeof(buf), "lat --  lon --");
		set_text_color(lbl_gnss2, COL_DIM);
	}
	lv_label_set_text(lbl_gnss2, buf);

	/* Rocket telemetry over HC-12. */
	if (s->tlm.valid) {
		bool stale = (k_uptime_get() - s->tlm.rx_uptime_ms) > TELEMETRY_STALE_MS;

		snprintf(buf, sizeof(buf), "RKT: %s%s  %s",
			 state_name(s->tlm.type, s->tlm.state),
			 s->tlm.armed ? " ARM" : "",
			 stale ? "(stale)" : "");
		set_text_color(lbl_rkt1, stale ? COL_DIM : COL_RKT);
		lv_label_set_text(lbl_rkt1, buf);

		snprintf(buf, sizeof(buf), "alt %.0fm  vv %+.0fm/s", s->tlm.altitude_m,
			 s->tlm.velocity_ms);
		set_text_color(lbl_rkt2, stale ? COL_DIM : COL_RKT);
		lv_label_set_text(lbl_rkt2, buf);
	} else {
		lv_label_set_text(lbl_rkt1, "RKT: no telemetry");
		set_text_color(lbl_rkt1, COL_DIM);
		lv_label_set_text(lbl_rkt2, "");
	}

	/* Pad link (BLE central) is not implemented yet. */
	lv_label_set_text(lbl_pad, s->pad_link_valid ? "PAD: linked" : "PAD: n/a");
	set_text_color(lbl_pad, s->pad_link_valid ? COL_OK : COL_DIM);
}

static void display_task(void *, void *, void *)
{
	const struct device *disp = DISPLAY_DEV;

	if (!device_is_ready(disp)) {
		LOG_ERR("Display %s not ready; UI disabled", disp->name);
		return;
	}

	build_ui();
	lv_timer_handler();
	display_blanking_off(disp);

	while (1) {
		struct gs_snapshot snap;

		gs_get(&snap);
		update_ui(&snap);
		lv_timer_handler();
		k_sleep(K_MSEC(DISPLAY_REFRESH_MS));
	}
}

/* Lowest of the app threads: a slow flush must never delay a sensor read or a
 * telemetry frame.
 */
K_THREAD_DEFINE(display, 8192, display_task, NULL, NULL, NULL, 8, 0, 0);

int display_init(void)
{
	if (!device_is_ready(DISPLAY_DEV)) {
		LOG_ERR("Display %s not ready", ((const struct device *)DISPLAY_DEV)->name);
		return -ENODEV;
	}

	LOG_INF("Display ready: %s", ((const struct device *)DISPLAY_DEV)->name);
	return 0;
}
