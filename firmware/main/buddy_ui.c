#include "buddy_ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "buddy_eva_music.h"
#include "buddy_i4.h"
#include "buddy_text_layout.h"
#include "lvgl.h"

LV_FONT_DECLARE(font_zh_16);

#define UI_W 240
#define UI_H 320
#define COL_BG lv_color_hex(0x080A0C)
#define COL_INK lv_color_hex(0xF7E9D7)
#define COL_DIM lv_color_hex(0x8B8178)
#define COL_LINE lv_color_hex(0x39332F)
#define COL_ORANGE lv_color_hex(0xE17B52)
#define COL_RED lv_color_hex(0xEF4B38)
#define COL_GREEN lv_color_hex(0x64C987)
#define COL_YELLOW lv_color_hex(0xF1C75B)
#define COL_BLUE lv_color_hex(0x72A7D8)

static lv_obj_t *s_screen;
static lv_obj_t *s_canvas;
static LV_ATTRIBUTE_MEM_ALIGN uint8_t s_canvas_buffer[
    LV_DRAW_BUF_SIZE(UI_W, UI_H, LV_COLOR_FORMAT_I4)];
static buddy_ui_snapshot_t s_snapshot;
static bool s_have_snapshot;
static uint32_t s_tick;
static uint64_t s_elapsed_ms;
static int s_scroll;
static buddy_i4_surface_t s_surface;

#define I4_PALETTE_BYTES (16U * sizeof(lv_color32_t))

static const uint32_t s_palette_rgb[] = {0x080A0C, 0xF7E9D7, 0x8B8178, 0x39332F,
    0xE17B52, 0xEF4B38, 0x64C987, 0xF1C75B, 0x72A7D8, 0xFFFFFF,
    /* EVA music player (FoloToy-EVA-musicplayer) */
    0x5B7337, 0xFF8800, 0xE9004D, 0x81A1C1, 0xEBCB8B, 0x151719};

static uint8_t color_index(lv_color_t color)
{
    uint32_t rgb = lv_color_to_int(color) & 0xffffffU;
    uint32_t best_distance = UINT32_MAX;
    uint8_t best = 0;
    uint8_t i;
    for (i = 0; i < sizeof(s_palette_rgb) / sizeof(s_palette_rgb[0]); ++i) {
        if (rgb == s_palette_rgb[i]) return i;
    }
    for (i = 0; i < sizeof(s_palette_rgb) / sizeof(s_palette_rgb[0]); ++i) {
        int dr = (int)((rgb >> 16) & 0xffU) - (int)((s_palette_rgb[i] >> 16) & 0xffU);
        int dg = (int)((rgb >> 8) & 0xffU) - (int)((s_palette_rgb[i] >> 8) & 0xffU);
        int db = (int)(rgb & 0xffU) - (int)(s_palette_rgb[i] & 0xffU);
        uint32_t distance = (uint32_t)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

static void pixel(int x, int y, uint8_t index)
{
    if ((unsigned)x >= UI_W || (unsigned)y >= UI_H) return;
    buddy_i4_set_pixel(s_canvas_buffer + I4_PALETTE_BYTES, UI_W,
                       (uint16_t)x, (uint16_t)y, index);
}

/** Decode one UTF-8 codepoint; returns bytes consumed (0 on empty/invalid). */
static size_t utf8_next(const char *s, size_t max_len, uint32_t *out_cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (max_len == 0 || p[0] == 0) {
        *out_cp = 0;
        return 0;
    }
    if (p[0] < 0x80U) {
        *out_cp = p[0];
        return 1;
    }
    if ((p[0] & 0xE0U) == 0xC0U && max_len >= 2 && (p[1] & 0xC0U) == 0x80U) {
        *out_cp = ((uint32_t)(p[0] & 0x1FU) << 6) | (uint32_t)(p[1] & 0x3FU);
        return 2;
    }
    if ((p[0] & 0xF0U) == 0xE0U && max_len >= 3 &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U) {
        *out_cp = ((uint32_t)(p[0] & 0x0FU) << 12) |
                  ((uint32_t)(p[1] & 0x3FU) << 6) |
                  (uint32_t)(p[2] & 0x3FU);
        return 3;
    }
    if ((p[0] & 0xF8U) == 0xF0U && max_len >= 4 &&
        (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U && (p[3] & 0xC0U) == 0x80U) {
        *out_cp = ((uint32_t)(p[0] & 0x07U) << 18) |
                  ((uint32_t)(p[1] & 0x3FU) << 12) |
                  ((uint32_t)(p[2] & 0x3FU) << 6) |
                  (uint32_t)(p[3] & 0x3FU);
        return 4;
    }
    *out_cp = '?';
    return 1;
}

static int line_width(const lv_font_t *font, const char *start, size_t length, int spacing)
{
    int result = 0;
    size_t i = 0;
    while (i < length) {
        uint32_t cp = 0;
        size_t n = utf8_next(start + i, length - i, &cp);
        lv_font_glyph_dsc_t glyph;
        if (n == 0) break;
        i += n;
        if (cp == 0) break;
        if (lv_font_get_glyph_dsc(font, &glyph, cp, 0)) result += glyph.adv_w + spacing;
    }
    return result > 0 ? result - spacing : 0;
}

typedef struct {
    const lv_font_t *font;
    int spacing;
} text_measure_context_t;

static unsigned measure_text(const char *value, size_t length, void *context)
{
    const text_measure_context_t *measure = context;
    return (unsigned)line_width(measure->font, value, length, measure->spacing);
}

static void glyph(int x, int y, uint8_t index, const lv_font_t *font, uint32_t cp)
{
    lv_font_glyph_dsc_t dsc;
    const uint8_t *bitmap;
    unsigned row;
    unsigned col;
    if (!lv_font_get_glyph_dsc(font, &dsc, cp, 0) || dsc.box_w == 0 || dsc.box_h == 0) return;
    /* Prefer raw A1 bitmaps (UNSCII + lv_font_conv 1bpp). */
    dsc.req_raw_bitmap = 1;
    bitmap = dsc.resolved_font->get_glyph_bitmap(&dsc, NULL);
    if (!bitmap || dsc.format != LV_FONT_GLYPH_FORMAT_A1) return;
    y += font->line_height - font->base_line - dsc.box_h - dsc.ofs_y;
    x += dsc.ofs_x;
    for (row = 0; row < dsc.box_h; ++row) {
        for (col = 0; col < dsc.box_w; ++col) {
            uint32_t bit = row * dsc.box_w + col;
            if ((bitmap[bit >> 3] & (0x80U >> (bit & 7U))) != 0) pixel(x + col, y + row, index);
        }
    }
}

static void text_font(lv_layer_t *layer, int x, int y, int width, lv_color_t color,
                      const char *value, const lv_font_t *font, int spacing,
                      lv_text_align_t align)
{
    int line_step = font->line_height + 1;
    uint8_t index = color_index(color);
    const char *cursor = value;
    (void)layer;
    while (*cursor && y < UI_H) {
        const char *para_end = strchr(cursor, '\n');
        size_t para_len = para_end ? (size_t)(para_end - cursor) : strlen(cursor);
        size_t offset = 0;

        while (offset < para_len && y < UI_H) {
            /* Grow one visual line by whole codepoints only. */
            size_t line_bytes = 0;
            int line_px = 0;
            size_t look = offset;
            while (look < para_len) {
                uint32_t cp = 0;
                size_t n = utf8_next(cursor + look, para_len - look, &cp);
                lv_font_glyph_dsc_t dsc;
                int glyph_w = 0;
                if (n == 0) break;
                if (lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                    glyph_w = (int)dsc.adv_w + spacing;
                }
                if (line_bytes > 0 && line_px + glyph_w > width) break;
                if (line_bytes == 0 && glyph_w > width) {
                    /* Single glyph wider than the box: still consume it on its own line. */
                    look += n;
                    line_bytes = look - offset;
                    line_px = glyph_w;
                    break;
                }
                look += n;
                line_bytes = look - offset;
                line_px += glyph_w;
            }

            if (line_bytes == 0) break;

            {
                int measured = line_width(font, cursor + offset, line_bytes, spacing);
                int pen = x;
                size_t i = 0;
                if (align == LV_TEXT_ALIGN_CENTER) pen += (width - measured) / 2;
                else if (align == LV_TEXT_ALIGN_RIGHT) pen += width - measured;
                while (i < line_bytes) {
                    uint32_t cp = 0;
                    size_t n = utf8_next(cursor + offset + i, line_bytes - i, &cp);
                    lv_font_glyph_dsc_t dsc;
                    int glyph_w = 0;
                    if (n == 0) break;
                    if (lv_font_get_glyph_dsc(font, &dsc, cp, 0)) glyph_w = (int)dsc.adv_w + spacing;
                    /* Never start a glyph that cannot fully fit the remaining width. */
                    if (pen - x + glyph_w > width && i > 0) break;
                    glyph(pen, y, index, font, cp);
                    pen += glyph_w;
                    i += n;
                }
            }

            y += line_step;
            offset += line_bytes;
        }

        cursor = para_end ? para_end + 1 : cursor + para_len;
        if (para_end) {
            /* Hard newline already advanced visually via new while-iteration; keep y. */
        }
    }
}

static void text_font_limited(lv_layer_t *layer, int x, int y, int width, int max_y,
                              unsigned max_lines, lv_color_t color, const char *value,
                              const lv_font_t *font, int spacing, lv_text_align_t align)
{
    int line_step = font->line_height + 2; /* extra gap to avoid glyph overlap */
    uint8_t index = color_index(color);
    const char *cursor = value;
    unsigned lines = 0;
    (void)layer;
    while (*cursor && y + line_step <= max_y && lines < max_lines) {
        const char *para_end = strchr(cursor, '\n');
        size_t para_len = para_end ? (size_t)(para_end - cursor) : strlen(cursor);
        size_t offset = 0;

        while (offset < para_len && y + line_step <= max_y && lines < max_lines) {
            size_t line_bytes = 0;
            int line_px = 0;
            size_t look = offset;
            while (look < para_len) {
                uint32_t cp = 0;
                size_t n = utf8_next(cursor + look, para_len - look, &cp);
                lv_font_glyph_dsc_t dsc;
                int glyph_w = 0;
                if (n == 0) break;
                if (lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                    glyph_w = (int)dsc.adv_w + spacing;
                }
                if (line_bytes > 0 && line_px + glyph_w > width) break;
                if (line_bytes == 0 && glyph_w > width) {
                    look += n;
                    line_bytes = look - offset;
                    line_px = glyph_w;
                    break;
                }
                look += n;
                line_bytes = look - offset;
                line_px += glyph_w;
            }
            if (line_bytes == 0) break;
            {
                int measured = line_width(font, cursor + offset, line_bytes, spacing);
                int pen = x;
                size_t i = 0;
                if (align == LV_TEXT_ALIGN_CENTER) pen += (width - measured) / 2;
                else if (align == LV_TEXT_ALIGN_RIGHT) pen += width - measured;
                while (i < line_bytes) {
                    uint32_t cp = 0;
                    size_t n = utf8_next(cursor + offset + i, line_bytes - i, &cp);
                    lv_font_glyph_dsc_t dsc;
                    int glyph_w = 0;
                    if (n == 0) break;
                    if (lv_font_get_glyph_dsc(font, &dsc, cp, 0)) glyph_w = (int)dsc.adv_w + spacing;
                    if (pen - x + glyph_w > width && i > 0) break;
                    glyph(pen, y, index, font, cp);
                    pen += glyph_w;
                    i += n;
                }
            }
            y += line_step;
            offset += line_bytes;
            ++lines;
        }
        cursor = para_end ? para_end + 1 : cursor + para_len;
    }
}

static int text_wrapped_height(const char *value, const lv_font_t *font, int width,
                               int spacing, int gap, unsigned max_lines)
{
    int line_step = font->line_height + gap;
    const char *cursor = value;
    unsigned lines = 0;
    while (*cursor && lines < max_lines) {
        const char *para_end = strchr(cursor, '\n');
        size_t para_len = para_end ? (size_t)(para_end - cursor) : strlen(cursor);
        size_t offset = 0;
        while (offset < para_len && lines < max_lines) {
            size_t line_bytes = 0;
            int line_px = 0;
            size_t look = offset;
            while (look < para_len) {
                uint32_t cp = 0;
                size_t n = utf8_next(cursor + look, para_len - look, &cp);
                lv_font_glyph_dsc_t dsc;
                int glyph_w = 0;
                if (n == 0) break;
                if (lv_font_get_glyph_dsc(font, &dsc, cp, 0)) {
                    glyph_w = (int)dsc.adv_w + spacing;
                }
                if (line_bytes > 0 && line_px + glyph_w > width) break;
                if (line_bytes == 0 && glyph_w > width) {
                    look += n;
                    line_bytes = look - offset;
                    break;
                }
                look += n;
                line_bytes = look - offset;
                line_px += glyph_w;
            }
            if (line_bytes == 0) break;
            offset += line_bytes;
            ++lines;
        }
        cursor = para_end ? para_end + 1 : cursor + para_len;
    }
    if (lines == 0) lines = 1;
    return (int)lines * line_step;
}

/* Optional prefix "#RRGGBB|" — stripped for display; maps to nearest I4 palette color. */
static lv_color_t strip_entry_color(const char **line)
{
    const char *s = *line;
    unsigned rgb = 0;
    int i;
    if (s == NULL || s[0] != '#') return COL_INK;
    for (i = 1; i <= 6; ++i) {
        char c = s[i];
        unsigned v;
        if (c >= '0' && c <= '9') v = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v = (unsigned)(c - 'A' + 10);
        else return COL_INK;
        rgb = (rgb << 4) | v;
    }
    if (s[7] != '|') return COL_INK;
    *line = s + 8;
    /* Near-white → cream ink for dark background readability. */
    if (((rgb >> 16) & 0xffU) >= 0xe0U &&
        ((rgb >> 8) & 0xffU) >= 0xe0U &&
        (rgb & 0xffU) >= 0xe0U) {
        return COL_INK;
    }
    return lv_color_hex(rgb);
}

static void text(lv_layer_t *layer, int x, int y, int width, lv_color_t color,
                 const char *value, bool large, lv_text_align_t align)
{
    const lv_font_t *font = large ? &lv_font_unscii_16 : &lv_font_unscii_8;
    int spacing = large ? 2 : 0;
    text_font(layer, x, y, width, color, value, font, spacing, align);
}

static void wrapped_text(lv_layer_t *layer, int x, int y, int width, lv_color_t color,
                         const char *value, unsigned max_lines)
{
    char wrapped[384];
    text_measure_context_t measure = {.font = &lv_font_unscii_8, .spacing = 0};
    (void)buddy_text_wrap(value, wrapped, sizeof(wrapped), (unsigned)width, max_lines,
                          measure_text, &measure);
    text(layer, x, y, width, color, wrapped, false, LV_TEXT_ALIGN_LEFT);
}

static void box(lv_layer_t *layer, int x, int y, int w, int h, lv_color_t fill,
                lv_color_t border, int border_width, int radius)
{
    uint8_t fill_index = color_index(fill);
    uint8_t border_index = color_index(border);
    int px;
    int py;
    (void)layer;
    (void)radius;
    for (py = 0; py < h; ++py) {
        for (px = 0; px < w; ++px) {
            bool edge = px < border_width || py < border_width ||
                        px >= w - border_width || py >= h - border_width;
            pixel(x + px, y + py, edge ? border_index : fill_index);
        }
    }
}

static void rule(lv_layer_t *layer, int x, int y, int w, lv_color_t color)
{
    box(layer, x, y, w, 1, color, color, 0, 0);
}

static void draw_status_bar(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char left[32];
    char mid[24];
    char right[32];
    uint64_t age_ms = s_elapsed_ms >= s->time_received_ms ? s_elapsed_ms - s->time_received_ms : 0;
    time_t epoch = (time_t)(s->epoch_seconds + s->timezone_offset_seconds + age_ms / 1000U);
    struct tm tm_value;

    snprintf(left, sizeof(left), "%s", s->ble_connected ? (s->ble_encrypted ? "BLE+" : "BLE") : "CLAUDE");
    if (s->battery_available) {
        snprintf(mid, sizeof(mid), "%u%%", (unsigned)s->battery_percent);
    } else {
        snprintf(mid, sizeof(mid), "--%%");
    }
    if (s->epoch_seconds > 0 && gmtime_r(&epoch, &tm_value) != NULL) {
        snprintf(right, sizeof(right), "%02d:%02d", tm_value.tm_hour, tm_value.tm_min);
    } else {
        snprintf(right, sizeof(right), "%s", s->heartbeat_stale ? "SLEEP" : "LIVE");
    }
    text(layer, 8, 7, 70, s->ble_connected ? COL_GREEN : COL_DIM, left, false, LV_TEXT_ALIGN_LEFT);
    text(layer, 78, 7, 84, s->battery_available ? COL_INK : COL_DIM, mid, false, LV_TEXT_ALIGN_CENTER);
    text(layer, 162, 7, 70, COL_DIM, right, false, LV_TEXT_ALIGN_RIGHT);
    rule(layer, 8, 25, 224, COL_LINE);
}

static bool text_has_utf8(const char *value)
{
    for (const char *p = value; *p; ++p) {
        if ((unsigned char)*p >= 0x80U) return true;
    }
    return false;
}

static const char *home_phase_label(const buddy_ui_snapshot_t *s)
{
    if (!s->ble_connected || s->heartbeat_stale) return "OFFLINE";
    if (s->character == BUDDY_CHARACTER_PAIRING) return "PAIR";
    if (s->waiting > 0) return "WAIT";
    if (s->running > 0 || s->total > 0 || s->message[0]) return "LIVE";
    return "IDLE";
}

static lv_color_t home_phase_color(const buddy_ui_snapshot_t *s)
{
    if (!s->ble_connected || s->heartbeat_stale) return COL_DIM;
    if (s->character == BUDDY_CHARACTER_PAIRING) return COL_YELLOW;
    if (s->waiting > 0) return COL_RED;
    if (s->running > 0 || s->total > 0 || s->message[0]) return COL_GREEN;
    return COL_DIM;
}

/* Layout: status 0-26 | ROOM ~28-110 | CHAT feed 114-292 | footer 300 */
#define HOME_NOW_Y 30
#define HOME_NOW_BODY_Y 50
#define HOME_SPLIT_Y 110
#define HOME_FEED_LABEL_Y 116
#define HOME_FEED_Y 132
#define HOME_FEED_BOTTOM 292

static void draw_home_now(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char meta[48];
    char task[176];
    const char *phase = home_phase_label(s);
    lv_color_t phase_color = home_phase_color(s);

    /* Clear ROOM panel so chat scroll cannot bleed through. */
    box(layer, 0, 26, UI_W, HOME_SPLIT_Y - 26, COL_BG, COL_BG, 0, 0);

    text(layer, 10, HOME_NOW_Y, 48, COL_DIM, "ROOM", false, LV_TEXT_ALIGN_LEFT);
    box(layer, 58, HOME_NOW_Y - 3, 56, 16, phase_color, phase_color, 0, 2);
    text(layer, 60, HOME_NOW_Y, 52, COL_BG, phase, false, LV_TEXT_ALIGN_CENTER);
    /* `total` carries live audience count from Danmaku (not push count). */
    if (s->total >= 10000U) {
        unsigned whole = s->total / 10000U;
        unsigned frac = (s->total % 10000U) / 1000U;
        if (frac > 0U) snprintf(meta, sizeof(meta), "%u.%u万", whole, frac);
        else snprintf(meta, sizeof(meta), "%u万", whole);
    } else if (s->total > 0U) {
        snprintf(meta, sizeof(meta), "%u", s->total);
    } else {
        meta[0] = '\0';
    }
    if (meta[0]) {
        if (text_has_utf8(meta)) {
            text_font(layer, 126, HOME_NOW_Y - 2, 104, COL_DIM, meta, &font_zh_16, 0, LV_TEXT_ALIGN_RIGHT);
        } else {
            text(layer, 126, HOME_NOW_Y, 104, COL_DIM, meta, false, LV_TEXT_ALIGN_RIGHT);
        }
    }

    if (s->message[0]) {
        snprintf(task, sizeof(task), "%s", s->message);
    } else if (s->ble_connected) {
        snprintf(task, sizeof(task), "%s", "Enter a room to see info");
    } else {
        snprintf(task, sizeof(task), "%s", "Open Danmaku and pair BLE");
    }

    /* Max 3 lines, hard-clipped above the CHAT split to avoid overlap. */
    if (text_has_utf8(task)) {
        text_font_limited(layer, 10, HOME_NOW_BODY_Y, 220, HOME_SPLIT_Y - 2, 3, COL_INK, task,
                          &font_zh_16, 0, LV_TEXT_ALIGN_LEFT);
    } else {
        wrapped_text(layer, 10, HOME_NOW_BODY_Y, 220, COL_INK, task, 3);
    }

    rule(layer, 8, HOME_SPLIT_Y, 224, COL_LINE);
}

static void draw_home_feed(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    unsigned i;
    int y = HOME_FEED_Y - s_scroll;
    unsigned drawn = 0;
    const int feed_gap = 4;
    const unsigned max_wraps = 3;

    /* Clear CHAT body so previous frames / tall glyphs cannot stack. */
    box(layer, 0, HOME_FEED_LABEL_Y + 10, UI_W, HOME_FEED_BOTTOM - (HOME_FEED_LABEL_Y + 10),
        COL_BG, COL_BG, 0, 0);

    text(layer, 10, HOME_FEED_LABEL_Y, 110, COL_ORANGE, "CHAT", false, LV_TEXT_ALIGN_LEFT);
    text(layer, 120, HOME_FEED_LABEL_Y, 110, COL_DIM, "DOWN:SCROLL", false, LV_TEXT_ALIGN_RIGHT);

    for (i = 0; i < BUDDY_ENTRY_COUNT; ++i) {
        const char *raw = s->entries[i];
        const char *line;
        lv_color_t c;
        int line_h;
        if (!raw[0]) continue;
        line = raw;
        c = strip_entry_color(&line);
        if (!line[0]) continue;
        /* Default (no prefix / near-white): newest ink, older dim. */
        if (drawn > 0 && color_index(c) == color_index(COL_INK)) {
            c = COL_DIM;
        }
        if (text_has_utf8(line)) {
            line_h = text_wrapped_height(line, &font_zh_16, 220, 0, 2, max_wraps) + feed_gap;
        } else {
            size_t len = strlen(line);
            unsigned wraps = (unsigned)((len + 27) / 28);
            if (wraps < 1) wraps = 1;
            if (wraps > max_wraps) wraps = max_wraps;
            line_h = (int)wraps * 10 + 6 + feed_gap;
        }
        if (y >= HOME_FEED_BOTTOM) break;
        if (y + line_h > HOME_FEED_LABEL_Y + 8 && y < HOME_FEED_BOTTOM) {
            if (text_has_utf8(line)) {
                text_font_limited(layer, 10, y, 220, HOME_FEED_BOTTOM, max_wraps, c, line,
                                  &font_zh_16, 0, LV_TEXT_ALIGN_LEFT);
            } else {
                wrapped_text(layer, 10, y, 220, c, line, (int)max_wraps);
            }
            ++drawn;
        }
        y += line_h;
    }

    if (drawn == 0) {
        wrapped_text(layer, 10, HOME_FEED_Y, 220, COL_DIM,
                     "No danmaku yet.\nChat scrolls here.", 4);
    }
}

static void draw_home(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    /* Feed first (may paint into ROOM); then re-paint fixed chrome. */
    draw_home_feed(layer, s);
    draw_status_bar(layer, s);
    draw_home_now(layer, s);
    text(layer, 10, HOME_FEED_LABEL_Y, 110, COL_ORANGE, "CHAT", false, LV_TEXT_ALIGN_LEFT);
    text(layer, 120, HOME_FEED_LABEL_Y, 110, COL_DIM, "DOWN:SCROLL", false, LV_TEXT_ALIGN_RIGHT);
    rule(layer, 8, HOME_SPLIT_Y, 224, COL_LINE);
    box(layer, 0, HOME_FEED_BOTTOM, UI_W, UI_H - HOME_FEED_BOTTOM, COL_BG, COL_BG, 0, 0);
    text(layer, 8, 300, 224, COL_DIM, BUDDY_ACTION_HOME, false, LV_TEXT_ALIGN_CENTER);
}

/* Full-height history (UP from home). Layout: title 34 | list 62-292 | footer 300 */
#define MSG_LIST_Y 62
#define MSG_LIST_BOTTOM 292

/*
 * MacBoard 音乐页线框对齐 PhoenixZHC/FoloToy-EVA-musicplayer
 * （eva_player.c build_player_screen / eva_player_frame.c）
 */
#define EVA_OLIVE lv_color_hex(0x5B7337)
#define EVA_ORANGE lv_color_hex(0xFF8800)
#define EVA_MAGENTA lv_color_hex(0xE9004D)

static void draw_eva_control_button(lv_layer_t *layer, int x, const char *label, bool active)
{
    lv_color_t fill = active ? EVA_MAGENTA : COL_BG;
    lv_color_t ink = active ? COL_BG : EVA_ORANGE;

    box(layer, x, 266, 52, 30, fill, EVA_ORANGE, 1, 0);
    text(layer, x + 2, 274, 48, ink, label, false, LV_TEXT_ALIGN_CENTER);
}

static void draw_eva_player_header(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char battery[12];
    const char *link = s->ble_encrypted ? "BLE+" : (s->ble_connected ? "BLE" : "OFF");

    box(layer, 4, 14, 132, 34, EVA_OLIVE, EVA_ORANGE, 1, 0);
    text(layer, 16, 22, 108, EVA_ORANGE, "EVANGELION", false, LV_TEXT_ALIGN_CENTER);

    box(layer, 142, 12, 94, 44, COL_BG, EVA_ORANGE, 1, 0);
    if (s->battery_available) {
        snprintf(battery, sizeof(battery), "BAT %u%%", (unsigned)s->battery_percent);
    } else {
        snprintf(battery, sizeof(battery), "BAT --");
    }
    text(layer, 146, 26, 86, EVA_ORANGE, battery, false, LV_TEXT_ALIGN_CENTER);

    box(layer, 142, 63, 94, 30, COL_BG, EVA_ORANGE, 1, 0);
    text(layer, 146, 70, 86, EVA_ORANGE, "MacBoard", false, LV_TEXT_ALIGN_CENTER);

    text(layer, 9, 57, 126, EVA_ORANGE, link, false, LV_TEXT_ALIGN_LEFT);
}

static void draw_eva_clock_panel(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char clock[16];
    char sub[48];
    uint64_t age_ms = s_elapsed_ms >= s->time_received_ms ? s_elapsed_ms - s->time_received_ms : 0;
    time_t epoch = (time_t)(s->epoch_seconds + s->timezone_offset_seconds + age_ms / 1000U);
    struct tm tm_value;

    box(layer, 8, 106, 224, 82, COL_BG, EVA_ORANGE, 1, 0);
    if (s->epoch_seconds > 0 && gmtime_r(&epoch, &tm_value) != NULL) {
        snprintf(clock, sizeof(clock), "%02d:%02d", tm_value.tm_hour, tm_value.tm_min);
    } else {
        snprintf(clock, sizeof(clock), "--:--");
    }
    text(layer, 72, 122, 96, EVA_ORANGE, clock, true, LV_TEXT_ALIGN_CENTER);
    snprintf(sub, sizeof(sub), "%s", s->running ? "PLAYING" : "PAUSED");
    text(layer, 8, 164, 224, COL_DIM, sub, false, LV_TEXT_ALIGN_CENTER);
}

static void draw_macboard_music(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    const char *title = s->message[0] ? s->message : "等待 Mac…";

    box(layer, 0, 0, UI_W, UI_H, COL_BG, COL_BG, 0, 0);
    draw_eva_player_header(layer, s);
    draw_eva_clock_panel(layer, s);

    box(layer, 4, 206, 232, 46, COL_BG, EVA_ORANGE, 1, 0);
    if (text_has_utf8(title)) {
        text_font_limited(layer, 8, 214, 224, 252, 2, EVA_ORANGE, title, &font_zh_16, 0,
                          LV_TEXT_ALIGN_CENTER);
    } else {
        wrapped_text(layer, 8, 218, 224, EVA_ORANGE, title, 2);
    }

    box(layer, 3, 258, 234, 45, EVA_OLIVE, EVA_ORANGE, 1, 0);
    draw_eva_control_button(layer, 5, "PREV", false);
    draw_eva_control_button(layer, 64, "PLAY", s->running);
    draw_eva_control_button(layer, 123, "NEXT", false);
    draw_eva_control_button(layer, 182, "PAUSE", !s->running);
    box(layer, 3, 313, 234, 2, EVA_OLIVE, EVA_OLIVE, 0, 0);
}

static void draw_macboard_build(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    unsigned i;
    int y = 96 - s_scroll;
    const char *status = s->message[0] ? s->message : "idle";
    lv_color_t status_color = COL_DIM;

    if (strcmp(status, "busy") == 0 || strcmp(status, "starting") == 0) {
        status_color = COL_YELLOW;
    } else if (strcmp(status, "success") == 0) {
        status_color = COL_GREEN;
    } else if (strcmp(status, "failure") == 0) {
        status_color = COL_RED;
    }

    draw_status_bar(layer, s);
    text(layer, 10, 40, 220, COL_ORANGE, "BUILD", true, LV_TEXT_ALIGN_LEFT);
    rule(layer, 8, 64, 224, COL_LINE);
    box(layer, 8, 72, 224, 22, status_color, COL_LINE, 1, 3);
    text(layer, 14, 78, 200, COL_BG, status, false, LV_TEXT_ALIGN_LEFT);
    text(layer, 10, 102, 120, COL_DIM, "LOG", false, LV_TEXT_ALIGN_LEFT);
    box(layer, 0, 114, UI_W, 178, COL_BG, COL_BG, 0, 0);
    for (i = 0; i < BUDDY_ENTRY_COUNT; ++i) {
        const char *line = s->entries[i];
        int line_h;
        if (!line[0]) {
            continue;
        }
        if (text_has_utf8(line)) {
            line_h = text_wrapped_height(line, &font_zh_16, 220, 0, 2, 2) + 4;
        } else {
            line_h = 18;
        }
        if (y >= 280) {
            break;
        }
        if (y + line_h > 112 && y < 280) {
            if (text_has_utf8(line)) {
                text_font_limited(layer, 10, y, 220, 286, 2, COL_INK, line, &font_zh_16, 0,
                                  LV_TEXT_ALIGN_LEFT);
            } else {
                wrapped_text(layer, 10, y, 220, COL_INK, line, 2);
            }
        }
        y += line_h;
    }
    text(layer, 8, 300, 224, COL_DIM, "OK:BUILD UP/DOWN:SCROLL", false, LV_TEXT_ALIGN_CENTER);
}

static void draw_messages(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    unsigned i;
    int y = MSG_LIST_Y - s_scroll;
    unsigned drawn = 0;
    const int feed_gap = 4;
    const unsigned max_wraps = 4;

    text(layer, 10, 34, 220, COL_ORANGE, "HISTORY", true, LV_TEXT_ALIGN_LEFT);
    rule(layer, 8, 56, 224, COL_LINE);
    box(layer, 0, 58, UI_W, MSG_LIST_BOTTOM - 58, COL_BG, COL_BG, 0, 0);

    for (i = 0; i < BUDDY_ENTRY_COUNT; ++i) {
        const char *raw = s->entries[i];
        const char *line;
        lv_color_t c;
        int line_h;
        if (!raw[0]) continue;
        line = raw;
        c = strip_entry_color(&line);
        if (!line[0]) continue;
        if (drawn > 0 && color_index(c) == color_index(COL_INK)) {
            c = COL_DIM;
        }
        if (text_has_utf8(line)) {
            line_h = text_wrapped_height(line, &font_zh_16, 220, 0, 2, max_wraps) + feed_gap;
        } else {
            size_t len = strlen(line);
            unsigned wraps = (unsigned)((len + 27) / 28);
            if (wraps < 1) wraps = 1;
            if (wraps > max_wraps) wraps = max_wraps;
            line_h = (int)wraps * 10 + 8 + feed_gap;
        }
        if (y >= MSG_LIST_BOTTOM) break;
        if (y + line_h > 56 && y < MSG_LIST_BOTTOM) {
            if (text_has_utf8(line)) {
                text_font_limited(layer, 10, y, 220, MSG_LIST_BOTTOM, max_wraps, c, line,
                                  &font_zh_16, 0, LV_TEXT_ALIGN_LEFT);
            } else {
                wrapped_text(layer, 10, y, 220, c, line, (int)max_wraps);
            }
            ++drawn;
        }
        y += line_h;
    }

    if (drawn == 0) {
        wrapped_text(layer, 10, MSG_LIST_Y, 220, COL_DIM,
                     "No history yet.\nDanmaku lines appear here.", 5);
    }

    box(layer, 0, MSG_LIST_BOTTOM, UI_W, UI_H - MSG_LIST_BOTTOM, COL_BG, COL_BG, 0, 0);
    text(layer, 8, 300, 224, COL_DIM, BUDDY_ACTION_MESSAGES, false, LV_TEXT_ALIGN_CENTER);
}

static void draw_heart(lv_layer_t *layer, int x, int y, bool on)
{
    lv_color_t c = on ? COL_RED : COL_LINE;
    box(layer, x + 2, y, 4, 4, c, c, 0, 0);
    box(layer, x + 8, y, 4, 4, c, c, 0, 0);
    box(layer, x, y + 3, 14, 5, c, c, 0, 0);
    box(layer, x + 3, y + 8, 8, 3, c, c, 0, 0);
    box(layer, x + 6, y + 11, 2, 2, c, c, 0, 0);
}

static void draw_pet(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char value[64];
    unsigned i;
    uint64_t level = s->tokens / 50000ULL;
    text(layer, 9, 40, 222, COL_ORANGE, "STATS", false, LV_TEXT_ALIGN_CENTER);
    rule(layer, 12, 62, 216, COL_LINE);
    text(layer, 16, 80, 62, COL_DIM, "MOOD", false, LV_TEXT_ALIGN_LEFT);
    for (i = 0; i < 4; ++i) draw_heart(layer, 84 + (int)i * 25, 78, !s->heartbeat_stale || i < 2);
    snprintf(value, sizeof(value), "LV %llu", (unsigned long long)level);
    box(layer, 184, 76, 42, 19, COL_ORANGE, COL_ORANGE, 0, 3);
    text(layer, 186, 81, 38, COL_BG, value, false, LV_TEXT_ALIGN_CENTER);
    text(layer, 16, 120, 75, COL_DIM, "TOKENS", false, LV_TEXT_ALIGN_LEFT);
    snprintf(value, sizeof(value), "%llu", (unsigned long long)s->tokens);
    text(layer, 92, 120, 132, COL_INK, value, false, LV_TEXT_ALIGN_RIGHT);
    text(layer, 16, 148, 75, COL_DIM, "TODAY", false, LV_TEXT_ALIGN_LEFT);
    snprintf(value, sizeof(value), "%llu", (unsigned long long)s->tokens_today);
    text(layer, 92, 148, 132, COL_INK, value, false, LV_TEXT_ALIGN_RIGHT);
    text(layer, 16, 180, 75, COL_DIM, "ENERGY", false, LV_TEXT_ALIGN_LEFT);
    for (i = 0; i < 8; ++i) {
        bool on = !s->heartbeat_stale && i < 6;
        box(layer, 93 + (int)i * 16, 180, 11, 8, on ? COL_YELLOW : COL_LINE,
            on ? COL_YELLOW : COL_LINE, 0, 1);
    }
    text(layer, 8, 300, 224, COL_DIM, BUDDY_ACTION_PET, false, LV_TEXT_ALIGN_CENTER);
}

static void draw_info(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    static const char *const titles[] = {"ABOUT", "BUTTONS", "CLAUDE", "DEVICE", "BLUETOOTH", "CREDITS"};
    char body[512];
    char page[16];
    unsigned p = s->info_page < 6 ? s->info_page : 0;
    text(layer, 14, 38, 180, COL_ORANGE, titles[p], true, LV_TEXT_ALIGN_LEFT);
    snprintf(page, sizeof(page), "%u / 6", p + 1);
    text(layer, 174, 43, 52, COL_DIM, page, false, LV_TEXT_ALIGN_RIGHT);
    rule(layer, 14, 66, 212, COL_LINE);
    switch (p) {
    case 0: snprintf(body, sizeof(body), "A TINY COMPANION FOR\nCLAUDE DESKTOP.\n\nIT SLEEPS, WORKS AND\nCELEBRATES BESIDE YOU.\n\nAPPROVE TOOL REQUESTS\nRIGHT FROM THE DEVICE."); break;
    case 1: snprintf(body, sizeof(body), "UP     NEXT SCREEN\nDOWN   SCROLL / DENY\nOK     APPROVE / CHANGE\nHOLD   OPEN MENU"); break;
    case 2: snprintf(body, sizeof(body), "SESSIONS     %u\nRUNNING      %u\nWAITING      %u\n\nTOKENS       %llu", s->total, s->running, s->waiting, (unsigned long long)s->tokens); break;
    case 3: snprintf(body, sizeof(body), "NAME\n%s\n\nOWNER\n%s\n\nDISPLAY     240 X 320", s->name[0] ? s->name : "CLAUDE BUDDY", s->owner[0] ? s->owner : "-"); break;
    case 4: snprintf(body, sizeof(body), "%s\n\n%s\n%s\n\nPAIR IN CLAUDE DESKTOP\nDEVELOPER > HARDWARE BUDDY", s->name[0] ? s->name : "CLAUDE-BUDDY", s->ble_connected ? "CONNECTED" : "ADVERTISING", s->ble_encrypted ? "ENCRYPTED" : "NOT ENCRYPTED"); break;
    default: snprintf(body, sizeof(body), "CLAUDE DESKTOP BUDDY\nBY FELIX RIESEBERG\n\nESP32-C3 HARDWARE PORT\nFOR TRAE CARD BSP\n\nAPACHE-2.0"); break;
    }
    wrapped_text(layer, 16, 82 - s_scroll, 208, COL_INK, body, 18);
    text(layer, 8, 300, 224, COL_DIM, BUDDY_ACTION_INFO, false, LV_TEXT_ALIGN_CENTER);
}

static void draw_list(lv_layer_t *layer, const char *title, const char *const *items,
                      unsigned count, unsigned selected, const buddy_ui_snapshot_t *s)
{
    unsigned first = selected > 5 ? selected - 5 : 0;
    unsigned i;
    text(layer, 14, 34, 212, COL_ORANGE, title, true, LV_TEXT_ALIGN_LEFT);
    rule(layer, 14, 62, 212, COL_LINE);
    for (i = first; i < count && i < first + 7; ++i) {
        int y = 76 + (int)(i - first) * 29;
        bool active = i == selected;
        char row[64];
        const char *suffix = "";
        char value[12];
        if (!s->reset_open && i == BUDDY_SETTINGS_BRIGHTNESS) { snprintf(value, sizeof(value), "%u/4", s->brightness_level); suffix = value; }
        else if (!s->reset_open && i == BUDDY_SETTINGS_SOUND) suffix = s->sound_enabled ? "ON" : "OFF";
        else if (!s->reset_open && i == BUDDY_SETTINGS_BLE) suffix = s->ble_enabled ? "ON" : "OFF";
        else if (!s->reset_open && i == BUDDY_SETTINGS_TRANSCRIPT) suffix = s->transcript_enabled ? "ON" : "OFF";
        snprintf(row, sizeof(row), "%s", items[i]);
        if (active) box(layer, 12, y - 7, 216, 24, COL_ORANGE, COL_ORANGE, 0, 3);
        text(layer, 20, y, 142, active ? COL_BG : COL_INK, row, false, LV_TEXT_ALIGN_LEFT);
        text(layer, 158, y, 62, active ? COL_BG : COL_DIM, suffix, false, LV_TEXT_ALIGN_RIGHT);
    }
    text(layer, 8, 300, 224, COL_DIM, BUDDY_ACTION_SETTINGS, false, LV_TEXT_ALIGN_CENTER);
}

static void draw_settings(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    static const char *const settings[] = {"BRIGHTNESS", "SOUND", "BLUETOOTH", "WIFI", "LED", "TRANSCRIPT", "CLOCK ROT", "RESET", "BACK"};
    static const char *const reset[] = {"DELETE CHARACTER", "FACTORY RESET", "UNPAIR", "BACK"};
    draw_list(layer, s->reset_open ? "RESET" : "SETTINGS", s->reset_open ? reset : settings,
              s->reset_open ? BUDDY_RESET_COUNT : BUDDY_SETTINGS_COUNT,
              s->reset_open ? s->reset_selection : s->settings_selection, s);
}

static void panel(lv_layer_t *layer, int y, int h, lv_color_t accent, const char *title,
                  const char *body, const char *footer, unsigned max_lines)
{
    box(layer, 10, y, 220, h, lv_color_hex(0x151719), accent, 2, 6);
    box(layer, 10, y, 220, 27, accent, accent, 0, 5);
    text(layer, 18, y + 8, 204, COL_BG, title, false, LV_TEXT_ALIGN_LEFT);
    if (text_has_utf8(body)) {
        text_font(layer, 20, y + 42 - s_scroll, 200, COL_INK, body, &font_zh_16, 0,
                  LV_TEXT_ALIGN_LEFT);
    } else {
        wrapped_text(layer, 20, y + 42 - s_scroll, 200, COL_INK, body, max_lines);
    }
    rule(layer, 20, y + h - 34, 200, COL_LINE);
    text(layer, 18, y + h - 23, 204, COL_DIM, footer, false, LV_TEXT_ALIGN_CENTER);
}

static void draw_overlay(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char body[520];
    int x;
    int y;
    buddy_overlay_kind_t overlay = buddy_overlay_select(s->confirmation_pending,
                                                        s->passkey_visible,
                                                        s->prompt_id[0] != '\0',
                                                        s->menu_open);
    if (overlay != BUDDY_OVERLAY_NONE) {
        for (y = BUDDY_UI_STATUS_H; y < BUDDY_UI_ACTION_Y; ++y) {
            for (x = (y & 1); x < UI_W; x += 2) {
                uint8_t current = buddy_i4_get_pixel(s_surface.pixels, UI_W,
                                                     (uint16_t)x, (uint16_t)y);
                if (current != 0) buddy_i4_set_pixel(s_surface.pixels, UI_W,
                                                          (uint16_t)x, (uint16_t)y,
                                                          15);
            }
        }
    }
    if (overlay == BUDDY_OVERLAY_CONFIRMATION) {
        panel(layer, 62, 196, COL_RED, "CONFIRM ACTION",
              s->confirmation == BUDDY_CONFIRM_FACTORY_RESET ? "FACTORY RESET?\n\nSETTINGS AND STATISTICS\nWILL BE ERASED." : "UNPAIR CLAUDE DESKTOP?\n\nTHE SAVED BLUETOOTH BOND\nWILL BE ERASED.",
              BUDDY_ACTION_CONFIRM, 10);
    } else if (overlay == BUDDY_OVERLAY_PAIRING) {
        snprintf(body, sizeof(body), "ENTER THIS CODE\nIN CLAUDE DESKTOP\n\n       %06lu", (unsigned long)s->passkey);
        panel(layer, 66, 188, COL_BLUE, "BLUETOOTH PAIRING", body, "KEEP THIS SCREEN OPEN", 10);
    } else if (overlay == BUDDY_OVERLAY_APPROVAL) {
        snprintf(body, sizeof(body), "%s\n\n%s", s->prompt_tool, s->prompt_hint);
        panel(layer, 154, 158, s->approval_locked ? COL_DIM : COL_RED, "CLAUDE NEEDS APPROVAL", body,
              s->approval_locked ? (s->permission_delivery == BUDDY_PERMISSION_DELIVERY_FAILED ? "SEND FAILED" : "SENDING...") : BUDDY_ACTION_APPROVAL,
              8);
    } else if (overlay == BUDDY_OVERLAY_MENU) {
        static const char *const menu[] = {"SETTINGS", "TURN OFF", "HELP", "ABOUT", "DEMO", "CLOSE"};
        unsigned i;
        box(layer, 38, 48, 164, 224, lv_color_hex(0x151719), COL_INK, 2, 5);
        text(layer, 52, 61, 136, COL_ORANGE, "MENU", true, LV_TEXT_ALIGN_CENTER);
        rule(layer, 52, 88, 136, COL_LINE);
        for (i = 0; i < BUDDY_MENU_COUNT; ++i) {
            int y = 103 + (int)i * 25;
            bool active = i == (unsigned)s->menu_selection;
            if (active) box(layer, 48, y - 7, 144, 21, COL_ORANGE, COL_ORANGE, 0, 2);
            text(layer, 56, y, 128, active ? COL_BG : COL_INK, menu[i], false, LV_TEXT_ALIGN_CENTER);
        }
    }
}

static void redraw(void)
{
    lv_layer_t *layer = NULL;
    buddy_overlay_kind_t overlay;
    bool use_eva_music;
    if (!s_canvas || !s_have_snapshot) return;
    overlay = buddy_overlay_select(s_snapshot.confirmation_pending,
                                   s_snapshot.passkey_visible,
                                   s_snapshot.prompt_id[0] != '\0',
                                   s_snapshot.menu_open);
    use_eva_music = s_snapshot.page == BUDDY_PAGE_MUSIC && s_snapshot.macboard_companion &&
                    overlay == BUDDY_OVERLAY_NONE;
    if (use_eva_music) {
        buddy_eva_music_set_visible(true);
        buddy_eva_music_update(&s_snapshot, s_elapsed_ms);
        return;
    }
    buddy_eva_music_set_visible(false);
    memset(s_canvas_buffer + I4_PALETTE_BYTES, 0, sizeof(s_canvas_buffer) - I4_PALETTE_BYTES);
    draw_status_bar(layer, &s_snapshot);
    switch (s_snapshot.page) {
    case BUDDY_PAGE_MUSIC:
        if (s_snapshot.macboard_companion) {
            draw_macboard_music(layer, &s_snapshot);
        } else {
            draw_home(layer, &s_snapshot);
        }
        break;
    case BUDDY_PAGE_BUILD:
        if (s_snapshot.macboard_companion) {
            draw_macboard_build(layer, &s_snapshot);
        } else {
            draw_home(layer, &s_snapshot);
        }
        break;
    case BUDDY_PAGE_PET: draw_pet(layer, &s_snapshot); break;
    case BUDDY_PAGE_INFO: draw_info(layer, &s_snapshot); break;
    case BUDDY_PAGE_TRANSCRIPT: draw_messages(layer, &s_snapshot); break;
    case BUDDY_PAGE_SETTINGS: draw_settings(layer, &s_snapshot); break;
    default: draw_home(layer, &s_snapshot); break;
    }
    draw_overlay(layer, &s_snapshot);
    lv_obj_invalidate(s_canvas);
}

void buddy_ui_init(void)
{
    unsigned i;
    if (s_screen) return;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, UI_W, UI_H);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    s_canvas = lv_canvas_create(s_screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buffer, UI_W, UI_H, LV_COLOR_FORMAT_I4);
    lv_obj_set_pos(s_canvas, 0, 0);
    buddy_i4_surface_init(&s_surface, s_canvas_buffer + I4_PALETTE_BYTES,
                          UI_W, UI_H, UI_W / 2);
    for (i = 0; i < sizeof(s_palette_rgb) / sizeof(s_palette_rgb[0]); ++i)
        lv_canvas_set_palette(s_canvas, i, lv_color_to_32(lv_color_hex(s_palette_rgb[i]), LV_OPA_COVER));
    buddy_eva_music_init();
    buddy_eva_music_register_i4_screen(s_screen);
    lv_screen_load(s_screen);
}

void buddy_ui_render(const buddy_ui_snapshot_t *snapshot)
{
    static buddy_page_t s_last_page = BUDDY_PAGE_HOME;
    static char s_last_top[BUDDY_MESSAGE_MAX];
    const char *top;
    if (!snapshot) return;
    buddy_ui_init();
    top = snapshot->entries[0][0] ? snapshot->entries[0] : snapshot->message;
    /* Reset scroll on page change or when the newest feed line changes. */
    if (!s_have_snapshot || snapshot->page != s_last_page ||
        strncmp(top, s_last_top, sizeof(s_last_top)) != 0) {
        s_scroll = 0;
    }
    s_last_page = snapshot->page;
    snprintf(s_last_top, sizeof(s_last_top), "%s", top);
    s_snapshot = *snapshot;
    s_have_snapshot = true;
    redraw();
}

void buddy_ui_show_passkey(uint32_t passkey)
{
    buddy_ui_snapshot_t snapshot = {.passkey_visible = true, .passkey = passkey};
    buddy_ui_render(&snapshot);
}

void buddy_ui_tick(uint64_t elapsed_ms)
{
    uint32_t tick = (uint32_t)(elapsed_ms / 200U);
    s_elapsed_ms = elapsed_ms;
    if (buddy_eva_music_active() && s_have_snapshot) {
        buddy_eva_music_update(&s_snapshot, s_elapsed_ms);
    }
    if (tick != s_tick) {
        s_tick = tick;
        redraw();
    }
}

void buddy_ui_scroll(int delta)
{
    s_scroll += delta;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > 280) s_scroll = 280;
    redraw();
}
