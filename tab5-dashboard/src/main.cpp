// OUI-SPY Mesh wall display for the M5Stack Tab5 + Tab5 Keyboard.
//
// The Tab5 is a client of the home dashboard (home-dashboard/ouispymesh.py): fleet,
// detections and sightings arrive over its Socket.IO feed, stats and metrics over HTTP.
// No mesh radio and no firmware changes on the detectors.
#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <Wire.h>
#include "app.h"
#include "model.h"
#include "net.h"
#include "maptiles.h"
#include "sdcard.h"
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include "settings.h"
#include "sfx.h"
#include "ui.h"
#include "feed.h"
#include "gps.h"

namespace {

m5::unit::UnitUnified Units;
m5::unit::UnitTab5Keyboard kb;
bool kbOk = false;
uint32_t kbRetryMs = 0;

// Tab5 ExtPort1: SDA G0, SCL G1, INT G50 (the library's default irq pin).
constexpr int8_t KB_SDA = 0, KB_SCL = 1;

bool kb_begin() {
    auto cfg = kb.config();
    cfg.mode = m5::unit::tab5_keyboard::Mode::HID;   // HID usages: arrows, Esc, Tab, Enter
    kb.config(cfg);
    Wire.end();
    Wire.begin(KB_SDA, KB_SCL, kb.component_config().clock);
    if (!Units.add(kb, Wire) || !Units.begin()) return false;
    kb.writeRgbMode(m5::unit::tab5_keyboard::RgbMode::Custom);
    kb.writeBrightness(12);
    Serial.printf("keyboard firmware %02X\n", kb.firmwareVersion());
    return true;
}

// Keyboard LEDs, kept dim: left = link (cyan live, amber no dashboard, red no Wi-Fi/node),
// right = three red blinks for every detection (the nodes' triple beep), dark otherwise.
void kb_leds() {
    static int lastLink = -1;
    static uint32_t lastEvents = 0, blinkAt = 0;
    static bool lit = false;
    int link = M->usbLive || M->sockUp ? 2 : (M->wifiUp ? 1 : 0);
    if (link != lastLink) {
        lastLink = link;
        if (link == 2) kb.writeRgb(0, 0x00, 0x30, 0x2c);
        else if (link == 1) kb.writeRgb(0, 0x30, 0x20, 0x00);
        else kb.writeRgb(0, 0x30, 0x00, 0x00);
    }
    if (M->events != lastEvents) {
        if (lastEvents) blinkAt = millis();   // not on the boot-time catch-up
        lastEvents = M->events;
    }
    // 120 ms on, 90 ms off, three times; a new detection mid-pattern starts it over.
    uint32_t t = millis() - blinkAt;
    bool on = blinkAt && t < 3 * 210 && t % 210 < 120;
    // Proximity mode owns the LED: a pulse per beat, red far .. green close.
    static uint32_t lastRgb = 0xFFFFFFFF;
    uint8_t r = 0, g = 0, b = 0;
    if (A.prox[0]) on = prox_led(r, g, b);
    else if (on) r = 0xFF;
    uint32_t rgb = on ? (uint32_t)r << 16 | g << 8 | b : 0;
    if (rgb != lastRgb) {
        lastRgb = rgb;
        kb.writeRgb(1, r, g, b);
    }
    lit = on;
}

// RTC (UTC). Read at boot so USB-only mode has wall time; set from the dashboard's clock.
int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

double rtc_epoch() {
    if (!M5.Rtc.isEnabled()) return 0;
    auto dt = M5.Rtc.getDateTime();
    if (dt.date.year < 2025 || dt.date.year > 2099) return 0;
    return days_from_civil(dt.date.year, dt.date.month, dt.date.date) * 86400.0 + dt.time.hours * 3600 +
           dt.time.minutes * 60 + dt.time.seconds;
}

void rtc_sync() {
    static uint32_t last = 0;
    if (millis() - last < 60000 || !M->sockUp || !M->clockValid) return;
    last = millis();
    double now = server_now(), rtc = rtc_epoch();
    if (fabs(now - rtc) < 3) return;
    time_t t = (time_t)now;
    struct tm tm;
    gmtime_r(&t, &tm);
    M5.Rtc.setDateTime(&tm);
    Serial.println("rtc set from the dashboard clock");
}

// Touch is sampled here at 100 Hz, apart from the UI loop: a full-frame redraw takes long
// enough that a quick tap could start and end between two samples and never be seen.
// Events queue up and the UI loop replays them in order. This task also owns the other
// internal-bus work (RTC), so nothing else talks on that I2C port concurrently.
struct TouchEv {
    uint8_t kind;   // 0 press, 1 move, 2 release, 3 pinch start, 4 pinch move, 5 pinch end,
                    // 6 pinch lift (down to one finger, the other may come back)
    int16_t x, y;   // pinch: midpoint
    int16_t d;      // pinch: finger distance
    uint32_t ms;    // when it was sampled (the UI replays a queue, often a frame late)
};
QueueHandle_t touchQ;

void hw_task(void *) {
    int lastX = -1, lastY = -1, lastD = -1, lastMx = 0, lastMy = 0;
    bool pinch = false, two = false;
    int settle = 0, spikes = 0;
    for (;;) {
        M5.update();
        auto t = M5.Touch.getDetail();
        TouchEv e;
        uint32_t ms = millis();
        // Two fingers: a pinch (zoom the map). It owns the gesture until every finger is
        // up, so the finger left behind never turns into a tap or a drag. Every fresh
        // two-finger contact (also a finger put back mid-pinch) starts again from where the
        // zoom is: measured against the first spacing, re-placing a finger closer read as a
        // hard pinch-in and threw the map far out.
        if (M5.Touch.getCount() >= 2) {
            auto a = M5.Touch.getDetail(0), b = M5.Touch.getDetail(1);
            int16_t mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
            int16_t d = (int16_t)sqrtf((float)(a.x - b.x) * (a.x - b.x) + (float)(a.y - b.y) * (a.y - b.y));
            if (!two) two = true, settle = 0, spikes = 0;
            if (settle < 2) {   // the controller's first report of a new finger is often off
                if (++settle == 2) {
                    pinch = true;
                    e = {3, mx, my, d, ms};
                    xQueueSend(touchQ, &e, 0);
                    lastD = d, lastMx = mx, lastMy = my;
                }
            } else if (abs(d - lastD) > max(48, lastD / 3) && ++spikes <= 2) {
                // a one-sample jump in spacing is a misread finger, not a pinch: skip it
            } else if (abs(d - lastD) > 2 || abs(mx - lastMx) > 2 || abs(my - lastMy) > 2) {
                spikes = 0;
                e = {4, mx, my, d, ms};
                xQueueSend(touchQ, &e, 0);
                lastD = d, lastMx = mx, lastMy = my;
            } else {
                spikes = 0;
            }
            rtc_sync();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (two) {
            two = false;
            if (pinch) {
                e = {6, 0, 0, 0, ms};
                xQueueSend(touchQ, &e, 0);
            }
        }
        if (pinch || settle) {
            if (M5.Touch.getCount() == 0) {
                if (pinch) {
                    e = {5, 0, 0, 0, ms};
                    xQueueSend(touchQ, &e, 0);
                }
                pinch = false, settle = 0;
            }
            rtc_sync();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (t.wasPressed()) {
            e = {0, (int16_t)t.x, (int16_t)t.y, 0, ms};
            xQueueSend(touchQ, &e, 0);
            lastX = t.x, lastY = t.y;
        } else if (t.isPressed() && (t.x != lastX || t.y != lastY)) {
            e = {1, (int16_t)t.x, (int16_t)t.y, 0, ms};
            xQueueSend(touchQ, &e, 0);
            lastX = t.x, lastY = t.y;
        }
        if (t.wasReleased()) {
            e = {2, (int16_t)t.x, (int16_t)t.y, 0, ms};
            xQueueSend(touchQ, &e, 0);
        }
        rtc_sync();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

String serialLine;

void serial_poll() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c != '\n') {
            if (serialLine.length() < 160) serialLine += c;
            continue;
        }
        if (serialLine == "shot") ui_screenshot(Serial);
        else if (serialLine.startsWith("key ")) ui_key(strtol(serialLine.c_str() + 4, nullptr, 16), 0);   // e.g. "key 51" = Down
        else if (serialLine.length() && !settings_serial_line(serialLine))
            Serial.println("commands: show | shot | key <hid hex> | set <ssid|pass|host|port|token|otapass|tz|hostname|flip|bright|sound> <value> | reboot | clear");
        serialLine = "";
    }
}

}  // namespace

// Health, read by the LAN debug port (/info): why it last restarted, memory, frame times.
uint32_t diagBoots = 0, diagFrameWorst = 0, diagDrawWorst = 0, diagPushWorst = 0;
int diagReset = 0;
const char *reset_name(int r) {
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_SW: return "restart (OTA / settings)";
    case ESP_RST_PANIC: return "CRASH (panic)";
    case ESP_RST_INT_WDT: return "CRASH (interrupt watchdog)";
    case ESP_RST_TASK_WDT: return "CRASH (task watchdog)";
    case ESP_RST_WDT: return "CRASH (watchdog)";
    case ESP_RST_BROWNOUT: return "BROWNOUT (power)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default: return "other";
    }
}

void setup() {
    diagReset = esp_reset_reason();
    {
        Preferences p;
        p.begin("tab5diag", false);
        diagBoots = p.getUInt("boots", 0) + 1;
        p.putUInt("boots", diagBoots);
        if (diagReset == ESP_RST_PANIC || diagReset == ESP_RST_INT_WDT || diagReset == ESP_RST_TASK_WDT ||
            diagReset == ESP_RST_WDT || diagReset == ESP_RST_BROWNOUT)
            p.putUInt("crashes", p.getUInt("crashes", 0) + 1);
        p.end();
    }
    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    M5.begin(cfg);
    settings_load();
    sfx_volume();
    model_init();
    double rtc = rtc_epoch();
    model_set_clock(rtc ? rtc : millis() / 1000.0, rtc != 0);
    ui_init();
    Serial.printf("\nOUI-SPY Mesh Tab5 %s  (type 'show' for settings)\n", FW_VERSION);
    kbOk = kb_begin();
    if (!kbOk) Serial.println("keyboard not found on ExtPort1 (touch still works); retrying");
    Serial.printf("touch %s\n", M5.Touch.isEnabled() ? "ready" : "NOT DETECTED");
    touchQ = xQueueCreate(64, sizeof(TouchEv));
    xTaskCreatePinnedToCore(hw_task, "hw", 6144, nullptr, 4, nullptr, 1);
    gps_begin();   // any GPS unit on Grove port A: our own position, wardrive stamping
    if (S.source != SRC_NETWORK) feed_start();
    net_start();
    sd_begin();
    maptiles_start();
}

void loop() {
    // Watchdog: a UI loop stuck for 10 s (a freeze) restarts the device instead of
    // leaving a dead screen; /info then reports "CRASH (task watchdog)".
    static bool wdtOn = false;
    if (!wdtOn) {
        esp_task_wdt_config_t wc = {};
        wc.timeout_ms = 10000;
        wc.idle_core_mask = 0;
        wc.trigger_panic = true;
        if (esp_task_wdt_reconfigure(&wc) != ESP_OK) esp_task_wdt_init(&wc);
        wdtOn = esp_task_wdt_add(nullptr) == ESP_OK;
    }
    if (wdtOn) esp_task_wdt_reset();
    serial_poll();

    if (kbOk) {
        Units.update();
        while (!kb.empty()) {
            const auto &e = kb.oldest();
            if (e.type == m5::unit::tab5_keyboard::EventType::Hid && e.hid.keycode)
                ui_key(e.hid.keycode, e.modifier);
            kb.discard();
        }
        kb_leds();
    } else if (millis() - kbRetryMs > 5000) {
        kbRetryMs = millis();
        kbOk = kb_begin();
        if (kbOk) ui_dirty();
    }

    TouchEv e;
    while (xQueueReceive(touchQ, &e, 0)) {
        if (e.kind == 0) ui_press(e.x, e.y);
        else if (e.kind == 1) ui_drag(e.x, e.y);
        else if (e.kind == 2) ui_release(e.x, e.y);
        else ui_pinch(e.kind - 3, e.x, e.y, e.d, e.ms);
        if (e.kind == 0 || e.kind == 2) Serial.printf("touch %s %d,%d\n", e.kind ? "up" : "down", e.x, e.y);
    }

    uint32_t t0 = millis();
    ui_frame();
    uint32_t dt = millis() - t0;
    static uint32_t worst = 0, wDraw = 0, wPush = 0, lastLog = 0;
    if (dt > worst) worst = dt, wDraw = ui_drawMs, wPush = ui_pushMs;
    if (millis() - lastLog > 10000) {
        lastLog = millis();
        Serial.printf("frame worst %u ms (draw %u, push %u)\n", (unsigned)worst, (unsigned)wDraw, (unsigned)wPush);
        diagFrameWorst = worst, diagDrawWorst = wDraw, diagPushWorst = wPush;
        worst = 0;
    }
    delay(5);
}

bool kb_present() { return kbOk; }   // the Tab5 Keyboard answers (else the UI offers its own)
