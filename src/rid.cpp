// Drone Remote ID decoding and reporting. See rid.h.
#include "rid.h"
#include "mesh.h"
extern "C" {
#include "opendroneid.h"
#include "odid_wifi.h"
}

bool isSerialConnected();
void ridOnNewDrone();   // main.cpp

#define RID_MAX        12
#define RID_REPORT_MS  1000UL     // per drone, at most this often (serial + mesh)
#define RID_EXPIRE_MS  60000UL

enum : uint8_t { RID_SRC_BEACON = 0, RID_SRC_NAN = 1, RID_SRC_BLE = 2 };
enum : uint8_t { H_BASIC = 1, H_LOC = 2, H_SYS = 4, H_OP = 8, H_SELF = 16 };

struct Drone {
    bool used, dirty, fresh;
    uint8_t mac[6];
    int8_t rssi;
    uint8_t src, has;
    uint32_t seen, reported;
    char id[ODID_ID_SIZE + 1];
    uint8_t idType, uaType;
    char opId[ODID_ID_SIZE + 1];
    char desc[ODID_STR_SIZE + 1];
    double lat, lon, plat, plon;
    float altGeo, altBaro, height, speed, vspeed, dir, palt;
    uint8_t status, opLocType;
};

static Drone drones[RID_MAX];
static portMUX_TYPE ridMux = portMUX_INITIALIZER_UNLOCKED;
// One decode buffer per producer task (WiFi driver, NimBLE host): ODID_UAS_Data
// is ~1 KB, too big for those stacks and not shared between them.
static ODID_UAS_Data wifiUas, bleUas;

static void merge(const uint8_t mac[6], int8_t rssi, uint8_t src, const ODID_UAS_Data& u) {
    uint8_t has = (u.BasicIDValid[0] ? H_BASIC : 0) | (u.LocationValid ? H_LOC : 0) |
                  (u.SystemValid ? H_SYS : 0) | (u.OperatorIDValid ? H_OP : 0) | (u.SelfIDValid ? H_SELF : 0);
    if (!has) return;
    uint32_t now = millis();
    portENTER_CRITICAL(&ridMux);
    int slot = -1, oldest = 0;
    for (int i = 0; i < RID_MAX; i++) {
        if (drones[i].used && !memcmp(drones[i].mac, mac, 6)) { slot = i; break; }
    }
    if (slot < 0) {
        for (int i = 0; i < RID_MAX; i++) {
            if (!drones[i].used) { slot = i; break; }
            if (drones[i].seen < drones[oldest].seen) oldest = i;
        }
        if (slot < 0) slot = oldest;
        memset(&drones[slot], 0, sizeof(Drone));
        drones[slot].used = true;
        drones[slot].fresh = true;
        memcpy(drones[slot].mac, mac, 6);
    }
    Drone& d = drones[slot];
    d.rssi = rssi;
    d.src = src;
    d.seen = now;
    d.has |= has;
    if (u.BasicIDValid[0]) {
        strlcpy(d.id, u.BasicID[0].UASID, sizeof(d.id));
        d.idType = u.BasicID[0].IDType;
        d.uaType = u.BasicID[0].UAType;
    }
    if (u.LocationValid) {
        d.lat = u.Location.Latitude; d.lon = u.Location.Longitude;
        d.altGeo = u.Location.AltitudeGeo; d.altBaro = u.Location.AltitudeBaro;
        d.height = u.Location.Height; d.speed = u.Location.SpeedHorizontal;
        d.vspeed = u.Location.SpeedVertical; d.dir = u.Location.Direction;
        d.status = u.Location.Status;
    }
    if (u.SystemValid) {
        d.plat = u.System.OperatorLatitude; d.plon = u.System.OperatorLongitude;
        d.palt = u.System.OperatorAltitudeGeo; d.opLocType = u.System.OperatorLocationType;
    }
    if (u.OperatorIDValid) strlcpy(d.opId, u.OperatorID.OperatorId, sizeof(d.opId));
    if (u.SelfIDValid) strlcpy(d.desc, u.SelfID.Desc, sizeof(d.desc));
    d.dirty = true;
    portEXIT_CRITICAL(&ridMux);
}

void ridWifiFrame(const uint8_t* f, int len, int8_t rssi) {
    if (len < 24) return;
    static const uint8_t nanDest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
    if (!memcmp(f + 4, nanDest, 6)) {
        memset(&wifiUas, 0, sizeof(wifiUas));
        char op[6];
        if (odid_wifi_receive_message_pack_nan_action_frame(&wifiUas, op, (uint8_t*)f, len) == 0)
            merge(f + 10, rssi, RID_SRC_NAN, wifiUas);
        return;
    }
    if (f[0] != 0x80) return;   // beacon
    int off = 36;
    while (off + 2 <= len) {
        uint8_t typ = f[off], l = f[off + 1];
        if (off + 2 + l > len) break;
        if (typ == 0xDD && l > 5 &&
            ((f[off + 2] == 0x90 && f[off + 3] == 0x3A && f[off + 4] == 0xE6) ||
             (f[off + 2] == 0xFA && f[off + 3] == 0x0B && f[off + 4] == 0xBC))) {
            // vendor IE: OUI(3) type(1) msg counter(1), then the message pack
            memset(&wifiUas, 0, sizeof(wifiUas));
            if (odid_message_process_pack(&wifiUas, (uint8_t*)f + off + 7, l - 5) >= 0)
                merge(f + 10, rssi, RID_SRC_BEACON, wifiUas);
            return;
        }
        off += 2 + l;
    }
}

void ridBleAdvert(const uint8_t* adv, int len, const uint8_t mac[6], int8_t rssi) {
    // Walk the AD structures for service data 0xFFFA with app code 0x0D.
    for (int i = 0; i + 1 < len; ) {
        uint8_t l = adv[i];
        if (!l || i + 1 + l > len) break;
        const uint8_t* s = adv + i + 1;   // s[0] = AD type
        if (s[0] == 0x16 && l >= 6 && s[1] == 0xFA && s[2] == 0xFF && s[3] == 0x0D) {
            const uint8_t* odid = s + 5;  // after uuid(2), app code, msg counter
            int olen = l - 5;
            if (olen < 25) return;
            memset(&bleUas, 0, sizeof(bleUas));
            if ((odid[0] & 0xF0) == (ODID_MESSAGETYPE_PACKED << 4))
                odid_message_process_pack(&bleUas, (uint8_t*)odid, olen);
            else
                decodeOpenDroneID(&bleUas, (uint8_t*)odid);
            merge(mac, rssi, RID_SRC_BLE, bleUas);
            return;
        }
        i += 1 + l;
    }
}

// --- wire format (mesh body, little endian) ---------------------------------
// id3 rssi1 src1 has1 mac6 lat4 lon4 (1e-7 deg) altGeo2 altBaro2 height2 (m)
// speed2 (cm/s) vspeed2 (cm/s) dir2 (0.1 deg) status1 uaType1 idType1 opLoc1
// plat4 plon4 palt2 id20 opId20 desc23                                  = 109
static void put16(uint8_t*& p, int32_t v) { *p++ = v & 0xFF; *p++ = (v >> 8) & 0xFF; }
static void put32(uint8_t*& p, int32_t v) { put16(p, v & 0xFFFF); put16(p, (v >> 16) & 0xFFFF); }
static int16_t get16(const uint8_t*& p) { int16_t v = (int16_t)(p[0] | (p[1] << 8)); p += 2; return v; }
static int32_t get32(const uint8_t*& p) { int32_t v = (int32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); p += 4; return v; }
static int16_t clamp16(float v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)lroundf(v); }

static void encode(const Drone& d, const uint8_t selfId[3], uint8_t* b) {
    uint8_t* p = b;
    memcpy(p, selfId, 3); p += 3;
    *p++ = (uint8_t)d.rssi; *p++ = d.src; *p++ = d.has;
    memcpy(p, d.mac, 6); p += 6;
    put32(p, (int32_t)llround(d.lat * 1e7)); put32(p, (int32_t)llround(d.lon * 1e7));
    put16(p, clamp16(d.altGeo)); put16(p, clamp16(d.altBaro)); put16(p, clamp16(d.height));
    put16(p, (uint16_t)lroundf(d.speed * 100)); put16(p, clamp16(d.vspeed * 100)); put16(p, (uint16_t)lroundf(d.dir * 10));
    *p++ = d.status; *p++ = d.uaType; *p++ = d.idType; *p++ = d.opLocType;
    put32(p, (int32_t)llround(d.plat * 1e7)); put32(p, (int32_t)llround(d.plon * 1e7));
    put16(p, clamp16(d.palt));
    memset(p, 0, 20 + 20 + 23);
    memcpy(p, d.id, strnlen(d.id, 20)); p += 20;
    memcpy(p, d.opId, strnlen(d.opId, 20)); p += 20;
    memcpy(p, d.desc, strnlen(d.desc, 23)); p += 23;
}

static void jsonStr(String& o, const char* s, int max) {
    o += '"';
    for (int i = 0; i < max && s[i]; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) o += '?';
        else o += c;
    }
    o += '"';
}

// Decode a wire body and print it as one "RID {...}" line.
static void printBody(const uint8_t* b) {
    if (!isSerialConnected()) return;
    const uint8_t* p = b;
    char node[7]; snprintf(node, sizeof(node), "%02X%02X%02X", p[0], p[1], p[2]); p += 3;
    int8_t rssi = (int8_t)*p++; uint8_t src = *p++; uint8_t has = *p++;
    char mac[18]; snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", p[0], p[1], p[2], p[3], p[4], p[5]); p += 6;
    double lat = get32(p) / 1e7, lon = get32(p) / 1e7;
    int altGeo = get16(p), altBaro = get16(p), height = get16(p);
    float speed = (uint16_t)get16(p) / 100.0f, vspeed = get16(p) / 100.0f, dir = (uint16_t)get16(p) / 10.0f;
    uint8_t status = *p++, uaType = *p++, idType = *p++, opLoc = *p++;
    double plat = get32(p) / 1e7, plon = get32(p) / 1e7;
    int palt = get16(p);
    const char* id = (const char*)p; p += 20;
    const char* opId = (const char*)p; p += 20;
    const char* desc = (const char*)p;
    static const char* srcs[] = {"wifi", "nan", "ble"};
    String o = "RID {\"mac\":\"";
    o += mac; o += "\",\"node\":\""; o += node; o += "\",\"rssi\":"; o += rssi;
    o += ",\"src\":\""; o += src < 3 ? srcs[src] : "?"; o += '"';
    if (has & H_BASIC) { o += ",\"id\":"; jsonStr(o, id, 20); o += ",\"idt\":"; o += idType; o += ",\"uat\":"; o += uaType; }
    if (has & H_LOC) {
        o += ",\"lat\":"; o += String(lat, 7); o += ",\"lon\":"; o += String(lon, 7);
        o += ",\"alt\":"; o += altGeo; o += ",\"altb\":"; o += altBaro; o += ",\"hagl\":"; o += height;
        o += ",\"spd\":"; o += String(speed, 2); o += ",\"vspd\":"; o += String(vspeed, 2);
        o += ",\"hdg\":"; o += String(dir, 1); o += ",\"st\":"; o += status;
    }
    if (has & H_SYS) {
        o += ",\"plat\":"; o += String(plat, 7); o += ",\"plon\":"; o += String(plon, 7);
        o += ",\"palt\":"; o += palt; o += ",\"ploc\":"; o += opLoc;
    }
    if (has & H_OP) { o += ",\"op\":"; jsonStr(o, opId, 20); }
    if (has & H_SELF) { o += ",\"desc\":"; jsonStr(o, desc, 23); }
    o += '}';
    Serial.println(o);
}

void ridFromMesh(const uint8_t* body, int len) {
    if (len < RID_BODY_LEN) return;
    printBody(body);
}

void ridLoop() {
    uint32_t now = millis();
    for (int i = 0; i < RID_MAX; i++) {
        Drone snap;
        bool go = false;
        portENTER_CRITICAL(&ridMux);
        Drone& d = drones[i];
        if (d.used && now - d.seen > RID_EXPIRE_MS) d.used = false;
        bool fresh = false;
        if (d.used && d.dirty && now - d.reported >= RID_REPORT_MS) {
            fresh = d.fresh;
            d.fresh = false;
            d.dirty = false;
            d.reported = now;
            snap = d;
            go = true;
        }
        portEXIT_CRITICAL(&ridMux);
        if (!go) continue;
        if (fresh) ridOnNewDrone();
        uint8_t selfId[3];
        meshSelfId(selfId);
        uint8_t body[RID_BODY_LEN];
        encode(snap, selfId, body);
        printBody(body);
        meshSendDrone(body, RID_BODY_LEN);
    }
}

uint8_t ridActive() {
    uint32_t now = millis();
    uint8_t n = 0;
    for (int i = 0; i < RID_MAX; i++) if (drones[i].used && now - drones[i].seen < RID_EXPIRE_MS) n++;
    return n;
}
