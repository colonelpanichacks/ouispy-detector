// Wi-Fi, the dashboard's Socket.IO feed, its HTTP endpoints, and OTA.
//
// Two tasks:
//   link  - Wi-Fi up/down, mDNS, ArduinoOTA, Socket.IO loop (events land in the model)
//   poll  - /api/state on (re)connect, /api/stats and /api/metrics on a timer
// Neither touches the display: the UI task redraws when M->version moves.
#include "net.h"
#include "app.h"
#include "model.h"
#include "settings.h"
#include "feed.h"
#include "usbhost.h"
#include "ui.h"
#include "gfx.h"
#include <Preferences.h>
#include "maptiles.h"
#include "sdcard.h"
#include "wd.h"
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <esp_core_dump.h>
#include <HTTPClient.h>
#include <SocketIOclient.h>
#include <WiFi.h>

// main.cpp: health counters for /info
extern uint32_t diagBoots, diagFrameWorst, diagDrawWorst, diagPushWorst;
extern int diagReset;
const char *reset_name(int r);
extern uint32_t ui_prof[12];

namespace {

struct SpiRamAllocator : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
SpiRamAllocator psram;

// Socket.IO straight over a WebSocket (Engine.IO v4 allows opening with transport=websocket).
// The library's SocketIOclient first polls over HTTP and then sends the WebSocket upgrade on
// the same TCP connection, but the dashboard's server closes that connection after the poll
// reply ("Connection: close"), so the upgrade never arrived and it retried every 3 s.
// Same calls as SocketIOclient, so the code below is unchanged.
class SioLite {
  public:
    using Cb = void (*)(socketIOmessageType_t, uint8_t *, size_t);
    void begin(const char *host, uint16_t port, const char *path) {
        ws.begin(host, port, String(path) + "&transport=websocket");
        ws.onEvent([this](WStype_t t, uint8_t *p, size_t n) { on_ws(t, p, n); });
    }
    void onEvent(Cb c) { cb = c; }
    void setReconnectInterval(unsigned long ms) { ws.setReconnectInterval(ms); }
    void setExtraHeaders(const char *h) { ws.setExtraHeaders(h); }
    void loop() { ws.loop(); }
    void disconnect() { ws.disconnect(); }
    void send(socketIOmessageType_t, const char *) {}   // the namespace connect goes out on open
    bool sendEVENT(const String &json) {
        String m = "42" + json;
        return ws.sendTXT(m);
    }

  private:
    WebSocketsClient ws;
    Cb cb = nullptr;
    void on_ws(WStype_t t, uint8_t *p, size_t n) {
        if (t == WStype_DISCONNECTED) {
            if (cb) cb(sIOtype_DISCONNECT, nullptr, 0);
            return;
        }
        if (t != WStype_TEXT || !n) return;
        switch (p[0]) {                       // Engine.IO packet type
        case '0': ws.sendTXT("40"); break;    // open -> connect the default namespace
        case '2': ws.sendTXT("3"); break;     // ping -> pong
        case '4':                             // message: a Socket.IO packet follows
            if (n < 2 || !cb) break;
            if (p[1] == '0') cb(sIOtype_CONNECT, p + 2, n - 2);
            else if (p[1] == '1') cb(sIOtype_DISCONNECT, p + 2, n - 2);
            else if (p[1] == '2') cb(sIOtype_EVENT, p + 2, n - 2);
            else if (p[1] == '4') cb(sIOtype_ERROR, p + 2, n - 2);
            break;
        }
    }
};
SioLite sio;
String addr;               // resolved dashboard address (IP text)
volatile bool sockStarted = false;
StreamBufferHandle_t bridgeBuf;   // USB node lines waiting to go up ("line\n" records)
uint32_t bridgeHelloMs = 0, bridgeSentMs = 0;
bool bridging = false;
volatile bool wantState = false;
volatile bool wantDetail = false;   // re-read the open device's record

// Writes to the dashboard (alias, group), queued by the UI, sent by the poll task.
struct PostJob { char path[64]; char body[200]; };
QueueHandle_t postQ;
volatile bool wantScan = false;
bool otaReady = false;
bool mdnsUp = false;

constexpr int MAX_SCAN = 16;
String scanSsid[MAX_SCAN];
int scanRssi[MAX_SCAN];
int scanCount = -1;

void set_error(const char *fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    model_lock();
    if (strcmp(M->lastError, buf)) {
        strlcpy(M->lastError, buf, sizeof M->lastError);
        M->version++;
    }
    model_unlock();
}

// ------------------------------------------------------------------ Socket.IO

// Events we draw from; everything else (console lines, jobs, gps...) is skipped
// before it is parsed: the console alone is one event per serial line.
bool wanted(const char *name) {
    static const char *keep[] = {"fleet", "det", "sighting", "node_filters", "node_event", "dets_cleared", "bridge_tx",
                                 "drone", "groups", "mac_aliases", "tri", "wd", "wd_session"};
    for (auto k : keep)
        if (!strcmp(name, k)) return true;
    return false;
}

void on_event(uint8_t *payload, size_t len) {
    // payload: ["name", {...}]
    const char *p = (const char *)payload;
    const char *q1 = (const char *)memchr(p, '"', len);
    if (!q1) return;
    const char *q2 = (const char *)memchr(q1 + 1, '"', len - (q1 + 1 - p));
    if (!q2 || q2 - q1 > 24) return;
    char name[24];
    memcpy(name, q1 + 1, q2 - q1 - 1);
    name[q2 - q1 - 1] = 0;
    model_lock();
    M->lastEventMs = millis();
    model_unlock();
    if (!wanted(name)) return;

    JsonDocument doc(&psram);
    if (!strcmp(name, "node_filters")) {
        // Only who and whether it took; the list itself is big and unused here.
        JsonDocument filter;
        filter[1]["id"] = true;
        filter[1]["filters_ok"] = true;
        if (deserializeJson(doc, payload, len, DeserializationOption::Filter(filter))) return;
    } else if (deserializeJson(doc, payload, len)) {
        return;
    }
    JsonObjectConst body = doc[1];
    if (!strcmp(name, "bridge_tx")) {   // a console command for the node on our USB port
        if (usb_live()) usb_send(body["line"] | "");
        return;
    }
    if (!strcmp(name, "fleet")) ingest_fleet(body);
    else if (!strcmp(name, "det")) ingest_det(body, true);
    else if (!strcmp(name, "sighting")) ingest_sighting(body);
    else if (!strcmp(name, "dets_cleared")) ingest_cleared(body);
    else if (!strcmp(name, "drone")) ingest_drone(body);
    else if (!strcmp(name, "tri")) ingest_tri(body);
    else if (!strcmp(name, "wd")) {   // wardrive devices the dashboard just stored (any wardriver)
        model_lock();
        for (JsonObjectConst d : body["devices"].as<JsonArrayConst>()) wd_ingest(d, true);
        model_unlock();
    } else if (!strcmp(name, "wd_session")) {
        model_lock();
        wd_new_session();
        model_unlock();
    }
    else if (!strcmp(name, "mac_aliases")) { ingest_names(body, JsonObjectConst(), JsonObjectConst()); wantDetail = true; }
    else if (!strcmp(name, "groups")) { ingest_names(JsonObjectConst(), JsonObjectConst(), body["group_of"]); wantDetail = true; }
    else if (!strcmp(name, "node_event")) {
        model_lock();
        const char *who = node_name(body["id"] | "");
        toast("%s %s", who, (const char *)(body["event"] | ""));
        model_unlock();
    } else if (!strcmp(name, "node_filters")) {
        model_lock();
        const char *who = node_name(body["id"] | "");
        bool ok = body["filters_ok"] | false;
        toast(ok ? "%s filters updated" : "%s filter push failed", who);
        model_unlock();
    }
}

void sio_cb(socketIOmessageType_t type, uint8_t *payload, size_t len) {
    switch (type) {
    case sIOtype_CONNECT:
        sio.send(sIOtype_CONNECT, "/");
        model_lock();
        M->sockUp = true;
        M->version++;
        model_unlock();
        wantState = true;   // resync: the feed may have moved while we were away
        bridgeHelloMs = 0;  // re-announce a USB node right away
        set_error("");
        break;
    case sIOtype_DISCONNECT:
        model_lock();
        if (M->sockUp) {
            M->sockUp = false;
            M->version++;
        }
        model_unlock();
        break;
    case sIOtype_EVENT:
        on_event(payload, len);
        break;
    case sIOtype_ERROR:
        set_error("socket.io refused (token?)");
        break;
    default:
        break;
    }
}

// Dashboard address: typed in Settings, or (blank / "auto") found on the LAN, where the
// dashboard announces itself as _ouispy._tcp. Rediscovered on every reconnect, so a
// DHCP reshuffle on the Mac doesn't strand the display.
uint16_t port = 5077;

bool resolve() {
    String h = S.host;
    h.trim();
    port = S.port;
    if (!h.length() || h == "auto") {
        if (!mdnsUp) return false;
        int n = MDNS.queryService("ouispy", "tcp");
        if (n <= 0) {
            set_error("no OUI-SPY Mesh found on this network");
            return false;
        }
        addr = MDNS.address(0).toString();
        port = MDNS.port(0);
        return true;
    }
    if (h.endsWith(".local")) {
        if (!mdnsUp) return false;
        IPAddress ip = MDNS.queryHost(h.substring(0, h.length() - 6), 3000);
        if (ip == IPAddress()) {
            set_error("can't resolve %s", h.c_str());
            return false;
        }
        addr = ip.toString();
    } else {
        addr = h;
    }
    return addr.length() > 0;
}

// The library's default extra header is "Origin: file://", which the dashboard (same-origin
// Socket.IO only) refuses with a 400; its WebSocket upgrade also refuses an http:// origin.
// A non-browser client needs no Origin at all, so send none (only the token cookie, if set).
void sock_headers() {
    static String hdr;
    hdr = S.token.length() ? "Cookie: ouispy_token=" + S.token : String("");
    sio.setExtraHeaders(hdr.c_str());   // "" = no extra headers at all
}

void sock_begin() {
    if (!resolve()) return;
    sock_headers();
    sio.begin(addr.c_str(), port, "/socket.io/?EIO=4");
    sio.onEvent(sio_cb);
    sio.setReconnectInterval(3000);
    sockStarted = true;
}

// ------------------------------------------------------------------ bridge


void bridge_pump() {
    bool live = usb_live() && M->sockUp && S.source != SRC_USB;
    if (live != bridging) {
        bridging = live;
        if (!live && M->sockUp) sio.sendEVENT("[\"bridge_bye\"]");
        if (!live && bridgeBuf) xStreamBufferReset(bridgeBuf);
        model_lock();
        M->bridging = live;
        M->version++;
        model_unlock();
    }
    if (!live) return;
    uint32_t ms = millis();
    if (!bridgeHelloMs || ms - bridgeHelloMs > 30000) {
        bridgeHelloMs = ms;
        String ev = String("[\"bridge_hello\",{\"dev\":\"") + usb_device() + "\"}]";
        sio.sendEVENT(ev);
    }
    if (ms - bridgeSentMs < 150 || !xStreamBufferBytesAvailable(bridgeBuf)) return;
    bridgeSentMs = ms;
    static char chunk[8192];
    static String carry;   // a record split by the previous batch boundary
    size_t n = xStreamBufferReceive(bridgeBuf, chunk, sizeof chunk - 1, 0);
    chunk[n] = 0;
    String all = carry + chunk;
    JsonDocument doc(&psram);
    doc.add("bridge_lines");
    JsonArray lines = doc.add<JsonObject>()["lines"].to<JsonArray>();
    int start = 0;
    for (int nl; (nl = all.indexOf('\n', start)) >= 0; start = nl + 1)
        if (nl > start) lines.add(all.substring(start, nl));
    carry = all.substring(start);
    if (carry.length() > 4096) carry = "";
    if (!lines.size()) return;
    String out;
    serializeJson(doc, out);
    sio.sendEVENT(out);
}

// ------------------------------------------------------------------ OTA

void ota_begin() {
    if (otaReady || !S.otaPass.length()) return;
    ArduinoOTA.setHostname(S.hostname.c_str());
    ArduinoOTA.setPassword(S.otaPass.c_str());
    ArduinoOTA.setMdnsEnabled(false);   // mdns is already up under the same name
    ArduinoOTA.onStart([] {
        A.otaActive = true;
        A.otaPct = 0;
        sio.disconnect();
        model_lock();
        M->version++;
        model_unlock();
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
        uint8_t pct = total ? done * 100 / total : 0;
        if (pct != A.otaPct) {
            A.otaPct = pct;
            model_lock();
            M->version++;
            model_unlock();
        }
    });
    ArduinoOTA.onEnd([] { A.otaPct = 100; });
    ArduinoOTA.onError([](ota_error_t e) {
        A.otaActive = false;
        toast("OTA failed (%d)", (int)e);
    });
    ArduinoOTA.begin();
    otaReady = true;
    // Its own task: the link task can be busy for a while (Socket.IO, a screenshot), and
    // a missed invitation packet made every other update fail on the first try.
    xTaskCreatePinnedToCore([](void *) {
        for (;;) {
            ArduinoOTA.handle();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }, "ota", 8192, nullptr, 4, nullptr, 1);
}

// ------------------------------------------------------------------ link task

void do_scan() {
    int n = WiFi.scanNetworks();
    model_lock();
    scanCount = 0;
    for (int i = 0; i < n && scanCount < MAX_SCAN; i++) {
        String s = WiFi.SSID(i);
        if (!s.length()) continue;
        bool dup = false;
        for (int j = 0; j < scanCount; j++) dup |= scanSsid[j] == s;
        if (dup) continue;
        scanSsid[scanCount] = s;
        scanRssi[scanCount++] = WiFi.RSSI(i);
    }
    M->version++;
    model_unlock();
    WiFi.scanDelete();
}

// LAN debug port: GET /shot?k=<OTA password> streams the screen (same format as the serial
// "shot"), /key?k=..&c=<hid hex> presses a key. Off without an OTA password.
WiFiServer dbgSrv(8081);
bool dbgUp = false;

void debug_poll() {
    if (!S.otaPass.length()) return;
    if (!dbgUp) {
        dbgSrv.begin();
        dbgUp = true;
    }
    WiFiClient c = dbgSrv.accept();
    if (!c) return;
    c.setTimeout(2000);
    String req = c.readStringUntil('\n');
    long clen = 0;
    while (c.connected()) {   // drain the headers
        String h = c.readStringUntil('\n');
        if (h.length() <= 1) break;
        if (h.startsWith("Content-Length:") || h.startsWith("content-length:")) clen = h.substring(15).toInt();
    }
    String key = "k=" + S.otaPass;
    if (req.indexOf(key) < 0) {
        c.print("HTTP/1.0 403 Forbidden\r\n\r\n");
    } else if (req.startsWith("GET /shot")) {
        c.print("HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n\r\n");
        ui_screenshot(c);
    } else if (req.startsWith("GET /info")) {
        Preferences p;
        p.begin("tab5diag", true);
        uint32_t crashes = p.getUInt("crashes", 0);
        p.end();
        char b[1000];
        snprintf(b, sizeof b,
                 "{\"up\":%lu,\"reset\":\"%s\",\"boots\":%lu,\"crashes\":%lu,"
                 "\"heap\":%u,\"heap_min\":%u,\"heap_big\":%u,\"psram\":%u,\"psram_min\":%u,\"psram_big\":%u,"
                 "\"frame_worst\":%lu,\"draw\":%lu,\"push\":%lu,\"tiles\":%lu,\"tile_stack_free\":%lu,"
                 "\"sd\":\"%s\",\"sd_tiles\":%lu,\"link_stack_free\":%u,\"rssi\":%d,\"sock\":%d,\"ppa\":%d,"
                 "\"prof_us\":{\"clear\":%lu,\"view\":%lu,\"top\":%lu,\"bottom\":%lu,\"view_id\":%lu,"
                 "\"sync\":%lu,\"ppa_push\":%lu,\"m_unused\":%lu,\"m_tiles\":%lu,\"m_rings\":%lu,\"m_tgt_nodes\":%lu,\"m_drones\":%lu,\"m_chrome\":%lu}}\n",
                 (unsigned long)(millis() / 1000), reset_name(diagReset), (unsigned long)diagBoots, (unsigned long)crashes,
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM), heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                 heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                 (unsigned long)diagFrameWorst, (unsigned long)diagDrawWorst, (unsigned long)diagPushWorst,
                 (unsigned long)maptiles_cached(), (unsigned long)maptiles_stack_min(), sd_state(), (unsigned long)sd_tiles(),
                 (unsigned)uxTaskGetStackHighWaterMark(nullptr), WiFi.RSSI(), M->sockUp ? 1 : 0, gfx_ok() ? 1 : 0,
                 (unsigned long)ui_prof[0], (unsigned long)ui_prof[1], (unsigned long)ui_prof[2], (unsigned long)ui_prof[3],
                 (unsigned long)ui_prof[5], (unsigned long)ui_prof[4], (unsigned long)ui_prof[6], 0UL, (unsigned long)ui_prof[7], (unsigned long)ui_prof[8],
                 (unsigned long)ui_prof[9], (unsigned long)ui_prof[10], (unsigned long)ui_prof[11]);
        c.print("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n");
        c.print(b);
    } else if (req.startsWith("GET /crash")) {
        // The last panic, from the core dump the framework saved to flash: task, PC, cause,
        // return address and the code addresses found on its stack (decode them with the
        // ELF whose sha256 starts with "elf").
        esp_core_dump_summary_t *cs = (esp_core_dump_summary_t *)heap_caps_malloc(sizeof(*cs), MALLOC_CAP_SPIRAM);
        c.print("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n");
        if (!cs || esp_core_dump_get_summary(cs) != ESP_OK) {
            c.print("{\"dump\":false}\n");
        } else {
            char b[160];
            snprintf(b, sizeof b, "{\"dump\":true,\"task\":\"%s\",\"pc\":\"0x%08lx\",\"ra\":\"0x%08lx\",\"mcause\":%lu,\"mtval\":\"0x%08lx\",\"elf\":\"",
                     cs->exc_task, (unsigned long)cs->exc_pc, (unsigned long)cs->ex_info.ra,
                     (unsigned long)cs->ex_info.mcause, (unsigned long)cs->ex_info.mtval);
            c.print(b);
            for (int i = 0; i < 8; i++) c.printf("%02x", cs->app_elf_sha256[i]);
            c.print("\",\"stack\":[");
            int n = 0;
            for (uint32_t i = 0; i + 4 <= cs->exc_bt_info.dump_size && n < 40; i += 4) {
                uint32_t v;
                memcpy(&v, cs->exc_bt_info.stackdump + i, 4);
                if ((v >= 0x40000000 && v < 0x44000000) || (v >= 0x4ff00000 && v < 0x4ffc0000))
                    c.printf("%s\"0x%08lx\"", n++ ? "," : "", (unsigned long)v);
            }
            c.print("]}\n");
        }
        free(cs);
    } else if (req.indexOf(" /sd/") > 0) {
        // Map packs onto the card over Wi-Fi: PUT /sd/put?f=<name.pak>&off=<bytes> (body =
        // the next piece), GET /sd/done?f=<name.pak> mounts it, GET /sd/ls lists the folder.
        auto arg = [&](const char *k) {
            int i = req.indexOf(k);
            if (i < 0) return String();
            int e = req.indexOf('&', i);
            int sp = req.indexOf(' ', i);
            if (e < 0 || (sp >= 0 && sp < e)) e = sp;
            return req.substring(i + strlen(k), e);
        };
        String f = arg("f=");
        if (req.startsWith("PUT /sd/put")) {
            long sz = sd_put(f.c_str(), arg("off=").toInt(), c, clen);
            c.printf("HTTP/1.0 %s\r\n\r\n%ld\n", sz >= 0 ? "200 OK" : "400 Bad Request", sz);
        } else if (req.startsWith("GET /sd/size")) {
            c.printf("HTTP/1.0 200 OK\r\n\r\n%ld\n", sd_part_size(f.c_str()));
        } else if (req.startsWith("GET /sd/done")) {
            bool ok = sd_finish(f.c_str());
            c.printf("HTTP/1.0 %s\r\n\r\n%s\n", ok ? "200 OK" : "400 Bad Request", sd_state());
        } else {
            c.print("HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n");
            sd_list(c);
        }
    } else if (req.startsWith("GET /key")) {
        int i = req.indexOf("c=");
        uint8_t kc = i >= 0 ? (uint8_t)strtol(req.c_str() + i + 2, nullptr, 16) : 0;
        if (kc) ui_key(kc, 0);
        c.print("HTTP/1.0 200 OK\r\n\r\nok\n");
    } else {
        c.print("HTTP/1.0 404 Not Found\r\n\r\n");
    }
    c.stop();
}

void link_task(void *) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(S.hostname.c_str());
    WiFi.setAutoReconnect(true);
    WiFi.begin(S.ssid.c_str(), S.pass.c_str());
    uint32_t tryMs = millis(), downSince = 0;
    bool wasUp = false;
    for (;;) {
        if (wantScan) {
            wantScan = false;
            do_scan();
        }
        bool up = WiFi.status() == WL_CONNECTED;
        if (up != wasUp) {
            wasUp = up;
            model_lock();
            M->wifiUp = up;
            M->version++;
            model_unlock();
            if (up) {
                if (!mdnsUp) mdnsUp = MDNS.begin(S.hostname.c_str());
                ota_begin();
                if (!sockStarted && S.source != SRC_USB) sock_begin();
            }
        }
        if (!up && S.ssid.length() && millis() - tryMs > 20000) {
            tryMs = millis();
            set_error("Wi-Fi: no link to %s", S.ssid.c_str());
            WiFi.disconnect();
            WiFi.begin(S.ssid.c_str(), S.pass.c_str());
        }
        if (up && !sockStarted && S.source != SRC_USB && millis() - tryMs > 5000) {   // name didn't resolve yet
            tryMs = millis();
            sock_begin();
        }
        // Lost the dashboard for 30 s: look it up again (DHCP may have moved it).
        if (sockStarted && !M->sockUp && !A.otaActive) {
            if (!downSince) downSince = millis(), node_memo_flush(true);   // offline from here: keep what we know
            if (millis() - downSince > 30000) {
                downSince = millis();
                String oldAddr = addr;
                uint16_t oldPort = port;
                if (resolve() && (addr != oldAddr || port != oldPort)) {
                    sio.disconnect();
                    sock_headers();
                    sio.begin(addr.c_str(), port, "/socket.io/?EIO=4");
                }
            }
        } else {
            downSince = 0;
        }
        if (up && !A.otaActive) debug_poll();
        if (sockStarted && !A.otaActive) {
            sio.loop();
            bridge_pump();
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// ------------------------------------------------------------------ poll task

bool http_json_once(const String &path, JsonDocument &doc, JsonDocument *filter);

// One bulk download at a time (dashboard JSON or a map tile), and only with internal RAM
// to spare: every byte arrives through the Wi-Fi driver's internal-RAM buffers, and two
// replies at once (or one into a nearly full heap) starved it until it asserted.
bool http_json(const String &path, JsonDocument &doc, JsonDocument *filter) {
    net_bulk_begin();
    bool ok = http_json_once(path, doc, filter);
    net_bulk_end();
    return ok;
}

bool http_json_once(const String &path, JsonDocument &doc, JsonDocument *filter) {
    if (!addr.length()) return false;
    WiFiClient c;
    HTTPClient h;
    h.useHTTP10(true);   // no chunked encoding: parse straight off the socket
    h.setTimeout(10000);
    h.setConnectTimeout(4000);
    if (!h.begin(c, addr, port, path)) return false;
    if (S.token.length()) h.addHeader("Cookie", "ouispy_token=" + S.token);
    int code = h.GET();
    if (code != 200) {
        set_error(code == 401 ? "dashboard wants the access token" : "HTTP %d on %s", code, path.c_str());
        h.end();
        return false;
    }
    // Drain the body into PSRAM in big reads first, then parse from memory. Parsing straight
    // off the socket is slow, and meanwhile the dashboard fills the whole 64 KB TCP window,
    // all of it in internal-RAM network buffers: /api/state drained internal RAM to a few KB
    // and the Wi-Fi driver crashed allocating its own buffers.
    int len = h.getSize();
    size_t cap = len > 0 ? (size_t)len + 1 : 64 * 1024, n = 0;
    char *buf = (char *)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    WiFiClient *st = h.getStreamPtr();
    uint32_t t0 = millis();
    while (buf && st && millis() - t0 < 15000) {
        int a = st->available();
        if (a > 0) {
            if (n + a + 1 > cap) {   // unknown length: grow (2 MB cap)
                size_t nc = min<size_t>(cap * 2, 2 * 1024 * 1024);
                if (nc <= cap) break;
                char *nb = (char *)heap_caps_realloc(buf, nc, MALLOC_CAP_SPIRAM);
                if (!nb) break;
                buf = nb, cap = nc;
            }
            n += st->readBytes(buf + n, min((size_t)a, cap - n - 1));
            if (len > 0 && (int)n >= len) break;
        } else if (!h.connected()) {
            break;
        } else {
            delay(2);
        }
    }
    h.end();
    if (!buf) {
        set_error("no memory for %s", path.c_str());
        return false;
    }
    DeserializationError e = filter ? deserializeJson(doc, buf, n, DeserializationOption::Filter(*filter))
                                    : deserializeJson(doc, buf, n);
    free(buf);
    if (e) {
        set_error("bad JSON on %s: %s", path.c_str(), e.c_str());
        return false;
    }
    return true;
}

void fetch_state() {
    JsonDocument filter;
    filter["mode"] = true;
    filter["stats"] = true;
    filter["hourly"] = true;
    filter["recent"] = true;
    filter["drones"] = true;
    filter["tri"] = true;
    filter["mac_aliases"] = true;
    filter["device_names"] = true;
    filter["group_of"] = true;
    filter["fleet"]["home"] = true;
    filter["fleet"]["ts"] = true;
    JsonObject nf = filter["fleet"]["nodes"][0].to<JsonObject>();
    for (const char *k : {"id", "alias", "role", "online", "is_home", "rssi", "age", "since", "color",
                          "filters_fresh", "cfg", "ota", "lat", "lon", "tlm"})
        nf[k] = true;
    JsonDocument doc(&psram);
    if (http_json("/api/state?for=tab5", doc, &filter)) ingest_state(doc);
    else wantState = true;
}

// The map's wardrive History view: every placed device the dashboard has.
void fetch_wardrive() {
    JsonDocument filter;
    JsonObject f = filter["devices"][0].to<JsonObject>();
    for (const char *k : {"kind", "mac", "name", "rssi", "sec", "ch", "lat", "lon", "last", "seen", "now"}) f[k] = true;
    JsonDocument doc(&psram);
    if (!http_json("/api/wardrive?scope=history&placed=1&limit=6000", doc, &filter)) return;
    model_lock();
    for (JsonObjectConst d : doc["devices"].as<JsonArrayConst>()) wd_ingest(d, false);
    model_unlock();
}

// One wardriven network / device, decoded by the dashboard, for the Tab5's info page.
void fetch_wd_info() {
    char mac[18];
    uint8_t kind;
    model_lock();
    strlcpy(mac, A.wdOpen, sizeof mac);
    kind = A.wdOpenKind;
    model_unlock();
    if (!mac[0]) return;
    String path = String("/api/wardrive/device/") + mac + (kind == 2 ? "?kind=ble" : "?kind=wifi");
    JsonDocument doc(&psram);
    bool ok = http_json(path, doc, nullptr);
    model_lock();
    WdInfo &w = M->wdInfo;
    memset(&w, 0, sizeof w);
    strlcpy(w.mac, mac, sizeof w.mac);
    w.failed = !ok;
    if (ok) {
        auto add = [&](const char *k, const String &v) {
            if (!v.length() || w.n >= 16) return;
            strlcpy(w.k[w.n], k, sizeof w.k[0]);
            strlcpy(w.v[w.n], v.c_str(), sizeof w.v[0]);
            w.n++;
        };
        JsonObjectConst dec = doc["decoded"];
        const char *name = doc["name"] | "";
        strlcpy(w.title, *name ? name : kind == 1 ? "(hidden network)" : (doc["vendor"] | "BLE device"), sizeof w.title);
        add("TYPE", kind == 1 ? "WiFi access point" : "Bluetooth LE");
        add("ADDRESS", String(dec["address"] | ""));
        add("MAKER (OUI)", String(doc["vendor"] | (doc["rnd"] | false ? "random address: no maker in it" : "not registered")));
        if (kind == 1) {
            String sec = doc["sec"] | "?";
            add("SECURITY", sec + "   channel " + String((int)(doc["ch"] | 0)));
            add("STANDARD", String(dec["standard"] | ""));
            add("TOP RATE", String(dec["top legacy rate"] | ""));
            add("COUNTRY", String(dec["country"] | ""));
            if (dec["hidden"] | false) add("HIDDEN", "yes, the SSID is blanked in its beacons");
            if (dec["WPS"] | false) add("WPS", "on");
            String vt;
            for (JsonObjectConst t : dec["vendor tags"].as<JsonArrayConst>()) vt += (vt.length() ? ", " : "") + String(t["name"] | "");
            add("VENDOR TAGS", vt);
        } else {
            if (!dec["company"].isNull()) add("COMPANY", String(dec["company"]["name"] | "") + " (" + String(dec["company"]["id"] | "") + ")");
            add("APPEARANCE", String(dec["appearance"] | ""));
            String sv;
            for (JsonObjectConst u : dec["services"].as<JsonArrayConst>())
                sv += (sv.length() ? ", " : "") + String(u["name"] | "") + " (" + String(u["uuid"] | "") + ")";
            add("SERVICES", sv);
            add("TX POWER", String(dec["tx power"] | ""));
            String fl;
            for (JsonVariantConst f : dec["flags"].as<JsonArrayConst>()) fl += (fl.length() ? ", " : "") + String(f.as<const char *>());
            add("FLAGS", fl);
        }
        add("BEST SIGNAL", String((int)(doc["rssi"] | -127)) + " dBm");
        add("SEEN", String((int)(doc["seen"] | 0)) + "x in " + String((int)(doc["sessions"] | 1)) + " session(s)");
        if (doc["lat"].is<double>()) add("PLACED AT", String((double)doc["lat"], 6) + ", " + String((double)doc["lon"], 6) + "  (where it was loudest)");
        else add("PLACED AT", "not placed: no GPS fix when it was heard");
        w.loaded = true;
    }
    M->version++;
    model_unlock();
}

void fetch_stats() {
    JsonDocument doc(&psram);
    if (http_json("/api/stats", doc, nullptr)) ingest_stats(doc);
}

void fetch_metrics() {
    uint32_t range = RANGES[A.range % NRANGES];
    String path = "/api/metrics?range=" + String(range);
    if (A.node[0]) path += "&node=" + String(A.node);
    if (A.dom) path += A.dom == 1 ? "&dom=ble" : "&dom=wifi";
    JsonDocument filter;
    for (const char *k : {"bucket", "start", "until", "total", "prev_total", "unique", "new_devices",
                          "nodes_active", "n", "peak", "series", "unique_series", "by_vendor", "by_node",
                          "by_type", "by_dom", "rssi", "wd"})
        filter[k] = true;   // everything but the 7x24 heat grid
    JsonDocument doc(&psram);
    if (http_json(path, doc, &filter)) ingest_metrics(doc, range);
}

bool http_post(const char *path, const char *body) {
    if (!addr.length()) return false;
    WiFiClient c;
    HTTPClient h;
    h.setTimeout(8000);
    h.setConnectTimeout(4000);
    if (!h.begin(c, addr, port, path)) return false;
    h.addHeader("Content-Type", "application/json");
    if (S.token.length()) h.addHeader("Cookie", "ouispy_token=" + S.token);
    int code = h.POST((uint8_t *)body, strlen(body));
    h.end();
    return code == 200;
}

void fetch_detail() {
    char mac[18];
    model_lock();
    strlcpy(mac, M->detail.mac, sizeof mac);
    model_unlock();
    if (!mac[0]) return;
    JsonDocument doc(&psram);
    if (http_json(String("/api/devices/") + mac + "?flat=1", doc, nullptr)) ingest_detail(doc);
    else {
        model_lock();
        M->detail.loading = false;
        M->version++;
        model_unlock();
    }
}

void poll_task(void *) {
    uint32_t lastStats = 0, lastMetrics = 0, lastState = 0, lastPrune = 0, gen = 0;
    char lastDetail[18] = "";
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (A.otaActive || !M->wifiUp || !addr.length() || S.source == SRC_USB) continue;
        uint32_t now = millis();
        PostJob job;
        if (xQueueReceive(postQ, &job, 0) == pdTRUE) {
            bool ok = http_post(job.path, job.body);
            toast(ok ? "saved on the dashboard" : "dashboard didn't take it");
            wantDetail = true;
            continue;
        }
        static char lastWdOpen[18] = "";
        if (strcmp(lastWdOpen, A.wdOpen)) {
            strlcpy(lastWdOpen, A.wdOpen, sizeof lastWdOpen);
            if (lastWdOpen[0]) fetch_wd_info();
            continue;
        }
        if (strcmp(lastDetail, M->detail.mac) || wantDetail) {
            wantDetail = false;
            strlcpy(lastDetail, M->detail.mac, sizeof lastDetail);
            fetch_detail();
            continue;
        }
        // /api/state on every socket (re)connect, and every 10 min as a safety net.
        if ((wantState && M->sockUp) || (M->sockUp && now - lastState > 600000)) {
            wantState = false;
            lastState = now;
            fetch_state();
            lastStats = now;
            continue;
        }
        if (now - lastStats > 30000) {
            lastStats = now;
            fetch_stats();
        }
        node_memo_flush(false);
        static uint32_t lastWd = 0;
        if (A.wdScope == 2 && M->sockUp && (!lastWd || now - lastWd > 300000)) {
            lastWd = now;
            fetch_wardrive();
            continue;
        }
        if (gen != A.metricsGen || now - lastMetrics > 30000) {
            gen = A.metricsGen;
            lastMetrics = now;
            fetch_metrics();
        }
        if (now - lastPrune > 5000) {
            lastPrune = now;
            prune_targets();
        }
    }
}

}  // namespace

static SemaphoreHandle_t bulkLock = xSemaphoreCreateMutex();
void net_bulk_begin() {
    xSemaphoreTake(bulkLock, portMAX_DELAY);
    for (int i = 0; i < 100 && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 48 * 1024; i++) delay(50);
}
void net_bulk_end() { xSemaphoreGive(bulkLock); }

void net_bridge_line(const char *line) {
    if (!bridging || !bridgeBuf) return;
    size_t n = strlen(line);
    if (xStreamBufferSpacesAvailable(bridgeBuf) < n + 1) return;   // dashboard lagging: drop
    xStreamBufferSend(bridgeBuf, line, n, 0);
    xStreamBufferSend(bridgeBuf, "\n", 1, 0);
}

void net_start() {
    bridgeBuf = xStreamBufferCreate(32768, 1);
    postQ = xQueueCreate(8, sizeof(PostJob));
    if (!wifi_wanted()) return;
    // Socket.IO + ArduinoOTA + scans on one task, blocking HTTP on the other, so a slow
    // /api/state never stalls the live feed.
    xTaskCreatePinnedToCore(link_task, "link", 12288, nullptr, 3, nullptr, 0);
    xTaskCreatePinnedToCore(poll_task, "poll", 12288, nullptr, 2, nullptr, 0);
}

String net_ip() { return WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String(); }
int net_rssi() { return WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0; }
String net_target() {
    if (addr.length()) return addr + ":" + port;
    return S.host.length() ? S.host + ":" + S.port : String("searching for OUI-SPY Mesh");
}
bool net_ota_enabled() { return otaReady; }
bool net_online() { return M->sockUp && S.source != SRC_USB; }
String net_base() { return addr.length() ? "http://" + addr + ":" + String(port) : String(); }

bool net_post(const char *path, const char *body) {
    if (!postQ || !net_online()) return false;
    PostJob j;
    strlcpy(j.path, path, sizeof j.path);
    strlcpy(j.body, body, sizeof j.body);
    return xQueueSend(postQ, &j, 0) == pdTRUE;
}

void net_request_scan() {
    if (!settings_ready()) {
        // No link task yet (first boot): scan on a one-shot task so the setup screen
        // paints at once instead of waiting seconds for the C6.
        static volatile bool scanning = false;
        if (scanning) return;
        scanning = true;
        xTaskCreatePinnedToCore([](void *) {
            WiFi.mode(WIFI_STA);
            do_scan();
            scanning = false;
            vTaskDelete(nullptr);
        }, "scan", 8192, nullptr, 2, nullptr, 0);
        return;
    }
    wantScan = true;
}

int net_scan_results(String *out, int *rssi, int max) {
    int n = 0;
    for (int i = 0; i < scanCount && n < max; i++, n++) {
        out[n] = scanSsid[i];
        rssi[n] = scanRssi[i];
    }
    return scanCount < 0 ? -1 : n;
}
