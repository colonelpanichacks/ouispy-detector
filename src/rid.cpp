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
// Per drone, at most this often over the mesh, on every node: as live as mesh-mapper
// (drones send their position ~1-4 Hz). What keeps the mesh clear is the hold-back below,
// not a slower pace.
#define RID_REPORT_MS  400UL
// Hold-back: a node skips its report when another node reported the same drone in the
// last RID_REPORT_MS and heard it louder. One drone then costs about one node's reports
// (~2.5 a second) however many nodes hear it, instead of one stream per node (at the LR
// rate a report is ~7 ms on the air, relayed ~3 times). Every node still sends one at
// least every RID_FULL_MS, so the drone's heard-by list stays whole.
#define RID_FULL_MS    2000UL
#define RID_EXPIRE_MS  60000UL
// USB is its own path: a host on this node's serial (the phone app, the dashboard) gets
// every decoded frame within this, not the mesh's pace. A burst of one advert's message
// parts (BLE sends Basic ID, Location, System one advert each) still comes out as one line.
#define RID_USB_MS     50UL

enum : uint8_t { RID_SRC_BEACON = 0, RID_SRC_NAN = 1, RID_SRC_BLE = 2 };
enum : uint8_t { H_BASIC = 1, H_LOC = 2, H_SYS = 4, H_OP = 8, H_SELF = 16 };

struct Drone {
    bool used, dirty, fresh, udirty;   // udirty: news for USB since the last line
    uint8_t mac[6];
    int8_t rssi;
    uint8_t src, has;
    uint32_t seen, reported, printed;
    uint32_t meshAt;                  // our last report into the mesh
    uint32_t otherAt; int8_t otherRssi;   // another node's last report of it, and how loud it heard it
    char id[ODID_ID_SIZE + 1];
    uint8_t idType, uaType;
    char opId[ODID_ID_SIZE + 1];
    char desc[ODID_STR_SIZE + 1];
    double lat, lon, plat, plon;
    float altGeo, altBaro, height, speed, vspeed, dir, palt;
    uint8_t status, opLocType;
    // the rest of Remote ID (wire extension)
    uint8_t heightRef, hAcc, vAcc, bAcc, sAcc, tsAcc;
    float fixTs;                       // seconds after the UTC hour, <0 unknown
    uint8_t cls, catEU, classEU, opIdType;
    uint16_t areaCount, areaRadius;
    float areaCeil, areaFloor;
    uint32_t sysTs;                    // seconds since 2019-01-01 UTC, 0 unknown
    char id2[ODID_ID_SIZE + 1];        // a second Basic ID (another ID type), when sent
    uint8_t idType2, authType;         // authType 0xFF: no authentication message seen
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
        drones[slot].fixTs = -1;
        drones[slot].authType = 0xFF;
        drones[slot].areaCeil = drones[slot].areaFloor = -1000;
    }
    Drone& d = drones[slot];
    d.rssi = rssi;
    d.src = src;
    d.seen = now;
    d.has |= has;
    // Basic ID: up to two (say a serial and a CAA registration), possibly one per advert.
    // The serial is the main one; another ID type is kept beside it.
    for (int b = 0; b < ODID_BASIC_ID_MAX_MESSAGES; b++) {
        if (!u.BasicIDValid[b] || !u.BasicID[b].UASID[0]) continue;
        const ODID_BasicID_data& bi = u.BasicID[b];
        bool main = !(d.has & H_BASIC) || bi.IDType == d.idType || bi.IDType == ODID_IDTYPE_SERIAL_NUMBER;
        if (main && (d.has & H_BASIC) && bi.IDType != d.idType && d.idType != ODID_IDTYPE_NONE) {
            // a serial arriving after another ID type: the old one becomes the second
            strlcpy(d.id2, d.id, sizeof(d.id2));
            d.idType2 = d.idType;
        }
        if (main) {
            strlcpy(d.id, bi.UASID, sizeof(d.id));
            d.idType = bi.IDType;
            d.uaType = bi.UAType;
        } else {
            strlcpy(d.id2, bi.UASID, sizeof(d.id2));
            d.idType2 = bi.IDType;
        }
    }
    if (u.LocationValid) {
        d.lat = u.Location.Latitude; d.lon = u.Location.Longitude;
        d.altGeo = u.Location.AltitudeGeo; d.altBaro = u.Location.AltitudeBaro;
        d.height = u.Location.Height; d.speed = u.Location.SpeedHorizontal;
        d.vspeed = u.Location.SpeedVertical; d.dir = u.Location.Direction;
        d.status = u.Location.Status;
        d.heightRef = u.Location.HeightType;
        d.hAcc = u.Location.HorizAccuracy; d.vAcc = u.Location.VertAccuracy;
        d.bAcc = u.Location.BaroAccuracy; d.sAcc = u.Location.SpeedAccuracy;
        d.tsAcc = u.Location.TSAccuracy;
        d.fixTs = (u.Location.TimeStamp >= 0 && u.Location.TimeStamp <= 3600) ? u.Location.TimeStamp : -1;
    }
    if (u.SystemValid) {
        d.plat = u.System.OperatorLatitude; d.plon = u.System.OperatorLongitude;
        d.palt = u.System.OperatorAltitudeGeo; d.opLocType = u.System.OperatorLocationType;
        d.cls = u.System.ClassificationType; d.catEU = u.System.CategoryEU; d.classEU = u.System.ClassEU;
        d.areaCount = u.System.AreaCount; d.areaRadius = u.System.AreaRadius;
        d.areaCeil = u.System.AreaCeiling; d.areaFloor = u.System.AreaFloor;
        d.sysTs = u.System.Timestamp;
    }
    if (u.AuthValid[0]) d.authType = u.Auth[0].AuthType;
    if (u.OperatorIDValid) {
        strlcpy(d.opId, u.OperatorID.OperatorId, sizeof(d.opId));
        d.opIdType = u.OperatorID.OperatorIdType;
    }
    if (u.SelfIDValid) strlcpy(d.desc, u.SelfID.Desc, sizeof(d.desc));
    d.dirty = true;
    d.udirty = true;
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
// extension (RID_EXT_LEN): ver1 (=1) heightRef1 hAcc|vAcc<<4 1 bAcc|sAcc<<4 1
// fixTs2 (0.1 s after the UTC hour, 0xFFFF unknown) tsAcc1 cls1 catEU|classEU<<4 1
// areaCount2 areaRadius2 areaCeil2 areaFloor2 (m) sysTs4 opIdType1 idType2 1
// authType1 (0xFF none) id2 20                                          = 44
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
    // extension
    memset(p, 0, RID_EXT_LEN);
    *p++ = 1;
    *p++ = d.heightRef;
    *p++ = (d.hAcc & 0x0F) | (d.vAcc << 4);
    *p++ = (d.bAcc & 0x0F) | (d.sAcc << 4);
    put16(p, d.fixTs < 0 ? 0xFFFF : (uint16_t)lroundf(d.fixTs * 10));
    *p++ = d.tsAcc;
    *p++ = d.cls;
    *p++ = (d.catEU & 0x0F) | (d.classEU << 4);
    put16(p, d.areaCount); put16(p, d.areaRadius);
    put16(p, clamp16(d.areaCeil)); put16(p, clamp16(d.areaFloor));
    put32(p, (int32_t)d.sysTs);
    *p++ = d.opIdType;
    *p++ = d.idType2;
    *p++ = d.authType;
    memcpy(p, d.id2, strnlen(d.id2, 20)); p += 20;
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

// Decode a wire body ([len] bytes: 109, or with the extension) and print it as one
// "RID {...}" line. Only what the drone actually said: unknown values are left out.
static void printBody(const uint8_t* b, int len) {
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
    const uint8_t* x = b + RID_BODY_LEN;
    if (len >= RID_WIRE_LEN && x[0] == 1) {
        x++;
        uint8_t href = *x++, a1 = *x++, a2 = *x++;
        uint16_t fts = (uint16_t)get16(x);
        uint8_t tsAcc = *x++, cls = *x++, eu = *x++;
        uint16_t acnt = (uint16_t)get16(x), arad = (uint16_t)get16(x);
        int aceil = get16(x), aflr = get16(x);
        uint32_t sts = (uint32_t)get32(x);
        uint8_t opt = *x++, idt2 = *x++, auth = *x++;
        const char* id2 = (const char*)x;
        if (has & H_LOC) {
            o += ",\"href\":"; o += href;
            if (a1 & 0x0F) { o += ",\"hacc\":"; o += (a1 & 0x0F); }
            if (a1 >> 4) { o += ",\"vacc\":"; o += (a1 >> 4); }
            if (a2 & 0x0F) { o += ",\"bacc\":"; o += (a2 & 0x0F); }
            if (a2 >> 4) { o += ",\"sacc\":"; o += (a2 >> 4); }
            if (fts != 0xFFFF) { o += ",\"fts\":"; o += String(fts / 10.0f, 1); }
            if (tsAcc) { o += ",\"tsacc\":"; o += tsAcc; }
        }
        if (has & H_SYS) {
            o += ",\"cls\":"; o += cls;
            if (cls == 1) { o += ",\"cat\":"; o += (eu & 0x0F); o += ",\"ecls\":"; o += (eu >> 4); }
            if (acnt > 1 || arad) { o += ",\"acnt\":"; o += acnt; o += ",\"arad\":"; o += arad; }
            if (aceil > -1000) { o += ",\"aceil\":"; o += aceil; }
            if (aflr > -1000) { o += ",\"aflr\":"; o += aflr; }
            if (sts) { o += ",\"sts\":"; o += (unsigned long)(sts + 1546300800UL); }
        }
        if (has & H_OP) { o += ",\"opt\":"; o += opt; }
        if (id2[0]) { o += ",\"id2\":"; jsonStr(o, id2, 20); o += ",\"idt2\":"; o += idt2; }
        if (auth != 0xFF) { o += ",\"auth\":"; o += auth; }
    }
    o += '}';
    Serial.println(o);
}

void ridFromMesh(const uint8_t* body, int len) {
    if (len < RID_BODY_LEN) return;
    printBody(body, len);
    // another node's report of a drone we hear too: note it for the hold-back
    uint8_t self[3];
    meshSelfId(self);
    if (!memcmp(body, self, 3)) return;   // our own, relayed back
    const uint8_t* mac = body + 6;
    uint32_t now = millis();
    portENTER_CRITICAL(&ridMux);
    for (int i = 0; i < RID_MAX; i++) {
        if (drones[i].used && !memcmp(drones[i].mac, mac, 6)) {
            drones[i].otherAt = now;
            drones[i].otherRssi = (int8_t)body[3];
            break;
        }
    }
    portEXIT_CRITICAL(&ridMux);
}

// Repeats: the home sweeps channels and a drone report is best effort (no ack), so
// each one goes out again ~150 ms and ~300 ms later; one copy reaching the home is
// enough (the dashboard keeps the newest position).
#define RID_COPIES 1              // the next report is only 400 ms behind
#define RID_COPY_MS 150UL
static uint8_t copyBody[RID_MAX][RID_WIRE_LEN];
static uint8_t copyLeft[RID_MAX];
static uint32_t copyAt[RID_MAX];
uint32_t ridLastSendMs = 0;   // a drone report went into the mesh queue (Sky-Spy hop logic)

static void sendDrone(const uint8_t *body) {
    meshSendDrone(body, RID_WIRE_LEN);
    ridLastSendMs = millis();
}

void ridLoop() {
    uint32_t now = millis();
    for (int i = 0; i < RID_MAX; i++) {
        if (copyLeft[i] && (int32_t)(now - copyAt[i]) >= 0) {
            sendDrone(copyBody[i]);
            copyLeft[i]--;
            copyAt[i] = now + RID_COPY_MS;
        }
    }
    bool host = isSerialConnected();   // outside the spinlock
    for (int i = 0; i < RID_MAX; i++) {
        Drone snap;
        bool go = false;
        portENTER_CRITICAL(&ridMux);
        Drone& d = drones[i];
        if (d.used && now - d.seen > RID_EXPIRE_MS) d.used = false;
        bool fresh = false;
        // held back: a louder node just reported it (and our full report is not due)
        bool held = d.used && now - d.otherAt < RID_REPORT_MS && d.otherRssi > d.rssi && now - d.meshAt < RID_FULL_MS && !d.fresh;
        if (d.used && d.dirty && !held && now - d.reported >= RID_REPORT_MS) {
            fresh = d.fresh;
            d.fresh = false;
            d.dirty = false;
            d.reported = now;
            snap = d;
            go = true;
        }
        // USB: every frame as it comes (within RID_USB_MS), whatever the mesh pace
        bool usb = host && d.used && d.udirty && now - d.printed >= RID_USB_MS;
        Drone usnap;
        if (usb) {
            d.udirty = false;
            d.printed = now;
            usnap = d;
        }
        portEXIT_CRITICAL(&ridMux);
        uint8_t selfId[3];
        meshSelfId(selfId);
        if (usb && Serial.availableForWrite() >= 600) {   // a busy host: skip this one, never stall
            uint8_t ub[RID_WIRE_LEN];
            encode(usnap, selfId, ub);
            printBody(ub, RID_WIRE_LEN);
        }
        if (!go) continue;
        if (fresh) ridOnNewDrone();
        uint8_t body[RID_WIRE_LEN];
        encode(snap, selfId, body);
        sendDrone(body);
        portENTER_CRITICAL(&ridMux);
        drones[i].meshAt = now;
        portEXIT_CRITICAL(&ridMux);
        memcpy(copyBody[i], body, RID_WIRE_LEN);
        copyLeft[i] = RID_COPIES;
        copyAt[i] = now + RID_COPY_MS;
    }
}

uint8_t ridActive() {
    uint32_t now = millis();
    uint8_t n = 0;
    for (int i = 0; i < RID_MAX; i++) if (drones[i].used && now - drones[i].seen < RID_EXPIRE_MS) n++;
    return n;
}
