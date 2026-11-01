#include "marks.h"
#include "model.h"
#include "net.h"
#include "sdcard.h"
#include "app.h"
#include <sys/stat.h>

const char *const MARK_ICON_IDS[MARK_ICONS] = {"pin", "flag", "target", "house", "car", "person",
                                               "drone", "camera", "warning", "star", "camp", "radio"};

namespace {
struct SpiRamAllocator : ArduinoJson::Allocator {
    void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void *p) override { heap_caps_free(p); }
    void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
SpiRamAllocator psram;
constexpr const char *FILE_PATH = "/sdcard/state/marks.json";
Mark *marks = nullptr;
int n = 0;
bool dirty = false;   // the card copy is behind

bool alloc() {
    if (!marks) marks = (Mark *)ps_calloc(MAX_MARKS, sizeof(Mark));
    return marks != nullptr;
}

int icon_index(const char *s) {
    for (int i = 0; i < MARK_ICONS; i++)
        if (s && !strcmp(s, MARK_ICON_IDS[i])) return i;
    return 0;
}

void from_json(Mark &m, JsonObjectConst o) {
    memset(&m, 0, sizeof m);
    strlcpy(m.id, o["id"] | "", sizeof m.id);
    strlcpy(m.name, o["name"] | "", sizeof m.name);
    m.kind = !strcmp(o["kind"] | "pin", "area") ? 1 : 0;
    m.icon = icon_index(o["icon"] | "pin");
    m.hue = (o["hue"] | 190) % 360;
    m.lat = o["lat"] | 0.0, m.lon = o["lon"] | 0.0;
    for (JsonArrayConst p : o["pts"].as<JsonArrayConst>()) {
        if (m.npts == MARK_PTS) break;
        m.pts[m.npts][0] = p[0] | 0.0f, m.pts[m.npts][1] = p[1] | 0.0f;
        m.npts++;
    }
    m.pending = o["pending"] | false;
    m.deleted = o["deleted"] | false;
}

void to_json(const Mark &m, JsonObject o, bool local) {
    o["id"] = m.id;
    if (m.deleted && !local) {
        o["delete"] = true;
        return;
    }
    o["kind"] = m.kind ? "area" : "pin";
    o["name"] = m.name;
    o["hue"] = m.hue;
    o["icon"] = MARK_ICON_IDS[m.icon % MARK_ICONS];
    if (m.kind) {
        JsonArray pts = o["pts"].to<JsonArray>();
        for (int i = 0; i < m.npts; i++) {
            JsonArray p = pts.add<JsonArray>();
            p.add(m.pts[i][0]), p.add(m.pts[i][1]);
        }
    } else {
        o["lat"] = m.lat, o["lon"] = m.lon;
    }
    if (local) o["pending"] = m.pending, o["deleted"] = m.deleted;
}
}  // namespace

int marks_count() { return n; }
Mark *mark_at(int i) { return i >= 0 && i < n ? &marks[i] : nullptr; }

int mark_find(const char *id) {
    for (int i = 0; i < n; i++)
        if (!strcmp(marks[i].id, id)) return i;
    return -1;
}

void mark_new_id(char *out, size_t cap) {
    snprintf(out, cap, "t%lx%04lx", (unsigned long)(time(nullptr) > 1700000000 ? time(nullptr) : millis()),
             (unsigned long)(esp_random() & 0xffff));
}

void mark_put(const Mark &m) {
    if (!alloc()) return;
    int i = mark_find(m.id);
    if (i < 0) {
        if (n == MAX_MARKS) return;
        i = n++;
    }
    marks[i] = m;
    marks[i].pending = true, marks[i].deleted = false;
    if (marks[i].kind && marks[i].npts) {   // an area's centre: where its name goes
        double la = 0, lo = 0;
        for (int k = 0; k < marks[i].npts; k++) la += marks[i].pts[k][0], lo += marks[i].pts[k][1];
        marks[i].lat = la / marks[i].npts, marks[i].lon = lo / marks[i].npts;
    }
    dirty = true;
    M->version++;
}

void mark_delete(const char *id) {
    int i = mark_find(id);
    if (i < 0) return;
    marks[i].deleted = true, marks[i].pending = true;
    dirty = true;
    M->version++;
}

void marks_ingest(JsonArrayConst arr) {
    if (!alloc()) return;
    // Ours not sent yet win over the dashboard's copy; everything else is the dashboard's.
    static Mark keep[MAX_MARKS];
    int nk = 0;
    for (int i = 0; i < n; i++)
        if (marks[i].pending) keep[nk++] = marks[i];
    n = 0;
    for (JsonObjectConst o : arr) {
        if (n == MAX_MARKS) break;
        from_json(marks[n], o);
        if (marks[n].id[0]) n++;
    }
    for (int k = 0; k < nk; k++) {
        int i = mark_find(keep[k].id);
        if (i >= 0) marks[i] = keep[k];
        else if (n < MAX_MARKS) marks[n++] = keep[k];
    }
    dirty = true;
    M->version++;
}

void marks_load() {
    if (!alloc() || !sd_ok()) return;
    FILE *f = fopen(FILE_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = len > 0 && len < 256 * 1024 ? (char *)ps_malloc(len) : nullptr;
    bool ok = buf && fread(buf, 1, len, f) == (size_t)len;
    fclose(f);
    JsonDocument doc(&psram);
    ok = ok && !deserializeJson(doc, buf, len);
    free(buf);
    if (!ok) return;
    model_lock();
    n = 0;
    for (JsonObjectConst o : doc["marks"].as<JsonArrayConst>()) {
        if (n == MAX_MARKS) break;
        from_json(marks[n], o);
        if (marks[n].id[0]) n++;
    }
    model_unlock();
}

void marks_sync() {
    if (!marks) return;
    // Send each change (one at a time, the lock not held over the network).
    if (net_online()) {
        for (int guard = 0; guard < MAX_MARKS; guard++) {
            char body[1400] = "";
            char id[25] = "";
            bool del = false;
            model_lock();
            for (int i = 0; i < n && !id[0]; i++) {
                if (!marks[i].pending) continue;
                JsonDocument d;
                to_json(marks[i], d.to<JsonObject>(), false);
                serializeJson(d, body, sizeof body);
                strlcpy(id, marks[i].id, sizeof id);
                del = marks[i].deleted;
            }
            model_unlock();
            if (!id[0]) break;
            bool ok = net_post_now("/api/marks", body);
            model_lock();
            int i = mark_find(id);
            if (i >= 0 && ok) {
                if (del) marks[i] = marks[--n];
                else marks[i].pending = false;
                dirty = true;
            }
            model_unlock();
            if (!ok) break;   // the dashboard said no or isn't there: try again later
        }
    }
    // The card copy: what we have, unsent changes included (they survive a reboot).
    if (dirty && sd_ok()) {
        JsonDocument doc(&psram);
        JsonArray a = doc["marks"].to<JsonArray>();
        model_lock();
        for (int i = 0; i < n; i++) to_json(marks[i], a.add<JsonObject>(), true);
        dirty = false;
        model_unlock();
        mkdir("/sdcard/state", 0775);
        if (FILE *f = fopen("/sdcard/state/marks.tmp", "wb")) {
            String out;
            serializeJson(doc, out);
            bool ok = fwrite(out.c_str(), 1, out.length(), f) == out.length();
            fclose(f);
            if (ok) {
                remove(FILE_PATH);
                rename("/sdcard/state/marks.tmp", FILE_PATH);
            } else {
                dirty = true;
            }
        }
    }
}
