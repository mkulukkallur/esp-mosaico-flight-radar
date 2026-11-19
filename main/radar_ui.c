/*
 * Green-phosphor radar UI for the 480x480 ESP-Mosaico display (LVGL 9).
 *
 *   +------------------------------------------+
 *   | FLIGHT RADAR                  12 AC 10:52 |  header
 *   |              N                            |
 *   |          .-------.                        |
 *   |        /   sweep  \                       |  radar (aircraft + sweep)
 *   |     W |     +     | E                     |
 *   |        \          /                       |
 *   |          '-------'                        |
 *   |              S                            |
 *   | CALLSIGN  DIST MI ALT FT SPD KT TRK      |  nearest 4 aircraft
 *   | ...                                       |
 *   | OVERHEAD!           10MI  UPD 3s  ADSB.FI |  footer
 *   +------------------------------------------+
 *
 * Tap anywhere to cycle the range. All updates run in LVGL timers, so no
 * locking is needed after radar_ui_create().
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lvgl.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "adsb.h"
#include "radar_math.h"
#include "radar_ui.h"
#include "wifi.h"

/* ---------- layout ---------- */
/* The Mosaico panel has strongly rounded corners: keep text near the top and
 * bottom edges at least SAFE_X pixels in from the sides. */
#define SAFE_X 40
#define HEADER_Y 24
#define CX 240
#define CY 200
#define RADIUS 144
#define TABLE_Y 352
#define ROW_H 20
#define TABLE_ROWS 4
#define FOOTER_Y 448

/* ---------- look & feel ---------- */
#define C_BG     0x000000
#define C_GREEN  0x33FF77
#define C_MID    0x1E9A48
#define C_DIM    0x0D4A22
#define C_AMBER  0xFFC531

#define SWEEP_SEGS 12          /* wedges making up the fading trail */
#define SWEEP_SEG_DEG 3
#define SWEEP_PERIOD_MS 4000   /* one revolution */
#define FRAME_MS 40            /* ~25 fps sweep */
#define TAGGED 6               /* nearest N aircraft get a callsign tag */
#define OVERHEAD_KM 1.6        /* "OVERHEAD!" alert distance */

#ifdef CONFIG_FR_UNITS_METRIC
static const int k_ranges[] = {3, 8, 15, 40};
#define UNIT_KM 1.0
#define UNIT_STR "KM"
#else
static const int k_ranges[] = {2, 5, 10, 25};
#define UNIT_KM KM_PER_MI
#define UNIT_STR "MI"
#endif
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define N_RANGES ((int)(sizeof(k_ranges) / sizeof(k_ranges[0])))

typedef struct {
    lv_obj_t *dot, *vec, *tag;
    lv_point_precise_t vp[2];
    float bearing;
    int opa;
    bool used;
} blip_t;

static int s_range_idx = 2;
static blip_t s_blips[ADSB_MAX_AIRCRAFT];
static lv_obj_t *s_sweep[SWEEP_SEGS];
static lv_obj_t *s_sweep_line;
static lv_point_precise_t s_sweep_pts[2];
static lv_obj_t *s_lbl_header, *s_lbl_footer, *s_lbl_alert;
static lv_obj_t *s_lbl_ring[3];
static lv_obj_t *s_cells[TABLE_ROWS][5];
static adsb_snapshot_t s_snap;
static uint32_t s_seq;
static bool s_have_data;
static bool s_overhead;
static volatile bool s_no_wifi;
static double s_sweep_deg;

/* unscii_16 is 16 px per character: CALLSIGN(8) DIST(4) ALT(5) SPD(3) TRK(3) */
static const int k_col_x[5] = {18, 162, 246, 346, 414};

void radar_ui_set_no_wifi(void)
{
    s_no_wifi = true;
}

double radar_ui_range_km(void)
{
    return k_ranges[s_range_idx] * UNIT_KM;
}

/* ---------- small helpers ---------- */

static lv_obj_t *mk_label(const lv_font_t *font, uint32_t color, int x, int y, const char *txt)
{
    lv_obj_t *l = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    lv_obj_set_pos(l, x, y);
    return l;
}

static void set_text(lv_obj_t *l, const char *txt)
{
    if (strcmp(lv_label_get_text(l), txt) != 0) {
        lv_label_set_text(l, txt);
    }
}

static lv_obj_t *mk_ring(int r, int width, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, 2 * r, 2 * r);
    lv_obj_set_pos(o, CX - r, CY - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_line(lv_point_precise_t *pts, int width, uint32_t color)
{
    lv_obj_t *l = lv_line_create(lv_screen_active());
    lv_obj_set_pos(l, 0, 0);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_line_set_points(l, pts, 2);
    return l;
}

static void fmt_num(char *buf, size_t n, double v)
{
    if (fabs(v - round(v)) < 0.05) {
        snprintf(buf, n, "%d", (int)lround(v));
    } else {
        snprintf(buf, n, "%.1f", v);
    }
}

/* ---------- range rings ---------- */

static void update_ring_labels(void)
{
    double range = k_ranges[s_range_idx];
    for (int i = 0; i < 3; i++) {
        char num[12], txt[20];
        fmt_num(num, sizeof(num), range * (i + 1) / 3.0);
        snprintf(txt, sizeof(txt), i == 2 ? "%s" UNIT_STR : "%s", num);
        lv_label_set_text(s_lbl_ring[i], txt);
    }
}

/* ---------- aircraft ---------- */

static void update_blips(void)
{
    double range = radar_ui_range_km();
    for (int i = 0; i < ADSB_MAX_AIRCRAFT; i++) {
        blip_t *b = &s_blips[i];
        if (i >= s_snap.count) {
            if (b->used) {
                lv_obj_add_flag(b->dot, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(b->vec, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(b->tag, LV_OBJ_FLAG_HIDDEN);
                b->used = false;
            }
            continue;
        }
        const aircraft_t *a = &s_snap.ac[i];
        bool outside = a->dist_km > range;
        int dx, dy;
        radar_project(a->dist_km, a->bearing_deg, range, RADIUS - 4, &dx, &dy);
        int x = CX + dx, y = CY + dy;
        uint32_t col = (i == 0 && !outside) ? C_AMBER : (a->on_ground ? C_MID : C_GREEN);
        int sz = outside ? 4 : 8;

        lv_obj_set_size(b->dot, sz, sz);
        lv_obj_set_pos(b->dot, x - sz / 2, y - sz / 2);
        lv_obj_set_style_bg_color(b->dot, lv_color_hex(col), 0);
        lv_obj_remove_flag(b->dot, LV_OBJ_FLAG_HIDDEN);

        if (a->has_track && !outside && !a->on_ground) {
            float len = 7.0f + (a->has_gs ? fminf(a->gs_kt, 600.0f) / 600.0f * 18.0f : 0.0f);
            float t = a->track_deg * (float)M_PI / 180.0f;
            b->vp[0].x = x;
            b->vp[0].y = y;
            b->vp[1].x = x + (int)lroundf(len * sinf(t));
            b->vp[1].y = y - (int)lroundf(len * cosf(t));
            lv_line_set_points(b->vec, b->vp, 2);
            lv_obj_set_style_line_color(b->vec, lv_color_hex(col), 0);
            lv_obj_remove_flag(b->vec, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(b->vec, LV_OBJ_FLAG_HIDDEN);
        }

        if (i < TAGGED && !outside) {
            set_text(b->tag, a->callsign);
            lv_obj_set_style_text_color(b->tag, lv_color_hex(col), 0);
            /* put the tag on the side facing away from the centre */
            int tx = (dx >= 0) ? x + 7 : x - 7 - 8 * (int)strlen(a->callsign);
            lv_obj_set_pos(b->tag, tx, y - 11);
            lv_obj_remove_flag(b->tag, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(b->tag, LV_OBJ_FLAG_HIDDEN);
        }

        b->bearing = a->bearing_deg;
        b->opa = -1; /* force opacity refresh on next frame */
        b->used = true;
    }
}

/* ---------- table ---------- */

static void update_table(void)
{
    char buf[5][16];
    for (int r = 0; r < TABLE_ROWS; r++) {
        if (r < s_snap.count) {
            const aircraft_t *a = &s_snap.ac[r];
            double d = a->dist_km / UNIT_KM;
            snprintf(buf[0], sizeof(buf[0]), "%.8s", a->callsign);
            snprintf(buf[1], sizeof(buf[1]), d < 10 ? "%.1f" : "%.0f", d);
            if (a->on_ground) {
                strlcpy(buf[2], "GND", sizeof(buf[2]));
            } else if (a->has_alt) {
                snprintf(buf[2], sizeof(buf[2]), "%d", a->alt_ft);
            } else {
                strlcpy(buf[2], "---", sizeof(buf[2]));
            }
            if (a->has_gs) {
                snprintf(buf[3], sizeof(buf[3]), "%d", (int)lroundf(a->gs_kt));
            } else {
                strlcpy(buf[3], "---", sizeof(buf[3]));
            }
            if (a->has_track) {
                snprintf(buf[4], sizeof(buf[4]), "%03d", ((int)lroundf(a->track_deg)) % 360);
            } else {
                strlcpy(buf[4], "---", sizeof(buf[4]));
            }
        } else {
            for (int c = 0; c < 5; c++) {
                buf[c][0] = '\0';
            }
        }
        for (int c = 0; c < 5; c++) {
            set_text(s_cells[r][c], buf[c]);
        }
    }

    s_overhead = s_snap.count > 0 && s_snap.ac[0].dist_km < OVERHEAD_KM && !s_snap.ac[0].on_ground;
}

/* ---------- header / footer ---------- */

static void update_status(void)
{
    char txt[64];

    /* header: aircraft count + clock */
    char clock[8] = "--:--";
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_year >= (2024 - 1900)) {
        strftime(clock, sizeof(clock), "%H:%M", &tm);
    }
    int in_range = 0;
    for (int i = 0; i < s_snap.count; i++) {
        if (s_snap.ac[i].dist_km <= radar_ui_range_km()) {
            in_range++;
        }
    }
    snprintf(txt, sizeof(txt), "%d AC %s", s_have_data ? in_range : 0, clock);
    set_text(s_lbl_header, txt);

    /* footer: state */
    if (s_no_wifi) {
        snprintf(txt, sizeof(txt), "NO WIFI: SET IT IN VIBE MODE");
    } else if (!wifi_is_connected()) {
        snprintf(txt, sizeof(txt), "CONNECTING WIFI...");
    } else if (!s_have_data && s_seq == 0) {
        snprintf(txt, sizeof(txt), "LOADING...  ADSB.FI");
    } else if (!s_snap.ok) {
        snprintf(txt, sizeof(txt), "API ERR %d  %d%s", s_snap.status, k_ranges[s_range_idx], UNIT_STR);
    } else {
        int age = (int)((esp_timer_get_time() - s_snap.fetched_us) / 1000000);
        snprintf(txt, sizeof(txt), "%d%s  UPD %ds  ADSB.FI", k_ranges[s_range_idx], UNIT_STR, age);
    }
    set_text(s_lbl_footer, txt);

    /* blinking OVERHEAD! */
    bool blink_on = (esp_timer_get_time() / 500000) % 2 == 0;
    if (s_overhead && blink_on) {
        lv_obj_remove_flag(s_lbl_alert, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_lbl_alert, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- timers ---------- */

static void sweep_timer_cb(lv_timer_t *t)
{
    (void)t;
    int64_t ms = esp_timer_get_time() / 1000;
    s_sweep_deg = (double)(ms % SWEEP_PERIOD_MS) * 360.0 / SWEEP_PERIOD_MS;

    /* Wedges trailing the beam. LVGL arc angles: 0 = east, clockwise;
     * compass bearings: 0 = north, clockwise -> subtract 90. */
    for (int i = 0; i < SWEEP_SEGS; i++) {
        double start = s_sweep_deg - SWEEP_SEG_DEG * (i + 1);
        lv_arc_set_rotation(s_sweep[i], (int32_t)wrap360(start - 90.0));
    }
    double rad = s_sweep_deg * M_PI / 180.0;
    s_sweep_pts[1].x = CX + (int)lround(RADIUS * sin(rad));
    s_sweep_pts[1].y = CY - (int)lround(RADIUS * cos(rad));
    lv_line_set_points(s_sweep_line, s_sweep_pts, 2);

    /* Aircraft light up as the beam passes, then fade. */
    for (int i = 0; i < ADSB_MAX_AIRCRAFT; i++) {
        blip_t *b = &s_blips[i];
        if (!b->used) {
            continue;
        }
        double since = sweep_since(s_sweep_deg, b->bearing);
        int opa = since < 12 ? 255 : 255 - (int)((since - 12) / 348.0 * 165.0);
        if (b->opa < 0 || abs(opa - b->opa) >= 8) {
            b->opa = opa;
            lv_obj_set_style_opa(b->dot, (lv_opa_t)opa, 0);
            lv_obj_set_style_opa(b->vec, (lv_opa_t)opa, 0);
            lv_obj_set_style_opa(b->tag, (lv_opa_t)opa, 0);
        }
    }
}

static void data_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (adsb_get_snapshot(&s_snap, s_seq)) {
        s_seq = s_snap.seq;
        if (s_snap.ok) {
            s_have_data = true;
        }
        update_blips();
        update_table();
    }
    update_status();
}

static void on_tap(lv_event_t *e)
{
    (void)e;
    s_range_idx = (s_range_idx + 1) % N_RANGES;
    update_ring_labels();
    update_blips();
    update_status();
    adsb_set_radius_km(radar_ui_range_km());
}

/* ---------- build ---------- */

void radar_ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, NULL);

    /* grid */
    static lv_point_precise_t h_pts[2] = {{CX - RADIUS, CY}, {CX + RADIUS, CY}};
    static lv_point_precise_t v_pts[2] = {{CX, CY - RADIUS}, {CX, CY + RADIUS}};
    mk_line(h_pts, 1, C_DIM);
    mk_line(v_pts, 1, C_DIM);
    mk_ring(RADIUS / 3, 1, C_DIM);
    mk_ring(RADIUS * 2 / 3, 1, C_DIM);
    mk_ring(RADIUS, 2, C_MID);

    mk_label(&lv_font_unscii_8, C_MID, CX - 3, CY - RADIUS - 12, "N");
    mk_label(&lv_font_unscii_8, C_MID, CX + 5, CY + RADIUS - 13, "S"); /* inside: table sits below */
    mk_label(&lv_font_unscii_8, C_MID, CX + RADIUS + 6, CY - 4, "E");
    mk_label(&lv_font_unscii_8, C_MID, CX - RADIUS - 13, CY - 4, "W");
    for (int i = 0; i < 3; i++) {
        int r = RADIUS * (i + 1) / 3;
        s_lbl_ring[i] = mk_label(&lv_font_unscii_8, C_DIM,
                                 CX + (int)(r * 0.7071) + 3, CY - (int)(r * 0.7071) - 10, "");
    }
    update_ring_labels();

    /* sweep: fading wedges + bright leading edge */
    for (int i = 0; i < SWEEP_SEGS; i++) {
        lv_obj_t *a = lv_arc_create(scr);
        lv_obj_remove_style_all(a);
        lv_obj_set_size(a, 2 * RADIUS, 2 * RADIUS);
        lv_obj_set_pos(a, CX - RADIUS, CY - RADIUS);
        lv_arc_set_bg_angles(a, 0, SWEEP_SEG_DEG);
        lv_obj_set_style_arc_width(a, RADIUS, LV_PART_MAIN);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_MAIN);
        lv_obj_set_style_arc_color(a, lv_color_hex(C_GREEN), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, (lv_opa_t)(120 - i * 120 / SWEEP_SEGS), LV_PART_MAIN);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_INDICATOR);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        s_sweep[i] = a;
    }
    s_sweep_pts[0].x = CX;
    s_sweep_pts[0].y = CY;
    s_sweep_pts[1].x = CX;
    s_sweep_pts[1].y = CY - RADIUS;
    s_sweep_line = mk_line(s_sweep_pts, 2, C_GREEN);

    /* home marker */
    lv_obj_t *home = lv_obj_create(scr);
    lv_obj_remove_style_all(home);
    lv_obj_set_size(home, 6, 6);
    lv_obj_set_pos(home, CX - 3, CY - 3);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(C_GREEN), 0);
    lv_obj_set_style_bg_opa(home, LV_OPA_COVER, 0);
    lv_obj_remove_flag(home, LV_OBJ_FLAG_CLICKABLE);

    /* aircraft pool */
    for (int i = 0; i < ADSB_MAX_AIRCRAFT; i++) {
        blip_t *b = &s_blips[i];
        b->dot = lv_obj_create(scr);
        lv_obj_remove_style_all(b->dot);
        lv_obj_set_style_radius(b->dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(b->dot, LV_OPA_COVER, 0);
        lv_obj_remove_flag(b->dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b->dot, LV_OBJ_FLAG_HIDDEN);

        b->vp[0].x = b->vp[1].x = CX;
        b->vp[0].y = b->vp[1].y = CY;
        b->vec = mk_line(b->vp, 2, C_GREEN);
        lv_obj_add_flag(b->vec, LV_OBJ_FLAG_HIDDEN);

        b->tag = mk_label(&lv_font_unscii_8, C_GREEN, 0, 0, "");
        lv_obj_add_flag(b->tag, LV_OBJ_FLAG_HIDDEN);
        b->opa = -1;
    }

    /* header */
    mk_label(&lv_font_unscii_16, C_GREEN, SAFE_X, HEADER_Y, "FLIGHT RADAR");
    s_lbl_header = mk_label(&lv_font_unscii_16, C_GREEN, 0, 0, "");
    lv_obj_align(s_lbl_header, LV_ALIGN_TOP_RIGHT, -SAFE_X, HEADER_Y);

    /* table */
    static const char *k_heads[5] = {"CALLSIGN", "DIST " UNIT_STR, "ALT FT", "SPD KT", "TRK"};
    for (int c = 0; c < 5; c++) {
        mk_label(&lv_font_unscii_8, C_GREEN, k_col_x[c], TABLE_Y, k_heads[c]);
        for (int r = 0; r < TABLE_ROWS; r++) {
            s_cells[r][c] = mk_label(&lv_font_unscii_16, C_AMBER, k_col_x[c], TABLE_Y + 12 + ROW_H * r, "");
        }
    }

    /* footer */
    s_lbl_alert = mk_label(&lv_font_unscii_16, C_AMBER, SAFE_X, FOOTER_Y, "OVERHEAD!");
    lv_obj_add_flag(s_lbl_alert, LV_OBJ_FLAG_HIDDEN);
    s_lbl_footer = mk_label(&lv_font_unscii_8, C_MID, 0, 0, "");
    lv_obj_align(s_lbl_footer, LV_ALIGN_TOP_RIGHT, -SAFE_X, FOOTER_Y + 5);

    update_status();
    lv_timer_create(sweep_timer_cb, FRAME_MS, NULL);
    lv_timer_create(data_timer_cb, 250, NULL);
}
