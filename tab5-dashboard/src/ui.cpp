// Rendering and input for the Tab5: one 1280x720 PSRAM canvas, redrawn when the model or
// the view changes (or once a second for ages and the clock), pushed in one go so nothing
// flickers or shifts. Every panel has a fixed size: new data fills it, never moves it.
#include "ui.h"
#include "app.h"
#include "model.h"
#include "net.h"
#include "settings.h"
#include "feed.h"
#include "maptiles.h"
#include "sdcard.h"
#include "gfx.h"
#include "gps.h"
#include "wd.h"
#include "sfx.h"
#include "usbhost.h"
#include "eye_art.h"
#include "boards_art.h"
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <M5Unified.h>
#include <M5UnitUnifiedKEYBOARD.h>

App A;
extern uint32_t ui_prof[12];

void toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(A.toast, sizeof A.toast, fmt, ap);
    va_end(ap);
    A.toastUntil = millis() + 4000;
}

namespace {

constexpr int W = 1280, H = 720;
constexpr int TOP = 60, BOT = 44;
constexpr int CY = TOP + 12;            // content top
constexpr int CB = H - BOT - 12;        // content bottom
constexpr int PAD = 20;

// Palette: the web dashboard's tokens.
constexpr uint16_t rgb(uint32_t c) { return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F); }
const uint16_t BG = rgb(0x06080b), PANEL = rgb(0x0c0f14), PANEL2 = rgb(0x10141b);
const uint16_t LINE = rgb(0x1b222c), LINE2 = rgb(0x2b3542);
const uint16_t TEXT = rgb(0xdbe4ee), SOFT = rgb(0x9ecfff), MUTED = rgb(0x8593a6), FAINT = rgb(0x5b6879);
const uint16_t ACCENT = rgb(0x4ecdc4), AMBER = rgb(0xfbbf24), RED = rgb(0xf87171);
const uint16_t PINK = rgb(0xf0abfc), PURPLE = rgb(0xc4b5fd), SKY = rgb(0x7dd3fc);

const lgfx::IFont *F_SM = &fonts::DejaVu12;
const lgfx::IFont *F_MD = &fonts::DejaVu18;
const lgfx::IFont *F_LG = &fonts::DejaVu24;
const lgfx::IFont *F_XL = &fonts::DejaVu40;
const lgfx::IFont *F_MONO = &fonts::FreeMono12pt7b;
const lgfx::IFont *F_MONO_S = &fonts::FreeMono9pt7b;   // MACs in narrow columns
const lgfx::IFont *F_BRAND = &fonts::Orbitron_Light_24;
const lgfx::IFont *F_BRAND_L = &fonts::Orbitron_Light_32;

M5Canvas C(&M5.Display);
uint32_t drawnVersion = 0xFFFFFFFF;
uint32_t drawnMs = 0;
bool dirty = true;
uint32_t lastAlertSeq = 0;
uint32_t alertFlashUntil = 0;

uint16_t dim(uint16_t c, float f) {
    uint8_t r = ((c >> 11) & 0x1F) * f, g = ((c >> 5) & 0x3F) * f, b = (c & 0x1F) * f;
    return (r << 11) | (g << 5) | b;
}

// ------------------------------------------------------------------ touch targets
// Everything tappable registers its rectangle while it is drawn, so a tap always lands on
// exactly what is on screen. Lists also register a drag-to-scroll area.

enum HitAct : uint8_t {
    H_TAB, H_ROW, H_TARGET, H_DOM, H_NODE, H_SEARCH, H_CLEAR, H_RANGE, H_ACK, H_MUTE, H_SILENCE,
    H_HELP, H_SETUP, H_CLOSE, H_FIELD, H_SRC, H_NET, H_SAVE, H_BACK, H_REVEAL, H_RESCAN,
    H_NONE, H_ZOOMIN, H_ZOOMOUT, H_LAYER, H_FIT, H_FOLLOW, H_DRONE, H_TGTMAP, H_DETAIL,
    H_DCLOSE, H_DALIAS, H_DGROUP, H_DNEWGROUP, H_DUNGROUP, H_DCOMP, H_DRONEMAP, H_DRONETAG,
    H_LAYERMENU, H_LAYERPICK, H_DRALIAS,
    H_NODEROW, H_NCLOSE, H_NALIAS, H_NCOLOR, H_NBUZZ, H_NLED, H_NIDENT, H_NSTATUS, H_NREBOOT, H_TRI, H_TRACKS, H_PILOT, H_WD, H_ME, H_NWDON, H_NWDCH, H_NBEEP, H_NLOCATE, H_NFW, H_KIND, H_SORT, H_WDCLOSE, H_OVDET, H_ZLOCK, H_OSK, H_NODEPIN, H_POPSET, H_POPMOVE, H_POPCLOSE, H_MXWD, H_OVWD, H_POPTRACK, H_DPROX, H_PROXSTOP, H_SOUNDPG, H_SND
};
struct Hit {
    int16_t x, y, w, h;
    uint8_t act;
    int16_t arg;
    bool in(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
constexpr int L_NETS = 10;   // scroll list id for the setup screen's networks
struct ScrollArea {
    int16_t x, y, w, h;
    uint8_t list;
    int16_t rowH, rows;
};
Hit hits[160];
int nhits = 0;
ScrollArea scrolls[4];
int nscrolls = 0;

void hit(int x, int y, int w, int h, uint8_t act, int arg = 0) {
    if (nhits < (int)(sizeof hits / sizeof hits[0])) hits[nhits++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, act, (int16_t)arg};
}
void scroll_area(int x, int y, int w, int h, uint8_t list, int rowH, int rows) {
    if (nscrolls < 4) scrolls[nscrolls++] = {(int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, list, (int16_t)rowH, (int16_t)rows};
}

// Finger state, between press and release.
struct {
    bool down, dragging;
    uint32_t downMs;      // when the finger went down (long press)
    int x0, y0, x, y;
    Hit pressed;
    bool hasPressed;
    int scroll;          // index into scrolls, -1 none
    ScrollArea area;
    int top0;
    double mx0, my0;     // map drag: world pixel of the centre at the press
} T = {};

// ------------------------------------------------------------------ primitives

void font(const lgfx::IFont *f) { C.setFont(f); }

void text(int x, int y, const char *s, uint16_t col, textdatum_t d = top_left) {
    C.setTextDatum(d);
    C.setTextColor(col);
    C.drawString(s, x, y);
}

// Neon text: a dim halo one pixel around, the bright face on top.
void glow(int x, int y, const char *s, uint16_t col, textdatum_t d = top_left) {
    C.setTextDatum(d);
    C.setTextColor(dim(col, 0.28f));
    C.drawString(s, x - 1, y - 1);   // two halo passes, not four: same glow, half the cost
    C.drawString(s, x + 1, y + 1);
    C.setTextColor(col);
    C.drawString(s, x, y);
}

// Truncate to fit `w` px with the current font.
const char *fit(const char *s, int w) {
    static char buf[96];
    if (C.textWidth(s) <= w) return s;
    strlcpy(buf, s, sizeof buf);
    int n = strlen(buf);
    while (n > 0) {
        buf[--n] = 0;
        char tmp[100];
        snprintf(tmp, sizeof tmp, "%s..", buf);
        if (C.textWidth(tmp) <= w) {
            strlcpy(buf, tmp, sizeof buf);
            return buf;
        }
    }
    return "";
}

uint8_t canvasRot();
uint16_t *fb = nullptr;   // the display's own framebuffer (DSI scans it out)

// Logical (landscape canvas) rectangle -> panel (portrait buffer) rectangle.
void to_panel(int x, int y, int w, int h, int &px, int &py, int &pw, int &ph) {
    pw = h, ph = w;
    if (canvasRot() == 3) px = y, py = 1280 - (x + w);
    else px = 720 - (y + h), py = x;
}

bool ppa_canvas() { return gfx_ok() && (((uintptr_t)C.getBuffer()) & 127) == 0; }

// Whole frame to the display: a DMA copy into its framebuffer (the CPU copy took ~30 ms).
void push_frame() {
    uint16_t *cb = (uint16_t *)C.getBuffer();
    uint32_t t0 = micros();
    // Write the CPU's drawing out and leave no canvas lines in the cache, so the next
    // frame's PPA fills and tile blits (DMA) can't be overwritten by a stale line.
    if (ppa_canvas()) gfx_cpu_to_mem(cb, (size_t)720 * 1280 * 2);
    uint32_t t1 = micros();
    C.pushSprite(0, 0);   // CPU copy into the framebuffer: ~30 ms (PPA SRM 42, GDMA crashed)
    ui_prof[4] = t1 - t0, ui_prof[6] = micros() - t1;
}

// A PPA fill: only where the CPU has not drawn yet this frame (the frame clear, the map
// before its overlays). Right after a push the cache holds nothing of the canvas, so the
// DMA write cannot be overwritten by a stale cache line.
void ppa_fill(int x, int y, int w, int h, uint16_t col) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    if (!ppa_canvas()) {
        C.fillRect(x, y, w, h, col);
        return;
    }
    int px, py, pw, ph;
    to_panel(x, y, w, h, px, py, pw, ph);
    gfx_fill((uint16_t *)C.getBuffer(), px, py, pw, ph, col);
}

// Solid rectangle, by the CPU straight into the buffer: one contiguous run per logical
// column (LovyanGFX fills a rotated canvas pixel by pixel). Safe anywhere in a frame.
void fast_fill(int x, int y, int w, int h, uint16_t col) {
    if (x < 0) w += x, x = 0;
    if (y < 0) h += y, y = 0;
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0) return;
    uint16_t *buf = (uint16_t *)C.getBuffer();
    bool r3 = canvasRot() == 3;
    for (int X = x; X < x + w; X++) {
        uint16_t *d = r3 ? buf + (size_t)(1279 - X) * 720 + y : buf + (size_t)X * 720 + (720 - y - h);
        for (int i = 0; i < h; i++) d[i] = col;
    }
}

// Rounded panel body: the flat parts through fast_fill, only the corners through LGFX.
void fast_round(int x, int y, int w, int h, int r, uint16_t col) {
    fast_fill(x, y + r, w, h - 2 * r, col);
    fast_fill(x + r, y, w - 2 * r, r, col);
    fast_fill(x + r, y + h - r, w - 2 * r, r, col);
    C.fillCircle(x + r, y + r, r, col);
    C.fillCircle(x + w - r - 1, y + r, r, col);
    C.fillCircle(x + r, y + h - r - 1, r, col);
    C.fillCircle(x + w - r - 1, y + h - r - 1, r, col);
}

// Names (nodes, devices, drones) are never cut while a smaller font fits them: drop to
// the small font first, shorten only if even that is too wide. (Leaves the small font
// selected, so the text() right after draws in it.)
const char *fit_name(const char *s, int w) {
    if (C.textWidth(s) <= w) return s;
    font(F_SM);
    return fit(s, w);
}

void panel(int x, int y, int w, int h, const char *title, uint16_t accent = ACCENT) {
    fast_round(x, y, w, h, 8, PANEL);
    C.drawRoundRect(x, y, w, h, 8, LINE2);
    if (title) {
        C.drawFastHLine(x + 14, y, 40, accent);   // a short neon tick on the frame
        C.drawFastHLine(x + 14, y + 1, 40, dim(accent, 0.5f));
        font(F_SM);
        text(x + 14, y + 10, title, dim(accent, 0.9f));
    }
}

void glow_rect(int x, int y, int w, int h, int r, uint16_t col) {
    C.drawRoundRect(x - 2, y - 2, w + 4, h + 4, r + 2, dim(col, 0.18f));
    C.drawRoundRect(x - 1, y - 1, w + 2, h + 2, r + 1, dim(col, 0.4f));
    C.drawRoundRect(x, y, w, h, r, col);
}

void chip(int x, int y, const char *s, uint16_t col, bool on, int *wOut = nullptr, int act = -1, int arg = 0) {
    font(F_SM);
    int w = C.textWidth(s) + 28, h = 30;
    if (act >= 0) hit(x - 4, y - 8, w + 8, h + 16, act, arg);   // a finger is bigger than the chip
    if (on) {
        C.fillRoundRect(x, y, w, h, 15, dim(col, 0.22f));
        glow_rect(x, y, w, h, 15, col);
        text(x + w / 2, y + h / 2 + 1, s, col, middle_center);
    } else {
        C.drawRoundRect(x, y, w, h, 15, LINE2);
        text(x + w / 2, y + h / 2 + 1, s, MUTED, middle_center);
    }
    if (wOut) *wOut = w;
}

void dot(int x, int y, int r, uint16_t col) {
    C.fillCircle(x, y, r + 3, dim(col, 0.15f));
    C.fillCircle(x, y, r + 1, dim(col, 0.4f));
    C.fillCircle(x, y, r, col);
}

uint16_t rssi_color(int rssi) {
    if (!rssi) return FAINT;
    if (rssi >= -60) return ACCENT;
    if (rssi >= -75) return SKY;
    if (rssi >= -85) return AMBER;
    return RED;
}

void rssi_bar(int x, int y, int w, int h, int rssi, uint16_t col) {
    C.fillRoundRect(x, y, w, h, h / 2, PANEL2);
    if (!rssi) return;
    float f = constrain((rssi + 100) / 70.0f, 0.03f, 1.0f);
    int fw = max(h, (int)(w * f));
    C.fillRoundRect(x, y, fw, h, h / 2, dim(col, 0.85f));
    C.drawFastHLine(x + h / 2, y, fw - h, col);
}

const char *age_str(double ts) {
    static char buf[4][12];
    static int k = 0;
    char *b = buf[k = (k + 1) & 3];
    double now = server_now();
    if (!ts || !now) return "-";
    double a = max(0.0, now - ts);
    if (a < 60) snprintf(b, 12, "%ds", (int)a);
    else if (a < 3600) snprintf(b, 12, "%dm", (int)(a / 60));
    else if (a < 86400) snprintf(b, 12, "%dh", (int)(a / 3600));
    else snprintf(b, 12, "%dd", (int)(a / 86400));
    return b;
}

const char *dur_str(uint32_t s) {
    static char b[16];
    if (s < 3600) snprintf(b, sizeof b, "%um", s / 60);
    else if (s < 86400) snprintf(b, sizeof b, "%uh %02um", s / 3600, (s / 60) % 60);
    else snprintf(b, sizeof b, "%ud %02uh", s / 86400, (s / 3600) % 24);
    return b;
}

const char *clock_str(double ts, bool secs) {
    static char b[16];
    if (!ts || !M->clockValid) return secs ? "--:--:--" : "--:--";
    time_t t = (time_t)ts;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(b, sizeof b, secs ? "%H:%M:%S" : "%H:%M", &tm);
    return b;
}

// The name to show for a detection: dashboard alias, else what it calls itself, else the vendor.
const char *vendor_of(const Det *d) {
    if (d->alias[0]) return d->alias;
    if (d->name[0]) return d->name;
    if (d->descr[0]) return d->descr;
    const char *v = sd_vendor(d->mac);   // the card's OUI table: no dashboard needed
    return v[0] ? v : d->type;
}

// An intel value with its registry name from the card: "0x004C" -> "0x004C  Apple, Inc.".
void intel_named(const char *key, char *v, size_t cap) {
    char nm[48];
    auto num = [](const char *s) { return (uint32_t)strtoul(s, nullptr, 16); };
    if (!strcmp(key, "cid") && v[0] && sd_lookup(LK_CID, num(v), nm, sizeof nm)) {
        strlcat(v, "  ", cap), strlcat(v, nm, cap);
    } else if (!strcmp(key, "app") && v[0]) {
        uint32_t a = (uint32_t)atoi(v);
        if (sd_lookup(LK_APPEAR, a, nm, sizeof nm) || sd_lookup(LK_APPEAR, a & ~0x3Fu, nm, sizeof nm))
            strlcat(v, "  ", cap), strlcat(v, nm, cap);
    }
}

// ------------------------------------------------------------------ filtering

bool icontains(const char *hay, const char *needle) {
    if (!*needle) return true;
    for (const char *h = hay; *h; h++) {
        const char *a = h, *b = needle;
        while (*a && *b && tolower(*a) == tolower(*b)) a++, b++;
        if (!*b) return true;
    }
    return false;
}

// Which firmware a node runs (its build stamp suffix): 0 detector, 1 Sky-Spy, 2 wardriver,
// 3 GPS node, 4 GPS wardriver (the last two: AtomS3 + GPS base).
int node_kind(const Node &n) {
    size_t l = strlen(n.fw);
    char c = l ? n.fw[l - 1] : 0;
    return c == 'S' ? 1 : c == 'W' ? 2 : c == 'G' ? 3 : c == 'Y' ? 4 : 0;
}
// Filters group them by what they do: a GPS node detects, a GPS wardriver wardrives.
int node_group(const Node &n) { int k = node_kind(n); return k == 3 ? 0 : k == 4 ? 2 : k; }
const char *KIND_NAME[5] = {"DETECTOR", "SKY-SPY", "WARDRIVER", "GPS NODE", "GPS WARDRIVE"};
uint16_t kind_color(int k) { return k == 1 ? PINK : k == 2 ? rgb(0xff2bd6) : k >= 3 ? rgb(0x39ff88) : SKY; }

// A little house: marks the home node next to its firmware type.
void home_icon(int x, int y, uint16_t c) {   // x, y = bottom-left
    C.fillTriangle(x, y - 9, x + 9, y - 17, x + 18, y - 9, c);
    C.fillRect(x + 3, y - 9, 12, 9, c);
    C.fillRect(x + 7, y - 5, 4, 5, BG);
}

// "DETECTOR" / "SKY-SPY" / "WARDRIVER", with the house in front for the home.
int kind_label(int x, int y, const Node &n) {   // y = middle; returns the width
    int k = node_kind(n), w = 0;
    uint16_t c = n.isHome ? ACCENT : kind_color(k);
    if (n.isHome) home_icon(x, y + 8, c), w = 26;
    text(x + w, y, KIND_NAME[k], c, middle_left);
    return w + C.textWidth(KIND_NAME[k]);
}

bool ota_busy(const Node &n) {
    return !strcmp(n.ota, "offered") || !strcmp(n.ota, "sending") || !strcmp(n.ota, "running") ||
           !strcmp(n.ota, "verifying") || !strcmp(n.ota, "staging");
}

// Firmware updates in flight: a strip at the bottom of Overview and Fleet, one bar per
// node, flowing blue to pink.
int ota_strip_h() {
    int k = 0;
    for (int i = 0; i < M->nnodes; i++) k += ota_busy(M->nodes[i]);
    return k ? 16 + min(k, 3) * 44 : 0;
}
void draw_ota_strip() {
    int h = ota_strip_h();
    if (!h) return;
    int y = CB - h, x = PAD, w = W - 2 * PAD;
    fast_round(x, y, w, h, 10, PANEL);
    glow_rect(x, y, w, h, 10, dim(PINK, 0.8f));
    int ry = y + 10, shown = 0;
    for (int i = 0; i < M->nnodes && shown < 3; i++) {
        const Node &n = M->nodes[i];
        if (!ota_busy(n)) continue;
        shown++;
        font(F_SM);
        char l[96];
        bool verify = !strcmp(n.ota, "verifying");
        snprintf(l, sizeof l, "FIRMWARE  %s  %s", n.alias[0] ? n.alias : n.id,
                 verify ? "restarting into the new build" : !strcmp(n.ota, "offered") ? "waiting for the node" : "sending over the mesh");
        text(x + 16, ry + 6, l, TEXT, middle_left);
        char pc[8];
        snprintf(pc, sizeof pc, "%d%%", n.otaPct);
        font(F_MD);
        text(x + w - 16, ry + 6, pc, PINK, middle_right);
        int bx = x + 16, bw = w - 32, by = ry + 18, bh = 10;
        C.fillRoundRect(bx, by, bw, bh, 5, PANEL2);
        int fw = bw * constrain((int)n.otaPct, 0, 100) / 100;
        for (int px = 0; px < fw; px += 4) {   // gradient fill, a shine that moves along it
            float t = (float)px / bw;
            uint16_t c = lgfx::color565(77 + (int)(178 * t), 163 - (int)(120 * t), 255 - (int)(41 * t));
            float shine = fmodf(millis() / 900.0f, 1.0f);
            if (fabsf(t - shine) < 0.04f) c = TEXT;
            C.fillRect(bx + px, by, min(4, fw - px), bh, c);
        }
        if (verify) C.drawRoundRect(bx, by, bw, bh, 5, dim(ACCENT, 0.8f));
        ry += 44;
    }
}

bool det_match(const Det *d) {
    if (A.dom == 1 && strcmp(d->dom, "ble")) return false;
    if (A.dom == 2 && strcmp(d->dom, "wifi")) return false;
    if (A.node[0]) {
        bool by = !strcmp(d->node, A.node);
        for (int i = 0; i < d->nseen && !by; i++) by = !strcmp(d->seen[i].node, A.node);
        if (!by) return false;
    }
    if (A.kindFilter) {   // only what nodes of one firmware type heard
        const Node *dn = node_by_id(d->node);
        bool by = dn && node_group(*dn) == A.kindFilter - 1;
        for (int i = 0; i < d->nseen && !by; i++) {
            const Node *sn = node_by_id(d->seen[i].node);
            by = sn && node_group(*sn) == A.kindFilter - 1;
        }
        if (!by) return false;
    }
    if (!A.search[0]) return true;
    return icontains(d->descr, A.search) || icontains(d->alias, A.search) || icontains(d->mac, A.search) ||
           icontains(d->type, A.search) || icontains(node_name(d->node), A.search) || icontains(d->node, A.search);
}

bool target_match(const Target *t) {
    if (A.dom == 1 && strcmp(t->dom, "ble")) return false;
    if (A.dom == 2 && strcmp(t->dom, "wifi")) return false;
    if (A.node[0]) {
        bool by = false;
        for (int i = 0; i < t->nseen && !by; i++) by = !strcmp(t->seen[i].node, A.node);
        if (!by) return false;
    }
    return !A.search[0] || icontains(t->descr, A.search) || icontains(t->mac, A.search) ||
           icontains(t->type, A.search);
}

// Filtered views, rebuilt per frame (cheap: a few thousand rows).
// Live, all nodes: detections AND this session's wardriven networks/devices, one list
// newest first. An entry >= 0 is a detection (det_at), < 0 a wardriven one (wd_at(-e - 1)).
int *liveIdx = nullptr, nLive = 0;

// One Live entry, either kind, as the table shows it.
struct LiveRow {
    const char *name, *type, *dom, *node, *closest;
    char mac[18];
    int rssi;
    double ts;
    bool isNew, wd;
};
void live_row(int e, LiveRow &r) {
    if (e >= 0) {
        const Det *d = det_at(e);
        r = {vendor_of(d), d->type, d->dom, d->node, d->closest, {}, d->rssi, d->ts, d->isNew, false};
        strlcpy(r.mac, d->mac, sizeof r.mac);
        return;
    }
    const WdDev &w = wd_at(-e - 1);
    r = {w.name[0] ? w.name : w.kind == 1 ? "(hidden network)" : "BLE device", w.kind == 1 ? "WIFI AP" : "BLE",
         w.kind == 1 ? "wifi" : "ble", w.node, "", {}, w.rssi, w.last, false, true};
    snprintf(r.mac, sizeof r.mac, "%02X:%02X:%02X:%02X:%02X:%02X", w.mac[0], w.mac[1], w.mac[2], w.mac[3], w.mac[4], w.mac[5]);
}
int tgtIdx[MAX_TARGETS], nTgt = 0;
extern int nDrone;

void build_lists() {
    if (!liveIdx) liveIdx = (int *)heap_caps_malloc((MAX_DETS + WD_MAX) * sizeof(int), MALLOC_CAP_SPIRAM);
    nLive = 0;
    if (!liveIdx) return;
    for (int i = 0; i < M->ndets; i++)
        if (det_match(det_at(i))) liveIdx[nLive++] = i;
    if (!A.node[0] && !A.kindFilter) {   // every node: the wardrivers' finds belong here too
        int nd = nLive;
        for (int i = 0, n = wd_count(); i < n && nLive < MAX_DETS + WD_MAX; i++) {
            const WdDev &w = wd_at(i);
            if (!w.session || (A.dom == 1 && w.kind != 2) || (A.dom == 2 && w.kind != 1)) continue;
            if (A.search[0]) {
                char m[18];
                snprintf(m, sizeof m, "%02X:%02X:%02X:%02X:%02X:%02X", w.mac[0], w.mac[1], w.mac[2], w.mac[3], w.mac[4], w.mac[5]);
                if (!icontains(w.name, A.search) && !icontains(m, A.search) && !icontains(node_name(w.node), A.search)) continue;
            }
            liveIdx[nLive++] = -i - 1;
        }
        if (nLive > nd) {   // one timeline: newest first, whichever kind
            auto ts = [](int e) { return e >= 0 ? det_at(e)->ts : wd_at(-e - 1).last; };
            std::stable_sort(liveIdx, liveIdx + nLive, [&](int a, int b) { return ts(a) > ts(b); });
        }
    }
    nTgt = 0;
    for (int i = 0; i < M->ntargets; i++)
        if (target_match(&M->targets[i])) tgtIdx[nTgt++] = i;
    std::sort(tgtIdx, tgtIdx + nTgt, [](int a, int b) { return M->targets[a].last > M->targets[b].last; });
}

// Live with the node filter on a wardriver: the list is its wardriven networks and devices.
int wdN = 0;
int *wdIdx = nullptr;
bool live_is_wardrive() {
    if (A.kindFilter == 3) return true;   // "WARDRIVERS"
    const Node *fn = A.node[0] ? node_by_id(A.node) : nullptr;
    return fn && node_is_wardriver(*fn);
}

int list_len(View v) {
    switch (v) {
    case V_FLEET: return M->nnodes;
    case V_LIVE: return live_is_wardrive() ? wdN : nLive;
    case V_TARGETS: return nTgt;
    case V_DRONES: return nDrone;
    default: return 0;
    }
}

void clamp_sel(View v, int rows) {
    int n = list_len(v);
    int &s = A.sel[v], &t = A.top[v];
    s = constrain(s, 0, max(0, n - 1));
    if (s < t) t = s;
    if (s >= t + rows) t = s - rows + 1;
    t = constrain(t, 0, max(0, n - rows));
}

// ------------------------------------------------------------------ MAP

// Web Mercator, in pixels at zoom z.
double lon2x(double lon, int z) { return (lon + 180.0) / 360.0 * TILE * (double)(1 << z); }
double lat2y(double lat, int z) {
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * TILE * (double)(1 << z);
}
double x2lon(double x, int z) { return x / (TILE * (double)(1 << z)) * 360.0 - 180.0; }
double y2lat(double y, int z) {
    double n = M_PI - 2.0 * M_PI * y / (TILE * (double)(1 << z));
    return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

constexpr int MX = 0, MY = TOP, MW = W, MH = H - TOP - BOT;
constexpr int L_MAP = 11;    // scroll id: dragging the map pans it
constexpr int L_OVF = 12, L_OVL = 13;   // Overview's fleet column and live feed scroll
int ovFleetTop = 0, ovLiveTop = 0;
constexpr int ZMIN = 3, ZMAX = 19;   // ZMAX: map_fit's limit
constexpr int OVERZOOM = 2;          // zoom this far past a layer's last real tiles (scaled up)
int zmax_layer() { return LAYER_MAX[A.mapLayer] + OVERZOOM; }
double gZoomShown = 17;              // the zoom actually on screen (snapped): all screen <-> map maths

const uint16_t ORANGE = rgb(0xfb923c);
uint16_t tag_color(const char *tag) {
    if (!strcmp(tag, "friendly")) return ACCENT;
    if (!strcmp(tag, "neutral")) return AMBER;
    if (!strcmp(tag, "suspicious")) return ORANGE;
    if (!strcmp(tag, "hostile")) return RED;
    return SKY;
}
static const char *DRONE_TAGS[] = {"unknown", "friendly", "neutral", "suspicious", "hostile"};

const char *drone_name(const Drone &d) {
    return d.alias[0] ? d.alias : d.desc[0] ? d.desc : d.id[0] ? d.id : d.mac;
}
bool drone_live(const Drone &d) { return server_now() - d.last < DRONE_LIVE_S; }
bool drone_tx(const Drone &d) { return server_now() - d.last <= DRONE_TX_S; }

// Like mesh-mapper (and the web dashboard): each drone keeps its own colour, hashed from
// its MAC, so several in the air never look alike; a tag the operator set takes over.
uint16_t drone_color(const Drone &d) {
    if (d.tag[0] && strcmp(d.tag, "unknown")) return tag_color(d.tag);
    int32_t h = 0;
    for (const char *p = d.mac; *p; p++) h = (int32_t)((uint32_t)h * 31u + (uint8_t)*p);
    float hue = (float)((h < 0 ? -(int64_t)h : (int64_t)h) % 360);   // JS: Math.abs(h) % 360
    // hsl(hue, 100%, 62%) -> rgb
    float l = 0.62f, ch = (1 - fabsf(2 * l - 1)), x = ch * (1 - fabsf(fmodf(hue / 60.0f, 2) - 1)), m = l - ch / 2;
    float r = 0, g = 0, b = 0;
    if (hue < 60) r = ch, g = x;
    else if (hue < 120) r = x, g = ch;
    else if (hue < 180) g = ch, b = x;
    else if (hue < 240) g = x, b = ch;
    else if (hue < 300) r = x, b = ch;
    else r = ch, b = x;
    return rgb(((uint32_t)((r + m) * 255) << 16) | ((uint32_t)((g + m) * 255) << 8) | (uint32_t)((b + m) * 255));
}

// Drones, live first then newest.
int droneIdx[MAX_DRONES], nDrone = 0;
void build_drones() {
    nDrone = 0;
    for (int i = 0; i < M->ndrones; i++) droneIdx[nDrone++] = i;
    std::sort(droneIdx, droneIdx + nDrone, [](int a, int b) {
        bool la = drone_live(M->drones[a]), lb = drone_live(M->drones[b]);
        if (la != lb) return la;
        return M->drones[a].last > M->drones[b].last;
    });
}

// Where a target probably is: the nodes that hear it, weighted by signal power.
bool target_pos(const Target &t, double &lat, double &lon) {
    double sw = 0, a = 0, o = 0;
    for (int i = 0; i < t.nseen; i++) {
        const Node *n = node_by_id(t.seen[i].node);
        if (!n || !n->hasPos) continue;
        double w = pow(10.0, t.seen[i].rssi / 10.0);
        sw += w, a += w * n->lat, o += w * n->lon;
    }
    if (!sw) return false;
    lat = a / sw, lon = o / sw;
    return true;
}

const char *target_label(const Target &t) {
    return t.alias[0] ? t.alias : t.name[0] ? t.name : t.descr[0] ? t.descr : t.type;
}

// Text that stays readable over satellite imagery: a dark outline, then the face.
void map_text(int x, int y, const char *s, uint16_t col, textdatum_t d = middle_left) {
    C.setTextDatum(d);
    C.setTextColor(BG);
    C.drawString(s, x - 1, y - 1), C.drawString(s, x + 1, y - 1);   // 4 outline passes: readable, half of 8
    C.drawString(s, x - 1, y + 1), C.drawString(s, x + 1, y + 1);
    C.setTextColor(col);
    C.drawString(s, x, y);
}

void dashed(int x0, int y0, int x1, int y1, uint16_t col) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    if (len < 1) return;
    for (float t = 0; t < len; t += 12) {
        float e = min(len, t + 7);
        C.drawLine(x0 + dx * t / len, y0 + dy * t / len, x0 + dx * e / len, y0 + dy * e / len, col);
    }
}

// Quadcopter glyph: ring, X arms, rotors, heading nose.
void drone_glyph(int x, int y, float hdg, bool hasHdg, uint16_t col, bool live, float scale = 1.0f, bool tx = false) {
    uint16_t c = live ? col : dim(col, 0.35f);
    int r = 17 * scale;
    if (tx) {   // a Remote ID frame in the last 5 s: lime ring, like mesh-mapper
        float p = fmodf(millis() / 1000.0f, 1.0f);
        uint16_t lime = rgb(0x7dff3a);
        int rr = r + 4 + (int)(7 * p);
        C.drawCircle(x, y, rr, dim(lime, 1.0f - 0.7f * p));
        C.drawCircle(x, y, rr + 1, dim(lime, 0.8f - 0.6f * p));
        C.drawCircle(x, y, r + 3, lime);
    } else if (live) {
        float p = 0.5f + 0.5f * sinf(millis() / 300.0f);
        C.drawCircle(x, y, r + 4 + (int)(4 * p), dim(c, 0.35f * (1 - p) + 0.1f));
    }
    C.fillCircle(x, y, r, dim(c, 0.18f));
    C.drawCircle(x, y, r, c);
    float a = (hasHdg ? hdg : 0) * M_PI / 180.0f;
    for (int k = 0; k < 4; k++) {
        float ang = a + M_PI / 4 + k * M_PI / 2;
        int ex = x + sinf(ang) * 11 * scale, ey = y - cosf(ang) * 11 * scale;
        C.drawLine(x, y, ex, ey, c);
        C.drawLine(x + 1, y, ex + 1, ey, c);
        C.fillCircle(ex, ey, 5 * scale, BG);
        C.drawCircle(ex, ey, 5 * scale, c);
    }
    if (hasHdg) {
        int nx = x + sinf(a) * (r + 3), ny = y - cosf(a) * (r + 3);
        int lx = x + sinf(a - 0.35f) * (r - 5), ly = y - cosf(a - 0.35f) * (r - 5);
        int rx = x + sinf(a + 0.35f) * (r - 5), ry = y - cosf(a + 0.35f) * (r - 5);
        C.fillTriangle(nx, ny, lx, ly, rx, ry, TEXT);
    }
}

void pilot_glyph(int x, int y, uint16_t col) {
    C.fillCircle(x, y - 7, 5, col);
    C.fillTriangle(x - 8, y + 9, x + 8, y + 9, x, y - 1, col);
    C.drawCircle(x, y, 14, dim(col, 0.5f));
}

void map_fit() {
    double la0 = 90, la1 = -90, lo0 = 180, lo1 = -180;
    int n = 0;
    auto add = [&](double la, double lo) {
        la0 = min(la0, la), la1 = max(la1, la), lo0 = min(lo0, lo), lo1 = max(lo1, lo);
        n++;
    };
    for (int i = 0; i < M->nnodes; i++)
        if (M->nodes[i].hasPos) add(M->nodes[i].lat, M->nodes[i].lon);
    for (int i = 0; i < M->ndrones; i++) {
        const Drone &d = M->drones[i];
        if (!drone_live(d)) continue;
        if (d.hasPos) add(d.lat, d.lon);
        if (d.hasPilot) add(d.plat, d.plon);
    }
    if (!n) return;
    A.mapLat = (la0 + la1) / 2, A.mapLon = (lo0 + lo1) / 2;
    int z = ZMAX;
    while (z > ZMIN && (lon2x(lo1, z) - lon2x(lo0, z) > MW - 260 || lat2y(la0, z) - lat2y(la1, z) > MH - 160)) z--;
    A.mapZoom = z;
    A.mapInit = true;
}

void map_center_on(double lat, double lon) {
    A.mapLat = lat, A.mapLon = lon;
    A.mapInit = true;
    maptiles_clear_requests();
}

bool mapAnimating = false;   // zoom easing in progress: frame at full rate

// Fractional-zoom Mercator (world pixels at zoom z, z not necessarily whole).
double lon2xf(double lon, double z) { return (lon + 180.0) / 360.0 * TILE * pow(2.0, z); }
double lat2yf(double lat, double z) {
    double r = lat * M_PI / 180.0;
    return (1.0 - log(tan(r) + 1.0 / cos(r)) / M_PI) / 2.0 * TILE * pow(2.0, z);
}
double x2lonf(double x, double z) { return x / (TILE * pow(2.0, z)) * 360.0 - 180.0; }
double y2latf(double y, double z) {
    double n = M_PI - 2.0 * M_PI * y / (TILE * pow(2.0, z));
    return 180.0 / M_PI * atan(0.5 * (exp(n) - exp(-n)));
}

// Zoom anchor: while the zoom eases (double tap, pinch), this map point stays under this
// screen point, so the map grows out of your finger instead of the screen centre.
struct { bool on; int x, y; double lat, lon; } ZA = {};
bool pinching = false;

void screen_to_ll(int x, int y, double zf, double &lat, double &lon) {
    double cx = lon2xf(A.mapLon, zf), cy = lat2yf(A.mapLat, zf);
    lon = x2lonf(cx + (x - MX - MW / 2.0), zf);
    lat = y2latf(cy + (y - MY - MH / 2.0), zf);
}

void apply_anchor(double zf) {
    if (!ZA.on) return;
    A.mapLon = x2lonf(lon2xf(ZA.lon, zf) - (ZA.x - MX - MW / 2.0), zf);
    A.mapLat = y2latf(lat2yf(ZA.lat, zf) - (ZA.y - MY - MH / 2.0), zf);
}

void map_zoom_at(int dz, int x, int y) {
    int nz = constrain(A.mapZoom + dz, ZMIN, zmax_layer());
    if (nz == A.mapZoom) return;
    ZA.on = true, ZA.x = x, ZA.y = y;
    screen_to_ll(x, y, gZoomShown, ZA.lat, ZA.lon);
    A.mapZoom = nz;
    A.mapFollow = false, A.gpsFollow = false;
    maptiles_clear_requests();
}

// Draw a tile so that tile (z, x, y) covers the given screen square (logical top left
// sx0, sy0; size ts, a whole number of pixels), even when the image is a coarser ancestor
// k levels up (its matching part is scaled up). The PPA rotates it onto the portrait
// framebuffer and scales it, by DMA; only what falls off the screen is cropped.
void tile_blit(const uint16_t *t, int k, int tx, int ty, double ts, int sx0, int sy0) {
    if (!ppa_canvas()) return;
    int sub = TILE >> k;
    int su = (tx & ((1 << k) - 1)) * sub, sv = (ty & ((1 << k) - 1)) * sub;
    float scale = (float)(ts / sub);   // a multiple of 1/16 (draw_map snaps it)
    int cu0 = sx0 < 0 ? (int)ceilf(-sx0 / scale) : 0;
    int cv0 = sy0 < 0 ? (int)ceilf(-sy0 / scale) : 0;
    int X = sx0 + (int)lroundf(cu0 * scale), Y = sy0 + (int)lroundf(cv0 * scale);
    int uw = min(sub - cu0, (int)floorf((W - X) / scale));
    int vh = min(sub - cv0, (int)floorf((H - Y) / scale));
    if (uw <= 0 || vh <= 0) return;
    int w = (int)(uw * scale), h = (int)(vh * scale);
    if (w <= 0 || h <= 0) return;
    int px, py, pw, ph;
    to_panel(X, Y, w, h, px, py, pw, ph);
    gfx_tile((uint16_t *)C.getBuffer(), t, su + cu0, sv + cv0, uw, vh, px, py, canvasRot() == 3, scale);
}

void draw_map() {
    if (!A.mapInit) {
        map_fit();
        A.mapZoomF = A.mapZoom;
        if (!A.mapInit) {   // no node positions yet: don't fetch the middle of the ocean
            C.fillRect(MX, MY, MW, MH, rgb(0x0a0d12));
            font(F_MD);
            text(W / 2, MY + MH / 2, net_online() || M->usbLive ? "waiting for node positions (set them on the dashboard map)"
                                                                 : "waiting for the dashboard", FAINT, middle_center);
            return;
        }
    }
    if (A.gpsFollow) {   // our own GPS position stays centred
        GpsFix gf = gps_fix();
        if (gf.valid) A.mapLat = gf.lat, A.mapLon = gf.lon;
    }
    if (A.mapFollow) {
        for (int k = 0; k < nDrone; k++) {
            const Drone &d = M->drones[droneIdx[k]];
            if (drone_live(d) && d.hasPos) { A.mapLat = d.lat, A.mapLon = d.lon; break; }
        }
    }
    // Ease the displayed zoom toward the target: ~150 ms per level, frame-rate independent.
    static uint32_t lastMs = 0;
    uint32_t nowMs = millis();
    float dt = lastMs ? min(0.1f, (nowMs - lastMs) / 1000.0f) : 0;
    lastMs = nowMs;
    float target = A.mapZoom;
    if (pinching) {
        mapAnimating = true;            // the fingers set the zoom directly
    } else if (fabsf(A.mapZoomF - target) > 0.004f) {
        A.mapZoomF += (target - A.mapZoomF) * min(1.0f, dt * 14.0f);
        if (fabsf(A.mapZoomF - target) < 0.004f) A.mapZoomF = target;
        mapAnimating = true;
    } else {
        A.mapZoomF = target;
        mapAnimating = false;
    }
    float zf = A.mapZoomF;
    int zb = constrain((int)floorf(zf + 0.0001f), ZMIN, zmax_layer());
    // Base-zoom pixels -> screen pixels, snapped to the PPA's 1/16 scale steps so every tile
    // is a whole number of pixels and neighbours meet exactly while zooming.
    double sc = round(pow(2.0, zf - zb) * 16.0) / 16.0;
    if (sc < 1.0) sc = 1.0;
    // Everything that maps screen <-> map (the finger anchor, drags, taps) uses this same
    // snapped zoom: mixing it with the unsnapped one slid the map under the finger.
    gZoomShown = zb + log2(sc);
    apply_anchor(gZoomShown);
    if (!mapAnimating) ZA.on = false;
    double ts = TILE * sc;
    int z = zb;
    double cx = lon2x(A.mapLon, zb) * sc, cy = lat2y(A.mapLat, zb) * sc;
    double left = cx - MW / 2.0, top = cy - MH / 2.0;
    auto sx = [&](double lon) { return (int)(MX + lon2x(lon, zb) * sc - left); };
    auto sy = [&](double lat) { return (int)(MY + lat2y(lat, zb) * sc - top); };

    uint32_t q0 = micros();
    scroll_area(MX, MY, MW, MH, L_MAP, 1, 1);
    uint32_t q1 = micros();
    // Tiles: this zoom's tile if loaded; else an ancestor scaled up, else the previous
    // layer's, so a zoom or a layer switch never shows a blank map.
    int n = 1 << zb;
    int tx0 = (int)floor(left / ts), ty0 = (int)floor(top / ts);
    int tx1 = (int)floor((left + MW) / ts), ty1 = (int)floor((top + MH) / ts);
    int missing = 0;
    bool online = net_online();
    bool settled = !mapAnimating;
    for (int ty = ty0; ty <= ty1; ty++) {
        if (ty < 0 || ty >= n) continue;
        for (int tx = tx0; tx <= tx1; tx++) {
            int wx = ((tx % n) + n) % n;
            int x = MX + (int)floor(tx * ts - left), y = MY + (int)floor(ty * ts - top);
            // Past the layer's last real zoom (kz levels): its closest tiles, scaled up.
            int kz = max(0, zb - (int)LAYER_MAX[A.mapLayer]);
            int zt = zb - kz;
            const uint16_t *img = !online ? nullptr
                : settled ? maptile(A.mapLayer, zt, wx >> kz, ty >> kz) : maptile_peek(A.mapLayer, zt, wx >> kz, ty >> kz);
            if (img) {
                tile_blit(img, kz, wx, ty, ts, x, y);
                continue;
            }
            missing++;
            bool drawn = false;
            for (int k = kz + 1; k <= kz + 3 && k <= 3 + OVERZOOM && !drawn && zb - k >= 0; k++) {
                const uint16_t *anc = maptile_peek(A.mapLayer, zb - k, wx >> k, ty >> k);
                if (anc) tile_blit(anc, k, wx, ty, ts, x, y), drawn = true;
            }
            if (!drawn && A.mapPrevLayer != A.mapLayer) {
                const uint16_t *old = maptile_peek(A.mapPrevLayer, zb, wx, ty);
                if (old) tile_blit(old, 0, wx, ty, ts, x, y), drawn = true;
            }
            if (!drawn) {
                int is = (int)ceil(ts);
                ppa_fill(x, y, is, is, rgb(0x0a0d12));
                C.drawRect(x, y, is, is, rgb(0x141a22));
                C.drawFastHLine(x, y + is / 2, is, rgb(0x10151c));
                C.drawFastVLine(x + is / 2, y, is, rgb(0x10151c));
            }
        }
    }
    C.setClipRect(MX, MY, MW, MH);
    uint32_t q2 = micros();
    uint32_t q3 = micros();
    // Live targets.
    double now = server_now();
    for (int i = 0; i < M->ntargets; i++) {
        const Target &t = M->targets[i];
        if (now - t.last > TARGET_LIVE_S) continue;
        double la, lo;
        // The dashboard's triangulated fix (same model as its web map), else a
        // signal-weighted estimate between the nodes that hear it.
        bool tri = A.triOn && t.triOk && now - t.triTs < 120;
        if (tri) la = t.triLat, lo = t.triLon;
        else if (!target_pos(t, la, lo)) continue;
        int x = sx(lo), y = sy(la);
        if (x < -40 || y < MY - 40 || x > W + 40 || y > H) continue;
        uint16_t c = mac_muted(t.mac) ? FAINT : PINK;
        if (tri) {
            // Like the web map's Triangulate: a faint line to every node that heard it.
            for (int s = 0; s < t.nseen; s++) {
                const Node *n = node_by_id(t.seen[s].node);
                if (n && n->hasPos) dashed(sx(n->lon), sy(n->lat), x, y, dim(rssi_color(t.seen[s].rssi), 0.55f));
            }
            double mpp = 156543.03392 * cos(la * M_PI / 180) / pow(2.0, zf);
            int r = (int)(t.triErr / mpp);
            uint16_t rc = t.triRough ? AMBER : rgb(0x39ff88);
            if (r > 6 && r < 1400) {
                C.drawCircle(x, y, r, dim(rc, 0.85f));
                C.drawCircle(x, y, r + 1, dim(rc, 0.35f));
            }
        }
        C.fillTriangle(x, y - 9, x + 9, y, x, y + 9, dim(c, 0.6f));
        C.fillTriangle(x, y + 9, x - 9, y, x, y - 9, dim(c, 0.6f));
        C.drawLine(x, y - 9, x + 9, y, c), C.drawLine(x + 9, y, x, y + 9, c);
        C.drawLine(x, y + 9, x - 9, y, c), C.drawLine(x - 9, y, x, y - 9, c);
        font(F_SM);
        char tl[64];
        if (tri) snprintf(tl, sizeof tl, "%s  +/-%dm%s", fit_name(target_label(t), 150), (int)lroundf(t.triErr), t.triRough ? " rough" : "");
        else snprintf(tl, sizeof tl, "%s", fit_name(target_label(t), 180));
        map_text(x + 13, y, tl, c);
        int ti = -1;
        for (int k = 0; k < nTgt; k++) if (tgtIdx[k] == i) ti = k;
        if (ti >= 0) hit(x - 16, y - 16, 32, 32, H_TGTMAP, ti);
    }
    // Mesh links: each pair of nodes that hear each other directly, coloured by the
    // signal, its dBm at the middle (drawn once per pair).
    for (int i = 0; i < M->nnodes; i++) {
        const Node &a = M->nodes[i];
        if (!a.hasPos || !a.online) continue;
        for (int k = 0; k < a.nnbr; k++) {
            const Node *b = node_by_id(a.nbr[k].node);
            if (!b || !b->hasPos || strcmp(a.id, b->id) >= 0) continue;
            int x0 = sx(a.lon), y0 = sy(a.lat), x1 = sx(b->lon), y1 = sy(b->lat);
            uint16_t lc = rssi_color(a.nbr[k].rssi);
            C.drawLine(x0, y0, x1, y1, dim(lc, 0.9f));
            C.drawLine(x0 + 1, y0, x1 + 1, y1, dim(lc, 0.45f));
            if (abs(x1 - x0) + abs(y1 - y0) > 80) {
                char db[8];
                snprintf(db, sizeof db, "%d", a.nbr[k].rssi);
                font(F_SM);
                map_text((x0 + x1) / 2, (y0 + y1) / 2, db, lc, middle_center);
            }
        }
    }
    // Nodes: dots first, then labels placed so none overlap (nodes a few metres apart
    // sit in a cluster). Each label tries right, left, then a column stepping down and
    // up beside its dot, joined to it by a thin leader line when it moved.
    struct Box { int x0, y0, x1, y1; };
    Box placed[MAX_NODES * 2 + 2];
    int np = 0;
    auto clear = [&](const Box &b) {
        for (int k = 0; k < np; k++)
            if (b.x0 < placed[k].x1 && b.x1 > placed[k].x0 && b.y0 < placed[k].y1 && b.y1 > placed[k].y0) return false;
        return true;
    };
    if (A.droneTracks) {   // GPS nodes' tracks, under the markers (TRACKS hides them; each its own in its card)
        static float (*ntp)[2] = (float (*)[2])heap_caps_malloc(NODE_TRAIL * sizeof(float[2]), MALLOC_CAP_SPIRAM);
        for (int i = 0; ntp && i < M->nnodes; i++) {
            const Node &nd = M->nodes[i];
            int k = node_trail(nd.id, ntp, NODE_TRAIL);
            uint16_t c = node_color565(nd.id);
            for (int p = 1; p < k; p++)
                C.drawLine(sx(ntp[p - 1][1]), sy(ntp[p - 1][0]), sx(ntp[p][1]), sy(ntp[p][0]), dim(c, 0.3f + 0.6f * p / k));
        }
    }
    int order[MAX_NODES], no = 0;
    for (int i = 0; i < M->nnodes; i++)
        if (M->nodes[i].hasPos) order[no++] = i;
    std::sort(order, order + no, [](int a, int b) { return M->nodes[a].isHome > M->nodes[b].isHome; });
    for (int k = 0; k < no; k++) {   // markers: solid dot, dark gap, outer ring; home bigger
        const Node &nd = M->nodes[order[k]];
        int x = sx(nd.lon), y = sy(nd.lat);
        uint16_t c = node_color565(nd.id);
        int r = nd.isHome ? 10 : 8;
        if (nd.online) {
            C.fillCircle(x, y, r + 7, dim(c, 0.18f));
            C.drawCircle(x, y, r + 5, dim(c, 0.75f));
            C.drawCircle(x, y, r + 4, dim(c, 0.45f));
            C.fillCircle(x, y, r + 2, BG);
            C.fillCircle(x, y, r, c);
            C.fillCircle(x, y, 2, TEXT);
        } else {
            C.fillCircle(x, y, r + 2, BG);
            C.drawCircle(x, y, r, FAINT);
            C.drawCircle(x, y, r - 1, FAINT);
        }
        placed[np++] = {x - r - 6, y - r - 6, x + r + 6, y + r + 6};   // labels keep off the dots
        hit(x - 24, y - 24, 48, 48, H_NODEPIN, order[k]);   // tap: its info card
    }
    font(F_SM);
    const int lh = 28;
    for (int k = 0; k < no; k++) {   // labels: dark pills edged in the node's colour, never overlapping
        const Node &nd = M->nodes[order[k]];
        int x = sx(nd.lon), y = sy(nd.lat);
        uint16_t c = nd.online ? node_color565(nd.id) : FAINT;
        const char *nm = nd.alias[0] ? nd.alias : nd.id;
        int tw = C.textWidth(nm) + 22;
        Box b = {};
        bool ok = false;
        int lx = 0, ly = 0;
        for (int step = 0; step <= 6 && !ok; step++)
            for (int side = 0; side < 2 && !ok; side++)
                for (int dir = (step ? -1 : 1); dir <= 1 && !ok; dir += 2) {
                    lx = side ? x - 22 - tw : x + 22;
                    ly = y + step * lh * dir;
                    b = {lx - 2, ly - lh / 2, lx + tw + 2, ly + lh / 2};
                    ok = clear(b) && b.y0 > MY && b.y1 < MY + MH && b.x0 > 0 && b.x1 < W;
                }
        if (!ok) continue;
        placed[np++] = b;
        if (ly != y) C.drawLine(x, y, lx < x ? b.x1 - 2 : lx, ly, dim(c, 0.7f));
        fast_round(lx, ly - 12, tw, 24, 8, rgb(0x0b0e13));
        C.drawRoundRect(lx, ly - 12, tw, 24, 8, dim(c, 0.9f));
        C.fillRect(lx + 4, ly - 7, 3, 14, c);
        text(lx + 12, ly, nm, nd.online ? TEXT : MUTED, middle_left);
    }
    uint32_t q4 = micros();
    // Drones: track, pilot and the line between them, then the aircraft on top.
    for (int k = nDrone - 1; k >= 0; k--) {
        const Drone &d = M->drones[droneIdx[k]];
        bool live = drone_live(d);
        if (!live && now - d.last > 1800) continue;
        uint16_t c = drone_color(d);
        // Pilot track: dashed, in the drone's colour (mesh-mapper's pilot path).
        if (A.pilotOn && A.droneTracks)
            for (int p = 1; p < d.nptrack; p++)
                dashed(sx(d.ptrack[p - 1][1]), sy(d.ptrack[p - 1][0]), sx(d.ptrack[p][1]), sy(d.ptrack[p][0]),
                       dim(c, live ? 0.75f : 0.35f));
        for (int p = 1; A.droneTracks && p < d.ntrack; p++) {
            int x0 = sx(d.track[p - 1][1]), y0 = sy(d.track[p - 1][0]), x1 = sx(d.track[p][1]), y1 = sy(d.track[p][0]);
            uint16_t tc = dim(c, (0.35f + 0.65f * p / d.ntrack) * (live ? 1.0f : 0.45f));
            C.drawLine(x0, y0, x1, y1, tc);
            C.drawLine(x0 + 1, y0, x1 + 1, y1, tc);
        }
        if (d.hasPilot && A.pilotOn) {
            int px = sx(d.plon), py = sy(d.plat);
            if (d.hasPos) dashed(px, py, sx(d.lon), sy(d.lat), dim(c, live ? 0.9f : 0.45f));
            pilot_glyph(px, py, live ? c : dim(c, 0.35f));
            font(F_SM);
            char b[32];
            if (d.pilotDist >= 0) snprintf(b, sizeof b, "PILOT  %ld m", (long)d.pilotDist);
            else snprintf(b, sizeof b, "PILOT");
            map_text(px + 18, py, b, c);
            hit(px - 18, py - 18, 36, 36, H_DRONE, k);
        }
        if (d.hasPos) {
            int x = sx(d.lon), y = sy(d.lat);
            drone_glyph(x, y, d.hdg, d.hasHdg, c, live, 1.0f, drone_tx(d));
            font(F_MD);
            map_text(x + 26, y - 9, fit_name(drone_name(d), 220), c);
            char b[48];
            if (d.hasHagl) snprintf(b, sizeof b, "%d m AGL%s", (int)d.hagl, "");
            else if (d.hasAlt) snprintf(b, sizeof b, "%d m MSL", (int)d.alt);
            else b[0] = 0;
            if (d.hasSpd) snprintf(b + strlen(b), sizeof b - strlen(b), "%s%.1f m/s", b[0] ? "  " : "", d.spd);
            font(F_SM);
            if (b[0]) map_text(x + 26, y + 11, b, TEXT);
            hit(x - 24, y - 24, 48, 48, H_DRONE, k);
        }
    }
    // Wardriven devices: one dot each where it was loudest, coloured like the web map
    // (WiFi by security, BLE pink); this session or all history.
    if (A.wdScope) {
        static const uint32_t SECC[6] = {0x39ff88, 0xff4d6d, 0xffb020, 0x22d3ee, 0xa78bfa, 0xa78bfa};
        int drawn = 0;
        for (int i = 0, n = wd_count(); i < n && drawn < 5000; i++) {
            const WdDev &d = wd_at(i);
            if (!d.placed || (A.wdScope == 1 && !d.session)) continue;
            int x = sx(d.lon), y = sy(d.lat);
            if (x < MX - 4 || x > MX + MW + 4 || y < MY - 4 || y > MY + MH + 4) continue;
            uint16_t c = d.kind == 2 ? rgb(0xff2bd6) : rgb(d.sec < 6 ? SECC[d.sec] : 0x7dd3fc);
            if (d.kind == 2) C.fillRect(x - 2, y - 2, 5, 5, c);
            else C.fillCircle(x, y, 3, c);
            drawn++;
        }
    }
    // Us: where the Tab5 is (GPS), its trail, a pulse around the dot and which way we move.
    {
        GpsFix gf = gps_fix();
        static float tr[GPS_TRAIL][2];
        int nt = gps_trail(tr, GPS_TRAIL);
        uint16_t me = rgb(0x4da3ff);
        for (int p = 1; A.droneTracks && p < nt; p++)   // TRACKS hides every track, ours too
            C.drawLine(sx(tr[p - 1][1]), sy(tr[p - 1][0]), sx(tr[p][1]), sy(tr[p][0]), dim(me, 0.3f + 0.6f * p / nt));
        if (gf.valid) {
            int x = sx(gf.lon), y = sy(gf.lat);
            float ph = (millis() % 1800) / 1800.0f;   // one ring out every 1.8 s, fading
            int pr = 13 + (int)(ph * 20);
            C.drawCircle(x, y, pr, dim(me, 0.8f * (1 - ph)));
            C.drawCircle(x, y, pr + 1, dim(me, 0.5f * (1 - ph)));
            C.fillCircle(x, y, 11, BG);
            C.fillCircle(x, y, 9, me);
            C.drawCircle(x, y, 12, TEXT);
            if (gf.speed > 0.8f) {
                float a = gf.course * M_PI / 180;
                C.fillTriangle(x + sinf(a) * 24, y - cosf(a) * 24, x + sinf(a + 2.5f) * 12, y - cosf(a + 2.5f) * 12,
                               x + sinf(a - 2.5f) * 12, y - cosf(a - 2.5f) * 12, me);
            }
            font(F_SM);
            map_text(x + 18, y, "YOU", me);
        }
    }
    // The tapped node's card: what it is, how it is doing, where it is; SETTINGS, MOVE.
    if (A.mapPop[0]) {
        const Node *pn = node_by_id(A.mapPop);
        if (!pn || !pn->hasPos) A.mapPop[0] = 0;
        else {
            int px = sx(pn->lon), py = sy(pn->lat), cw = 380, chh = 236;
            int cx = constrain(px + 30, MX + 8, MX + MW - cw - 8), cy = constrain(py - chh / 2, MY + 84, MY + MH - chh - 8);
            if (cx < px + 30 && px + 30 + cw > MX + MW - 8) cx = constrain(px - 30 - cw, MX + 8, MX + MW - cw - 8);
            uint16_t nc = node_color565(pn->id);
            fast_round(cx, cy, cw, chh, 10, dim(BG, 0.95f));
            glow_rect(cx, cy, cw, chh, 10, nc);
            hit(cx, cy, cw, chh, H_NONE);
            font(F_MD);
            text(cx + 16, cy + 22, fit_name(pn->alias[0] ? pn->alias : pn->id, cw - 80), nc, middle_left);
            chip(cx + cw - 52, cy + 8, "X", MUTED, false, nullptr, H_POPCLOSE);
            font(F_SM);
            int kw = kind_label(cx + 16, cy + 50, *pn);
            text(cx + 16 + kw + 14, cy + 50, pn->id, FAINT, middle_left);
            char l[64];
            snprintf(l, sizeof l, "%s   seen %s ago", pn->online ? "ONLINE" : "OFFLINE", pn->age >= 0 ? age_str(server_now() - pn->age) : "-");
            text(cx + 16, cy + 76, l, pn->online ? ACCENT : RED, middle_left);
            int rs = pn->isHome ? pn->lrssi : pn->rssi;
            snprintf(l, sizeof l, "link %s dBm   peers %d   ch %d", rs ? String(rs).c_str() : "--", pn->peers, pn->ch);
            text(cx + 16, cy + 100, l, TEXT, middle_left);
            snprintf(l, sizeof l, "fw %s   filters %d", pn->fw[0] ? pn->fw : "?", pn->filters);
            text(cx + 16, cy + 124, l, MUTED, middle_left);
            snprintf(l, sizeof l, "%.6f, %.6f%s", pn->lat, pn->lon, node_kind(*pn) >= 3 ? "  (its GPS)" : "");
            text(cx + 16, cy + 148, l, MUTED, middle_left);
            int bx = cx + 16, w2;
            chip(bx, cy + 176, "SETTINGS", ACCENT, false, &w2, H_POPSET);
            bx += w2 + 10;
            chip(bx, cy + 176, A.mapMove ? "TAP THE MAP..." : "MOVE", AMBER, A.mapMove, &w2, net_online() ? H_POPMOVE : -1);
            if (node_kind(*pn) >= 3) {   // a GPS node: its own track on or off
                bx += w2 + 10;
                bool tk = node_track_on(pn->id);
                chip(bx, cy + 176, tk ? "TRACK ON" : "TRACK OFF", rgb(0x39ff88), tk, &w2, H_POPTRACK);
            }
        }
    }
    C.clearClipRect();
    uint32_t q5 = micros();

    // Layer dropdown + status, top left: tap to pick the base map.
    int ix = PAD, iy = MY + 12, iw = 360, ih = 64;
    hit(ix, iy, iw, ih, H_LAYERMENU);
    fast_round(ix, iy, iw, ih, 8, PANEL);
    if (A.layerMenu) glow_rect(ix, iy, iw, ih, 8, ACCENT);
    else C.drawRoundRect(ix, iy, iw, ih, 8, LINE2);
    font(F_MD);
    char b[64];
    snprintf(b, sizeof b, "%s", LAYER_NAMES[A.mapLayer]);
    glow(ix + 14, iy + 20, b, ACCENT, middle_left);
    int ax = ix + 14 + C.textWidth(b) + 14, ay = iy + 20;   // the dropdown arrow
    if (A.layerMenu) C.fillTriangle(ax, ay + 4, ax + 12, ay + 4, ax + 6, ay - 4, ACCENT);
    else C.fillTriangle(ax, ay - 4, ax + 12, ay - 4, ax + 6, ay + 4, ACCENT);
    font(F_SM);
    char zt[12];
    snprintf(zt, sizeof zt, "zoom %d", A.mapZoom);
    text(ix + iw - 14, iy + 20, zt, MUTED, middle_right);
    font(F_SM);
    int ld = live_drones();
    if (!online) snprintf(b, sizeof b, "map tiles come from the dashboard (Wi-Fi)");
    else if (missing) snprintf(b, sizeof b, "loading %d tiles   %d drone%s live", missing, ld, ld == 1 ? "" : "s");
    else snprintf(b, sizeof b, "%u tiles   %d drone%s live   %s", (unsigned)maptiles_cached(), ld, ld == 1 ? "" : "s", gps_state());
    text(ix + 14, iy + 46, b, online ? MUTED : AMBER, middle_left);
    if (A.layerMenu) {
        int ly = iy + ih + 6, lh = 52;
        fast_round(ix, ly, iw, NLAYERS * lh + 8, 8, PANEL);
        glow_rect(ix, ly, iw, NLAYERS * lh + 8, 8, dim(ACCENT, 0.8f));
        for (int i = 0; i < NLAYERS; i++) {
            int yy = ly + 4 + i * lh;
            hit(ix, yy, iw, lh, H_LAYERPICK, i);
            bool cur = i == A.mapLayer;
            if (cur) fast_fill(ix + 6, yy + 4, iw - 12, lh - 8, dim(ACCENT, 0.18f));
            font(F_MD);
            text(ix + 20, yy + lh / 2, LAYER_NAMES[i], cur ? ACCENT : TEXT, middle_left);
        }
    }

    // Controls, right edge.
    struct { const char *t; uint8_t act; bool on; } btn[] = {
        {"+", H_ZOOMIN, false}, {"-", H_ZOOMOUT, false},
        {"FIT", H_FIT, false}, {"FOLLOW", H_FOLLOW, A.mapFollow}, {"TRI", H_TRI, A.triOn},
        {"TRACKS", H_TRACKS, A.droneTracks}, {"PILOT", H_PILOT, A.pilotOn},
        {A.wdScope == 2 ? "WD ALL" : A.wdScope == 1 ? "WD NOW" : "WD OFF", H_WD, A.wdScope > 0},
        {"ME", H_ME, A.gpsFollow}};
    int bw = 120, bh = 52, bx = W - PAD - bw, by = MY + 12;
    for (auto &k : btn) {
        hit(bx - 6, by - 4, bw + 12, bh + 8, k.act);
        C.fillRoundRect(bx, by, bw, bh, 10, k.on ? dim(PINK, 0.25f) : dim(PANEL, 0.95f));
        if (k.on) glow_rect(bx, by, bw, bh, 10, PINK);
        else C.drawRoundRect(bx, by, bw, bh, 10, LINE2);
        font(k.t[1] ? F_MD : F_LG);
        glow(bx + bw / 2, by + bh / 2, k.t, k.on ? PINK : ACCENT, middle_center);
        by += bh + 10;
    }
    uint32_t q6 = micros();
    (void)q0, (void)q1; ui_prof[7] = q2 - q1, ui_prof[8] = q3 - q2, ui_prof[9] = q4 - q3, ui_prof[10] = q5 - q4, ui_prof[11] = q6 - q5;
}

void map_zoom(int dz) { map_zoom_at(dz, MX + MW / 2, MY + MH / 2); }

// Keep the map ready from boot, whatever tab is open: request this view's tiles plus one
// zoom level out (fallback while zooming) and in around the centre (the next zoom). The
// loaders fetch newest-first, so this view's own tiles are requested last.
void map_warm() {
    if (!net_online()) return;
    if (!A.mapInit) {
        map_fit();
        if (!A.mapInit) return;
        A.mapZoomF = A.mapZoom;
    }
    auto want = [&](int z, double fracW, double fracH) {
        if (z > LAYER_MAX[A.mapLayer]) z = LAYER_MAX[A.mapLayer];   // overzoom draws the last real tiles
        if (z < ZMIN) return;
        int n = 1 << z;
        double cx = lon2x(A.mapLon, z), cy = lat2y(A.mapLat, z);
        double hw = MW * fracW / 2, hh = MH * fracH / 2;
        for (int ty = (int)floor((cy - hh) / TILE); ty <= (int)floor((cy + hh) / TILE); ty++) {
            if (ty < 0 || ty >= n) continue;
            for (int tx = (int)floor((cx - hw) / TILE); tx <= (int)floor((cx + hw) / TILE); tx++)
                maptile(A.mapLayer, z, ((tx % n) + n) % n, ty);
        }
    };
    want(A.mapZoom + 1, 0.6, 0.6);
    want(A.mapZoom - 1, 1.0, 1.0);
    want(A.mapZoom - 2, 1.0, 1.0);
    want(A.mapZoom, 1.0, 1.0);
}

// ------------------------------------------------------------------ DRONES

const char *fmt_m(char *b, size_t n, bool ok, float v, const char *unit) {
    if (ok) snprintf(b, n, "%d %s", (int)lroundf(v), unit);
    else snprintf(b, n, "-");
    return b;
}

void draw_drones() {
    int y = CY;
    int lw = 500, rows = 7, rh = 78;
    panel(PAD, y, lw, CB - y, "DRONES  REMOTE ID");
    A.sel[V_DRONES] = constrain(A.sel[V_DRONES], 0, max(0, nDrone - 1));
    A.top[V_DRONES] = constrain(A.top[V_DRONES], 0, max(0, nDrone - rows));
    if (A.sel[V_DRONES] < A.top[V_DRONES]) A.top[V_DRONES] = A.sel[V_DRONES];
    if (A.sel[V_DRONES] >= A.top[V_DRONES] + rows) A.top[V_DRONES] = A.sel[V_DRONES] - rows + 1;
    int ry = y + 34;
    scroll_area(PAD, ry, lw, rows * rh, V_DRONES, rh, rows);
    for (int k = 0; k < rows; k++, ry += rh) {
        int i = A.top[V_DRONES] + k;
        if (i >= nDrone) break;
        const Drone &d = M->drones[droneIdx[i]];
        bool live = drone_live(d), sel = i == A.sel[V_DRONES];
        uint16_t c = drone_color(d);
        hit(PAD + 6, ry, lw - 12, rh - 6, H_ROW, i);
        if (sel) {
            C.fillRoundRect(PAD + 8, ry, lw - 16, rh - 8, 6, dim(c, 0.12f));
            glow_rect(PAD + 8, ry, lw - 16, rh - 8, 6, dim(c, 0.8f));
        }
        C.fillRoundRect(PAD + 14, ry + 8, 5, rh - 24, 2, live ? c : dim(c, 0.35f));
        drone_glyph(PAD + 50, ry + (rh - 8) / 2, d.hdg, d.hasHdg, c, live, 0.75f);
        font(F_LG);
        text(PAD + 84, ry + 8, fit_name(drone_name(d), lw - 220), live ? TEXT : MUTED);
        font(F_SM);
        char l[64];
        snprintf(l, sizeof l, "%s  via %s", d.ua[0] ? d.ua : "type not sent", node_name(d.closest));
        text(PAD + 84, ry + 42, fit(l, lw - 220), FAINT);
        const char *st = live ? (d.status[0] ? d.status : "LIVE") : age_str(d.last);
        font(F_SM);
        int sw = C.textWidth(st) + 18;
        int sx = PAD + lw - 22 - sw;
        if (live) C.fillRoundRect(sx, ry + 10, sw, 22, 11, c), text(sx + sw / 2, ry + 21, st, BG, middle_center);
        else C.drawRoundRect(sx, ry + 10, sw, 22, 11, LINE2), text(sx + sw / 2, ry + 21, st, MUTED, middle_center);
        char h[24];
        if (d.hasHagl) snprintf(h, sizeof h, "%d m", (int)d.hagl);
        else if (d.hasAlt) snprintf(h, sizeof h, "%d m MSL", (int)d.alt);
        else snprintf(h, sizeof h, "-");
        font(F_MD);
        text(PAD + lw - 22, ry + 50, h, live ? SKY : FAINT, middle_right);
    }
    if (!nDrone) {
        font(F_MD);
        text(PAD + lw / 2, (y + CB) / 2 - 14, "no drones yet", FAINT, middle_center);
        font(F_SM);
        text(PAD + lw / 2, (y + CB) / 2 + 14, "every node decodes Remote ID (WiFi + Bluetooth)", FAINT, middle_center);
    }

    // Selected drone.
    int x = PAD + lw + 12, w = W - PAD - x;
    panel(x, y, w, CB - y, "SELECTED");
    if (!nDrone) return;
    const Drone &d = M->drones[droneIdx[A.sel[V_DRONES]]];
    bool live = drone_live(d);
    uint16_t c = drone_color(d);
    font(F_BRAND);
    glow(x + 18, y + 40, fit_name(drone_name(d), w - 300), c, middle_left);
    font(F_SM);
    char sub[96];
    snprintf(sub, sizeof sub, "%s   %s   %s", d.mac, d.src[0] ? d.src : "", live ? (d.status[0] ? d.status : "live") : "not heard lately");
    text(x + 18, y + 66, sub, MUTED, middle_left);
    // SHOW ON MAP, top right.
    font(F_SM);
    int cw;
    int mx = x + w - 18 - (C.textWidth("SHOW ON MAP") + 28);
    chip(mx, y + 26, "SHOW ON MAP", ACCENT, false, &cw, H_DRONEMAP);
    // Its name: a full-width field of its own (tap or E, Enter saves to the dashboard and
    // every screen), laid out like the node editor's.
    {
        int ny = y + 84, nh = 40;
        font(F_SM);
        text(x + 18, ny + nh / 2, "NAME", dim(ACCENT, 0.9f), middle_left);
        int fx = x + 18 + 64, fw = w - 36 - 64;
        hit(fx, ny, fw, nh, net_online() ? H_DRALIAS : H_NONE);
        fast_round(fx, ny, fw, nh, 8, PANEL2);
        if (A.editDrone) glow_rect(fx, ny, fw, nh, 8, PINK);
        else C.drawRoundRect(fx, ny, fw, nh, 8, d.alias[0] ? dim(c, 0.6f) : LINE2);
        char ab[48];
        if (A.editDrone) snprintf(ab, sizeof ab, "%s%s", A.editBuf, (millis() / 500) % 2 ? "_" : "");
        else snprintf(ab, sizeof ab, "%s", d.alias[0] ? d.alias : (net_online() ? "tap here or press E to name it" : "naming needs the dashboard"));
        font(F_MD);
        text(fx + 14, ny + nh / 2, fit(ab, fw - 28), A.editDrone ? PINK : d.alias[0] ? TEXT : FAINT, middle_left);
        if (A.editDrone) {
            font(F_SM);
            text(fx + fw - 12, ny + nh / 2, "ENTER save   ESC cancel", MUTED, middle_right);
        }
    }
    // Tags.
    int tx = x + 18, ty = y + 134;
    for (int t = 0; t < 5; t++) {
        bool on = !strcmp(d.tag, DRONE_TAGS[t]);
        int tw2;
        chip(tx, ty, DRONE_TAGS[t], tag_color(DRONE_TAGS[t]), on, &tw2, net_online() ? H_DRONETAG : -1, t);
        tx += tw2 + 8;
    }
    // Numbers.
    char b[9][24];
    const char *k[9] = {"HEIGHT AGL", "ALTITUDE MSL", "SPEED", "CLIMB", "HEADING", "PILOT DISTANCE", "SIGNAL", "HEARD BY", "REPORTS"};
    fmt_m(b[0], 24, d.hasHagl, d.hagl, "m");
    fmt_m(b[1], 24, d.hasAlt, d.alt, "m");
    if (d.hasSpd) snprintf(b[2], 24, "%.1f m/s", d.spd); else strcpy(b[2], "-");
    snprintf(b[3], 24, "%+.1f m/s", d.vspd);
    if (d.hasHdg) snprintf(b[4], 24, "%d deg", (int)d.hdg); else strcpy(b[4], "-");
    if (d.pilotDist >= 0) snprintf(b[5], 24, "%ld m", (long)d.pilotDist); else strcpy(b[5], d.hasPilot ? "located" : "unknown");
    snprintf(b[6], 24, "%d dBm", d.rssi);
    strlcpy(b[7], node_name(d.closest), 24);
    snprintf(b[8], 24, "%u", (unsigned)d.reports);
    int gx = x + 18, gy = y + 180, gw = (w - 36) / 3;
    for (int i = 0; i < 9; i++) {
        int cx2 = gx + (i % 3) * gw, cy2 = gy + (i / 3) * 56;
        font(F_SM);
        text(cx2, cy2, k[i], FAINT);
        font(F_LG);
        text(cx2, cy2 + 18, fit(b[i], gw - 12), i < 2 ? SKY : TEXT);
    }
    // Remote ID fields + a radar of its track around the pilot.
    int fy = gy + 3 * 56 + 8;
    // Who made it: the FAA registry's make and model for its serial, else the radio's
    // maker from its MAC (DJI's CTA-2063 serials start with manufacturer code 1581).
    char maker[64];
    if (d.maker[0]) snprintf(maker, sizeof maker, "%s (FAA)", d.maker);
    else if (!strncmp(d.id, "1581", 4)) snprintf(maker, sizeof maker, "DJI (serial code 1581)");
    else if (d.vendor[0]) snprintf(maker, sizeof maker, "%s (radio)", d.vendor);
    else maker[0] = 0;
    struct { const char *k; const char *v; } f[] = {{"MAKER", maker}, {"SERIAL / ID", d.id}, {"OPERATOR ID", d.op},
                                                    {"DESCRIPTION", d.desc}, {"AIRCRAFT", d.ua}};
    // Values run up to the radar box and wrap onto a second line rather than being cut.
    int radarL = x + w - 18 - min(CB - (gy + 3 * 56) - 20, w / 2 - 40);
    int vw = radarL - (gx + 150) - 12;
    for (auto &e : f) {
        font(F_SM);
        text(gx, fy, e.k, FAINT);
        font(F_MONO_S);
        if (!e.v[0]) {
            text(gx + 150, fy - 1, "-", FAINT);
            fy += 26;
            continue;
        }
        String v = e.v;
        if ((int)C.textWidth(v.c_str()) <= vw) {
            text(gx + 150, fy - 1, v.c_str(), AMBER);
            fy += 26;
        } else {   // split at the last space that fits, else hard at the width
            int cut = v.length();
            while (cut > 1 && (int)C.textWidth(v.substring(0, cut).c_str()) > vw) cut--;
            int sp = v.lastIndexOf(' ', cut);
            if (sp > cut / 2) cut = sp;
            text(gx + 150, fy - 1, v.substring(0, cut).c_str(), AMBER);
            String rest = v.substring(cut);
            rest.trim();
            text(gx + 150, fy + 18, fit(rest.c_str(), vw), AMBER);
            fy += 44;
        }
    }
    char ft[64];
    snprintf(ft, sizeof ft, "first %s ago   last %s ago", age_str(d.first), age_str(d.last));
    font(F_SM);
    text(gx, fy + 4, ft, FAINT);
    // Radar box.
    int rs = min(CB - (gy + 3 * 56) - 20, w / 2 - 40), rx = x + w - 18 - rs, ry2 = CB - 14 - rs;
    C.fillRoundRect(rx, ry2, rs, rs, 8, PANEL2);
    C.drawRoundRect(rx, ry2, rs, rs, 8, LINE2);
    int ccx = rx + rs / 2, ccy = ry2 + rs / 2;
    for (int r = rs / 6; r < rs / 2; r += rs / 6) C.drawCircle(ccx, ccy, r, LINE);
    double ola = d.hasPilot ? d.plat : d.lat, olo = d.hasPilot ? d.plon : d.lon;
    double span = 30;   // metres from centre to edge, at least
    for (int p = 0; p < d.ntrack; p++) {
        double dy = (d.track[p][0] - ola) * 111320.0, dx = (d.track[p][1] - olo) * 111320.0 * cos(ola * M_PI / 180);
        span = max(span, max(fabs(dx), fabs(dy)) * 1.15);
    }
    auto px = [&](double lo) { return (int)(ccx + (lo - olo) * 111320.0 * cos(ola * M_PI / 180) / span * (rs / 2 - 8)); };
    auto py = [&](double la) { return (int)(ccy - (la - ola) * 111320.0 / span * (rs / 2 - 8)); };
    for (int p = 1; p < d.ntrack; p++)
        C.drawLine(px(d.track[p - 1][1]), py(d.track[p - 1][0]), px(d.track[p][1]), py(d.track[p][0]), dim(c, 0.4f + 0.6f * p / d.ntrack));
    if (d.hasPilot) pilot_glyph(ccx, ccy, c);
    if (d.hasPos) {
        if (d.hasPilot) dashed(ccx, ccy, px(d.lon), py(d.lat), dim(c, 0.7f));
        drone_glyph(px(d.lon), py(d.lat), d.hdg, d.hasHdg, c, live, 0.6f);
    }
    font(F_SM);
    char sc[24];
    snprintf(sc, sizeof sc, "%d m", (int)span);
    text(rx + rs - 8, ry2 + 8, sc, FAINT, top_right);
    text(rx + 8, ry2 + 8, d.hasPilot ? "around the pilot" : "track", FAINT);
}

// ------------------------------------------------------------------ device detail

// Labels for the raw over-the-air fields (USB mode, no dashboard to name them).
const char *info_label(const char *k) {
    static const char *map[][2] = {{"name", "Broadcast name"}, {"rnd", "Random address"}, {"conn", "Connectable"},
                                   {"tx", "TX power"}, {"app", "Appearance"}, {"adv", "Advert type"},
                                   {"uuids", "Services"}, {"uuid128", "128-bit service"}, {"cid", "Company ID"},
                                   {"mfr", "Manufacturer data"}, {"sdata", "Service data"}, {"ssid", "SSID"},
                                   {"ch", "Channel"}, {"sec", "Security"}, {"bssid", "Joins (BSSID)"},
                                   {"frame", "Frame"}, {"phy", "Wi-Fi"}, {"wps", "WPS"}, {"hidden", "Hidden SSID"},
                                   {"rate", "Max legacy rate"}, {"vie", "Vendor IEs"}, {"cc", "Country"}};
    for (auto &m : map)
        if (!strcmp(m[0], k)) return m[1];
    return k;
}

void open_detail(const char *mac) {
    if (!mac || !*mac) return;
    DevDetail &dd = M->detail;
    memset(&dd, 0, sizeof dd);
    strlcpy(dd.mac, mac, sizeof dd.mac);
    dd.loading = net_online();
    A.detail = true;
    A.editAlias = A.newGroup = false;
}

const Target *target_by_mac(const char *mac) {
    for (int i = 0; i < M->ntargets; i++)
        if (!strcasecmp(M->targets[i].mac, mac)) return &M->targets[i];
    return nullptr;
}

void draw_detail() {
    const DevDetail &dd = M->detail;
    const Target *t = target_by_mac(dd.mac);
    int x = PAD, y = CY, w = W - 2 * PAD, h = CB - CY;
    fast_fill(0, TOP, W, H - TOP - BOT, dim(BG, 0.6f));
    hit(0, TOP, W, H - TOP - BOT, H_NONE);   // swallow taps under the page
    C.fillRoundRect(x, y, w, h, 10, PANEL);
    glow_rect(x, y, w, h, 10, dim(ACCENT, 0.8f));
    const char *alias = dd.valid ? dd.alias : (t ? t->alias : "");
    const char *name = dd.valid && dd.name[0] ? dd.name : (t ? t->name : "");
    // No alias or broadcast name: the vendor, from the live target or else the feed.
    const char *vend = t && t->descr[0] ? t->descr : "";
    for (int i = 0; !vend[0] && i < M->ndets; i++)
        if (!strcasecmp(det_at(i)->mac, dd.mac) && det_at(i)->descr[0]) vend = det_at(i)->descr;
    if (!vend[0]) vend = sd_vendor(dd.mac);
    const char *title = alias[0] ? alias : name[0] ? name : vend[0] ? vend : dd.mac;
    font(F_BRAND);
    glow(x + 20, y + 30, fit(title, w - 360), alias[0] ? PINK : ACCENT, middle_left);
    font(F_MD);
    char sub[120];
    snprintf(sub, sizeof sub, "%s%s%s%s%s", dd.mac, name[0] && strcmp(title, name) ? "   \"" : "", name[0] && strcmp(title, name) ? name : "",
             name[0] && strcmp(title, name) ? "\"" : "", t && t->descr[0] ? "" : "");
    text(x + 20, y + 62, sub, SOFT, middle_left);
    if (vend[0] && strcmp(title, vend)) {
        font(F_MD);
        int sw = C.textWidth(sub);
        font(F_SM);
        text(x + 20 + sw + 24, y + 62, fit(vend, 300), MUTED, middle_left);
    }
    font(F_SM);
    int cw;
    chip(x + w - 20 - (C.textWidth("CLOSE  ESC") + 28), y + 16, "CLOSE  ESC", ACCENT, false, &cw, H_DCLOSE);
    {   // proximity mode for this device: the keyboard LED (and a beep) by its signal
        bool pon = !strcasecmp(A.prox, dd.mac);
        const char *pl = pon ? "PROXIMITY ON" : "PROXIMITY";
        int pw = C.textWidth(pl) + 28;
        chip(x + w - 20 - cw - 12 - pw, y + 16, pl, rgb(0x39ff88), pon, nullptr, H_DPROX);
    }

    // Left: everything it said over the air.
    int lx = x + 20, ly = y + 92, lw = 720;
    font(F_SM);
    text(lx, ly, "OVER THE AIR", dim(ACCENT, 0.9f));
    C.drawFastHLine(lx, ly + 18, lw, LINE2);
    int ry = ly + 28, rowH = 30;
    int shown = 0;
    if (dd.valid) {
        for (int i = 0; i < dd.nrows && ry + rowH <= y + h - 10; i++, ry += rowH, shown++) {
            font(F_SM);
            text(lx, ry + 8, dd.rows[i][0], MUTED);
            font(F_MD);
            bool hl = !strcmp(dd.rows[i][0], "Broadcast name") || !strcmp(dd.rows[i][0], "SSID") || !strcmp(dd.rows[i][0], "Looks for");
            text(lx + 190, ry + 4, fit(dd.rows[i][1], lw - 190), hl ? SKY : TEXT);
        }
    } else {
        if (const char *ov = sd_vendor(dd.mac); ov[0]) {   // offline: the card's OUI table
            font(F_SM);
            text(lx, ry + 8, "Vendor (OUI)", MUTED);
            font(F_MD);
            text(lx + 190, ry + 4, fit(ov, lw - 190), TEXT);
            ry += rowH, shown++;
        } else if (strlen(dd.mac) > 1 && strtoul(dd.mac, nullptr, 16) & 0x02) {
            font(F_SM);
            text(lx, ry + 8, "Address", MUTED);
            font(F_MD);
            text(lx + 190, ry + 4, "random (changes; no vendor in it)", TEXT);
            ry += rowH, shown++;
        }
    }
    if (!dd.valid && t && t->info[0]) {
        JsonDocument doc;
        if (!deserializeJson(doc, t->info)) {
            for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
                if (ry + rowH > y + h - 10) break;
                char v[120];
                if (kv.value().is<JsonArrayConst>()) {
                    v[0] = 0;
                    bool uu = !strcmp(kv.key().c_str(), "uuids");
                    for (JsonVariantConst e : kv.value().as<JsonArrayConst>()) {
                        char one[56], nm[40];
                        if (e.is<JsonObjectConst>()) snprintf(one, sizeof one, "%s=%s", (const char *)(e["uuid"] | ""), (const char *)(e["data"] | ""));
                        else if (uu && e.as<const char *>() && sd_lookup(LK_UUID, strtoul(e.as<const char *>(), nullptr, 16), nm, sizeof nm))
                            snprintf(one, sizeof one, "%s", nm);   // a 16-bit service by its name
                        else snprintf(one, sizeof one, "%s", e.as<const char *>() ? e.as<const char *>() : "");
                        if (v[0]) strlcat(v, ", ", sizeof v);
                        strlcat(v, one, sizeof v);
                    }
                } else if (kv.value().is<const char *>()) {
                    strlcpy(v, kv.value().as<const char *>(), sizeof v);
                } else {
                    snprintf(v, sizeof v, "%g", kv.value().as<double>());
                }
                intel_named(kv.key().c_str(), v, sizeof v);
                font(F_SM);
                text(lx, ry + 8, info_label(kv.key().c_str()), MUTED);
                font(F_MD);
                text(lx + 190, ry + 4, fit(v, lw - 190), TEXT);
                ry += rowH, shown++;
            }
        }
    }
    if (!shown) {
        font(F_MD);
        text(lx, ry + 10, dd.loading ? "loading from the dashboard..." : "nothing beyond the MAC yet (nodes on the device-intel firmware report more)", FAINT);
    }

    // Right: alias, group, devices that travel with it.
    int rx = x + 20 + lw + 30, rw = x + w - 20 - rx, yy = ly;
    bool canEdit = net_online();
    font(F_SM);
    text(rx, yy, "ALIAS", dim(ACCENT, 0.9f));
    yy += 22;
    hit(rx, yy - 4, rw, 48, canEdit ? H_DALIAS : H_NONE);
    C.fillRoundRect(rx, yy, rw, 40, 8, PANEL2);
    if (A.editAlias) glow_rect(rx, yy, rw, 40, 8, PINK);
    else C.drawRoundRect(rx, yy, rw, 40, 8, LINE2);
    font(F_MD);
    if (A.editAlias) {
        char b[48];
        snprintf(b, sizeof b, "%s%s", A.editBuf, (millis() / 500) % 2 ? "_" : "");
        text(rx + 12, yy + 20, b, TEXT, middle_left);
    } else {
        text(rx + 12, yy + 20, alias[0] ? alias : (canEdit ? "tap or E to name it" : "naming needs the dashboard"), alias[0] ? PINK : FAINT, middle_left);
    }
    yy += 60;
    font(F_SM);
    text(rx, yy, "GROUP", dim(ACCENT, 0.9f));
    yy += 22;
    int gx = rx;
    const char *cur = dd.valid ? dd.group : (t ? t->group : "");
    if (A.newGroup) {
        C.fillRoundRect(rx, yy, rw, 40, 8, PANEL2);
        glow_rect(rx, yy, rw, 40, 8, PINK);
        char b[48];
        snprintf(b, sizeof b, "new group: %s%s", A.editBuf, (millis() / 500) % 2 ? "_" : "");
        font(F_MD);
        text(rx + 12, yy + 20, b, TEXT, middle_left);
        yy += 52;
    } else {
        int cwid;
        chip(gx, yy, "NONE", AMBER, !cur[0], &cwid, canEdit ? H_DUNGROUP : -1);
        gx += cwid + 8;
        for (int i = 0; i < dd.ngroups; i++) {
            font(F_SM);
            int need = C.textWidth(dd.groups[i].name) + 36;
            if (gx + need > rx + rw) { gx = rx; yy += 40; }
            if (yy > y + h - 220) break;
            chip(gx, yy, dd.groups[i].name, PURPLE, !strcmp(cur, dd.groups[i].name), &cwid, canEdit ? H_DGROUP : -1, i);
            gx += cwid + 8;
        }
        font(F_SM);
        if (gx + 110 > rx + rw) { gx = rx; yy += 40; }
        if (canEdit) chip(gx, yy, "+ NEW", ACCENT, false, &cwid, H_DNEWGROUP);
        yy += 52;
    }
    font(F_SM);
    text(rx, yy, "TRAVELS WITH", dim(ACCENT, 0.9f));
    text(rx + rw, yy, "same node, within a minute", FAINT, top_right);
    yy += 24;
    if (!dd.valid || !dd.ncomp) {
        font(F_MD);
        text(rx, yy + 6, dd.valid ? "nothing turns up with it yet" : (canEdit ? "-" : "needs the dashboard"), FAINT);
    }
    for (int i = 0; dd.valid && i < dd.ncomp && yy + 44 <= y + h - 10; i++, yy += 46) {
        const auto &c = dd.comp[i];
        hit(rx, yy, rw, 42, H_DCOMP, i);
        C.fillRoundRect(rx, yy, rw, 40, 6, PANEL2);
        font(F_MD);
        text(rx + 10, yy + 12, fit_name(c.label[0] ? c.label : c.mac, rw - 170), TEXT, middle_left);
        font(F_SM);
        text(rx + 10, yy + 30, c.mac, FAINT, middle_left);
        char tg[24];
        snprintf(tg, sizeof tg, "%u of %ux", c.together, c.occasions);
        text(rx + rw - 10, yy + 12, tg, SKY, middle_right);
        if (c.group[0]) text(rx + rw - 10, yy + 30, fit(c.group, 150), PURPLE, middle_right);
        int bwid = c.occasions ? (int)(c.together * 100 / c.occasions) : 0;
        C.fillRect(rx + rw - 120, yy + 37, min(100, bwid), 2, PINK);
    }
}

// ------------------------------------------------------------------ node settings
// Long press on a node (Overview / Fleet): name, colour, buzzer + LED, identify, status,
// reboot. Everything goes through the dashboard, which pushes it to the node.

const Node *edit_node() {
    for (int i = 0; i < M->nnodes; i++)
        if (!strcmp(M->nodes[i].id, A.nodeEdit)) return &M->nodes[i];
    return nullptr;
}

constexpr int NODE_HUES[] = {0, 25, 45, 90, 140, 175, 195, 220, 260, 285, 310, 335};

void node_cmd(const char *id, const char *op, const char *args) {
    char path[48], body[120];
    snprintf(path, sizeof path, "/api/nodes/%s/cmd", id);
    if (args) snprintf(body, sizeof body, "{\"op\":\"%s\",\"args\":%s}", op, args);
    else snprintf(body, sizeof body, "{\"op\":\"%s\"}", op);
    net_post(path, body);
}

void node_set(const char *id, const char *json) {
    char path[48];
    snprintf(path, sizeof path, "/api/nodes/%s", id);
    net_post(path, json);
}

// A wardriven network / device: everything the dashboard decoded (long-press its row).
void draw_wd_info() {
    int x = PAD + 120, y = CY + 10, w = W - 2 * PAD - 240, h = CB - CY - 20;
    fast_fill(0, TOP, W, H - TOP - BOT, dim(BG, 0.6f));
    hit(0, TOP, W, H - TOP - BOT, H_WDCLOSE);   // a tap outside closes it
    hit(x, y, w, h, H_NONE);
    fast_round(x, y, w, h, 10, PANEL);
    uint16_t c = A.wdOpenKind == 2 ? rgb(0xff2bd6) : SKY;
    glow_rect(x, y, w, h, 10, c);
    const WdInfo &wi = M->wdInfo;
    bool here = wi.loaded && !strcmp(wi.mac, A.wdOpen);
    const char *title = here ? wi.title : "";
    if (!here) {
        for (int i = 0, n = wd_count(); i < n; i++) {
            const WdDev &d = wd_at(i);
            char m[18];
            snprintf(m, sizeof m, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
            if (d.kind == A.wdOpenKind && !strcmp(m, A.wdOpen)) { title = d.name[0] ? d.name : ""; break; }
        }
    }
    font(F_BRAND);
    glow(x + 24, y + 32, fit_name(title[0] ? title : A.wdOpen, w - 260), c, middle_left);
    font(F_SM);
    text(x + 24, y + 62, A.wdOpen, MUTED, middle_left);
    int cw;
    chip(x + w - 24 - (C.textWidth("CLOSE  ESC") + 28), y + 18, "CLOSE  ESC", ACCENT, false, &cw, H_WDCLOSE);
    int yy = y + 96;
    if (!here) {
        font(F_MD);
        text(x + w / 2, y + h / 2, wi.failed && !strcmp(wi.mac, A.wdOpen) ? "the dashboard doesn't know it yet"
                                   : net_online() ? "loading..." : "full details come from the dashboard (Wi-Fi)",
             FAINT, middle_center);
        return;
    }
    for (int i = 0; i < wi.n && yy + 30 < y + h; i++) {
        font(F_SM);
        text(x + 24, yy + 10, wi.k[i], dim(ACCENT, 0.9f), middle_left);
        font(F_MD);
        const char *v = wi.v[i];
        int vw = w - 260;
        if (C.textWidth(v) <= vw) {
            text(x + 210, yy + 10, v, TEXT, middle_left);
            yy += 36;
        } else {   // long values wrap onto a second line instead of being cut
            String a(v);
            int cut = a.length();
            while (cut > 0 && (C.textWidth(a.substring(0, cut).c_str()) > vw || (a[cut] != ' ' && a[cut] != ',' && a[cut]))) cut--;
            if (cut <= 0) cut = a.length() / 2;
            String l2 = a.substring(cut);
            l2.trim();
            text(x + 210, yy + 10, a.substring(0, cut).c_str(), TEXT, middle_left);
            text(x + 210, yy + 38, fit(l2.c_str(), vw), TEXT, middle_left);
            yy += 64;
        }
    }
}

void open_wd_dev(int i) {
    if (i < 0 || i >= wd_count()) return;
    const WdDev &d = wd_at(i);
    snprintf(A.wdOpen, sizeof A.wdOpen, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
    A.wdOpenKind = d.kind;
}

// A Live row, either kind: its full info.
void open_live_row(int li) {
    if (li < 0 || li >= nLive) return;
    int e = liveIdx[li];
    if (e < 0) open_wd_dev(-e - 1);
    else open_detail(det_at(e)->mac);
}

void open_wd_info(int li) {
    if (!wdIdx || li < 0 || li >= wdN) return;
    const WdDev &d = wd_at(wdIdx[li]);
    snprintf(A.wdOpen, sizeof A.wdOpen, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
    A.wdOpenKind = d.kind;
}

void draw_node_editor() {
    const Node *n = edit_node();
    if (!n) { A.nodeEdit[0] = 0; return; }
    int x = PAD + 120, y = CY + 10, w = W - 2 * PAD - 240, h = CB - CY - 20;
    fast_fill(0, TOP, W, H - TOP - BOT, dim(BG, 0.6f));
    hit(0, TOP, W, H - TOP - BOT, H_NONE);
    uint16_t nc = node_color565(n->id);
    fast_round(x, y, w, h, 10, PANEL);
    glow_rect(x, y, w, h, 10, nc);
    font(F_BRAND);
    glow(x + 24, y + 32, fit_name(n->alias[0] ? n->alias : n->id, w - 260), nc, middle_left);
    font(F_SM);
    size_t fl = strlen(n->fw);
    bool sky = fl && n->fw[fl - 1] == 'S';
    char sub[120];
    snprintf(sub, sizeof sub, "%s   %s%s   %s   fw %s   up %s", n->id, n->isHome ? "HOME, " : "", KIND_NAME[node_kind(*n)],
             n->online ? "online" : "OFFLINE", n->fw[0] ? n->fw : "?", n->up ? dur_str(n->up) : "-");
    text(x + 24, y + 62, sub, MUTED, middle_left);
    int cw;
    chip(x + w - 24 - (C.textWidth("CLOSE  ESC") + 28), y + 18, "CLOSE  ESC", ACCENT, false, &cw, H_NCLOSE);
    bool can = net_online();
    int lx = x + 24, yy = y + 92, fwid = w - 48;
    // Name
    font(F_SM);
    text(lx, yy, "NAME", dim(ACCENT, 0.9f));
    yy += 20;
    hit(lx, yy - 4, fwid, 46, can ? H_NALIAS : H_NONE);
    fast_round(lx, yy, fwid, 40, 8, PANEL2);
    if (A.editNode) glow_rect(lx, yy, fwid, 40, 8, PINK);
    else C.drawRoundRect(lx, yy, fwid, 40, 8, LINE2);
    font(F_MD);
    char nb[48];
    if (A.editNode) snprintf(nb, sizeof nb, "%s%s", A.editBuf, (millis() / 500) % 2 ? "_" : "");
    else snprintf(nb, sizeof nb, "%s", n->alias[0] ? n->alias : (can ? "tap or E to name it" : "needs the dashboard"));
    text(lx + 12, yy + 20, nb, A.editNode || n->alias[0] ? TEXT : FAINT, middle_left);
    yy += 58;
    // Colour
    font(F_SM);
    text(lx, yy, "COLOUR", dim(ACCENT, 0.9f));
    yy += 22;
    int sw = 44, gap = 10, sx = lx;
    for (int i = 0; i < 12; i++, sx += sw + gap) {
        int hue = NODE_HUES[i];
        float hp = hue / 60.0f;
        float xx = 1 - fabsf(fmodf(hp, 2) - 1);
        float r = 0, g = 0, b = 0;
        if (hp < 1) r = 1, g = xx; else if (hp < 2) r = xx, g = 1; else if (hp < 3) g = 1, b = xx;
        else if (hp < 4) g = xx, b = 1; else if (hp < 5) r = xx, b = 1; else r = 1, b = xx;
        uint16_t c = lgfx::color565(60 + r * 195, 60 + g * 195, 60 + b * 195);
        hit(sx - 3, yy - 3, sw + 6, sw + 6, can ? H_NCOLOR : H_NONE, hue);
        C.fillRoundRect(sx, yy, sw, sw, 10, c);
        if (n->hue == hue) glow_rect(sx - 3, yy - 3, sw + 6, sw + 6, 12, TEXT);
    }
    chip(sx + 4, yy + 7, "DEFAULT", MUTED, n->hue < 0, &cw, can ? H_NCOLOR : -1, -1);
    yy += sw + 22;
    // Buzzer / LED
    font(F_SM);
    text(lx, yy, "ON THE NODE", dim(ACCENT, 0.9f));
    yy += 22;
    int bx = lx;
    char t1[24], t2[24];
    snprintf(t1, sizeof t1, "BUZZER %s", n->buzzer < 0 ? "?" : n->buzzer ? "ON" : "OFF");
    snprintf(t2, sizeof t2, "LED %s", n->led < 0 ? "?" : n->led ? "ON" : "OFF");
    chip(bx, yy, t1, AMBER, n->buzzer == 1, &cw, can ? H_NBUZZ : -1);
    bx += cw + 10;
    chip(bx, yy, t2, AMBER, n->led == 1, &cw, can ? H_NLED : -1);
    bx += cw + 30;
    chip(bx, yy, "IDENTIFY 10 S", SKY, false, &cw, can ? H_NIDENT : -1);
    bx += cw + 10;
    chip(bx, yy, "REFRESH STATUS", ACCENT, false, &cw, can ? H_NSTATUS : -1);
    bx += cw + 30;
    bool armed = A.rebootArm && millis() - A.rebootArm < 3000;
    chip(bx, yy, armed ? "TAP AGAIN TO REBOOT" : "REBOOT", RED, armed, &cw, can ? H_NREBOOT : -1);
    yy += 56;
    // Find it by ear, and which firmware it runs (any of the three, over the mesh).
    {
        font(F_SM);
        text(lx, yy, "FIND IT", dim(ACCENT, 0.9f));
        int fx = lx + C.textWidth("FIND IT") + 16;
        bool loc = !strcmp(A.locateId, n->id);
        chip(fx, yy - 8, "BEEP 3X", AMBER, false, &cw, can ? H_NBEEP : -1);
        fx += cw + 10;
        chip(fx, yy - 8, loc ? "LOCATE BEEP ON" : "LOCATE BEEP", AMBER, loc, &cw, can ? H_NLOCATE : -1);
        fx += cw + 40;
        font(F_SM);
        text(fx, yy, "FIRMWARE", dim(ACCENT, 0.9f));
        fx += C.textWidth("FIRMWARE") + 16;
        // The builds for this board: XIAO (detector, Sky-Spy, wardriver) or AtomS3 + GPS base
        // (GPS node, GPS wardriver). Chip k is FW_KINDS[k + base].
        bool atom = node_kind(*n) >= 3;
        static const char *FWK[] = {"DETECTOR", "SKY-SPY", "WARDRIVER", "GPS NODE", "GPS WARDRIVE"};
        int base = atom ? 3 : 0, nk = atom ? 2 : 3;
        int cur = node_kind(*n) - base;
        bool armedFw = A.fwArm >= 0 && millis() - A.fwArmMs < 4000;
        for (int k = 0; k < nk; k++) {
            char lbl[32];
            if (armedFw && A.fwArm == k + base) snprintf(lbl, sizeof lbl, "TAP TO INSTALL %s", FWK[k + base]);
            else strlcpy(lbl, FWK[k + base], sizeof lbl);
            bool arm = armedFw && A.fwArm == k + base;
            chip(fx, yy - 8, lbl, k == cur ? ACCENT : (arm ? RED : SKY), k == cur || arm, &cw, can ? H_NFW : -1, k + base);
            fx += cw + 8;
        }
        yy += 48;
        if (n->ota[0]) {   // its last / current firmware update
            font(F_SM);
            char os[96];
            bool busy = ota_busy(*n);
            snprintf(os, sizeof os, "FIRMWARE UPDATE  %s  %d%%", !strcmp(n->ota, "verifying") ? "restarting into the new build" : n->ota,
                     n->otaPct);
            uint16_t oc = !strcmp(n->ota, "failed") ? RED : busy ? PINK : ACCENT;
            text(lx, yy, os, oc, middle_left);
            int bx = lx + 380, bw = fwid - 380;
            C.fillRoundRect(bx, yy - 5, bw, 10, 5, PANEL2);
            C.fillRoundRect(bx, yy - 5, max(10, bw * constrain((int)n->otaPct, 0, 100) / 100), 10, 5, oc);
            yy += 30;
        }
    }
    // A wardriver: its switches. Straight down our USB when it is the node on it, else
    // through the dashboard (and the mesh).
    if (node_is_wardriver(*n)) {
        bool direct = !strcmp(n->id, usb_node_id()) && usb_live();
        bool wcan = direct || can;
        font(F_SM);
        char wt[96];
        snprintf(wt, sizeof wt, "WARDRIVER   %lu access points, %lu Bluetooth since boot", (unsigned long)n->wdWifi,
                 (unsigned long)n->wdBle);
        text(lx, yy, wt, dim(PINK, 0.95f));
        yy += 22;
        int wx = lx;
        font(F_SM);
        text(wx, yy + 15, "CHANNELS", MUTED, middle_left);
        wx += C.textWidth("CHANNELS") + 12;
        static const char *CH[] = {"1-11", "all", "1,6,11", "1-6", "7-11", "1", "6", "11"};
        static const char *CHL[] = {"1-11 HOP", "1-13 HOP", "1/6/11", "1-6", "7-11", "CH 1", "CH 6", "CH 11"};
        for (int i = 0; i < 8; i++) {
            bool on = !strcmp(n->wdCh, CH[i]) || (i == 1 && !strcmp(n->wdCh, "1-13"));
            int tw;
            font(F_SM);
            if (wx + C.textWidth(CHL[i]) + 28 > lx + fwid) wx = lx + 180, yy += 44;
            chip(wx, yy, CHL[i], SKY, on, &tw, wcan ? H_NWDCH : -1, i);
            wx += tw + 8;
        }
        yy += 56;
    }
    // What it reports
    font(F_SM);
    text(lx, yy, "STATUS", dim(ACCENT, 0.9f));
    yy += 24;
    char rows[4][64];
    snprintf(rows[0], 64, "filters %d  %s", n->filters, n->fcrc[0] ? n->fcrc : "");
    snprintf(rows[1], 64, "peers %d   channel %d   devices %d", n->peers, n->ch, n->dev);
    snprintf(rows[2], 64, "link %s dBm   strongest neighbour %d dBm", n->rssi ? String(n->rssi).c_str() : "--", n->lrssi);
    snprintf(rows[3], 64, "heap %u KB   mode %s", (unsigned)(n->heap / 1024), n->mode[0] ? n->mode : "?");
    font(F_MD);
    for (int i = 0; i < 4 && yy + 26 < y + h; i++, yy += 30) text(lx, yy, rows[i], TEXT);
    if (!can) {
        font(F_SM);
        text(x + w / 2, y + h - 20, "settings go through the dashboard: connect the Tab5 to it (Wi-Fi)", AMBER, middle_center);
    }
}

void detail_submit_text() {
    DevDetail &dd = M->detail;
    char body[160];
    if (A.editAlias) {
        JsonDocument j;
        j["alias"] = A.editBuf;
        serializeJson(j, body, sizeof body);
        char path[64];
        snprintf(path, sizeof path, "/api/devices/%s", dd.mac);
        if (net_post(path, body)) strlcpy(dd.alias, A.editBuf, sizeof dd.alias);
    } else if (A.newGroup && A.editBuf[0]) {
        JsonDocument j;
        j["name"] = A.editBuf;
        j["color"] = (int)(esp_random() % 360);
        j["add"].add(dd.mac);
        serializeJson(j, body, sizeof body);
        if (net_post("/api/groups", body)) strlcpy(dd.group, A.editBuf, sizeof dd.group);
    }
    A.editAlias = A.newGroup = false;
    A.editBuf[0] = 0;
}

void detail_group(int gi) {
    DevDetail &dd = M->detail;
    char body[160];
    if (gi < 0) {   // take it out of its group
        int id = 0;
        for (int i = 0; i < dd.ngroups; i++) if (!strcmp(dd.groups[i].name, dd.group)) id = dd.groups[i].id;
        if (!id) return;
        snprintf(body, sizeof body, "{\"id\":%d,\"remove\":[\"%s\"]}", id, dd.mac);
        if (net_post("/api/groups", body)) dd.group[0] = 0;
        return;
    }
    if (gi >= dd.ngroups) return;
    snprintf(body, sizeof body, "{\"id\":%d,\"add\":[\"%s\"]}", dd.groups[gi].id, dd.mac);
    if (net_post("/api/groups", body)) strlcpy(dd.group, dd.groups[gi].name, sizeof dd.group);
}

void drone_set_tag(int t) {
    if (!nDrone || t < 0 || t > 4) return;
    Drone &d = M->drones[droneIdx[A.sel[V_DRONES]]];
    char path[64], body[48];
    snprintf(path, sizeof path, "/api/drones/%s", d.mac);
    snprintf(body, sizeof body, "{\"tag\":\"%s\"}", DRONE_TAGS[t]);
    if (net_post(path, body)) strlcpy(d.tag, DRONE_TAGS[t], sizeof d.tag);
}

// ------------------------------------------------------------------ chrome

const char *TABS[V_COUNT] = {"OVERVIEW", "FLEET", "LIVE", "TARGETS", "MAP", "DRONES", "METRICS"};
// The tabs on the bar, in order (Targets is gone: Live and the map cover it).
const View SHOWN[] = {V_OVERVIEW, V_FLEET, V_LIVE, V_MAP, V_DRONES, V_METRICS};
constexpr int NSHOWN = sizeof(SHOWN) / sizeof(SHOWN[0]);
View next_tab(int dir) {   // the tab beside the current one, on the bar
    int k = 0;
    for (int i = 0; i < NSHOWN; i++) if (SHOWN[i] == A.view) k = i;
    return SHOWN[(k + dir + NSHOWN) % NSHOWN];
}

// ------------------------------------------------------------------ cached bands
// The top and bottom bars change at most once a second but cost ~19 ms a frame to draw
// (glowing text). Each is drawn once into the canvas, saved, and pasted back (~1 ms) while
// what it shows stays the same, together with the touch targets it registered.
struct Band {
    int y0, h;
    uint16_t *px = nullptr;
    uint32_t key = 0;
    bool valid = false;
    Hit hits[24];
    int nh = 0;
};
Band topBand{0, TOP}, botBand{H - BOT, BOT};

uint32_t fnv(const char *s, uint32_t h = 2166136261u) {
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// Panel buffer offset of logical row y0 inside each panel row (the band is h pixels wide there).
int band_off(const Band &b) { return canvasRot() == 3 ? b.y0 : 720 - b.y0 - b.h; }

void cached_band(Band &b, uint32_t key, void (*draw)()) {
    uint16_t *buf = (uint16_t *)C.getBuffer();
    if (!b.px) b.px = (uint16_t *)heap_caps_malloc((size_t)1280 * b.h * 2, MALLOC_CAP_SPIRAM);
    int off = band_off(b);
    key ^= canvasRot();
    if (b.px && b.valid && b.key == key) {
        for (int r = 0; r < 1280; r++) memcpy(buf + (size_t)r * 720 + off, b.px + (size_t)r * b.h, b.h * 2);
        for (int i = 0; i < b.nh; i++) {
            const Hit &h = b.hits[i];
            hit(h.x, h.y, h.w, h.h, h.act, h.arg);
        }
        return;
    }
    int h0 = nhits;
    draw();
    if (!b.px) return;
    for (int r = 0; r < 1280; r++) memcpy(b.px + (size_t)r * b.h, buf + (size_t)r * 720 + off, b.h * 2);
    b.nh = 0;
    for (int i = h0; i < nhits && b.nh < 24; i++) b.hits[b.nh++] = hits[i];
    b.key = key;
    b.valid = true;
}

void draw_top();
void draw_bottom();
int prox_rssi(const char **by = nullptr);   // proximity mode: the device's signal (below)

uint32_t top_key() {
    char k[160];
    int ld = live_drones();
    bool red = M->bridging ? false : M->usbLive ? false : (S.source == SRC_USB || !M->wifiUp || !M->sockUp);
    snprintf(k, sizeof k, "%d|%s|%d|%d%d%d%d%d|%s|%d|%d|%s|%s|%lu", (int)A.view, clock_str(server_now(), true),
             net_rssi(), M->bridging, M->usbLive, M->home.alive, M->wifiUp, M->sockUp, M->mode, (int)S.source, ld,
             M->home.id, A.detail ? "d" : "", (red || ld) ? (unsigned long)(millis() / 120) : 0UL);   // pulses animate
    return fnv(k);
}

uint32_t bottom_key() {
    char k[300];
    bool toastOn = millis() < A.toastUntil;
    snprintf(k, sizeof k, "%d|%d%d%d%d%d|%s|%d%d%d%d%d|%s|%s|%s|%d", (int)A.view, A.detail, A.editAlias, A.newGroup,
             A.searching, M->muteAll, toastOn ? A.toast : "", M->usbLive, M->bridging, M->sockUp, (int)S.source,
             net_ota_enabled(), M->lastError, net_ip().c_str(), net_target().c_str(), (int)M->alertsUnacked);
    return fnv(k);
}

void draw_top() {
    C.fillRect(0, 0, W, TOP, BG);
    C.drawFastHLine(0, TOP - 1, W, LINE2);
    // Brand, stacked so seven tabs keep a full-size label.
    font(F_MD);
    glow(PAD, TOP / 2 - 11, "OUI-SPY", ACCENT, middle_left);
    int bw = C.textWidth("OUI-SPY");
    glow(PAD + bw / 2, TOP / 2 + 12, "M E S H", PINK, middle_center);   // as big, centred under it
    int tabsL = PAD + bw + 20;

    // Top right: the data-source badge, the clock to its left.
    const char *state;
    char label[32];
    uint16_t lc;
    int r = net_rssi();
    if (M->bridging) state = "USB BRIDGE", lc = ACCENT;
    else if (M->usbLive) state = M->home.id[0] ? M->home.id : "NODE", lc = M->home.alive ? ACCENT : AMBER;
    else if (S.source == SRC_USB) state = "NO USB NODE", lc = RED;
    else if (!M->wifiUp) state = "NO WIFI", lc = RED;
    else if (!M->sockUp) state = "NO LINK", lc = RED;
    else if (!strcmp(M->mode, "demo")) state = "DEMO", lc = AMBER;
    else if (!strcmp(M->mode, "replay")) state = "REPLAY", lc = PURPLE;
    else if (!M->home.alive) state = "HOME DOWN", lc = AMBER;
    else state = "LIVE", lc = ACCENT;
    bool usb = M->usbLive || S.source == SRC_USB;
    if (usb && M->usbLive && !M->bridging) snprintf(label, sizeof label, "USB  %s", state);
    else if (!usb && r && M->sockUp) snprintf(label, sizeof label, "%s  WIFI %d", state, r);
    else snprintf(label, sizeof label, "%s", state);
    font(F_MD);
    int bwid = C.textWidth(label) + 44, bh = 34, bx = W - PAD - bwid, by = (TOP - bh) / 2;
    bool down = lc == RED;
    float pulse = down ? 0.55f + 0.45f * sinf(millis() / 400.0f) : 1.0f;
    C.fillRoundRect(bx, by, bwid, bh, bh / 2, dim(lc, 0.14f));
    glow_rect(bx, by, bwid, bh, bh / 2, dim(lc, pulse));
    dot(bx + 18, TOP / 2, 5, dim(lc, pulse));
    glow(bx + 32, TOP / 2, label, lc, middle_left);
    hit(bx, 0, bwid + PAD, TOP, H_SETUP);   // tap the badge to change the source
    int x = bx - 16;
    font(F_MD);
    glow(x, TOP / 2, clock_str(server_now(), true), SOFT, middle_right);
    int tabsR = x - C.textWidth("00:00:00") - 18;

    // Tabs share what's left evenly; labels shrink to fit if they ever have to.
    int tw = (tabsR - tabsL) / NSHOWN;
    // One font for all of them, so they always match.
    const lgfx::IFont *tf = F_MD;
    font(F_MD);
    for (int k = 0; k < NSHOWN; k++) {
        if (C.textWidth(TABS[SHOWN[k]]) > tw - 6) tf = F_SM;
    }
    for (int k = 0; k < NSHOWN; k++) {
        int i = SHOWN[k];
        int tx = tabsL + k * tw;
        bool on = A.view == i;
        hit(tx, 0, tw, TOP, H_TAB, i);
        const char *label = TABS[i];
        font(tf);
        if (i == V_DRONES) {   // live drones: a pink count badge on the tab
            int ld = live_drones();
            if (ld) {
                char nb[6];
                snprintf(nb, sizeof nb, "%d", ld);
                font(F_SM);
                int bwid = max(20, (int)C.textWidth(nb) + 10);
                int bxx = tx + tw - bwid - 2, byy = 6;
                float p = 0.6f + 0.4f * sinf(millis() / 250.0f);
                C.fillRoundRect(bxx - 2, byy - 2, bwid + 4, 22, 11, dim(PINK, 0.3f * p));
                C.fillRoundRect(bxx, byy, bwid, 18, 9, PINK);
                text(bxx + bwid / 2, byy + 9, nb, BG, middle_center);
                font(tf);
            }
        }
        if (on) {
            C.fillRect(tx + 3, 8, tw - 6, TOP - 16, dim(ACCENT, 0.12f));
            C.fillRect(tx + 3, TOP - 5, tw - 6, 3, ACCENT);
            C.drawFastHLine(tx + 3, TOP - 6, tw - 6, dim(ACCENT, 0.4f));
            glow(tx + tw / 2, TOP / 2 - 1, label, ACCENT, middle_center);
        } else {
            text(tx + tw / 2, TOP / 2 - 1, label, MUTED, middle_center);
        }
    }
}

void draw_bottom() {
    int y = H - BOT;
    C.fillRect(0, y, W, BOT, BG);
    bool alert = M->alertsUnacked && M->hasAlert && !M->muteAll;
    if (alert) {
        // Fixed-size bar: an alert recolours it, never resizes anything.
        uint32_t t = millis();
        bool fresh = t < alertFlashUntil;
        float pulse = fresh ? 0.5f + 0.5f * sinf(t / 90.0f) : 0.75f + 0.25f * sinf(t / 600.0f);
        C.fillRect(0, y, W, BOT, dim(PINK, 0.10f));
        C.drawFastHLine(0, y, W, dim(PINK, 0.4f + 0.6f * pulse));
        C.drawFastHLine(0, y + 1, W, dim(PINK, 0.25f * pulse + 0.1f));
        const Det &d = M->lastAlert;
        font(F_MD);
        char head[24];
        snprintf(head, sizeof head, "ALERT x%u", (unsigned)M->alertsUnacked);
        glow(PAD, y + BOT / 2, head, PINK, middle_left);
        int x = PAD + C.textWidth("ALERT x000") + 20;
        font(F_LG);
        text(x, y + BOT / 2, fit_name(vendor_of(&d), 210), TEXT, middle_left);
        x += min(210, (int)C.textWidth(vendor_of(&d))) + 16;
        font(F_MONO);
        text(x, y + BOT / 2, d.mac, SOFT, middle_left);
        x += C.textWidth(d.mac) + 18;
        font(F_MD);
        char via[64];
        snprintf(via, sizeof via, "%d dBm  via %s", d.rssi, node_name(d.node));
        text(x, y + BOT / 2, fit(via, 190), rssi_color(d.rssi), middle_left);
        // Touch buttons, right-aligned.
        int bx = W - PAD, w;
        static const struct { const char *t; uint8_t act; } btn[] = {{"SILENCE  M", H_SILENCE}, {"MUTE DEVICE  X", H_MUTE}, {"ACK  A", H_ACK}};
        for (auto &b : btn) {
            font(F_SM);
            w = C.textWidth(b.t) + 28;
            bx -= w;
            chip(bx, y + 7, b.t, PINK, b.act == H_ACK, nullptr, b.act);
            bx -= 10;
        }
        return;
    }
    C.drawFastHLine(0, y, W, LINE2);
    font(F_SM);
    const char *hints = A.detail ? (A.editAlias || A.newGroup ? "type the name   ENTER save   ESC cancel" : "E name it   G new group   ESC close")
                        : A.view == V_METRICS ? "R range   D domain   N node   C clear"
                        : A.view == V_MAP     ? "drag to pan   + - zoom   L layer   Z fit   O follow   tap a marker"
                        : A.view == V_DRONES  ? "UP DN select   ENTER show on map   tap a tag to label it"
                        : A.searching         ? "type to filter   ENTER keep   ESC clear"
                                              : "/ search   ENTER details   D domain   N node   X mute   A ack";
    if (A.prox[0]) {   // proximity mode: who, how strong, by whom; STOP
        const char *by = nullptr;
        int r = prox_rssi(&by);
        const Target *t = target_by_mac(A.prox);
        const char *nm = t ? (t->alias[0] ? t->alias : t->name[0] ? t->name : t->descr[0] ? t->descr : A.prox) : A.prox;
        char l[96];
        if (r == INT16_MIN) snprintf(l, sizeof l, "PROXIMITY  %s   not heard for 20 s", nm);
        else snprintf(l, sizeof l, "PROXIMITY  %s   %d dBm at %s", nm, r, node_name(by));
        int cw;
        chip(PAD, y + BOT / 2 - 15, "STOP", RED, true, &cw, H_PROXSTOP);
        font(F_SM);
        text(PAD + cw + 12, y + BOT / 2, fit(l, 560), r == INT16_MIN ? AMBER : rgb(0x39ff88), middle_left);
    } else
    text(PAD, y + BOT / 2, hints, FAINT, middle_left);
    // SETUP / HELP buttons at the far right; status sits left of them.
    int bx = W - PAD, w;
    font(F_SM);
    w = C.textWidth("HELP") + 28;
    bx -= w;
    chip(bx, y + 7, "HELP", ACCENT, false, nullptr, H_HELP);
    font(F_SM);
    w = C.textWidth("SETUP") + 28;
    bx -= w + 10;
    chip(bx, y + 7, "SETUP", ACCENT, false, nullptr, H_SETUP);
    int rx = bx - 18;
    font(F_SM);
    char right[96];
    if (millis() < A.toastUntil) {
        font(F_MD);
        glow(rx, y + BOT / 2, A.toast, AMBER, middle_right);
        return;
    }
    if (M->usbLive && !M->bridging) {
        snprintf(right, sizeof right, "USB %s  local view%s%s", M->home.id, net_ota_enabled() ? "   OTA" : "",
                 M->muteAll ? "   SILENCED" : "");
        text(rx, y + BOT / 2, right, M->muteAll ? AMBER : FAINT, middle_right);
        return;
    }
    if (M->lastError[0] && !M->sockUp && S.source != SRC_USB) {
        text(rx, y + BOT / 2, M->lastError, RED, middle_right);
        return;
    }
    if (S.source == SRC_USB) snprintf(right, sizeof right, "plug a node into the USB-A port");
    else snprintf(right, sizeof right, "%s%s  >  %s%s%s", net_ip().c_str(), net_ota_enabled() ? "  OTA" : "",
                  net_target().c_str(), M->bridging ? "   bridging USB node" : "", M->muteAll ? "   SILENCED" : "");
    text(rx, y + BOT / 2, right, M->muteAll ? AMBER : FAINT, middle_right);
}

// ------------------------------------------------------------------ shared rows

const char *waiting_text() {
    if (M->sockUp || M->usbLive) return "no nodes reported yet";
    return S.source == SRC_USB ? "waiting for a node on the USB-A port" : "waiting for the dashboard";
}

int online_count() {
    int n = 0;
    for (int i = 0; i < M->nnodes; i++) n += M->nodes[i].online;
    return n;
}

int live_targets() {
    int n = 0;
    double now = server_now();
    for (int i = 0; i < M->ntargets; i++) n += now - M->targets[i].last < TARGET_LIVE_S;
    return n;
}

void kpi(int x, int y, int w, int h, const char *label, const char *value, uint16_t col, const char *sub = nullptr) {
    panel(x, y, w, h, nullptr);
    C.fillRect(x + 1, y + 10, 3, h - 20, col);
    font(F_SM);
    text(x + 18, y + 12, label, MUTED);
    font(F_XL);
    int vw = C.textWidth(value);
    glow(x + 18, y + h - 14, value, col, bottom_left);
    if (sub) {   // beside the value when it fits; else up on the label line, trimmed to fit
        font(F_SM);
        int room = w - 14 - (18 + vw + 14);
        if (C.textWidth(sub) <= room) {
            text(x + w - 14, y + h - 16, sub, FAINT, bottom_right);
        } else {
            int lw = C.textWidth(label);
            text(x + w - 14, y + 12, fit(sub, w - 14 - (18 + lw + 14)), FAINT, top_right);
        }
    }
}

void filters_cell(int x, int y, const Node &n, bool compact) {
    char b[40];
    uint16_t col = n.filtersFresh == 1 ? ACCENT : n.filtersFresh == 0 ? AMBER : FAINT;
    const char *st = n.filtersFresh == 1 ? "in sync" : n.filtersFresh == 0 ? "STALE" : "";
    if (n.filters < 0) snprintf(b, sizeof b, "-");
    else if (compact) snprintf(b, sizeof b, "%d  %s", n.filters, n.fcrc[0] ? n.fcrc : "--------");
    else snprintf(b, sizeof b, "%d", n.filters);
    font(compact ? F_SM : F_MD);
    text(x, y, b, n.filters < 0 ? FAINT : TEXT, middle_left);
    if (!compact) {
        int w = C.textWidth(b) + 12;
        font(F_SM);
        text(x + w, y - 8, n.fcrc[0] ? n.fcrc : "--------", MUTED, middle_left);
        text(x + w, y + 9, st, col, middle_left);
    } else if (*st) {
        text(x + C.textWidth(b) + 10, y, st, col, middle_left);
    }
}

// ------------------------------------------------------------------ OVERVIEW

void draw_overview() {
    int tw = (W - 2 * PAD - 4 * 12) / 5, th = 104, y = CY;
    char v[24], sub[32];
    const bool wdm = A.ovWd;
    const double nowS = server_now();
    // What the filters keep: nodes (wardrivers only in WARDRIVE mode, unless one is picked),
    // detections, wardriven devices (newest first).
    static int nIdx[64], dIdx[300], wIdx[200];
    int nn = 0, nd = 0, nw = 0;
    for (int i = 0; i < M->nnodes && nn < 64; i++) {
        const Node &n = M->nodes[i];
        if (A.node[0] && strcmp(n.id, A.node)) continue;
        if (wdm && !A.node[0] && !node_is_wardriver(n)) continue;
        nIdx[nn++] = i;
    }
    for (int i = 0; i < M->ndets && nd < 300; i++) {
        const Det *d = det_at(i);
        if (A.node[0] && strcmp(d->node, A.node) && strcmp(d->closest, A.node)) continue;
        if (A.dom && strcmp(d->dom, A.dom == 1 ? "ble" : "wifi")) continue;
        dIdx[nd++] = i;
    }
    uint32_t wLive = 0, wWifi = 0, wBle = 0, wOpen = 0, wPlaced = 0, wSess = 0;
    uint16_t wHour[24] = {0};
    if (wdm) {
        static int *all = (int *)heap_caps_malloc(WD_MAX * sizeof(int), MALLOC_CAP_SPIRAM);   // 32 KB: PSRAM, not internal RAM
        int na = 0;
        for (int i = 0; all && i < wd_count(); i++) {
            const WdDev &d = wd_at(i);
            if (A.node[0] && strcmp(d.node, A.node)) continue;
            if (A.dom && d.kind != (A.dom == 1 ? 2 : 1)) continue;
            if (d.first && nowS - d.first < 86400) wHour[min(23, (int)((86400 - (nowS - d.first)) / 3600))]++;
            if (!d.session) continue;
            wSess++;
            if (nowS - d.last < 60) wLive++;
            if (d.kind == 1) wWifi++, wOpen += d.sec == 0; else wBle++;
            if (d.placed) wPlaced++;
            all[na++] = i;
        }
        nw = min(na, 200);
        if (all) std::partial_sort(all, all + nw, all + na, [](int a, int b) { return wd_at(a).last > wd_at(b).last; });
        if (all) memcpy(wIdx, all, nw * sizeof(int));
    }
    int on = 0;
    for (int k = 0; k < nn; k++) on += M->nodes[nIdx[k]].online;
    if (wdm) {
        snprintf(v, sizeof v, "%d/%d", on, nn);
        kpi(PAD, y, tw, th, "WARDRIVERS ONLINE", v, on ? rgb(0x22d3ee) : FAINT, nn ? "wardriving now" : "no wardriver nodes");
        snprintf(v, sizeof v, "%u", (unsigned)wSess);
        kpi(PAD + (tw + 12), y, tw, th, "THIS SESSION", v, rgb(0x22d3ee), "devices heard");
        snprintf(v, sizeof v, "%u", (unsigned)wLive);
        kpi(PAD + 2 * (tw + 12), y, tw, th, "IN RANGE NOW", v, AMBER, "last minute");
        snprintf(v, sizeof v, "%u / %u", (unsigned)wWifi, (unsigned)wBle);
        snprintf(sub, sizeof sub, "%u open", (unsigned)wOpen);
        kpi(PAD + 3 * (tw + 12), y, tw, th, "WIFI / BLE", v, rgb(0xff2bd6), sub);
        snprintf(v, sizeof v, "%u", (unsigned)wPlaced);
        snprintf(sub, sizeof sub, "%u in memory", (unsigned)wd_count());
        kpi(PAD + 4 * (tw + 12), y, tw, th, "ON THE MAP", v, rgb(0x39ff88), sub);
    } else {
    snprintf(v, sizeof v, "%d/%d", on, nn);
    kpi(PAD, y, tw, th, "NODES ONLINE", v, on == nn && on ? ACCENT : AMBER,
        M->home.alive ? "home up" : "home down");
    snprintf(v, sizeof v, "%u", (unsigned)M->stats.day);
    snprintf(sub, sizeof sub, "%u all time", (unsigned)M->stats.total);
    kpi(PAD + (tw + 12), y, tw, th, "DETECTIONS 24H", v, SKY, sub);
    snprintf(v, sizeof v, "%u", M->metrics.valid ? (unsigned)M->metrics.unique : (unsigned)M->stats.macs);
    snprintf(sub, sizeof sub, "%s window", RANGE_NAMES[A.range % NRANGES]);
    kpi(PAD + 2 * (tw + 12), y, tw, th, "UNIQUE DEVICES", v, PURPLE, sub);
    snprintf(v, sizeof v, "%d", live_targets());
    snprintf(sub, sizeof sub, "%d in 10 min", M->ntargets);
    kpi(PAD + 3 * (tw + 12), y, tw, th, "TARGETS IN VIEW", v, AMBER, sub);
    snprintf(v, sizeof v, "%u", (unsigned)M->alertsUnacked);
    kpi(PAD + 4 * (tw + 12), y, tw, th, M->muteAll ? "ALERTS (SILENCED)" : "UNACKED ALERTS", v,
        M->alertsUnacked ? PINK : FAINT, M->nmuted ? "devices muted" : nullptr);
    }

    // Fleet column.
    int fy = y + th + 12, fw = 500, fh = CB - fy;
    panel(PAD, fy, fw, fh, "FLEET");
    // Three lines a node: name | status; type + id | link; filters + firmware. Drag to scroll.
    int rh = 86, ry = fy + 34, vis = max(1, (fh - 40) / rh);
    ovFleetTop = constrain(ovFleetTop, 0, max(0, nn - vis));
    scroll_area(PAD, ry, fw, vis * rh, L_OVF, rh, vis);
    for (int k = 0; k < vis; k++, ry += rh) {
        if (ovFleetTop + k >= nn) break;
        int i = nIdx[ovFleetTop + k];
        const Node &n = M->nodes[i];
        uint16_t nc = node_color565(n.id);
        hit(PAD + 8, ry, fw - 16, rh - 2, H_NODEROW, i);   // tap: to Fleet; hold: its settings
        C.fillRoundRect(PAD + 12, ry + 8, 5, rh - 18, 2, n.online ? nc : dim(nc, 0.35f));
        int rx = PAD + fw - 16;
        // 1: name, status
        const char *st = n.online ? "ONLINE" : "OFFLINE";
        font(F_SM);
        int sw = C.textWidth(st);
        text(rx, ry + 18, st, n.online ? ACCENT : RED, middle_right);
        dot(rx - sw - 12, ry + 18, 4, n.online ? ACCENT : RED);
        font(F_LG);
        text(PAD + 30, ry + 18, fit_name(n.alias[0] ? n.alias : n.id, fw - 90 - sw - 40), n.online ? TEXT : MUTED, middle_left);
        // 2: firmware type + id, link signal
        font(F_SM);
        int kw = kind_label(PAD + 30, ry + 46, n);
        text(PAD + 30 + kw + 14, ry + 46, n.id, FAINT, middle_left);
        int rssi = n.isHome ? n.lrssi : n.rssi;
        char rs[16];
        snprintf(rs, sizeof rs, rssi ? "%d" : "--", rssi);
        font(F_SM);
        text(rx, ry + 46, rs, rssi_color(rssi), middle_right);
        rssi_bar(rx - 140, ry + 42, 96, 8, rssi, rssi_color(rssi));
        // 3: filters, firmware build
        filters_cell(PAD + 30, ry + 70, n, true);
        font(F_SM);
        char fwb[32];
        snprintf(fwb, sizeof fwb, "fw %s", n.fw[0] ? n.fw : "?");
        text(rx, ry + 70, fwb, FAINT, middle_right);
        if (k + 1 < vis && ovFleetTop + k + 1 < nn) C.drawFastHLine(PAD + 12, ry + rh - 1, fw - 24, LINE);
    }
    if (nn > vis) {   // more than fit: a slim scroll bar
        int th = fh - 40, bh = max(24, th * vis / nn), by = fy + 34 + (th - bh) * ovFleetTop / max(1, nn - vis);
        C.fillRoundRect(PAD + fw - 6, by, 3, bh, 1, dim(ACCENT, 0.6f));
    }
    if (!nn) {
        font(F_MD);
        text(PAD + fw / 2, fy + fh / 2, !M->nnodes ? waiting_text() : wdm ? "no wardriver nodes" : "no node matches", FAINT, middle_center);
    }

    // Live feed.
    int lx = PAD + fw + 12, lw = W - PAD - lx, lh = 340;
    panel(lx, fy, lw, lh, wdm ? "WARDRIVE FEED" : "LIVE FEED");
    {   // mode, node and radio: the same switch and filters as Metrics
        static const char *doms[] = {"BLE + WIFI", "BLE", "WIFI"};
        int cw, cx = lx + lw - 14;
        font(F_SM);
        cx -= C.textWidth(doms[A.dom]) + 28;
        chip(cx, fy + 4, doms[A.dom], A.dom == 2 ? SKY : A.dom == 1 ? PURPLE : ACCENT, A.dom != 0, &cw, H_DOM, -1);
        const char *nl = A.node[0] ? node_name(A.node) : "ALL NODES";
        cx -= C.textWidth(fit_name(nl, 150)) + 28 + 8;
        chip(cx, fy + 4, fit_name(nl, 150), A.node[0] ? node_color565(A.node) : ACCENT, A.node[0] != 0, &cw, H_NODE);
        const char *ml = wdm ? "WARDRIVE" : "DETECTIONS";
        cx -= C.textWidth(ml) + 28 + 8;
        chip(cx, fy + 4, ml, wdm ? rgb(0x22d3ee) : ACCENT, true, &cw, H_OVWD);
    }
    int hy = fy + 44;
    font(F_SM);
    const int cAge = lx + 16, cVen = lx + 76, cMac = lx + 290, cRssi = lx + 508, cNode = lx + 566;
    text(cAge, hy, "AGE", FAINT);
    text(cVen, hy, "DEVICE", FAINT);
    text(cMac, hy, "MAC", FAINT);
    text(cRssi, hy, "RSSI", FAINT);
    text(cNode, hy, wdm ? "NODE" : "NODE / CLOSEST", FAINT);
    int row = 30, yy = hy + 20, lvis = max(1, (fy + lh - 4 - yy) / row);
    int total = wdm ? nw : nd;
    ovLiveTop = constrain(ovLiveTop, 0, max(0, total - lvis));
    scroll_area(lx, yy, lw, lvis * row, L_OVL, row, lvis);
    if (total > lvis) {
        int th = lvis * row, bh = max(20, th * lvis / total), by = yy + (th - bh) * ovLiveTop / max(1, total - lvis);
        C.fillRoundRect(lx + lw - 6, by, 3, bh, 1, dim(ACCENT, 0.6f));
    }
    for (int k = ovLiveTop; wdm && k < nw && yy + row <= fy + lh - 4; k++, yy += row) {
        const WdDev &d = wd_at(wIdx[k]);
        bool wifi = d.kind == 1;
        if (nowS - d.first < 60) C.fillRect(lx + 4, yy, lw - 8, row - 2, dim(PINK, 0.08f));
        hit(lx + 4, yy, lw - 12, row, H_OVDET, wIdx[k]);   // tap: its info page
        font(F_MD);
        text(cAge, yy + row / 2, age_str(d.last), MUTED, middle_left);
        const char *nm = d.name[0] ? d.name : wifi ? "(hidden network)" : "BLE device";
        font(F_SM);
        static const char *secs[] = {"OPEN", "WEP", "WPA", "WPA2", "WPA3", "WPA2/3"};
        const char *tag = wifi ? (d.sec < 6 ? secs[d.sec] : "WIFI") : "BLE";
        int tgw = C.textWidth(tag);
        text(cMac - 14, yy + row / 2, tag, wifi ? (d.sec <= 1 ? RED : rgb(0x22d3ee)) : rgb(0xff2bd6), middle_right);
        font(F_MD);
        text(cVen, yy + row / 2, fit_name(nm, cMac - cVen - tgw - 26), nowS - d.first < 60 ? PINK : TEXT, middle_left);
        char m[18];
        snprintf(m, sizeof m, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        font(F_MONO_S);
        text(cMac, yy + row / 2, m, SOFT, middle_left);
        font(F_MD);
        char r[8];
        snprintf(r, sizeof r, "%d", d.rssi);
        text(cRssi, yy + row / 2, r, rssi_color(d.rssi), middle_left);
        text(cNode, yy + row / 2, fit_name(d.node[0] ? node_name(d.node) : "-", lx + lw - cNode - 12),
             d.node[0] ? node_color565(d.node) : FAINT, middle_left);
    }
    for (int k = ovLiveTop; !wdm && k < nd && yy + row <= fy + lh - 4; k++, yy += row) {
        int i = dIdx[k];
        const Det *d = det_at(i);
        bool muted = mac_muted(d->mac);
        if (i == 0 && server_now() - d->ts < 10) C.fillRect(lx + 4, yy, lw - 8, row - 2, dim(PINK, 0.08f));
        hit(lx + 4, yy, lw - 12, row, H_OVDET, i);   // tap: the device's details
        font(F_MD);
        text(cAge, yy + row / 2, age_str(d->ts), MUTED, middle_left);
        text(cVen, yy + row / 2, fit_name(vendor_of(d), 206), muted ? FAINT : (d->isNew ? PINK : TEXT), middle_left);
        font(F_MONO_S);
        text(cMac, yy + row / 2, d->mac, muted ? FAINT : SOFT, middle_left);
        font(F_MD);
        char r[8];
        snprintf(r, sizeof r, "%d", d->rssi);
        text(cRssi, yy + row / 2, r, rssi_color(d->rssi), middle_left);
        char nb[40];
        if (d->closest[0]) snprintf(nb, sizeof nb, "%s > %s", node_name(d->node), node_name(d->closest));
        else snprintf(nb, sizeof nb, "%s", node_name(d->node));
        text(cNode, yy + row / 2, fit_name(nb, lx + lw - cNode - 12), node_color565(d->closest[0] ? d->closest : d->node), middle_left);
    }
    if (!total) {
        font(F_MD);
        text(lx + lw / 2, fy + lh / 2 + 20, wdm ? "nothing wardriven this session" : M->ndets ? "nothing matches the filter" : "no detections yet",
             FAINT, middle_center);
    }

    // 24 h activity.
    int ay = fy + lh + 12, ah = CB - ay;
    panel(lx, ay, lw, ah, wdm ? "NEW DEVICES  LAST 24 H" : "ACTIVITY  LAST 24 H");
    const uint16_t *hours = wdm ? wHour : M->stats.hourly;
    int n = wdm ? 24 : M->stats.nhourly;
    if (n) {
        uint16_t mx = 1;
        for (int i = 0; i < n; i++) mx = max(mx, hours[i]);
        int bx = lx + 16, bw = (lw - 32) / n, by = ay + ah - 26, bh = ah - 64;
        for (int i = 0; i < n; i++) {
            int h = hours[i] * bh / mx;
            if (hours[i] && h < 2) h = 2;
            uint16_t col = i == n - 1 ? PINK : wdm ? rgb(0x22d3ee) : ACCENT;
            C.fillRect(bx + i * bw + 2, by - h, bw - 4, h, dim(col, 0.75f));
            if (h) C.drawFastHLine(bx + i * bw + 2, by - h, bw - 4, col);
        }
        C.drawFastHLine(bx, by, bw * n, LINE2);
        font(F_SM);
        char m[16];
        snprintf(m, sizeof m, "peak %u/h", mx);
        text(lx + lw - 16, ay + 10, m, FAINT, top_right);
        text(bx, by + 6, "-24h", FAINT);
        text(bx + bw * n, by + 6, "now", FAINT, top_right);
    }
    draw_ota_strip();   // firmware updates in flight, on top
}

// ------------------------------------------------------------------ FLEET

void draw_fleet() {
    int y = CY, rows = 6, rh = 62, th = 36 + rows * rh;
    panel(PAD, y, W - 2 * PAD, th + 8, "FLEET");
    clamp_sel(V_FLEET, rows);
    // Columns sized to what they hold: TYPE fits "[home] WARDRIVER", FIRMWARE a full build stamp.
    const int cx[] = {PAD + 24, PAD + 252, PAD + 432, PAD + 566, PAD + 718, PAD + 778, PAD + 968, PAD + 1140};
    const char *hd[] = {"NODE", "TYPE", "STATUS", "LINK RSSI", "NBR", "FIRMWARE", "FILTERS / CRC", ""};
    font(F_SM);
    for (int i = 0; i < 7; i++) text(cx[i], y + 34, hd[i], FAINT);
    text(W - PAD - 24, y + 34, "UPTIME", FAINT, top_right);
    int ry = y + 56;
    scroll_area(PAD, ry, W - 2 * PAD, rows * rh, V_FLEET, rh, rows);
    for (int k = 0; k < rows; k++, ry += rh) {
        int i = A.top[V_FLEET] + k;
        if (i >= M->nnodes) break;
        const Node &n = M->nodes[i];
        bool sel = i == A.sel[V_FLEET];
        uint16_t nc = node_color565(n.id);
        hit(PAD + 8, ry, W - 2 * PAD - 16, rh, H_ROW, i);
        if (sel) {
            C.fillRoundRect(PAD + 8, ry, W - 2 * PAD - 16, rh - 6, 6, dim(ACCENT, 0.10f));
            glow_rect(PAD + 8, ry, W - 2 * PAD - 16, rh - 6, 6, dim(ACCENT, 0.8f));
        }
        int my = ry + (rh - 6) / 2;
        C.fillRoundRect(PAD + 12, ry + 8, 5, rh - 22, 2, n.online ? nc : dim(nc, 0.35f));
        font(F_LG);
        text(cx[0], my - 10, fit_name(n.alias[0] ? n.alias : n.id, cx[1] - cx[0] - 16), n.online ? TEXT : MUTED, middle_left);
        font(F_SM);
        text(cx[0], my + 14, n.id, FAINT, middle_left);
        font(F_MD);
        size_t fl = strlen(n.fw);
        bool sky = fl && n.fw[fl - 1] == 'S';   // a dedicated Sky-Spy drone node (build stamp suffix)
        (void)sky;
        kind_label(cx[1], my, n);
        dot(cx[2] + 6, my, 5, n.online ? ACCENT : RED);
        text(cx[2] + 20, my - 8, n.online ? "ONLINE" : "OFFLINE", n.online ? ACCENT : RED, middle_left);
        font(F_SM);
        char seen[24];
        snprintf(seen, sizeof seen, "seen %s ago", n.age >= 0 ? age_str(server_now() - n.age) : "-");
        text(cx[2] + 20, my + 12, seen, FAINT, middle_left);
        int rssi = n.isHome ? n.lrssi : n.rssi;
        rssi_bar(cx[3], my - 4, 90, 10, rssi, rssi_color(rssi));
        char rs[12];
        snprintf(rs, sizeof rs, rssi ? "%d" : "--", rssi);
        font(F_MD);
        text(cx[3] + 100, my, rs, rssi_color(rssi), middle_left);
        snprintf(rs, sizeof rs, n.lrssi ? "%d" : "--", n.lrssi);
        text(cx[4], my, rs, rssi_color(n.lrssi), middle_left);
        text(cx[5], my, n.fw[0] ? n.fw : "?", n.fw[0] ? TEXT : FAINT, middle_left);
        if (n.ota[0] && strcmp(n.ota, "done")) {
            font(F_SM);
            char o[24];
            snprintf(o, sizeof o, "OTA %s %d%%", n.ota, n.otaPct);
            text(cx[5], my + 18, o, AMBER, middle_left);
        }
        filters_cell(cx[6], my, n, false);
        font(F_MD);
        text(W - PAD - 24, my, n.up ? dur_str(n.up) : "-", MUTED, middle_right);
    }
    if (!M->nnodes) {
        font(F_MD);
        text(W / 2, y + th / 2, waiting_text(), FAINT, middle_center);
    }

    // Detail: selected node's neighbours + the home's link counters.
    int dy = y + th + 20, dh = CB - dy, dw = 640;
    panel(PAD, dy, dw, dh, "NEIGHBOURS");
    if (M->nnodes) {
        const Node &n = M->nodes[A.sel[V_FLEET]];
        font(F_MD);
        char h[64];
        snprintf(h, sizeof h, "%s   ch %d   peers %d   dev %d   %s", n.alias[0] ? n.alias : n.id, n.ch, n.peers, n.dev,
                 n.mode[0] ? n.mode : "");
        text(PAD + 16, dy + 32, h, TEXT);
        int yy = dy + 66;
        for (int i = 0; i < n.nnbr && yy + 28 < dy + dh; i++, yy += 30) {
            const Seen &s = n.nbr[i];
            font(F_MD);
            text(PAD + 16, yy, fit_name(node_name(s.node), 200), node_color565(s.node));
            rssi_bar(PAD + 230, yy + 6, 260, 10, s.rssi, rssi_color(s.rssi));
            char r[24];
            snprintf(r, sizeof r, "%d%s", s.rssi, s.age ? "  relay" : "");
            text(PAD + 504, yy, r, rssi_color(s.rssi));
        }
        if (!n.nnbr) text(PAD + 16, yy, "no neighbour report", FAINT);
    }
    int hx = PAD + dw + 12, hw = W - PAD - hx;
    panel(hx, dy, hw, dh, "HOME LINK");
    const Home &hm = M->home;
    struct { const char *k; uint32_t v; } cells[] = {{"rx", hm.rx}, {"dup", hm.dup}, {"tx", hm.tx},
                                                    {"tx fail", hm.txf}, {"pending", hm.pend}, {"bad", hm.bad}};
    font(F_SM);
    for (int i = 0; i < 6; i++) {
        int cxp = hx + 16 + (i % 3) * ((hw - 32) / 3), cyp = dy + 34 + (i / 3) * 56;
        text(cxp, cyp, cells[i].k, FAINT);
        char b[16];
        snprintf(b, sizeof b, "%u", (unsigned)cells[i].v);
        font(F_LG);
        text(cxp, cyp + 18, b, (i == 3 || i == 5) && cells[i].v ? AMBER : TEXT);
        font(F_SM);
    }
    char l[96];
    snprintf(l, sizeof l, "rate %s   ch %d   %s   via %s   up %s", hm.rate[0] ? hm.rate : "?", hm.ch,
             hm.enc ? "encrypted" : "PLAINTEXT", hm.source[0] ? hm.source : "?", dur_str(hm.up));
    text(hx + 16, dy + dh - 24, l, hm.enc ? MUTED : AMBER);
    draw_ota_strip();   // firmware updates in flight, on top
}

// ------------------------------------------------------------------ LIVE / TARGETS filter row

void draw_filter_row(int y, int shown, int total) {
    int x = PAD;
    int sw = 420, sh = 38;
    hit(x, y - 6, sw, sh + 12, H_SEARCH);
    C.fillRoundRect(x, y, sw, sh, 8, PANEL);
    if (A.searching) glow_rect(x, y, sw, sh, 8, ACCENT);
    else C.drawRoundRect(x, y, sw, sh, 8, A.search[0] ? dim(ACCENT, 0.7f) : LINE2);
    font(F_MD);
    if (A.search[0] || A.searching) {
        // The text, its tail kept in view, then a thin blinking caret right after it.
        const char *t = A.search;
        while (*t && C.textWidth(t) > sw - 40) t++;
        text(x + 14, y + sh / 2, t, TEXT, middle_left);
        if (A.searching && (millis() / 450) % 2) {
            int cx = x + 16 + C.textWidth(t);
            C.fillRect(cx, y + 8, 2, sh - 16, ACCENT);
            C.fillRect(cx - 1, y + 8, 4, 1, dim(ACCENT, 0.5f));
            C.fillRect(cx - 1, y + sh - 9, 4, 1, dim(ACCENT, 0.5f));
        }
    } else {
        text(x + 14, y + sh / 2, "/  search vendor, MAC, node", FAINT, middle_left);
    }
    x += sw + 18;
    int w;
    static const char *doms[] = {"ALL", "BLE", "WIFI"};
    font(F_SM);
    text(x, y + sh / 2, "D", FAINT, middle_left);
    x += 16;
    for (int i = 0; i < 3; i++) {
        chip(x, y + 4, doms[i], i == 2 ? SKY : i == 1 ? PURPLE : ACCENT, A.dom == i, &w, H_DOM, i);
        x += w + 8;
    }
    x += 16;
    font(F_SM);
    text(x, y + sh / 2, "N", FAINT, middle_left);
    x += 16;
    chip(x, y + 4, A.node[0] ? node_name(A.node) : "ALL NODES", A.node[0] ? node_color565(A.node) : ACCENT, true, &w, H_NODE);
    x += w + 8;
    static const char *KF[] = {"ALL TYPES", "DETECTORS", "SKY-SPY", "WARDRIVERS"};
    chip(x, y + 4, KF[A.kindFilter % 4], A.kindFilter ? kind_color(A.kindFilter - 1) : ACCENT, A.kindFilter != 0, &w, H_KIND);
    x += w + 16;
    if (A.search[0] || A.dom || A.node[0] || A.kindFilter) chip(x, y + 4, "CLEAR", AMBER, false, &w, H_CLEAR), x += w + 8;
    if (A.view == V_LIVE) chip(x, y + 4, A.liveZoomLock ? "ZOOM LOCKED" : "ZOOM: PINCH", SKY, A.liveZoomLock, &w, H_ZLOCK);
    char c[32];
    snprintf(c, sizeof c, "%d of %d", shown, total);
    font(F_MD);
    text(W - PAD, y + sh / 2, c, MUTED, middle_right);
}

void seen_bars(int x, int y, int w, const Seen *s, int n, int rowH, int maxRows) {
    for (int i = 0; i < n && i < maxRows; i++) {
        int yy = y + i * rowH;
        font(F_MD);
        uint16_t nc = node_color565(s[i].node);
        if (i == 0 && n > 1) dot(x + 6, yy + 10, 4, nc);
        text(x + 18, yy, fit_name(node_name(s[i].node), 150), nc);
        rssi_bar(x + 180, yy + 6, w - 270, 10, s[i].rssi, rssi_color(s[i].rssi));
        char r[16];
        snprintf(r, sizeof r, "%d", s[i].rssi);
        text(x + w - 20, yy, r, rssi_color(s[i].rssi), top_right);
    }
}

// ------------------------------------------------------------------ LIVE

// On-screen keyboard for the search box when no keyboard is plugged in: letters, digits and
// what MACs need, SPACE / DEL / CLEAR / DONE. It sits over the bottom of the page.
void draw_osk() {
    if (!A.searching || kb_present()) return;
    static const char *ROWS[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL:", "ZXCVBNM-._"};
    int kw = (W - 2 * PAD - 9 * 8) / 10, kh = 50, gap = 8;
    int h = 5 * kh + 4 * gap + 24, y0 = CB - h;
    fast_round(PAD - 6, y0 - 6, W - 2 * PAD + 12, h + 6, 12, dim(BG, 0.95f));
    glow_rect(PAD - 6, y0 - 6, W - 2 * PAD + 12, h + 6, 12, dim(ACCENT, 0.7f));
    hit(PAD - 6, y0 - 6, W - 2 * PAD + 12, h + 6, H_NONE);
    font(F_MD);
    for (int r = 0; r < 4; r++) {
        int n = strlen(ROWS[r]), x = PAD + (W - 2 * PAD - (n * kw + (n - 1) * gap)) / 2, y = y0 + 6 + r * (kh + gap);
        for (int i = 0; i < n; i++, x += kw + gap) {
            fast_round(x, y, kw, kh, 8, PANEL2);
            C.drawRoundRect(x, y, kw, kh, 8, LINE2);
            char k[2] = {ROWS[r][i], 0};
            text(x + kw / 2, y + kh / 2, k, TEXT, middle_center);
            hit(x, y, kw, kh, H_OSK, ROWS[r][i]);
        }
    }
    struct { const char *t; int code; int span; uint16_t c; } bot[] = {
        {"CLEAR", 27, 2, AMBER}, {"SPACE", ' ', 4, TEXT}, {"DEL", 8, 2, PINK}, {"DONE", 13, 2, ACCENT}};
    int x = PAD, y = y0 + 6 + 4 * (kh + gap);
    for (auto &b : bot) {
        int w = b.span * kw + (b.span - 1) * gap;
        fast_round(x, y, w, kh, 8, PANEL2);
        C.drawRoundRect(x, y, w, kh, 8, dim(b.c, 0.7f));
        text(x + w / 2, y + kh / 2, b.t, b.c, middle_center);
        hit(x, y, w, kh, H_OSK, b.code);
        x += w + gap;
    }
}

// A sortable column head: tap to sort by it, again to reverse.
void sort_head(int x, int y, const char *label, int key) {
    font(F_SM);
    bool on = A.liveSort == key;
    text(x, y, label, on ? ACCENT : FAINT);
    int w = C.textWidth(label);
    if (on) {
        int ax = x + w + 6, ay = y + 6;
        if (A.liveSortRev) C.fillTriangle(ax, ay + 3, ax + 8, ay + 3, ax + 4, ay - 3, ACCENT);
        else C.fillTriangle(ax, ay - 3, ax + 8, ay - 3, ax + 4, ay + 3, ACCENT);
    }
    hit(x - 6, y - 8, w + 26, 30, H_SORT, key);
}

void draw_live_wardrive() {
    if (!wdIdx) wdIdx = (int *)heap_caps_malloc(WD_MAX * sizeof(int), MALLOC_CAP_SPIRAM);
    if (!wdIdx) return;
    wdN = 0;
    for (int i = 0, n = wd_count(); i < n; i++) {
        const WdDev &d = wd_at(i);
        if (!d.session) continue;
        if (A.search[0]) {   // name / SSID or MAC
            char m[18];
            snprintf(m, sizeof m, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
            if (!icontains(d.name, A.search) && !icontains(m, A.search)) continue;
        }
        wdIdx[wdN++] = i;
    }
    // Sort: last seen (default), name, MAC, type, security, closeness (strongest first).
    auto key = [](int a, int b) -> int {   // <0: a first
        const WdDev &x = wd_at(a), &y = wd_at(b);
        switch (A.liveSort) {
        case 1: return strcasecmp(x.name[0] ? x.name : "~", y.name[0] ? y.name : "~");
        case 2: return memcmp(x.mac, y.mac, 6);
        case 3: return (int)x.kind - (int)y.kind;
        case 4: return (int)x.sec - (int)y.sec;
        case 5: return (int)y.rssi - (int)x.rssi;
        default: return x.last > y.last ? -1 : x.last < y.last ? 1 : 0;
        }
    };
    std::sort(wdIdx, wdIdx + wdN, [&](int a, int b) { int c = key(a, b); return A.liveSortRev ? c > 0 : c < 0; });
    draw_filter_row(CY, wdN, wd_count());
    int ty = CY + 50, rh = (int)A.liveRh, rows = max(1, (CB - ty - 62) / rh), th = 56 + rows * rh + 6;
    char hdr[64];
    snprintf(hdr, sizeof hdr, "WARDRIVE THIS SESSION   %lu APs   %lu BLE", (unsigned long)wd_session_count(1),
             (unsigned long)wd_session_count(2));
    panel(PAD, ty, W - 2 * PAD, th, hdr);   // the counts are the panel's title: nothing sits on the headers
    ty += 22;
    clamp_sel(V_LIVE, rows);
    const int cAge = PAD + 18, cName = PAD + 90, cMac = PAD + 430, cType = PAD + 670, cSec = PAD + 790,
              cCh = PAD + 920, cRssi = PAD + 980, cPos = PAD + 1130;
    sort_head(cAge, ty + 12, "AGE", 0);
    sort_head(cName, ty + 12, "NAME / SSID", 1);
    sort_head(cMac, ty + 12, "MAC", 2);
    sort_head(cType, ty + 12, "TYPE", 3);
    sort_head(cSec, ty + 12, "SECURITY", 4);
    text(cCh, ty + 12, "CH", FAINT);
    sort_head(cRssi, ty + 12, "CLOSEST", 5);
    text(cPos, ty + 12, "MAP", FAINT);
    static const char *SEC[] = {"open", "WEP", "WPA", "WPA2", "WPA3", "WPA2/3"};
    static const uint32_t SECC[6] = {0x39ff88, 0xff4d6d, 0xffb020, 0x22d3ee, 0xa78bfa, 0xa78bfa};
    int ry = ty + 34;
    scroll_area(PAD, ry, W - 2 * PAD, rows * rh, V_LIVE, rh, rows);
    double now = server_now();
    for (int k = 0; k < rows; k++, ry += rh) {
        int li = A.top[V_LIVE] + k;
        if (li >= wdN) break;
        const WdDev &d = wd_at(wdIdx[li]);
        bool sel = li == A.sel[V_LIVE];
        hit(PAD, ry, W - 2 * PAD, rh, H_ROW, li);
        if (sel) {
            C.fillRect(PAD + 4, ry, W - 2 * PAD - 8, rh - 2, dim(PINK, 0.12f));
            C.fillRect(PAD + 4, ry, 3, rh - 2, PINK);
        } else if (k & 1) C.fillRect(PAD + 4, ry, W - 2 * PAD - 8, rh - 2, PANEL2);
        int my = ry + rh / 2;
        uint16_t c = d.kind == 2 ? rgb(0xff2bd6) : rgb(d.sec < 6 ? SECC[d.sec] : 0x7dd3fc);
        const lgfx::IFont *rf = rh < 30 ? F_SM : rh < 48 ? F_MD : F_LG;
        font(rf);
        text(cAge, my, age_str(d.last ? d.last : now), MUTED, middle_left);
        const char *nm = d.name[0] ? d.name : d.kind == 1 ? "(hidden network)" : "BLE device";
        text(cName, my, fit_name(nm, 320), d.name[0] ? TEXT : FAINT, middle_left);
        char mac[18];
        snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
        font(F_MONO_S);
        text(cMac, my, mac, SOFT, middle_left);
        font(F_MD);
        text(cType, my, d.kind == 1 ? "WIFI AP" : "BLE", c, middle_left);
        text(cSec, my, d.kind == 1 && d.sec < 6 ? SEC[d.sec] : "-", d.kind == 1 ? c : FAINT, middle_left);
        char ch[6];
        snprintf(ch, sizeof ch, "%u", d.ch);
        text(cCh, my, d.kind == 1 && d.ch ? ch : "-", MUTED, middle_left);
        rssi_bar(cRssi, my - 4, 70, 8, d.rssi, rssi_color(d.rssi));
        char r[8];
        snprintf(r, sizeof r, "%d", d.rssi);
        text(cRssi + 78, my, r, rssi_color(d.rssi), middle_left);
        text(cPos, my, d.placed ? "yes" : "no GPS", d.placed ? ACCENT : FAINT, middle_left);
    }
    if (!wdN) {
        font(F_MD);
        text(W / 2, ty + th / 2, "nothing wardriven this session yet", FAINT, middle_center);
    }
}

void draw_live() {
    if (live_is_wardrive()) { draw_live_wardrive(); return; }
    if (A.liveSort) {   // newest first otherwise (the ring order)
        auto key = [](int a, int b) -> int {
            LiveRow x, y;
            live_row(a, x), live_row(b, y);
            switch (A.liveSort) {
            case 1: return strcasecmp(x.name, y.name);
            case 2: return strcmp(x.mac, y.mac);
            case 3: return strcmp(x.type, y.type);
            case 4: return strcmp(x.dom, y.dom);
            case 5: return y.rssi - x.rssi;
            default: return 0;
            }
        };
        std::stable_sort(liveIdx, liveIdx + nLive, [&](int a, int b) { int c = key(a, b); return A.liveSortRev ? c > 0 : c < 0; });
    } else if (A.liveSortRev) {
        std::reverse(liveIdx, liveIdx + nLive);
    }
    draw_filter_row(CY, nLive, M->ndets);
    // Rows follow the pinch zoom inside a fixed frame, so the SELECTED box below never moves.
    int ty = CY + 50, rh = (int)A.liveRh, rows = max(3, min(30, 340 / rh)), th = 34 + 340 + 6;
    panel(PAD, ty, W - 2 * PAD, th, nullptr);
    clamp_sel(V_LIVE, rows);
    const int cAge = PAD + 18, cVen = PAD + 90, cMac = PAD + 400, cType = PAD + 640, cDom = PAD + 740,
              cRssi = PAD + 820, cNode = PAD + 980, cClose = PAD + 1110;
    font(F_SM);
    sort_head(cAge, ty + 12, "AGE", 0);
    sort_head(cVen, ty + 12, "DEVICE", 1);
    sort_head(cMac, ty + 12, "MAC", 2);
    sort_head(cType, ty + 12, "TYPE", 3);
    sort_head(cDom, ty + 12, "DOM", 4);
    sort_head(cRssi, ty + 12, "CLOSEST", 5);
    text(cNode, ty + 12, "NODE", FAINT);
    text(cClose, ty + 12, "CLOSEST", FAINT);
    int ry = ty + 34;
    scroll_area(PAD, ry, W - 2 * PAD, rows * rh, V_LIVE, rh, rows);
    for (int k = 0; k < rows; k++, ry += rh) {
        int li = A.top[V_LIVE] + k;
        if (li >= nLive) break;
        LiveRow d;
        live_row(liveIdx[li], d);
        bool sel = li == A.sel[V_LIVE];
        hit(PAD, ry, W - 2 * PAD, rh, H_ROW, li);
        bool muted = !d.wd && mac_muted(d.mac);
        if (sel) {
            C.fillRect(PAD + 4, ry, W - 2 * PAD - 8, rh - 2, dim(ACCENT, 0.12f));
            C.fillRect(PAD + 4, ry, 3, rh - 2, ACCENT);
        } else if (k & 1) {
            C.fillRect(PAD + 4, ry, W - 2 * PAD - 8, rh - 2, PANEL2);
        }
        int my = ry + rh / 2;
        const lgfx::IFont *rf = rh < 30 ? F_SM : rh < 48 ? F_MD : F_LG;
        font(rf);
        text(cAge, my, age_str(d.ts), MUTED, middle_left);
        text(cVen, my, fit_name(d.name, 290), muted ? FAINT : (d.isNew ? PINK : TEXT), middle_left);
        if (muted) {
            font(F_SM);
            text(cMac - 14, my, "MUTED", AMBER, middle_right);
        }
        font(F_MONO_S);
        text(cMac, my, d.mac, muted ? FAINT : SOFT, middle_left);
        font(F_MD);
        text(cType, my, d.wd ? (d.dom[0] == 'w' ? "AP" : "BLE") : d.type, d.wd ? rgb(0x22d3ee) : MUTED, middle_left);
        text(cDom, my, d.wd ? "WD" : d.dom, d.wd ? rgb(0x22d3ee) : !strcmp(d.dom, "ble") ? PURPLE : SKY, middle_left);
        rssi_bar(cRssi, my - 4, 90, 8, d.rssi, rssi_color(d.rssi));
        char r[8];
        snprintf(r, sizeof r, "%d", d.rssi);
        text(cRssi + 100, my, r, rssi_color(d.rssi), middle_left);
        text(cNode, my, fit_name(node_name(d.node), 120), node_color565(d.node), middle_left);
        text(cClose, my, d.closest[0] ? fit_name(node_name(d.closest), 130) : "-",
             d.closest[0] ? node_color565(d.closest) : FAINT, middle_left);
    }
    if (!nLive) {
        font(F_MD);
        text(W / 2, ty + th / 2, M->ndets || wd_count() ? "nothing matches the filter" : "no detections yet", FAINT, middle_center);
    }

    // Detail of the selected row.
    int dy = ty + th + 12, dh = CB - dy;
    panel(PAD, dy, W - 2 * PAD, dh, "SELECTED");
    if (!nLive) return;
    if (int e = liveIdx[A.sel[V_LIVE]]; e < 0) {   // a wardriven network / device
        const WdDev &w = wd_at(-e - 1);
        LiveRow r;
        live_row(e, r);
        font(F_LG);
        glow(PAD + 16, dy + 32, fit_name(r.name, 420), TEXT);
        font(F_SM);
        chip(PAD + 440, dy + 28, "DETAILS  ENTER", ACCENT, true, nullptr, H_DETAIL);
        font(F_MONO);
        text(PAD + 16, dy + 68, r.mac, SOFT);
        font(F_SM);
        static const char *SEC[] = {"open", "WEP", "WPA", "WPA2", "WPA3", "WPA2/3"};
        char l[128];
        snprintf(l, sizeof l, "WARDRIVEN %s%s%s   by %s   first %s   %s", w.kind == 1 ? "WIFI AP " : "BLE",
                 w.kind == 1 && w.sec < 6 ? SEC[w.sec] : "", w.kind == 1 && w.ch ? (String("  ch ") + w.ch).c_str() : "",
                 node_name(w.node), clock_str(w.first ? w.first : w.last, true), w.placed ? "on the map" : "no GPS fix");
        text(PAD + 16, dy + 100, fit(l, 1180), MUTED);
        return;
    }
    const Det *d = det_at(liveIdx[A.sel[V_LIVE]]);
    font(F_LG);
    glow(PAD + 16, dy + 32, fit_name(vendor_of(d), 420), d->isNew ? PINK : TEXT);
    {
        font(F_SM);
        int cw;
        chip(PAD + 440, dy + 28, "DETAILS  ENTER", ACCENT, true, &cw, H_DETAIL);
        if (d->group[0]) chip(PAD + 440 + cw + 8, dy + 28, d->group, PURPLE, false, nullptr);
    }
    font(F_MONO);
    text(PAD + 16, dy + 68, d->mac, SOFT);
    font(F_SM);
    char l[96];
    snprintf(l, sizeof l, "%s %s   %s   %s %s   %s", d->type, d->dom, d->descr[0] ? d->descr : "-",
             clock_str(d->ts, true), d->isNew ? "  FIRST SIGHTING" : "", mac_muted(d->mac) ? "MUTED (X to unmute)" : "X mutes this device");
    text(PAD + 16, dy + 100, fit(l, 560), MUTED);
    if (d->nseen) seen_bars(PAD + 620, dy + 30, 600, d->seen, d->nseen, 28, 3);
    else text(PAD + 620, dy + 34, "heard by one node", FAINT);
}

// ------------------------------------------------------------------ TARGETS

void draw_targets() {
    draw_filter_row(CY, nTgt, M->ntargets);
    int gy = CY + 50, cols = 3, rowsN = 3, gap = 12;
    int cw = (W - 2 * PAD - (cols - 1) * gap) / cols, ch = (CB - gy - (rowsN - 1) * gap) / rowsN;
    int per = cols * rowsN;
    // Page-based scrolling: the selection's page is the one shown.
    A.sel[V_TARGETS] = constrain(A.sel[V_TARGETS], 0, max(0, nTgt - 1));
    int page = A.sel[V_TARGETS] / per;
    double now = server_now();
    scroll_area(PAD, gy, W - 2 * PAD, CB - gy, V_TARGETS, ch + gap, rowsN);
    if (nTgt > per) {   // page indicator
        font(F_SM);
        char pg[24];
        snprintf(pg, sizeof pg, "page %d / %d   drag to turn", page + 1, (nTgt + per - 1) / per);
        text(W / 2, CY + 19, pg, FAINT, middle_center);
    }
    for (int k = 0; k < per; k++) {
        int ti = page * per + k;
        int x = PAD + (k % cols) * (cw + gap), y = gy + (k / cols) * (ch + gap);
        if (ti < nTgt) hit(x, y, cw, ch, H_TARGET, ti);
        if (ti >= nTgt) {
            if (nTgt) C.drawRoundRect(x, y, cw, ch, 8, LINE);   // empty slots only beside real cards
            continue;
        }
        const Target &t = M->targets[tgtIdx[ti]];
        bool live = now - t.last < TARGET_LIVE_S;
        bool sel = ti == A.sel[V_TARGETS];
        bool muted = mac_muted(t.mac);
        C.fillRoundRect(x, y, cw, ch, 8, PANEL);
        if (sel) glow_rect(x, y, cw, ch, 8, ACCENT);
        else C.drawRoundRect(x, y, cw, ch, 8, live ? dim(PINK, 0.5f) : LINE2);
        font(F_LG);
        text(x + 16, y + 12, fit_name(target_label(t), cw - 110), muted ? FAINT : (t.alias[0] ? PINK : TEXT));
        if (t.group[0]) {
            font(F_SM);
            text(x + 16, y + ch - 20, fit(t.group, cw - 32), PURPLE);
        }
        font(F_MD);
        text(x + cw - 16, y + 14, age_str(t.last), live ? PINK : FAINT, top_right);
        if (live) dot(x + cw - 16 - C.textWidth(age_str(t.last)) - 14, y + 24, 4, PINK);
        font(F_MONO_S);
        text(x + 16, y + 48, t.mac, muted ? FAINT : SOFT);
        font(F_SM);
        char l[48];
        snprintf(l, sizeof l, "%s  %s  %u hits%s", t.type, t.dom, (unsigned)t.hits, muted ? "  MUTED" : "");
        text(x + cw - 16, y + 52, l, muted ? AMBER : FAINT, top_right);
        if (t.nseen) {
            const Seen &best = t.seen[0];
            font(F_SM);
            text(x + 16, y + 82, t.nseen > 1 ? "NEAREST" : "HEARD BY", FAINT);
            font(F_LG);
            glow(x + 110, y + 76, fit_name(node_name(best.node), cw - 220), node_color565(best.node));
            char r[12];
            snprintf(r, sizeof r, "%d", best.rssi);
            text(x + cw - 16, y + 76, r, rssi_color(best.rssi), top_right);
            // Up to two runners-up.
            for (int i = 1; i < t.nseen && i < 3; i++) {
                int yy = y + 108 + (i - 1) * 24;
                if (yy + 18 > y + ch) break;
                font(F_SM);
                text(x + 16, yy, fit_name(node_name(t.seen[i].node), 120), node_color565(t.seen[i].node));
                rssi_bar(x + 140, yy + 4, cw - 220, 8, t.seen[i].rssi, rssi_color(t.seen[i].rssi));
                snprintf(r, sizeof r, "%d", t.seen[i].rssi);
                text(x + cw - 16, yy, r, rssi_color(t.seen[i].rssi), top_right);
            }
        }
    }
    if (!nTgt) {
        font(F_MD);
        text(W / 2, (gy + CB) / 2, M->ntargets ? "nothing matches the filter" : "no devices heard in the last 10 minutes",
             FAINT, middle_center);
    }
}

// ------------------------------------------------------------------ METRICS

void hbars(int x, int y, int w, int h, const char *title, const Bar *bars, int n, bool nodes) {
    panel(x, y, w, h, title);
    uint32_t mx = 1;
    for (int i = 0; i < n; i++) mx = max(mx, bars[i].count);
    int rh = 28, yy = y + 34;
    for (int i = 0; i < n && yy + rh <= y + h - 4; i++, yy += rh) {
        uint16_t col = nodes ? node_color565(bars[i].name) : (i == 0 ? PINK : ACCENT);
        const char *label = nodes ? node_name(bars[i].name) : bars[i].name;
        font(F_MD);
        text(x + 16, yy + rh / 2, fit(label, 150), TEXT, middle_left);
        int bx = x + 176, bw = w - 176 - 80;
        int fw = max(2, (int)((uint64_t)bars[i].count * bw / mx));
        C.fillRoundRect(bx, yy + 8, bw, rh - 16, 3, PANEL2);
        C.fillRoundRect(bx, yy + 8, fw, rh - 16, 3, dim(col, 0.8f));
        C.drawFastHLine(bx + 2, yy + 8, fw - 4, col);
        char c[16];
        snprintf(c, sizeof c, "%u", (unsigned)bars[i].count);
        text(x + w - 16, yy + rh / 2, c, MUTED, middle_right);
    }
    if (!n) {
        font(F_MD);
        text(x + w / 2, y + h / 2, "-", FAINT, middle_center);
    }
}

// Metrics, wardriving: what the wardriver nodes heard in the range (same filters).
void draw_metrics_wd(int y) {
    const Metrics &m = M->metrics;
    const auto &w = m.wd;
    const uint16_t CWIFI = rgb(0x22d3ee), CBLE = rgb(0xff2bd6);
    int tw = (W - 2 * PAD - 4 * 12) / 5, th = 88;
    char v[24], sub[40];
    snprintf(v, sizeof v, "%u", (unsigned)w.total);
    snprintf(sub, sizeof sub, "%u wardriver%s", (unsigned)w.nodes, w.nodes == 1 ? "" : "s");
    kpi(PAD, y, tw, th, "DEVICES HEARD", v, CWIFI, sub);
    snprintf(v, sizeof v, "%u", (unsigned)w.fresh);
    kpi(PAD + (tw + 12), y, tw, th, "NEW", v, PINK, "first heard in range");
    snprintf(v, sizeof v, "%u", (unsigned)w.wifi);
    snprintf(sub, sizeof sub, "%u open", (unsigned)w.open);
    kpi(PAD + 2 * (tw + 12), y, tw, th, "WIFI NETWORKS", v, CWIFI, sub);
    snprintf(v, sizeof v, "%u", (unsigned)w.ble);
    kpi(PAD + 3 * (tw + 12), y, tw, th, "BLE DEVICES", v, CBLE);
    snprintf(v, sizeof v, "%u", (unsigned)w.placed);
    snprintf(sub, sizeof sub, "%u%% with a GPS fix", w.total ? (unsigned)(w.placed * 100 / w.total) : 0);
    kpi(PAD + 4 * (tw + 12), y, tw, th, "ON THE MAP", v, ACCENT, sub);

    // Discovered over time: WiFi and BLE stacked.
    y += th + 12;
    int chH = 214;
    panel(PAD, y, W - 2 * PAD, chH, "DEVICES DISCOVERED OVER TIME");
    font(F_SM);
    text(W - PAD - 16, y + 10, "new per bucket", FAINT, top_right);
    text(W - PAD - 150, y + 10, "BLE", CBLE, top_right);
    text(W - PAD - 190, y + 10, "WIFI", CWIFI, top_right);
    if (m.valid && m.nbuckets && w.fresh) {
        int n = m.nbuckets;
        uint32_t mx = 1;
        for (int i = 0; i < n; i++) mx = max(mx, (uint32_t)w.wifiNew[i] + w.bleNew[i]);
        int px = PAD + 56, pw = W - 2 * PAD - 72, py = y + 36, ph = chH - 64;
        float bw = (float)pw / n;
        C.drawFastHLine(px, py + ph, pw, LINE2);
        for (int g = 1; g <= 2; g++) C.drawFastHLine(px, py + ph - ph * g / 2, pw, LINE);
        char lab[12];
        snprintf(lab, sizeof lab, "%u", (unsigned)mx);
        text(px - 8, py, lab, FAINT, top_right);
        text(px - 8, py + ph, "0", FAINT, bottom_right);
        for (int i = 0; i < n; i++) {
            int hw = (int)((uint64_t)w.wifiNew[i] * ph / mx), hb = (int)((uint64_t)w.bleNew[i] * ph / mx);
            if (w.wifiNew[i] && hw < 2) hw = 2;
            if (w.bleNew[i] && hb < 2) hb = 2;
            int bx = px + (int)(i * bw), bwi = max(1, (int)bw - 1);
            if (hw) C.fillRect(bx, py + ph - hw, bwi, hw, dim(CWIFI, 0.75f));
            if (hb) C.fillRect(bx, py + ph - hw - hb, bwi, hb, dim(CBLE, 0.75f));
            if (hw + hb) C.drawFastHLine(bx, py + ph - hw - hb, bwi, hb ? CBLE : CWIFI);
        }
        text(px, py + ph + 6, clock_str(m.start, false), FAINT);
        text(px + pw / 2, py + ph + 6, clock_str((m.start + m.until) / 2, false), FAINT, top_center);
        text(px + pw, py + ph + 6, "now", FAINT, top_right);
    } else {
        font(F_MD);
        text(W / 2, y + chH / 2, !m.valid ? (M->sockUp ? "loading metrics" : waiting_text()) : "nothing wardriven in this range",
             FAINT, middle_center);
    }

    // Breakdown: makers, wardrivers, WiFi security, WiFi channels.
    y += chH + 12;
    int bh = CB - y;
    int w1 = 420, w2 = 300, w3 = 260, w4 = W - 2 * PAD - w1 - w2 - w3 - 36;
    hbars(PAD, y, w1, bh, "TOP MAKERS", w.maker, w.nmaker, false);
    hbars(PAD + w1 + 12, y, w2, bh, "BY WARDRIVER", w.node, w.nnode, true);
    int sx0 = PAD + w1 + w2 + 24;
    panel(sx0, y, w3, bh, "WIFI SECURITY");
    {
        uint32_t mx = 1;
        for (int i = 0; i < w.nsec; i++) mx = max(mx, w.sec[i].count);
        int rh = 28, yy = y + 34;
        for (int i = 0; i < w.nsec && yy + rh <= y + bh - 4; i++, yy += rh) {
            bool weak = !strcmp(w.sec[i].name, "open") || !strcmp(w.sec[i].name, "WEP");
            uint16_t col = weak ? rgb(0xff4d6d) : CWIFI;
            font(F_SM);
            text(sx0 + 14, yy + rh / 2, fit(w.sec[i].name, 82), weak ? col : TEXT, middle_left);
            int bx = sx0 + 100, bwd = w3 - 100 - 50;
            int fw = max(2, (int)((uint64_t)w.sec[i].count * bwd / mx));
            C.fillRoundRect(bx, yy + 9, bwd, rh - 18, 3, PANEL2);
            C.fillRoundRect(bx, yy + 9, fw, rh - 18, 3, dim(col, 0.8f));
            char c[12];
            snprintf(c, sizeof c, "%u", (unsigned)w.sec[i].count);
            text(sx0 + w3 - 12, yy + rh / 2, c, MUTED, middle_right);
        }
        if (!w.nsec) {
            font(F_MD);
            text(sx0 + w3 / 2, y + bh / 2, "-", FAINT, middle_center);
        }
    }
    int cx0 = sx0 + w3 + 12;
    panel(cx0, y, w4, bh, "WIFI CHANNELS");
    if (w.nch) {
        uint32_t mx = 1;
        for (int i = 0; i < w.nch; i++) mx = max(mx, (uint32_t)w.chCount[i]);
        int px = cx0 + 14, pw = w4 - 28, py = y + 36, ph = bh - 66;
        int bwd = pw / w.nch;
        font(F_SM);
        for (int i = 0; i < w.nch; i++) {
            int h = (int)((uint64_t)w.chCount[i] * ph / mx);
            if (w.chCount[i] && h < 2) h = 2;
            C.fillRect(px + i * bwd + 1, py + ph - h, max(1, bwd - 2), h, dim(CWIFI, 0.75f));
            C.drawFastHLine(px + i * bwd + 1, py + ph - h, max(1, bwd - 2), CWIFI);
            char c[6];
            snprintf(c, sizeof c, "%u", w.ch[i]);
            if (bwd >= 14) text(px + i * bwd + bwd / 2, py + ph + 6, c, FAINT, top_center);
        }
        C.drawFastHLine(px, py + ph, bwd * w.nch, LINE2);
    } else {
        font(F_MD);
        text(cx0 + w4 / 2, y + bh / 2, "-", FAINT, middle_center);
    }
}

void draw_metrics() {
    const Metrics &m = M->metrics;
    int x = PAD, y = CY, w;
    font(F_SM);
    text(x, y + 12, "R", FAINT, middle_left);
    x += 16;
    for (int i = 0; i < NRANGES; i++) {
        chip(x, y, RANGE_NAMES[i], ACCENT, A.range == i, &w, H_RANGE, i);
        x += w + 8;
    }
    x += 20;
    static const char *doms[] = {"ALL", "BLE", "WIFI"};
    chip(x, y, doms[A.dom], A.dom == 2 ? SKY : A.dom == 1 ? PURPLE : ACCENT, A.dom != 0, &w, H_DOM, -1);
    x += w + 8;
    chip(x, y, A.node[0] ? node_name(A.node) : "ALL NODES", A.node[0] ? node_color565(A.node) : ACCENT, A.node[0] != 0, &w, H_NODE);
    x += w + 20;
    chip(x, y, A.mxWd ? "WARDRIVE" : "DETECTIONS", A.mxWd ? rgb(0x22d3ee) : ACCENT, true, &w, H_MXWD);
    if (m.valid) {
        char u[48];
        snprintf(u, sizeof u, "updated %us ago", (unsigned)((millis() - m.fetchedMs) / 1000));
        font(F_SM);
        text(W - PAD, y + 12, u, FAINT, middle_right);
    }
    if (M->usbLive && !M->bridging) {
        font(F_SM);
        text(W - PAD, y + 15, "local history since boot", AMBER, middle_right);
    }
    y += 42;
    if (A.mxWd) {
        draw_metrics_wd(y);
        return;
    }
    int tw = (W - 2 * PAD - 4 * 12) / 5, th = 88;
    char v[24], sub[40];
    snprintf(v, sizeof v, "%u", (unsigned)m.total);
    if (m.prevTotal) snprintf(sub, sizeof sub, "%+d%% vs prev", (int)((int64_t)m.total * 100 / m.prevTotal) - 100);
    else snprintf(sub, sizeof sub, "prev %u", (unsigned)m.prevTotal);
    kpi(PAD, y, tw, th, "DETECTIONS", v, SKY, sub);
    snprintf(v, sizeof v, "%u", (unsigned)m.unique);
    kpi(PAD + (tw + 12), y, tw, th, "UNIQUE DEVICES", v, PURPLE);
    snprintf(v, sizeof v, "%u", (unsigned)m.peak);
    snprintf(sub, sizeof sub, "%s / %um", m.peakTs ? clock_str(m.peakTs, false) : "-", (unsigned)(m.bucket / 60));
    kpi(PAD + 2 * (tw + 12), y, tw, th, "PEAK", v, AMBER, sub);
    snprintf(v, sizeof v, "%u", (unsigned)m.newDevices);
    kpi(PAD + 3 * (tw + 12), y, tw, th, "NEW DEVICES", v, PINK);
    snprintf(v, sizeof v, "%u", (unsigned)m.nodesActive);
    snprintf(sub, sizeof sub, "BLE %u  WIFI %u", (unsigned)m.ble, (unsigned)m.wifi);
    kpi(PAD + 4 * (tw + 12), y, tw, th, "NODES ACTIVE", v, ACCENT, sub);

    // Detections over time (bars) with unique devices (pink line).
    y += th + 12;
    int chH = 214;
    panel(PAD, y, W - 2 * PAD, chH, "DETECTIONS OVER TIME");
    font(F_SM);
    text(W - PAD - 16, y + 10, "bars detections   line unique devices", FAINT, top_right);
    if (m.valid && m.nbuckets) {
        int n = m.nbuckets;
        uint16_t mx = 1, mu = 1;
        for (int i = 0; i < n; i++) mx = max(mx, m.series[i]), mu = max(mu, m.uniqueSeries[i]);
        int px = PAD + 56, pw = W - 2 * PAD - 72, py = y + 36, ph = chH - 64;
        float bw = (float)pw / n;
        C.drawFastHLine(px, py + ph, pw, LINE2);
        for (int g = 1; g <= 2; g++) C.drawFastHLine(px, py + ph - ph * g / 2, pw, LINE);
        char lab[12];
        snprintf(lab, sizeof lab, "%u", mx);
        text(px - 8, py, lab, FAINT, top_right);
        text(px - 8, py + ph, "0", FAINT, bottom_right);
        for (int i = 0; i < n; i++) {
            int h = (int)m.series[i] * ph / mx;
            if (m.series[i] && h < 2) h = 2;
            int bx = px + (int)(i * bw);
            int bwi = max(1, (int)bw - 1);
            if (h) {
                C.fillRect(bx, py + ph - h, bwi, h, dim(ACCENT, 0.7f));
                C.drawFastHLine(bx, py + ph - h, bwi, ACCENT);
            }
        }
        int lx = -1, ly = 0;
        for (int i = 0; i < n; i++) {
            int xx = px + (int)(i * bw + bw / 2), yy = py + ph - (int)m.uniqueSeries[i] * ph / mu;
            if (lx >= 0) {
                C.drawLine(lx, ly, xx, yy, PINK);
                C.drawLine(lx, ly - 1, xx, yy - 1, dim(PINK, 0.5f));
            }
            lx = xx, ly = yy;
        }
        text(px, py + ph + 6, clock_str(m.start, false), FAINT);
        text(px + pw / 2, py + ph + 6, clock_str((m.start + m.until) / 2, false), FAINT, top_center);
        text(px + pw, py + ph + 6, "now", FAINT, top_right);
    } else {
        font(F_MD);
        text(W / 2, y + chH / 2, M->sockUp || M->usbLive ? "loading metrics" : waiting_text(), FAINT, middle_center);
    }

    // Breakdown row.
    y += chH + 12;
    int bh = CB - y;
    int w1 = 500, w2 = 380, w3 = W - 2 * PAD - w1 - w2 - 24;
    hbars(PAD, y, w1, bh, "BY VENDOR", m.vendor, m.nvendor, false);
    hbars(PAD + w1 + 12, y, w2, bh, "BY NODE", m.node, m.nnode, true);
    int rx = PAD + w1 + w2 + 24;
    panel(rx, y, w3, bh, "RSSI DISTRIBUTION");
    if (m.nrssi) {
        uint16_t mx = 1;
        for (int i = 0; i < m.nrssi; i++) mx = max(mx, m.rssi[i]);
        int px = rx + 16, pw = w3 - 32, py = y + 36, ph = bh - 66;
        int bw = pw / m.nrssi;
        for (int i = 0; i < m.nrssi; i++) {
            int h = m.rssi[i] * ph / mx;
            if (m.rssi[i] && h < 2) h = 2;
            uint16_t col = rssi_color(m.rssiFrom + i * m.rssiStep + m.rssiStep / 2);
            if (h) {
                C.fillRect(px + i * bw + 1, py + ph - h, bw - 2, h, dim(col, 0.75f));
                C.drawFastHLine(px + i * bw + 1, py + ph - h, bw - 2, col);
            }
        }
        C.drawFastHLine(px, py + ph, bw * m.nrssi, LINE2);
        font(F_SM);
        char a[12], b[12];
        snprintf(a, sizeof a, "%d", m.rssiFrom);
        snprintf(b, sizeof b, "%d dBm", m.rssiFrom + m.nrssi * m.rssiStep);
        text(px, py + ph + 6, a, FAINT);
        text(px + bw * m.nrssi, py + ph + 6, b, FAINT, top_right);
    }
}

// ------------------------------------------------------------------ help / OTA / settings

void draw_help() {
    int w = 860, h = 640, x = (W - w) / 2, y = (H - h) / 2;
    hit(0, 0, W, H, H_CLOSE);
    C.fillRoundRect(x, y, w, h, 10, PANEL);
    glow_rect(x, y, w, h, 10, ACCENT);
    font(F_BRAND);
    glow(x + 24, y + 22, "KEYS", ACCENT);
    static const char *rows[][2] = {
        {"1 - 7,  TAB,  LEFT RIGHT", "switch view"},
        {"ENTER   (tap a row twice)", "everything about the device / drone"},
        {"MAP:  + -  L  Z  O  UP DN", "zoom, layer, fit, follow drone, pan"},
        {"DETAIL:  E   G", "name the device, put it in a new group"},
        {"UP  DOWN   (Ctrl: page)", "select a node / detection / target"},
        {"/", "search; ENTER keeps it, ESC clears"},
        {"D   N", "domain (all / BLE / WiFi), node filter"},
        {"C   ESC", "clear filters"},
        {"R   [   ]", "metrics range 1H / 6H / 24H / 7D"},
        {"A", "acknowledge alerts"},
        {"X", "mute / unmute the selected device"},
        {"M", "silence all alerts (banner + sound)"},
        {"V", "alert sound on / off"},
        {"B   F", "brightness, flip the screen"},
        {"S", "setup: source, Wi-Fi, dashboard, token, OTA"},
        {"TOUCH", "tap anything; drag lists to scroll"},
    };
    int yy = y + 76;
    for (auto &r : rows) {
        font(F_MD);
        text(x + 24, yy, r[0], SOFT);
        text(x + 330, yy, r[1], TEXT);
        yy += 32;
    }
    font(F_SM);
    char b[96];
    snprintf(b, sizeof b, "firmware %s   %s   OTA %s", FW_VERSION, S.hostname.c_str(), net_ota_enabled() ? "on" : "off (set an OTA password)");
    text(x + 24, y + h - 26, b, FAINT);
}

void tint_rect(int x, int y, int w, int h, uint16_t col, int a);

// Behind the update: a field of ASCII characters morphing like plasma, dim, its colours
// drifting through the theme's gradient (teal -> sky -> purple -> pink).
uint16_t mix565(uint16_t a, uint16_t b, float t) {
    int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    return ((int)(ar + (br - ar) * t) << 11) | ((int)(ag + (bg - ag) * t) << 5) | (int)(ab + (bb - ab) * t);
}

// A morphing ASCII blob: four metaballs on slow Lissajous orbits merge and split; the
// characters get denser toward its core and the colour runs deep blue -> violet ->
// purple -> pink -> hot pink with the density, over a faint blue texture.
uint16_t blob_color(float v) {
    static const uint16_t STOPS[] = {rgb(0x1d4ed8), rgb(0x3b82f6), rgb(0x7c3aed), rgb(0xa855f7), rgb(0xec4899), rgb(0xff4fd8)};
    const int ns = sizeof(STOPS) / sizeof(STOPS[0]);
    v = constrain(v, 0.0f, 1.0f) * (ns - 1);
    int i = min((int)v, ns - 2);
    return mix565(STOPS[i], STOPS[i + 1], v - i);
}

void ascii_plasma(float t, float fade) {
    static const char RAMP[] = " .,-~:;=+*x#%@";
    const int nr = sizeof(RAMP) - 2;
    const int CW = 14, CH = 20, GW = W / CW + 1, GH = H / CH + 1;
    // Ball centres (grid cells) and radii.
    float bx[4], by[4], br[4];
    for (int k = 0; k < 4; k++) {
        float ph = k * 1.7f;
        bx[k] = GW * (0.5f + 0.32f * sinf(t * (0.61f + 0.13f * k) + ph));
        by[k] = GH * (0.5f + 0.30f * cosf(t * (0.47f + 0.11f * k) + ph * 1.3f));
        br[k] = 6.5f + 2.5f * sinf(t * 0.9f + k);
    }
    font(F_SM);
    C.setTextDatum(middle_center);
    char one[2] = {0, 0};
    for (int gy = 0; gy < GH; gy++) {
        for (int gx = 0; gx < GW; gx++) {
            float f = 0;
            for (int k = 0; k < 4; k++) {
                float dx = gx - bx[k], dy = (gy - by[k]) * 1.45f;   // cells are taller than wide
                f += br[k] * br[k] / (dx * dx + dy * dy + 1.0f);
            }
            // Faint drifting texture between the blobs.
            float tex = 0.5f + 0.5f * sinf(gx * 0.31f + t * 0.8f) * sinf(gy * 0.43f - t * 0.6f);
            float v = f * 0.55f;              // ~1 at the blob's surface
            float u = v > 1 ? 1 : v;
            float a = max(u, tex * 0.18f);
            int ci = (int)(a * nr + 0.5f);
            if (ci <= 0) continue;
            float colorPos = v > 1 ? 0.55f + 0.45f * min(1.0f, (v - 1) * 0.6f) : v * 0.55f;
            uint16_t col = dim(blob_color(colorPos), (0.18f + 0.62f * u) * fade);
            one[0] = RAMP[min(ci, nr)];
            C.setTextColor(col);
            C.drawString(one, gx * CW + CW / 2, gy * CH + CH / 2);
        }
    }
}

// ---- the colonelpanic.tech wordmark (the website's ascii-wordmark.js, ported) ----
// Letterforms as a window onto a live ASCII field: a flowing gradient that cross-fades
// into Chladni plate figures, coloured pink > purple > blue > purple, a white scan line
// and the odd glitched row. The blob condenses into it, it holds, then dissolves back.
constexpr int WM_CW = 6, WM_CH = 9;                       // fine cells (Font0, 6x8)
constexpr int WM_COLS = W / WM_CW, WM_ROWS = 34;          // the wordmark band: 34 rows
constexpr int WM_Y0 = 150;                                // its top on screen
uint8_t *wmMask = nullptr;                                // WM_ROWS x WM_COLS: 1 inside a letter
float wmSize = 1;                                         // the name's text size (wm_build)

void wm_build() {
    if (wmMask) return;
    wmMask = (uint8_t *)heap_caps_calloc(WM_ROWS * WM_COLS, 1, MALLOC_CAP_SPIRAM);
    M5Canvas mk;
    mk.setColorDepth(1);
    mk.setPsram(true);
    if (!wmMask || !mk.createSprite(W, WM_ROWS * WM_CH)) return;
    mk.fillSprite(0);
    mk.setFont(&fonts::Orbitron_Light_32);
    mk.setTextColor(1);
    // The whole name on one line, as large as the screen width allows.
    const char *name = "COLONELPANIC.TECH";
    float sz = 1;
    for (float z = 6; z >= 1; z -= 0.05f) {
        mk.setTextSize(z);
        if (mk.textWidth(name) <= W - 60 && mk.fontHeight() <= WM_ROWS * WM_CH - 6) { sz = z; break; }
    }
    mk.setTextSize(sz);
    wmSize = sz;
    mk.setTextDatum(middle_center);
    mk.drawString(name, W / 2, WM_ROWS * WM_CH / 2);
    for (int r = 0; r < WM_ROWS; r++)
        for (int c = 0; c < WM_COLS; c++) {
            int hitN = 0;   // a cell counts when most of its samples are ink
            for (int sy = 2; sy < WM_CH; sy += 3)
                for (int sx = 1; sx < WM_CW; sx += 2) hitN += mk.readPixel(c * WM_CW + sx, r * WM_CH + sy) ? 1 : 0;
            wmMask[r * WM_COLS + c] = hitN >= 4;
        }
    mk.deleteSprite();
}

float smoothf(float u) { u = constrain(u, 0.0f, 1.0f); return u * u * (3 - 2 * u); }

// pink > purple > blue > purple > pink, the website's stops
uint16_t wm_loop(float p, float gain, float white) {
    static const float ST[3][3] = {{255, 43, 214}, {176, 92, 255}, {61, 130, 255}};
    static const int SEG[4][2] = {{0, 1}, {1, 2}, {2, 1}, {1, 0}};
    float q = (p - floorf(p)) * 4;
    int si = min(3, (int)q);
    float f = q - si;
    const float *A = ST[SEG[si][0]], *B = ST[SEG[si][1]];
    float rgbv[3];
    for (int k = 0; k < 3; k++) {
        float v = (A[k] + (B[k] - A[k]) * f) * gain;
        rgbv[k] = v + (255 - v) * white;
    }
    return lgfx::color565(rgbv[0], rgbv[1], rgbv[2]);
}

void wm_draw(float t, float show) {
    if (!wmMask || show <= 0.01f) return;
    static const char RAMPW[] = ".:-=+*#%@";
    const int R = sizeof(RAMPW) - 1;
    // 0 = flowing gradient, 1 = plate figures (4 s flow, 1.5 s fade, 4 s plate, 1.5 s fade)
    float u = fmodf(t, 11.0f), mix;
    if (u < 4) mix = 0; else if (u < 5.5f) mix = smoothf((u - 4) / 1.5f);
    else if (u < 9.5f) mix = 1; else mix = 1 - smoothf((u - 9.5f) / 1.5f);
    float pn = (1.2f + 0.45f * sinf(t * 0.13f)) * PI, pm = (2.5f + 0.6f * cosf(t * 0.097f)) * PI;
    int scanRow = (int)fmodf(t * 9, WM_ROWS + 30);
    static float glUntil = 0, glNext = 0;
    static int glR0 = 0, glR1 = 0, glDx = 0;
    if (t > glNext) {
        glR0 = esp_random() % WM_ROWS;
        glR1 = glR0 + 1 + esp_random() % 4;
        glDx = ((esp_random() & 1) ? 1 : -1) * (1 + esp_random() % 4);
        glUntil = t + 0.09f + (esp_random() % 100) / 1000.0f;
        glNext = t + 1.8f + (esp_random() % 400) / 100.0f;
    }
    bool glitching = t < glUntil;
    C.setFont(&fonts::Font0);
    C.setTextDatum(top_left);
    char one[2] = {0, 0};
    for (int y = 0; y < WM_ROWS; y++) {
        for (int x = 0; x < WM_COLS; x++) {
            if (!wmMask[y * WM_COLS + x]) continue;
            uint32_t hsh = (uint32_t)(x * 73856093u ^ y * 19349663u);
            float jit = (hsh % 1000) / 1000.0f;
            // The reveal: cells fill in left to right as `show` rises.
            float front = show * 1.25f - (x / (float)WM_COLS) * 0.8f - jit * 0.2f;
            if (front < 0) continue;
            float v = 0.8f + 0.14f * sinf(x * 0.09f - t * 1.4f + y * 0.35f) + (jit - 0.5f) * 0.14f;
            if (mix > 0) {
                float X = (x - WM_COLS / 2.0f) / WM_ROWS, Y = (y - WM_ROWS / 2.0f) / WM_ROWS;
                X += 0.12f * sinf(Y * 3.1f + t * 0.5f);
                float wave = cosf(pn * X) * cosf(pm * Y) - cosf(pm * X) * cosf(pn * Y);
                float line = 1 - fminf(1, fabsf(wave) * 1.25f);
                float plate = 0.16f + 0.84f * smoothf(line) + (jit - 0.5f) * 0.06f;
                v += (plate - v) * mix;
            }
            v = constrain(v, 0.0f, 1.0f);
            bool white = y == scanRow || front < 0.04f;
            int glyph = min(R - 1, (int)(v * (R - 1) + 0.5f));
            if ((esp_random() % 100) < 2 || front < 0.04f) glyph = 3 + esp_random() % (R - 3);
            float hp = (x / (float)WM_COLS) * 1.4f + t * 0.05f + v * 0.2f * mix;
            int dx = 0;
            if (glitching && y >= glR0 && y < glR1) dx = glDx * WM_CW, hp += 0.5f;
            float gain = (0.35f + 0.65f * v) * fminf(1, front * 3);
            one[0] = RAMPW[glyph];
            C.setTextColor(wm_loop(hp, gain, white ? 0.85f : 0));
            C.drawString(one, x * WM_CW + dx, WM_Y0 + y * WM_CH);
        }
    }
    // Once it has formed: the name itself, crisp, over the moving field (a dark outline
    // keeps the edges clean against the glyphs), so it always reads.
    if (show > 0.75f) {
        float a = smoothf((show - 0.75f) / 0.25f);
        C.setFont(&fonts::Orbitron_Light_32);
        C.setTextSize(wmSize);
        C.setTextDatum(middle_center);
        int cx = W / 2, cy = WM_Y0 + WM_ROWS * WM_CH / 2;
        C.setTextColor(dim(BG, 1.0f));
        for (int ox = -2; ox <= 2; ox += 2)
            for (int oy = -2; oy <= 2; oy += 2)
                if (ox || oy) C.drawString("COLONELPANIC.TECH", cx + ox, cy + oy);
        C.setTextColor(dim(wm_loop(t * 0.05f, 1.0f, 0.35f), a));
        C.drawString("COLONELPANIC.TECH", cx, cy);
        C.setTextSize(1);
    }
}

// The OUI-SPY eye (colonelpanic.tech hero art): glyph density from the art, the site's
// pink > purple > blue gradient sweeping across it, revealed and dissolved like the mark.
void eye_draw(float t, float show) {
    if (show <= 0.01f) return;
    static const char RAMPE[] = " .:-=+*#%@";
    const int R = sizeof(RAMPE) - 1;
    int x0 = (W - EYE_COLS * WM_CW) / 2, y0 = 28;
    int scanRow = (int)fmodf(t * 9, EYE_ROWS + 30);
    C.setFont(&fonts::Font0);
    C.setTextDatum(top_left);
    char one[2] = {0, 0};
    for (int y = 0; y < EYE_ROWS; y++) {
        for (int x = 0; x < EYE_COLS; x++) {
            int d = EYE_ART[y * EYE_COLS + x];
            if (d < 30) continue;
            uint32_t hsh = (uint32_t)(x * 2654435761u ^ y * 40503u);
            float jit = (hsh % 1000) / 1000.0f;
            float front = show * 1.25f - (fabsf(x - EYE_COLS / 2.0f) / EYE_COLS) * 1.1f - jit * 0.2f;   // opens from the centre
            if (front < 0) continue;
            float v = d / 255.0f;
            v = constrain(v * (0.85f + 0.15f * sinf(x * 0.11f - t * 1.6f + y * 0.3f)), 0.0f, 1.0f);
            int glyph = min(R - 1, (int)(v * (R - 1) + 0.5f));
            if ((esp_random() % 100) < 2) glyph = 3 + esp_random() % (R - 3);
            float hp = (x / (float)EYE_COLS) * 1.2f - t * 0.06f;
            bool white = y == scanRow || front < 0.04f;
            float gain = (0.3f + 0.7f * v) * fminf(1, front * 3);
            one[0] = RAMPE[glyph];
            C.setTextColor(wm_loop(hp, gain, white ? 0.85f : 0));
            C.drawString(one, x0 + x * WM_CW, y0 + y * WM_CH);
        }
    }
}

// The site's moving background: the OUI-SPY board art, tiled and drifting slowly up and
// across, dim, in the pink > purple > blue loop.
void boards_draw(float t, float gain0) {
    static const char RAMPB[] = " .:-=+*#%@";
    const int R = sizeof(RAMPB) - 1;
    const int TW = BOARDS_COLS + 14;                       // tile width with a gap
    // Zoomed in 2x: big glyphs (12 x 18 cells), the art reads as a picture behind the mark.
    const int ZW = WM_CW * 2, ZH = WM_CH * 2;
    int ox = (int)(t * 1.2f) % TW, oy = (int)(t * 1.8f) % BOARDS_ROWS;
    const int cols = W / ZW + 1, rows = H / ZH + 1;
    C.setFont(&fonts::Font0);
    C.setTextSize(2);
    C.setTextDatum(top_left);
    char one[2] = {0, 0};
    for (int y = 0; y < rows; y++) {
        int ay = (y + oy) % BOARDS_ROWS;
        for (int x = 0; x < cols; x++) {
            int ax = (x + ox) % TW;
            if (ax >= BOARDS_COLS) continue;
            int d = BOARDS_ART[ay * BOARDS_COLS + ax];
            if (d < 40) continue;
            float v = d / 255.0f;
            int glyph = min(R - 1, (int)(v * (R - 1) + 0.5f));
            float hp = (x / (float)cols) * 0.9f + (y / (float)rows) * 0.3f + t * 0.03f;
            one[0] = RAMPB[glyph];
            C.setTextColor(wm_loop(hp, gain0 * (0.45f + 0.55f * v), 0));
            C.drawString(one, x * ZW, y * ZH);
        }
    }
    C.setTextSize(1);
}

void draw_ota() {
    wm_build();
    ppa_fill(0, 0, W, H, BG);
    // Time from the start of this update, so the art begins at once: the wordmark condenses
    // in over the drifting board art, then morphs into the OUI-SPY eye and back (20 s loop).
    static uint32_t t0 = 0, lastMs = 0;
    uint32_t ms = millis();
    if (!t0 || ms - lastMs > 3000) t0 = ms;
    lastMs = ms;
    float t = (ms - t0) / 1000.0f;
    float u = fmodf(t, 20.0f);
    float eye = u < 8 ? 0 : u < 10.5f ? smoothf((u - 8) / 2.5f) : u < 17.5f ? 1 : 1 - smoothf((u - 17.5f) / 2.5f);
    float mark = (t < 2.5f ? smoothf(t / 2.5f) : 1.0f) * (1 - eye);
    boards_draw(t, 0.30f);
    wm_draw(t, mark);
    eye_draw(t, eye);
    // Progress at the bottom, on a dark plate.
    int bw = 700, bx = (W - bw) / 2, by = H - 92;
    tint_rect(bx - 40, by - 34, bw + 80, 130, BG, 210);
    font(F_MD);
    glow(W / 2, by - 14, "FIRMWARE UPDATE", ACCENT, middle_center);
    C.fillRoundRect(bx, by, bw, 20, 10, PANEL2);
    C.fillRoundRect(bx, by, bw * A.otaPct / 100, 20, 10, ACCENT);
    glow_rect(bx, by, bw, 20, 10, dim(ACCENT, 0.7f));
    font(F_LG);
    char p[8];
    snprintf(p, sizeof p, "%u%%", A.otaPct);
    text(W / 2, by + 44, p, TEXT, top_center);
}

// Settings editor. Rows: 0 = data source, 1..NF = text fields, NF+1 = save.
struct Field {
    const char *label;
    String *val;
    bool secret;
    bool numeric;
};
String eSsid, ePass, eHost, ePort, eToken, eOta, eTz, eName;
uint8_t eSource = SRC_AUTO;
Field FIELDS[] = {
    {"Wi-Fi network (tap one on the right)", &eSsid, false, false}, {"Wi-Fi password", &ePass, true, false},
    {"Dashboard address (blank = find it on the network)", &eHost, false, false}, {"Port", &ePort, false, true},
    {"Access token (off-LAN only)", &eToken, true, false}, {"OTA password (empty = OTA off)", &eOta, true, false},
    {"Time zone (POSIX)", &eTz, false, false}, {"Hostname", &eName, false, false},
};
constexpr int NF = sizeof(FIELDS) / sizeof(FIELDS[0]);
constexpr int ROW_SRC = 0, ROW_SAVE = NF + 1;
int fsel = 0;
bool inList = false;
int listSel = 0, netTop = 0;
bool reveal = false;
bool soundPage = false;   // setup: the SOUND page instead of the network list
String scanNames[16];
int scanRssi[16];
static const char *SRC_NAMES[] = {"AUTO", "NETWORK", "USB ONLY"};
static const char *SRC_HELP[] = {"USB node first, else Wi-Fi", "dashboard over Wi-Fi", "no network feed"};

void settings_open() {
    eSsid = S.ssid, ePass = S.pass, eHost = S.host, ePort = String(S.port), eToken = S.token;
    eOta = S.otaPass, eTz = S.tz, eName = S.hostname, eSource = S.source;
    fsel = S.ssid.length() ? 3 : 1;
    inList = false;
    A.settings = true;
    net_request_scan();
}

void settings_commit() {
    eSsid.trim(), eHost.trim(), eName.trim(), eTz.trim();
    S.ssid = eSsid, S.pass = ePass, S.host = eHost, S.port = ePort.toInt() ? ePort.toInt() : 5077;
    S.token = eToken, S.otaPass = eOta, S.tz = eTz, S.hostname = eName.length() ? eName : String("ouispy-tab5");
    S.source = eSource;
    settings_save();
    font(F_BRAND_L);
    C.fillScreen(BG);
    glow(W / 2, H / 2, "SAVED  RESTARTING", ACCENT, middle_center);
    C.pushSprite(0, 0);
    delay(600);
    ESP.restart();
}

constexpr int SF_X = PAD + 20, SF_Y = 92, SF_H = 56, SF_W = 700;
constexpr int SL_X = SF_X + SF_W + 40, SL_W = W - PAD - 20 - SL_X;
constexpr int NET_Y = SF_Y + 40, NET_RH = 40, NET_ROWS = 10;

// ------------------------------------------------------------------ proximity

// The device's signal as heard by our own USB node when it heard it (we carry that one),
// else by the closest node; INT16_MIN when it has gone quiet (20 s).
int prox_rssi(const char **by) {
    const Target *t = target_by_mac(A.prox);
    if (!t || !t->nseen || server_now() - t->last > 20) return INT16_MIN;
    const char *us = usb_node_id();
    int k = 0;
    for (int i = 0; us[0] && i < t->nseen; i++)
        if (!strcmp(t->seen[i].node, us)) { k = i; break; }
    if (by) *by = t->seen[k].node;
    return t->seen[k].rssi;
}
namespace {
uint32_t proxPulse = 0;   // millis() of the last pulse
uint8_t proxR, proxG, proxB;
}
// Pulse rate and pitch from the signal: -95 dBm slow and low, -35 dBm fast and high.
void prox_loop() {
    if (!A.prox[0]) return;
    int r = prox_rssi();
    float c = r == INT16_MIN ? 0 : constrain((r + 95) / 60.0f, 0.0f, 1.0f);   // 0 far .. 1 close
    uint32_t every = r == INT16_MIN ? 2500 : (uint32_t)(1400 - 1250 * c);
    if (millis() - proxPulse < every) return;
    proxPulse = millis();
    // far red, then amber, close green: always bright enough to see
    proxR = c < 0.5f ? 255 : (uint8_t)(255 * (1 - c) * 2);
    proxG = r == INT16_MIN ? 0 : c < 0.5f ? (uint8_t)(255 * c * 2) : 255;
    proxB = 0;
    if (S.proxBeep && r != INT16_MIN) sfx_tick(500 + (int)(2000 * c), 30);
}
bool prox_led_now(uint8_t &r, uint8_t &g, uint8_t &b) {
    if (!A.prox[0] || millis() - proxPulse > 90) return false;
    r = proxR, g = proxG, b = proxB;
    return true;
}

// Setup, SOUND page: which events make a sound, the volume, and a test of each.
void draw_sound_page(int x, int y, int w, int h) {
    panel(x, y, w, h, "SOUND");
    struct Row { const char *label; bool *v; int test; };
    Row rows[] = {
        {"ALL SOUNDS", &S.sound, -1},
        {"DETECTIONS", &S.sndDet, SFX_DETECT},
        {"DRONES", &S.sndDrone, SFX_DRONE},
        {"NODES DOWN / BACK", &S.sndNode, SFX_NODE_DOWN},
        {"PROXIMITY BEEP", &S.proxBeep, -1},
    };
    int ry = y + 44;
    for (int i = 0; i < 5; i++, ry += 54) {
        bool on = *rows[i].v;
        font(F_MD);
        text(x + 18, ry + 15, rows[i].label, i && !S.sound ? FAINT : TEXT, middle_left);
        int cw;
        font(F_SM);
        int bx = x + w - 18 - (C.textWidth(on ? "ON" : "OFF") + 28);
        chip(bx, ry, on ? "ON" : "OFF", on ? rgb(0x39ff88) : FAINT, on, &cw, H_SND, i);
        if (rows[i].test >= 0) {
            int tw2 = C.textWidth("TEST") + 28;
            chip(bx - 10 - tw2, ry, "TEST", ACCENT, false, &cw, H_SND, 100 + rows[i].test);
        }
    }
    font(F_MD);
    text(x + 18, ry + 15, "VOLUME", TEXT, middle_left);
    static const uint8_t vols[] = {20, 40, 60, 80, 100};
    font(F_SM);
    int bx = x + w - 18;
    for (int i = 4; i >= 0; i--) {
        char v[8];
        snprintf(v, sizeof v, "%u", vols[i]);
        int cw = C.textWidth(v) + 28;
        bx -= cw;
        chip(bx, ry, v, AMBER, S.vol == vols[i], nullptr, H_SND, 200 + vols[i]);
        bx -= 8;
    }
    font(F_SM);
    text(x + 18, y + h - 20, "Proximity: open a device's details and tap PROXIMITY. The keyboard's right LED", FAINT, middle_left);
    text(x + 18, y + h - 2, "pulses faster and greener as it gets closer.", FAINT, middle_left);
}

void draw_settings() {
    C.fillScreen(BG);
    font(F_BRAND_L);
    glow(PAD + 20, 26, "SETUP", ACCENT);
    int bx = W - PAD - 20, w;
    if (settings_ready()) {
        font(F_SM);
        w = C.textWidth("BACK") + 28;
        bx -= w;
        chip(bx, 28, "BACK", ACCENT, false, nullptr, H_BACK);
        bx -= 10;
    }
    font(F_SM);
    w = C.textWidth(reveal ? "HIDE SECRETS" : "SHOW SECRETS") + 28;
    bx -= w;
    chip(bx, 28, reveal ? "HIDE SECRETS" : "SHOW SECRETS", AMBER, reveal, nullptr, H_REVEAL);
    font(F_SM);
    w = C.textWidth("SOUND") + 28;
    bx -= w + 10;
    chip(bx, 28, "SOUND", rgb(0x39ff88), soundPage, nullptr, H_SOUNDPG);
    font(F_SM);
    text(bx - 20, 43, "tap a field or UP DN TAB   ENTER next   ESC back", FAINT, middle_right);

    for (int r = 0; r <= ROW_SAVE; r++) {
        int y = SF_Y + r * SF_H;
        bool on = fsel == r && !inList;
        if (r == ROW_SRC) {
            font(F_SM);
            text(SF_X, y, "Data source", on ? ACCENT : MUTED);
            int sx = SF_X;
            for (int k = 0; k < 3; k++) {
                int sw = 140;
                bool cur = eSource == k;
                hit(sx, y + 14, sw, 40, H_SRC, k);
                C.fillRoundRect(sx, y + 18, sw, 32, 8, cur ? dim(ACCENT, 0.22f) : PANEL);
                if (cur) glow_rect(sx, y + 18, sw, 32, 8, on ? ACCENT : dim(ACCENT, 0.7f));
                else C.drawRoundRect(sx, y + 18, sw, 32, 8, LINE2);
                font(F_MD);
                text(sx + sw / 2, y + 35, SRC_NAMES[k], cur ? ACCENT : MUTED, middle_center);
                sx += sw + 10;
            }
            font(F_SM);
            text(sx + 4, y + 35, fit(SRC_HELP[eSource], SF_X + SF_W - sx - 4), FAINT, middle_left);
            continue;
        }
        if (r == ROW_SAVE) {
            int sw = 300;
            hit(SF_X, y + 4, sw, 50, H_SAVE);
            C.fillRoundRect(SF_X, y + 8, sw, 42, 8, on ? dim(ACCENT, 0.25f) : dim(ACCENT, 0.08f));
            glow_rect(SF_X, y + 8, sw, 42, 8, on ? ACCENT : dim(ACCENT, 0.6f));
            font(F_MD);
            glow(SF_X + sw / 2, y + 30, "SAVE AND RESTART", ACCENT, middle_center);
            break;
        }
        const Field &f = FIELDS[r - 1];
        hit(SF_X, y, SF_W, SF_H, H_FIELD, r);
        font(F_SM);
        text(SF_X, y, f.label, on ? ACCENT : MUTED);
        C.fillRoundRect(SF_X, y + 18, SF_W, 34, 6, PANEL);
        if (on) glow_rect(SF_X, y + 18, SF_W, 34, 6, ACCENT);
        else C.drawRoundRect(SF_X, y + 18, SF_W, 34, 6, LINE2);
        String shown = *f.val;
        if (f.secret && !reveal) {
            shown = "";
            for (unsigned k = 0; k < f.val->length(); k++) shown += '*';
        }
        if (on && (millis() / 500) % 2) shown += "_";
        font(F_MD);
        text(SF_X + 12, y + 36, fit(shown.c_str(), SF_W - 24), TEXT, middle_left);
    }

    // Networks: tap to use, drag to scroll. (Or the SOUND page in their place.)
    int ph = NF * SF_H + 40;
    if (soundPage) {
        draw_sound_page(SL_X, SF_Y, SL_W, ph);
        return;
    }
    panel(SL_X, SF_Y, SL_W, ph, "NETWORKS NEARBY");
    font(F_SM);
    w = C.textWidth("RESCAN") + 28;
    chip(SL_X + SL_W - w - 12, SF_Y + 6, "RESCAN", ACCENT, false, nullptr, H_RESCAN);
    int n = net_scan_results(scanNames, scanRssi, 16);
    if (n < 0) {
        font(F_MD);
        text(SL_X + 16, NET_Y + 10, "scanning...", FAINT);
    }
    netTop = constrain(netTop, 0, max(0, n - NET_ROWS));
    scroll_area(SL_X, NET_Y, SL_W, NET_ROWS * NET_RH, L_NETS, NET_RH, NET_ROWS);
    for (int k = 0; k < NET_ROWS; k++) {
        int i = netTop + k;
        if (i >= n) break;
        int y = NET_Y + k * NET_RH;
        bool on = inList && listSel == i;
        bool cur = scanNames[i] == eSsid;
        hit(SL_X, y, SL_W, NET_RH, H_NET, i);
        if (on || cur) {
            C.fillRect(SL_X + 4, y + 2, SL_W - 8, NET_RH - 4, dim(ACCENT, on ? 0.18f : 0.10f));
            C.fillRect(SL_X + 4, y + 2, 3, NET_RH - 4, ACCENT);
        }
        font(F_MD);
        text(SL_X + 18, y + NET_RH / 2, fit(scanNames[i].c_str(), SL_W - 110), cur ? ACCENT : TEXT, middle_left);
        char r[8];
        snprintf(r, sizeof r, "%d", scanRssi[i]);
        text(SL_X + SL_W - 16, y + NET_RH / 2, r, rssi_color(scanRssi[i]), middle_right);
    }
    font(F_SM);
    if (n > NET_ROWS) {
        char more[32];
        snprintf(more, sizeof more, "%d-%d of %d   drag to scroll", netTop + 1, min(n, netTop + NET_ROWS), n);
        text(SL_X + 16, SF_Y + ph - 22, more, FAINT);
    }
    text(SF_X, H - 26,
         "Stored on this device only. On the dashboard's LAN no token is needed; over Tailscale paste the token from home-dashboard/access_token.",
         FAINT);
}

void pick_network(int i) {
    int n = net_scan_results(scanNames, scanRssi, 16);
    if (i < 0 || i >= n) return;
    eSsid = scanNames[i];
    inList = false;
    fsel = 2;   // straight to the password
}

void settings_key(uint8_t kc, uint8_t mod, char ch) {
    bool ctrl = mod & 0x11;
    if (kc == 0x29) {   // Esc
        if (inList) inList = false;
        else if (settings_ready()) A.settings = false;
        return;
    }
    if (ctrl && (ch == 'v' || ch == 'V')) {
        reveal = !reveal;
        return;
    }
    if (inList) {
        int n = net_scan_results(scanNames, scanRssi, 16);
        if (kc == 0x52) listSel = max(0, listSel - 1);
        else if (kc == 0x51) listSel = min(max(0, n - 1), listSel + 1);
        else if (kc == 0x50) inList = false;
        else if (ch == 'r' || ch == 'R') net_request_scan();
        else if (kc == 0x28) pick_network(listSel);
        if (listSel < netTop) netTop = listSel;
        if (listSel >= netTop + NET_ROWS) netTop = listSel - NET_ROWS + 1;
        return;
    }
    if (kc == 0x52) fsel = max(0, fsel - 1);
    else if (kc == 0x51 || kc == 0x2B) fsel = min((int)ROW_SAVE, fsel + 1);
    else if (fsel == ROW_SRC && (kc == 0x50 || kc == 0x4F)) eSource = (eSource + (kc == 0x4F ? 1 : 2)) % 3;
    else if (kc == 0x4F && fsel == 1) inList = true, listSel = netTop;
    else if (kc == 0x28) {
        if (fsel == ROW_SAVE) settings_commit();
        else fsel++;
    } else if (fsel >= 1 && fsel <= NF) {
        String &v = *FIELDS[fsel - 1].val;
        if (kc == 0x2A || kc == 0x4C) {
            if (v.length()) v.remove(v.length() - 1);
        } else if (ch >= 0x20 && ch < 0x7f && v.length() < 96) {
            if (!FIELDS[fsel - 1].numeric || isdigit(ch)) v += ch;
        }
    }
}

// ------------------------------------------------------------------ actions

void cycle_node() {
    if (!M->nnodes) return;
    int cur = -1;
    for (int i = 0; i < M->nnodes; i++)
        if (!strcmp(M->nodes[i].id, A.node)) cur = i;
    if (cur + 1 >= M->nnodes) A.node[0] = 0;
    else strlcpy(A.node, M->nodes[cur + 1].id, sizeof A.node);
    A.metricsGen++;
}

void dispatch_drone_map() {
    if (!nDrone) return;
    const Drone &d = M->drones[droneIdx[A.sel[V_DRONES]]];
    if (d.hasPos) map_center_on(d.lat, d.lon);
    else if (d.hasPilot) map_center_on(d.plat, d.plon);
    else return;
    A.mapZoom = max((int)A.mapZoom, 17);
    A.view = V_MAP;
}

const char *selected_mac() {
    if (A.view == V_LIVE && nLive && liveIdx[A.sel[V_LIVE]] >= 0) return det_at(liveIdx[A.sel[V_LIVE]])->mac;
    if (A.view == V_TARGETS && nTgt) return M->targets[tgtIdx[A.sel[V_TARGETS]]].mac;
    if (M->hasAlert) return M->lastAlert.mac;
    return nullptr;
}

// The panel stays in its native portrait orientation and the canvas is rotated instead:
// M5GFX pushes an unrotated sprite with one memcpy per line, while a rotated panel copies
// pixel by pixel down PSRAM columns (~800 ms a frame). Touch therefore arrives in panel
// coordinates and is mapped here. Landscape with the keyboard edge at the bottom; F flips it.
constexpr int PW = 720, PH = 1280;   // panel, native
uint8_t canvasRot() { return S.flip ? 1 : 3; }

void apply_rotation() {
    M5.Display.setRotation(0);
    C.setRotation(canvasRot());
}

void to_canvas(int &x, int &y) {
    int px = x, py = y;
    if (canvasRot() == 3) x = PH - 1 - py, y = px;
    else x = py, y = PW - 1 - px;
}

Hit flashHit;            // the control a tap just fired: shown filled for a moment
uint32_t flashUntil = 0;
volatile bool pressPaint = false;   // a press arrived: paint its pressed state right away

// Copy one logical rectangle of the canvas to the panel: the pressed state of a button
// goes out in a few ms instead of waiting for a whole frame (what LVGL calls a partial
// refresh). Canvas and panel share pixel format and orientation, so it's row memcpys.
void push_rect(int x, int y, int w, int h) {
    x = max(0, x), y = max(0, y);
    w = min(W - x, w), h = min(H - y, h);
    if (w <= 0 || h <= 0) return;
    int px0, py0, pw, ph;
    if (canvasRot() == 3) px0 = y, pw = h, py0 = PH - (x + w), ph = w;
    else px0 = PW - (y + h), pw = h, py0 = x, ph = w;
    const lgfx::rgb565_t *buf = (const lgfx::rgb565_t *)C.getBuffer();
    M5.Display.startWrite();
    for (int r = 0; r < ph; r++) M5.Display.pushImage(px0, py0 + r, pw, 1, buf + (size_t)(py0 + r) * PW + px0);
    M5.Display.endWrite();
}

// Blend a logical rectangle of the canvas toward `col` (a = 0..256): a see-through fill, so
// the label under a pressed button stays readable and simply lights up.
void tint_rect(int x, int y, int w, int h, uint16_t col, int a) {
    x = max(0, x), y = max(0, y);
    w = min(W - x, w), h = min(H - y, h);
    if (w <= 0 || h <= 0) return;
    int px0, py0, pw, ph;
    if (canvasRot() == 3) px0 = y, pw = h, py0 = PH - (x + w), ph = w;
    else px0 = PW - (y + h), pw = h, py0 = x, ph = w;
    uint16_t *buf = (uint16_t *)C.getBuffer();   // rgb565, native byte order
    int cr = col >> 11, cg = (col >> 5) & 63, cb = col & 31, ia = 256 - a;
    for (int r = 0; r < ph; r++) {
        uint16_t *p = buf + (size_t)(py0 + r) * PW + px0;
        for (int k = 0; k < pw; k++, p++) {
            uint16_t v = *p;
            int pr = ((v >> 11) * ia + cr * a) >> 8, pg = (((v >> 5) & 63) * ia + cg * a) >> 8,
                pb = ((v & 31) * ia + cb * a) >> 8;
            *p = (pr << 11) | (pg << 5) | pb;
        }
    }
}

void paint_pressed(const Hit &h) {
    tint_rect(h.x, h.y, h.w, h.h, ACCENT, 80);
    glow_rect(h.x, h.y, h.w, h.h, 8, ACCENT);
}

}  // namespace

// ------------------------------------------------------------------ public

bool prox_led(uint8_t &r, uint8_t &g, uint8_t &b) { return prox_led_now(r, g, b); }

uint32_t ui_drawMs = 0, ui_pushMs = 0;
uint32_t ui_prof[12] = {};   // last frame, us: clear, view, top, bottom, -, view id, map: -, tiles, rings, targets+nodes, drones, chrome

void ui_dirty() { dirty = true; }

void ui_screenshot(Stream &out) {
    model_lock();
    const uint8_t *buf = (const uint8_t *)C.getBuffer();
    size_t n = (size_t)PW * PH * 2;
    uint32_t sum = 0;
    for (size_t i = 0; buf && i < n; i += 97) sum += buf[i];
    out.printf("SHOT %d %d %u %d\n", PW, PH, (unsigned)sum, canvasRot());
    static uint8_t chunk[2048];   // internal RAM bounce: write PSRAM through a copy
    for (size_t off = 0; buf && off < n; off += sizeof chunk) {
        size_t k = min(sizeof chunk, n - off);
        memcpy(chunk, buf + off, k);
        for (size_t w = 0; w < k;) {   // HWCDC returns short writes when the host lags
            size_t r = out.write(chunk + w, k - w);
            if (!r) delay(1);
            w += r;
        }
    }
    out.flush();
    model_unlock();
}

void ui_init() {
    apply_rotation();
    M5.Display.setBrightness(S.bright * 255 / 100);
    setenv("TZ", S.tz.c_str(), 1);
    tzset();
    // The DSI framebuffer's own pixel format: the push is then a plain copy, no conversion.
    C.setColorDepth(lgfx::color_depth_t::rgb565_nonswapped);
    // Our own buffer, cache-line aligned: the PPA (DMA) fills it, blits tiles into it and
    // pushes it to the display's framebuffer.
    void *cbuf = heap_caps_aligned_alloc(128, (size_t)PW * PH * 2, MALLOC_CAP_SPIRAM);
    if (cbuf) C.setBuffer(cbuf, PW, PH);
    else {
        C.setPsram(true);
        if (!C.createSprite(PW, PH)) Serial.println("canvas alloc failed");
    }
    fb = (uint16_t *)((lgfx::Panel_DSI *)M5.Display.getPanel())->config_detail().buffer;
    bool ppaUp = gfx_init();
    Serial.printf("ppa %s, framebuffer %p\n", ppaUp && fb && cbuf ? "on" : "OFF", fb);
    apply_rotation();   // now the canvas reads 1280 x 720
    if (!settings_ready()) {
        settings_open();
        A.autoSetup = true;   // closes by itself if a node shows up on USB
    }
}

void ui_key(uint8_t kc, uint8_t mod, char ch);

void ui_key(uint8_t kc, uint8_t mod) {
    char ch = m5::unit::tab5_keyboard::hidUsageToChar(kc, mod);
    dirty = true;
    model_lock();
    ui_key(kc, mod, ch);
    model_unlock();
}

void ui_key(uint8_t kc, uint8_t mod, char ch) {
    if (A.settings) {
        settings_key(kc, mod, ch);
        return;
    }
    bool shift = mod & 0x22, ctrl = mod & 0x11;
    if (A.help) {
        A.help = false;
        return;
    }
    if (A.wdOpen[0]) {
        if (kc == 0x29 || kc == 0x28) A.wdOpen[0] = 0;   // ESC / Enter closes it
        return;
    }
    if (A.nodeEdit[0]) {
        if (A.editNode) {
            size_t n = strlen(A.editBuf);
            if (kc == 0x29) A.editNode = false;
            else if (kc == 0x28) {
                JsonDocument j;
                j["alias"] = A.editBuf;
                char body[96];
                serializeJson(j, body, sizeof body);
                node_set(A.nodeEdit, body);
                A.editNode = false;
            } else if (kc == 0x2A || kc == 0x4C) { if (n) A.editBuf[n - 1] = 0; }
            else if (ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\' && n + 1 < 33) A.editBuf[n] = ch, A.editBuf[n + 1] = 0;
            return;
        }
        if (kc == 0x29) { A.nodeEdit[0] = 0; return; }
        if ((ch == 'e' || ch == 'E') && net_online()) {
            const Node *nd = edit_node();
            A.editNode = true;
            strlcpy(A.editBuf, nd ? nd->alias : "", sizeof A.editBuf);
        }
        return;
    }
    if (A.editDrone) {   // typing the selected drone's alias
        size_t n = strlen(A.editBuf);
        if (kc == 0x29) A.editDrone = false;
        else if (kc == 0x28) {
            if (nDrone) {
                Drone &d = M->drones[droneIdx[A.sel[V_DRONES]]];
                JsonDocument j;
                j["alias"] = A.editBuf;
                char body[96], path[48];
                serializeJson(j, body, sizeof body);
                snprintf(path, sizeof path, "/api/drones/%s", d.mac);
                if (net_post(path, body)) strlcpy(d.alias, A.editBuf, sizeof d.alias);
            }
            A.editDrone = false;
        } else if (kc == 0x2A || kc == 0x4C) { if (n) A.editBuf[n - 1] = 0; }
        else if (ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\' && n + 1 < 24) A.editBuf[n] = ch, A.editBuf[n + 1] = 0;
        return;
    }
    if (A.view == V_DRONES && (ch == 'e' || ch == 'E') && nDrone && net_online()) {
        A.editDrone = true;
        strlcpy(A.editBuf, M->drones[droneIdx[A.sel[V_DRONES]]].alias, sizeof A.editBuf);
        return;
    }
    if (A.detail) {
        if (A.editAlias || A.newGroup) {
            size_t n = strlen(A.editBuf);
            if (kc == 0x29) A.editAlias = A.newGroup = false;
            else if (kc == 0x28) detail_submit_text();
            else if (kc == 0x2A || kc == 0x4C) { if (n) A.editBuf[n - 1] = 0; }
            else if (ch >= 0x20 && ch < 0x7f && ch != '"' && ch != '\\' && n + 1 < 33) A.editBuf[n] = ch, A.editBuf[n + 1] = 0;
            return;
        }
        if (kc == 0x29 || kc == 0x2A) { A.detail = false; return; }
        if (net_online() && (ch == 'e' || ch == 'E')) {
            A.editAlias = true;
            strlcpy(A.editBuf, M->detail.alias, sizeof A.editBuf);
        } else if (net_online() && (ch == 'g' || ch == 'G')) {
            A.newGroup = true, A.editBuf[0] = 0;
        }
        return;
    }
    if (A.searching) {
        size_t n = strlen(A.search);
        if (kc == 0x29) {
            A.search[0] = 0;
            A.searching = false;
        } else if (kc == 0x28 || kc == 0x51 || kc == 0x52) {
            A.searching = false;
        } else if (kc == 0x2A || kc == 0x4C) {
            if (n) A.search[n - 1] = 0;
        } else if (ch >= 0x20 && ch < 0x7f && n + 1 < sizeof A.search) {
            A.search[n] = ch;
            A.search[n + 1] = 0;
        }
        A.sel[V_LIVE] = A.top[V_LIVE] = A.sel[V_TARGETS] = 0;
        return;
    }
    int page = A.view == V_LIVE ? 10 : A.view == V_TARGETS ? 9 : A.view == V_DRONES ? 7 : 6;
    if (kc == 0x28) {   // Enter: everything about the selection
        if (A.view == V_DRONES) dispatch_drone_map();
        else if (const char *mac = selected_mac()) open_detail(mac);
        return;
    }
    if (A.view == V_MAP) {
        if (ch == '+' || ch == '=') { map_zoom(1); return; }
        if (ch == '-' || ch == '_') { map_zoom(-1); return; }
        if (ch == 'l' || ch == 'L') { A.mapPrevLayer = A.mapLayer; A.mapLayer = (A.mapLayer + 1) % NLAYERS; maptiles_clear_requests(); return; }
        if (ch == 'z' || ch == 'Z') { A.mapFollow = false, A.gpsFollow = false; map_fit(); maptiles_clear_requests(); return; }
        if (ch == 'o' || ch == 'O') { A.mapFollow = !A.mapFollow; return; }
        if (kc == 0x52 || kc == 0x51) {   // Up / Down pan; Left / Right still switch views
            double y = lat2y(A.mapLat, A.mapZoom) + (kc == 0x52 ? -200 : 200);
            A.mapLat = y2lat(y, A.mapZoom);
            maptiles_clear_requests();
            return;
        }
    }
    switch (kc) {
    case 0x2B: A.view = next_tab(shift ? -1 : 1); return;   // Tab
    case 0x4F: A.view = next_tab(1); return;                   // Right
    case 0x50: A.view = next_tab(-1); return;                  // Left
    case 0x52: A.sel[A.view] -= ctrl ? page : 1; return;                                    // Up
    case 0x51: A.sel[A.view] += ctrl ? page : 1; return;                                    // Down
    case 0x4A: A.sel[A.view] = 0; return;                                                   // Home
    case 0x29:                                                                             // Esc
        A.search[0] = 0, A.dom = 0, A.node[0] = 0;
        A.metricsGen++;
        return;
    }
    if (ch >= '1' && ch < '1' + NSHOWN) {
        A.view = SHOWN[ch - '1'];
        return;
    }
    switch (tolower(ch)) {
    case '/':
        if (A.view != V_LIVE && A.view != V_TARGETS) A.view = V_LIVE;
        A.searching = true;
        break;
    case 'a':
        alerts_ack();
        toast("alerts acknowledged");
        break;
    case 'm':
        M->muteAll = !M->muteAll;
        if (M->muteAll) M->alertsUnacked = 0;
        toast(M->muteAll ? "alerts silenced" : "alerts on");
        break;
    case 'x': {
        const char *mac = selected_mac();
        if (mac) {
            char m[18];
            strlcpy(m, mac, sizeof m);
            mute_toggle(m);
            toast(mac_muted(m) ? "muted %s" : "unmuted %s", m);
        }
        break;
    }
    case 'd':
        A.dom = (A.dom + 1) % 3;
        A.metricsGen++;
        break;
    case 'n': cycle_node(); break;
    case 'c':
        A.search[0] = 0, A.dom = 0, A.node[0] = 0;
        A.metricsGen++;
        break;
    case 'r':
    case ']':
    case '[':
        A.range = (A.range + ((shift && ch == 'R') || ch == '[' ? NRANGES - 1 : 1)) % NRANGES;
        A.metricsGen++;
        toast("metrics range %s", RANGE_NAMES[A.range.load()]);
        break;
    case 'v':
        S.sound = !S.sound;
        settings_save();
        toast(S.sound ? "alert sound on" : "alert sound off");
        break;
    case 'b': {
        static const uint8_t steps[] = {15, 35, 55, 75, 100};
        int i = 0;
        while (i < 5 && steps[i] <= S.bright) i++;
        S.bright = steps[i % 5];
        M5.Display.setBrightness(S.bright * 255 / 100);
        settings_save();
        toast("brightness %u%%", S.bright);
        break;
    }
    case 'f':
        S.flip = !S.flip;
        settings_save();
        apply_rotation();
        break;
    case 's': settings_open(); break;
    case 'h':
    case '?': A.help = true; break;
    }
}

// A tap = press and release on the same target without moving; moving more than a few
// pixels turns it into a drag that scrolls the list under the finger.
constexpr int DRAG_SLOP = 14;

static void dispatch(const Hit &h) {
    if (A.settings) {
        switch (h.act) {
        case H_FIELD: fsel = h.arg, inList = false; break;
        case H_SRC: eSource = h.arg, fsel = ROW_SRC; break;
        case H_NET: pick_network(h.arg); break;
        case H_SAVE: settings_commit(); break;
        case H_BACK: if (settings_ready()) A.settings = false; break;
        case H_REVEAL: reveal = !reveal; break;
        case H_SOUNDPG: soundPage = !soundPage; break;
        case H_SND: {
            bool *vs[] = {&S.sound, &S.sndDet, &S.sndDrone, &S.sndNode, &S.proxBeep};
            if (h.arg >= 200) S.vol = h.arg - 200, sfx_volume(), sfx_play(SFX_DETECT);
            else if (h.arg >= 100) { bool m = S.sound; S.sound = true; sfx_play((Sfx)(h.arg - 100)); S.sound = m; }
            else if (h.arg < 5) *vs[h.arg] = !*vs[h.arg];
            settings_save();
            break;
        }
        case H_RESCAN: net_request_scan(); break;
        }
        return;
    }
    if (A.wdOpen[0]) {   // a wardriven device's info page is up: only closing it
        if (h.act == H_WDCLOSE) A.wdOpen[0] = 0;
        return;
    }
    if (A.nodeEdit[0]) {
        const Node *n = edit_node();
        char id[7];
        strlcpy(id, A.nodeEdit, sizeof id);
        switch (h.act) {
        case H_NCLOSE: A.nodeEdit[0] = 0; A.editNode = false; break;
        case H_NALIAS:
            A.editNode = true;
            strlcpy(A.editBuf, n ? n->alias : "", sizeof A.editBuf);
            break;
        case H_NCOLOR: {
            char b[32];
            if (h.arg < 0) snprintf(b, sizeof b, "{\"color\":null}");
            else snprintf(b, sizeof b, "{\"color\":%d}", h.arg);
            node_set(id, b);
            break;
        }
        case H_NBUZZ:
            if (n) {
                char a[40];
                snprintf(a, sizeof a, "{\"buzzer\":%d,\"led\":%d}", n->buzzer == 1 ? 0 : 1, n->led == 0 ? 0 : 1);
                node_cmd(id, "audio", a);
            }
            break;
        case H_NLED:
            if (n) {
                char a[40];
                snprintf(a, sizeof a, "{\"buzzer\":%d,\"led\":%d}", n->buzzer == 0 ? 0 : 1, n->led == 1 ? 0 : 1);
                node_cmd(id, "audio", a);
            }
            break;
        case H_NIDENT: node_cmd(id, "identify", "{\"secs\":10}"); toast("identify: %s beeps and flashes", id); break;
        case H_NSTATUS: node_cmd(id, "status", nullptr); break;
        case H_NREBOOT:
            if (A.rebootArm && millis() - A.rebootArm < 3000) {
                node_cmd(id, "reboot", nullptr);
                toast("rebooting %s", id);
                A.rebootArm = 0;
            } else {
                A.rebootArm = millis();
            }
            break;
        case H_NBEEP:
            node_cmd(id, "locate", "{\"mode\":1}");
            toast("%s: beep 3 times", id);
            break;
        case H_NLOCATE: {
            bool on = strcmp(A.locateId, id) != 0;
            if (on && A.locateId[0]) node_cmd(A.locateId, "locate", "{\"mode\":0}");   // one at a time
            node_cmd(id, "locate", on ? "{\"mode\":2}" : "{\"mode\":0}");
            strlcpy(A.locateId, on ? id : "", sizeof A.locateId);
            toast(on ? "%s: beeping until you stop it" : "%s: locate beep off", id);
            break;
        }
        case H_NFW: {
            static const char *KIND[] = {"detector", "skyspy", "wardrive", "gps", "gpswd"};
            int k = constrain((int)h.arg, 0, 4);
            if (A.fwArm == k && millis() - A.fwArmMs < 4000) {
                char args[40];
                snprintf(args, sizeof args, "{\"kind\":\"%s\"}", KIND[k]);
                node_cmd(id, "fw_install", args);
                toast("%s: installing %s firmware over the mesh", id, KIND[k]);
                A.fwArm = -1;
            } else {
                A.fwArm = k;
                A.fwArmMs = millis();
            }
            break;
        }
        case H_NWDON:
        case H_NWDCH: {
            static const char *CH[] = {"1-11", "all", "1,6,11", "1-6", "7-11", "1", "6", "11"};
            const Node *wn = edit_node();
            char words[24], args[48];
            if (h.act == H_NWDON) {
                bool on = !wn || wn->wdOn == 0;
                snprintf(words, sizeof words, "wd %s", on ? "on" : "off");
                snprintf(args, sizeof args, "{\"on\":%s}", on ? "true" : "false");
            } else {
                const char *c = CH[constrain((int)h.arg, 0, 7)];
                snprintf(words, sizeof words, "wd ch %s", c);
                snprintf(args, sizeof args, "{\"ch\":\"%s\"}", c);
            }
            if (!strcmp(id, usb_node_id()) && usb_live()) usb_send(words);   // on our USB: straight to it
            else node_cmd(id, "wardrive", args);
            toast("%s: %s", id, words + 3);
            break;
        }
        case H_TAB: A.nodeEdit[0] = 0; A.view = (View)h.arg; break;
        default: break;
        }
        return;
    }
    if (A.detail) {
        switch (h.act) {
        case H_DCLOSE: A.detail = false; A.editAlias = A.newGroup = false; break;
        case H_DPROX:
            if (!strcasecmp(A.prox, M->detail.mac)) A.prox[0] = 0, toast("proximity off");
            else strlcpy(A.prox, M->detail.mac, sizeof A.prox), toast("proximity: watch the keyboard LED");
            break;
        case H_DALIAS:
            A.editAlias = true, A.newGroup = false;
            strlcpy(A.editBuf, M->detail.alias, sizeof A.editBuf);
            break;
        case H_DNEWGROUP: A.newGroup = true, A.editAlias = false, A.editBuf[0] = 0; break;
        case H_DGROUP: detail_group(h.arg); break;
        case H_DUNGROUP: detail_group(-1); break;
        case H_DCOMP:
            if (h.arg < M->detail.ncomp) {
                char m[18];
                strlcpy(m, M->detail.comp[h.arg].mac, sizeof m);
                open_detail(m);
            }
            break;
        case H_TAB: A.detail = false; A.view = (View)h.arg; break;
        case H_SETUP: settings_open(); break;
        case H_HELP: A.help = true; break;
        default: break;
        }
        return;
    }
    switch (h.act) {
    case H_TAB: A.view = (View)h.arg; A.searching = false; A.layerMenu = false; A.editDrone = false; break;
    case H_ROW:
        // Tap a row to select it, tap it again for everything about it.
        if (A.view == V_LIVE && A.sel[V_LIVE] == h.arg) {
            if (live_is_wardrive()) open_wd_info(h.arg);
            else open_live_row(h.arg);
        }
        A.sel[A.view] = h.arg;
        break;
    case H_TARGET:
        if (A.sel[V_TARGETS] == h.arg && nTgt) open_detail(M->targets[tgtIdx[h.arg]].mac);
        A.sel[V_TARGETS] = h.arg;
        break;
    case H_NODEROW:   // a tap on an Overview node row: that node on the Fleet tab
        A.view = V_FLEET;
        A.sel[V_FLEET] = h.arg;
        break;
    case H_DETAIL: {
        if (A.view == V_LIVE && !live_is_wardrive() && nLive && liveIdx[A.sel[V_LIVE]] < 0) {
            open_live_row(A.sel[V_LIVE]);
            break;
        }
        const char *mac = selected_mac();
        if (mac) open_detail(mac);
        break;
    }
    case H_TGTMAP:
        if (h.arg < nTgt) open_detail(M->targets[tgtIdx[h.arg]].mac);
        break;
    case H_DRONE: A.view = V_DRONES; A.sel[V_DRONES] = h.arg; break;
    case H_DRONEMAP:
        if (nDrone) {
            const Drone &d = M->drones[droneIdx[A.sel[V_DRONES]]];
            if (d.hasPos) map_center_on(d.lat, d.lon);
            else if (d.hasPilot) map_center_on(d.plat, d.plon);
            A.mapZoom = max((int)A.mapZoom, 17);
            A.view = V_MAP;
        }
        break;
    case H_DRONETAG: drone_set_tag(h.arg); break;
    case H_ZOOMIN: map_zoom(1); break;
    case H_ZOOMOUT: map_zoom(-1); break;
    case H_LAYER:
        A.mapPrevLayer = A.mapLayer;
        A.mapLayer = (A.mapLayer + 1) % NLAYERS;
        maptiles_clear_requests();
        toast("map: %s", LAYER_NAMES[A.mapLayer]);
        break;
    case H_LAYERMENU: A.layerMenu = !A.layerMenu; break;
    case H_LAYERPICK:
        if (h.arg != A.mapLayer) {
            A.mapPrevLayer = A.mapLayer;
            A.mapLayer = h.arg;
            maptiles_clear_requests();
        }
        A.layerMenu = false;
        break;
    case H_DRALIAS:
        if (nDrone && net_online()) {
            A.editDrone = true;
            strlcpy(A.editBuf, M->drones[droneIdx[A.sel[V_DRONES]]].alias, sizeof A.editBuf);
        }
        break;
    case H_FIT: A.mapFollow = false, A.gpsFollow = false; map_fit(); maptiles_clear_requests(); break;
    case H_FOLLOW: A.mapFollow = !A.mapFollow; toast(A.mapFollow ? "following the newest live drone" : "follow off"); break;
    case H_WD:
        A.wdScope = (A.wdScope + 1) % 3;
        toast(A.wdScope == 2 ? "wardrive: all history" : A.wdScope == 1 ? "wardrive: this session" : "wardrive off");
        break;
    case H_ME: {
        GpsFix gf = gps_fix();
        A.gpsFollow = !A.gpsFollow;
        if (A.gpsFollow && !gf.valid) toast(gps_present() ? "following you once the GPS has a fix" : "no GPS on the Grove port");
        else if (A.gpsFollow) { A.mapFollow = false; A.mapLat = gf.lat, A.mapLon = gf.lon; toast("following you"); }
        else toast("follow off");
        break;
    }
    case H_TRACKS: A.droneTracks = !A.droneTracks; toast(A.droneTracks ? "tracks on" : "tracks off"); break;
    case H_PILOT: A.pilotOn = !A.pilotOn; toast(A.pilotOn ? "pilots on" : "pilots off"); break;
    case H_TRI: A.triOn = !A.triOn; toast(A.triOn ? "triangulation on: devices at their fix" : "triangulation off"); break;
    case H_DOM:
        A.dom = h.arg >= 0 ? h.arg : (A.dom + 1) % 3;
        A.metricsGen++;
        break;
    case H_NODE: cycle_node(); break;
    case H_MXWD: A.mxWd = !A.mxWd; break;
    case H_OSK: {
        size_t n = strlen(A.search);
        if (h.arg == 8) { if (n) A.search[n - 1] = 0; }
        else if (h.arg == 27) A.search[0] = 0;
        else if (h.arg == 13) A.searching = false;
        else if (n + 1 < sizeof A.search) A.search[n] = (char)h.arg, A.search[n + 1] = 0;
        A.top[V_LIVE] = A.sel[V_LIVE] = 0;
        break;
    }
    case H_NODEPIN:
        if (h.arg >= 0 && h.arg < M->nnodes) {
            strlcpy(A.mapPop, M->nodes[h.arg].id, sizeof A.mapPop);
            A.mapMove = false;
        }
        break;
    case H_POPCLOSE: A.mapPop[0] = 0; A.mapMove = false; break;
    case H_POPSET:
        if (A.mapPop[0]) { strlcpy(A.nodeEdit, A.mapPop, sizeof A.nodeEdit); A.editNode = false; A.rebootArm = 0; }
        break;
    case H_POPTRACK:
        if (A.mapPop[0]) {
            bool on = !node_track_on(A.mapPop);
            node_track_set(A.mapPop, on);
            toast(on ? "%s track on" : "%s track off", node_name(A.mapPop));
        }
        break;
    case H_POPMOVE:
        A.mapMove = !A.mapMove;
        if (A.mapMove) toast("tap the map where %s is", node_name(A.mapPop));
        break;
    case H_PROXSTOP: A.prox[0] = 0; toast("proximity off"); break;
    case H_ZLOCK: A.liveZoomLock = !A.liveZoomLock; toast(A.liveZoomLock ? "list zoom locked" : "pinch the list to zoom it"); break;
    case H_OVDET:
        if (A.ovWd) {   // a wardriven device (its index in the store): its info page
            if (h.arg >= 0 && h.arg < wd_count()) {
                const WdDev &d = wd_at(h.arg);
                snprintf(A.wdOpen, sizeof A.wdOpen, "%02X:%02X:%02X:%02X:%02X:%02X", d.mac[0], d.mac[1], d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
                A.wdOpenKind = d.kind;
            }
        } else if (h.arg >= 0 && h.arg < M->ndets) {
            open_detail(det_at(h.arg)->mac);
        }
        break;
    case H_OVWD: A.ovWd = !A.ovWd; ovLiveTop = ovFleetTop = 0; break;
    case H_SORT:
        if (A.liveSort == h.arg) A.liveSortRev = !A.liveSortRev;
        else A.liveSort = h.arg, A.liveSortRev = false;
        break;
    case H_KIND: A.kindFilter = (A.kindFilter + 1) % 4; A.metricsGen++; break;
    case H_SEARCH: A.searching = !A.searching; break;
    case H_CLEAR:
        A.search[0] = 0, A.dom = 0, A.node[0] = 0, A.searching = false, A.kindFilter = 0;
        A.metricsGen++;
        break;
    case H_RANGE:
        A.range = h.arg;
        A.metricsGen++;
        break;
    case H_ACK: alerts_ack(); toast("alerts acknowledged"); break;
    case H_MUTE: {
        const char *mac = selected_mac();
        if (mac) {
            char m[18];
            strlcpy(m, mac, sizeof m);
            mute_toggle(m);
            toast(mac_muted(m) ? "muted %s" : "unmuted %s", m);
        }
        break;
    }
    case H_SILENCE:
        M->muteAll = !M->muteAll;
        if (M->muteAll) M->alertsUnacked = 0;
        toast(M->muteAll ? "alerts silenced" : "alerts on");
        break;
    case H_HELP: A.help = true; break;
    case H_SETUP: settings_open(); break;
    case H_CLOSE: A.help = false; break;
    }
}

static void apply_drag(int dx, int dy) {
    const ScrollArea &a = T.area;
    if (a.list == L_MAP) {
        A.mapLon = x2lonf(T.mx0 - dx, gZoomShown);
        A.mapLat = y2latf(T.my0 - dy, gZoomShown);
        A.mapFollow = false, A.gpsFollow = false;
        return;
    }
    if (a.list == L_NETS) {
        netTop = T.top0 - (dy + (dy > 0 ? a.rowH / 2 : -a.rowH / 2)) / a.rowH;
        return;
    }
    if (a.list == L_OVF || a.list == L_OVL) {
        int t = T.top0 - (dy + (dy > 0 ? a.rowH / 2 : -a.rowH / 2)) / a.rowH;
        (a.list == L_OVF ? ovFleetTop : ovLiveTop) = max(0, t);   // the draw clamps the end
        return;
    }
    View v = (View)a.list;
    if (v == V_TARGETS) {   // whole pages: a firm swipe turns one
        int per = 9, pages = max(1, (nTgt + per - 1) / per);
        int page = constrain(T.top0 + (dy < -70 ? 1 : dy > 70 ? -1 : 0), 0, pages - 1);
        A.sel[V_TARGETS] = constrain(page * per + A.sel[V_TARGETS] % per, 0, max(0, nTgt - 1));
        return;
    }
    int n = list_len(v);
    int top = constrain(T.top0 - (dy + (dy > 0 ? a.rowH / 2 : -a.rowH / 2)) / a.rowH, 0, max(0, n - a.rows));
    A.top[v] = top;
    A.sel[v] = constrain(A.sel[v], top, max(top, min(n - 1, top + a.rows - 1)));   // keep the selection in view
}

void ui_press(int x, int y) {
    to_canvas(x, y);
    model_lock();
    T = {};
    T.down = true;
    T.downMs = millis();
    T.x0 = T.x = x, T.y0 = T.y = y;
    T.scroll = -1;
    for (int i = nhits - 1; i >= 0; i--)   // last drawn = on top
        if (hits[i].in(x, y)) {
            T.pressed = hits[i];
            T.hasPressed = true;
            break;
        }
    for (int i = 0; i < nscrolls; i++) {
        const ScrollArea &a = scrolls[i];
        if (x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h) {
            T.scroll = i;
            T.area = a;
            T.top0 = a.list == L_NETS ? netTop : a.list == L_OVF ? ovFleetTop : a.list == L_OVL ? ovLiveTop
                   : a.list == V_TARGETS ? A.sel[V_TARGETS] / 9 : a.list == L_MAP ? 0 : A.top[a.list];
            if (a.list == L_MAP) T.mx0 = lon2xf(A.mapLon, gZoomShown), T.my0 = lat2yf(A.mapLat, gZoomShown);
        }
    }
    if (A.help || A.settings) T.scroll = T.scroll >= 0 && T.area.list == L_NETS && A.settings ? T.scroll : -1;
    if (A.detail || A.nodeEdit[0]) T.scroll = -1;
    // Instant pressed look only for real controls: tinting and pushing a screen-sized
    // "nothing here" area (the dim backdrop around a panel) row by row straight into the
    // panel's framebuffer starved its scan-out: a blue flash.
    if (T.hasPressed && T.pressed.act != H_NONE && (int)T.pressed.w * T.pressed.h < 260000) pressPaint = true;
    model_unlock();
}

void ui_drag(int x, int y) {
    if (!T.down) return;
    to_canvas(x, y);
    T.x = x, T.y = y;
    int dx = x - T.x0, dy = y - T.y0;
    if (!T.dragging && (abs(dx) > DRAG_SLOP || abs(dy) > DRAG_SLOP)) {
        T.dragging = true;
        T.hasPressed = false;   // it's a drag, not a tap: drop the pressed look
        pressPaint = false;
        dirty = true;
    }
    if (T.dragging && T.scroll >= 0) {
        model_lock();
        apply_drag(dx, dy);
        model_unlock();
        dirty = true;
    }
}

void ui_release(int x, int y) {
    if (!T.down) return;
    to_canvas(x, y);
    model_lock();
    bool tap = !T.dragging && T.hasPressed && T.pressed.in(x, y);
    Hit h = T.pressed;
    T.down = false;
    T.hasPressed = false;
    pressPaint = false;
    if (T.dragging && T.scroll >= 0 && T.area.list == L_MAP) maptiles_clear_requests();
    // MOVE armed on a node's card: this tap is where it is.
    if (A.mapMove && A.mapPop[0] && !T.dragging && !T.hasPressed && T.scroll >= 0 && T.area.list == L_MAP && A.view == V_MAP) {
        double la, lo;
        screen_to_ll(x, y, gZoomShown, la, lo);
        char body[64];
        snprintf(body, sizeof body, "{\"lat\":%.7f,\"lon\":%.7f}", la, lo);
        node_set(A.mapPop, body);
        toast("%s placed here", node_name(A.mapPop));
        A.mapMove = false;
    } else
    // Double tap on the map: zoom in around that spot.
    if (!T.dragging && !T.hasPressed && T.scroll >= 0 && T.area.list == L_MAP && A.view == V_MAP && !A.detail) {
        static uint32_t lastTap = 0;
        static int ltx = 0, lty = 0;
        uint32_t ms = millis();
        if (ms - lastTap < 350 && abs(x - ltx) < 60 && abs(y - lty) < 60) {
            map_zoom_at(1, x, y);
            lastTap = 0;
        } else {
            lastTap = ms, ltx = x, lty = y;
        }
    }
    if (tap) {
        flashHit = h;
        flashUntil = millis() + 180;
        if (A.help && h.act != H_CLOSE) A.help = false;
        else dispatch(h);
    }
    dirty = true;
    model_unlock();
}

void ui_frame() {
    uint32_t now = millis();
    sfx_loop();
    prox_loop();
    {   // a node dropping off the mesh or coming back: a falling / rising jingle
        static char was[MAX_NODES][7];
        static bool wasOn[MAX_NODES];
        static int nwas = -1;
        if (M->stateLoaded && nwas >= 0 && S.sndNode && !M->muteAll) {
            for (int i = 0; i < M->nnodes; i++) {
                const Node &n = M->nodes[i];
                for (int k = 0; k < nwas; k++)
                    if (!strcmp(was[k], n.id) && wasOn[k] != n.online) {
                        static uint32_t lastNodeSnd = 0;   // a flapping link: once per 30 s at most
                        if (!lastNodeSnd || now - lastNodeSnd > 30000) lastNodeSnd = now, sfx_play(n.online ? SFX_NODE_UP : SFX_NODE_DOWN);
                        break;
                    }
            }
        }
        if (M->stateLoaded) {
            nwas = min(M->nnodes, (int)MAX_NODES);
            for (int i = 0; i < nwas; i++) strlcpy(was[i], M->nodes[i].id, 7), wasOn[i] = M->nodes[i].online;
        }
    }
    bool alertShown = M->alertsUnacked && M->hasAlert && !M->muteAll;
    static uint32_t lastDroneSeq = 0;
    if (M->droneSeq != lastDroneSeq) {
        bool boot = lastDroneSeq == 0 && !M->stateLoaded;
        lastDroneSeq = M->droneSeq;
        if (!boot && M->ndrones) {
            toast("DRONE DETECTED  %d live", live_drones());
            if (S.sndDrone && !M->muteAll) sfx_play(SFX_DRONE);
            dirty = true;
        }
    }
    if (M->alertSeq != lastAlertSeq) {
        lastAlertSeq = M->alertSeq;
        alertFlashUntil = now + 1500;
        if (S.sndDet && !M->muteAll && M->alertsUnacked) sfx_play(SFX_DETECT);
    }
    // Redraw on change, every second (ages, clock), faster while text cursors blink or an
    // alert pulses: never more than ~12 fps.
    if (pressPaint && !dirty) {   // instant pressed state: paint + push just that control
        pressPaint = false;
        model_lock();
        Hit h = T.pressed;
        bool ok = T.down && T.hasPressed;
        if (ok) paint_pressed(h);
        model_unlock();
        if (ok) push_rect(h.x - 3, h.y - 3, h.w + 6, h.h + 6);
        return;
    }
    // Long press (~0.55 s, finger still) on a node row: its settings panel.
    // Long press on a Live row: its full info (a device, or a wardriven network).
    if (T.down && T.hasPressed && !T.dragging && millis() - T.downMs > 550 && !A.nodeEdit[0] && !A.detail &&
        !A.wdOpen[0] && T.pressed.act == H_ROW && A.view == V_LIVE) {
        model_lock();
        int li = T.pressed.arg;
        A.sel[V_LIVE] = li;
        if (live_is_wardrive()) open_wd_info(li);
        else open_live_row(li);
        if (S.sound) M5.Speaker.tone(2400, 40);
        T.hasPressed = false;
        dirty = true;
        model_unlock();
    }
    if (T.down && T.hasPressed && !T.dragging && millis() - T.downMs > 550 && !A.nodeEdit[0] && !A.detail &&
        (T.pressed.act == H_NODEROW || (T.pressed.act == H_ROW && A.view == V_FLEET))) {
        model_lock();
        int i = T.pressed.arg;
        if (i >= 0 && i < M->nnodes) {
            strlcpy(A.nodeEdit, M->nodes[i].id, sizeof A.nodeEdit);
            A.editNode = false;
            A.rebootArm = 0;
            if (S.sound) M5.Speaker.tone(2400, 40);
        }
        T.hasPressed = false;   // the release is not a tap any more
        dirty = true;
        model_unlock();
    }
    bool anim = (T.down && T.dragging) || millis() < flashUntil || (mapAnimating && A.view == V_MAP);
    bool pulse = (live_drones() && (A.view == V_MAP || A.view == V_DRONES)) || (A.view == V_MAP && gps_fix().valid);
    uint32_t period = anim ? 16 : A.otaActive ? 180 : alertShown ? 120 : pulse ? 150
                    : (A.searching || A.settings || A.editAlias || A.newGroup) ? 500 : 1000;
    if (A.settings && A.autoSetup && M->usbLive) {   // a node got plugged in: no setup needed
        A.settings = false;
        A.autoSetup = false;
        dirty = true;
    }
    static uint32_t warmMs = 0;
    if (now - warmMs > 1500 && !mapAnimating && !pinching) {
        warmMs = now;
        model_lock();
        map_warm();
        model_unlock();
    }
    if (!dirty && M->version == drawnVersion && now - drawnMs < period) return;
    if (now - drawnMs < (anim ? 16 : 40)) return;
    dirty = false;
    model_lock();
    nhits = 0;
    nscrolls = 0;
    drawnVersion = M->version;
    drawnMs = now;
    if (A.otaActive) {
        draw_ota();
    } else if (A.settings) {
        draw_settings();
    } else {
        uint32_t p0 = micros();
        if (A.view != V_MAP) ppa_fill(0, 0, W, H, BG);   // the map paints its own area
        else ppa_fill(0, 0, W, TOP, BG), ppa_fill(0, H - BOT, W, BOT, BG);
        build_lists();
        build_drones();
        uint32_t p1 = micros();
        switch (A.view) {
        case V_OVERVIEW: draw_overview(); break;
        case V_FLEET: draw_fleet(); break;
        case V_LIVE: draw_live(); break;
        case V_TARGETS: draw_targets(); break;
        case V_METRICS: draw_metrics(); break;
        case V_MAP: draw_map(); break;
        case V_DRONES: draw_drones(); break;
        default: break;
        }
        if (A.detail) draw_detail();
        if (A.view == V_LIVE) draw_osk();
        if (A.wdOpen[0]) draw_wd_info();
        if (A.nodeEdit[0]) draw_node_editor();
        uint32_t p2 = micros();
        cached_band(topBand, top_key(), draw_top);
        uint32_t p3 = micros();
        if (M->alertsUnacked && M->hasAlert && !M->muteAll) draw_bottom();   // the alert bar pulses: always live
        else cached_band(botBand, bottom_key(), draw_bottom);
        uint32_t p4 = micros();
        if (A.help) draw_help();
        ui_prof[0] = p1 - p0, ui_prof[1] = p2 - p1, ui_prof[2] = p3 - p2, ui_prof[3] = p4 - p3, ui_prof[5] = A.view;
    }
    // Touch feedback: the control under the finger fills while held, and flashes bright
    // as the tap fires (fading over ~180 ms).
    if (!A.otaActive) {
        if (T.down && T.hasPressed) {
            paint_pressed(T.pressed);
        } else if (millis() < flashUntil) {
            float f = (flashUntil - millis()) / 180.0f;
            const Hit &h = flashHit;
            tint_rect(h.x, h.y, h.w, h.h, ACCENT, (int)(110 * f));
            glow_rect(h.x, h.y, h.w, h.h, 8, dim(ACCENT, 0.4f + 0.6f * f));
        }
    }
    model_unlock();
    uint32_t t1 = millis();
    push_frame();
    ui_drawMs = t1 - now;
    ui_pushMs = millis() - t1;
}

// Two-finger pinch on the map: continuous zoom around the midpoint; on release it settles
// to a whole zoom level (where tiles are sharp), leaning the way the fingers went. Each new
// two-finger contact (phase 0, also a finger put back mid-gesture) restarts from the zoom on
// screen. Lifting fingers rarely leave together and the last samples of a lifting finger
// are unreliable, so a lift falls back to the zoom of ~70 ms before it.
void ui_pinch(int phase, int x, int y, int dist, uint32_t ms) {
    if (phase <= 1) to_canvas(x, y);
    model_lock();
    if (A.view == V_LIVE && !A.detail && !A.wdOpen[0] && !A.nodeEdit[0] && !A.settings && !A.help) {
        static float rh0 = 34;
        static int ld0 = 100;
        T = {};
        if (!A.liveZoomLock) {
            if (phase == 0) rh0 = A.liveRh, ld0 = max(dist, 30);
            else if (phase == 1) A.liveRh = constrain(rh0 * (float)max(dist, 10) / ld0, 24.0f, 72.0f);
        }
        dirty = true;
        model_unlock();
        return;
    }
    if (A.view != V_MAP || A.detail || A.settings || A.help) {
        pinching = false;
        model_unlock();
        return;
    }
    static float z0 = 17, zGesture = 17;
    static int d0 = 100;
    static struct { uint32_t ms; float z; } hist[16];
    static int hn = 0;
    static bool moved = false;   // a two-finger touch that never moved leaves the zoom alone
    auto settle_back = [&] {
        for (int i = hn - 1; i >= 0; i--)
            if (ms - hist[i].ms >= 70) {
                A.mapZoomF = hist[i].z;
                break;
            }
        hn = 0;
    };
    T = {};   // whatever the first finger started is cancelled
    if (phase == 0) {
        if (!pinching) zGesture = gZoomShown, moved = false;
        pinching = true;
        z0 = A.mapZoomF;
        d0 = max(dist, 30);
        hn = 0;
        ZA.on = true, ZA.x = x, ZA.y = y;
        screen_to_ll(x, y, gZoomShown, ZA.lat, ZA.lon);
        A.mapFollow = false, A.gpsFollow = false;
    } else if (phase == 1 && pinching) {
        float z = constrain(z0 + log2f((float)max(dist, 10) / d0), (float)ZMIN, (float)zmax_layer());
        A.mapZoomF = z;
        moved = true;
        ZA.x = x, ZA.y = y;   // two-finger drag pans as it zooms
        if (hn == 16) memmove(hist, hist + 1, sizeof hist[0] * 15), hn = 15;
        hist[hn++] = {ms, z};
    } else if (phase == 3 && pinching) {
        settle_back();
    } else if (phase == 2 && pinching) {
        settle_back();
        pinching = false;
        if (!moved) {
            A.mapZoomF = z0;   // let the zoom it was easing to finish
            dirty = true;
            model_unlock();
            return;
        }
        float dz = A.mapZoomF - zGesture;
        float lean = dz > 0.15f ? 0.2f : dz < -0.15f ? -0.2f : 0;
        A.mapZoom = constrain((int)lroundf(A.mapZoomF + lean), ZMIN, zmax_layer());
        maptiles_clear_requests();
    }
    dirty = true;
    model_unlock();
}
