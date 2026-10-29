// Device intel: what a detection heard over the air besides the MAC, packed as
// a small TLV blob (tag u8, len u8, value). It rides in the detection ring, on
// the tail of a MESH_T_DETECT packet (older nodes ignore the extra bytes) and
// is decoded to JSON ("info":{...}) only where a line is printed.
//
// Everything here is header-only, heap-free and safe in the WiFi driver
// callback (fixed buffers, no String).
#pragma once
#include <Arduino.h>

#define DEVINFO_MAX 120   // fits a sealed 250-byte frame after the 98-byte detect body

// BLE
#define DI_NAME      1    // str: advertised local name
#define DI_BLEFLAGS  2    // u8: bit0 random address, bit1 connectable, bit2 scan response seen
#define DI_TXPWR     3    // i8: advertised TX power (dBm)
#define DI_APPEAR    4    // u16 le: GAP appearance
#define DI_UUID16    5    // u16 le list: service UUIDs (16-bit, or bits 16-31 of 128-bit base UUIDs)
#define DI_UUID128   6    // 16 bytes: first full 128-bit service UUID (string order)
#define DI_MFR       7    // company id u16 le + first bytes of manufacturer data
#define DI_SVCDATA   8    // u16 le uuid + first bytes of its service data
#define DI_ADVTYPE   9    // u8: HCI advertising type
// WiFi
#define DI_SSID      16   // str (0 = wildcard probe / hidden)
#define DI_CHAN      17   // u8: channel from the DS parameter set (else the RX channel)
#define DI_SEC       18   // u8: 0 open, 1 WEP, 2 WPA, 3 WPA2, 4 WPA3, 5 WPA2/WPA3
#define DI_BSSID     19   // 6 bytes: AP a client is (re)associating with
#define DI_FRAME     20   // u8: 802.11 mgmt subtype
#define DI_WCAP      21   // u8: bit0 HT (11n), bit1 VHT (11ac), bit2 HE (11ax), bit3 WPS, bit4 hidden SSID
#define DI_RATE      22   // u8: highest legacy rate, units of 500 kb/s
#define DI_VENDOR    23   // list of 3-byte vendor IE OUIs (who built the stack: Apple, MS, Broadcom...)
#define DI_COUNTRY   24   // 2 chars: country code IE

struct DevInfo {
    uint8_t b[DEVINFO_MAX];
    uint8_t n = 0;
    // Append one TLV; truncates the value to what is left. Returns false when full.
    bool add(uint8_t tag, const void* v, uint8_t len) {
        if (n + 2 > DEVINFO_MAX) return false;
        uint8_t room = DEVINFO_MAX - n - 2;
        if (len > room) len = room;
        if (len == 0 && tag != DI_SSID) return false;
        b[n++] = tag;
        b[n++] = len;
        memcpy(b + n, v, len);
        n += len;
        return true;
    }
    bool add8(uint8_t tag, uint8_t v) { return add(tag, &v, 1); }
    bool add16(uint8_t tag, uint16_t v) { uint8_t t[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; return add(tag, t, 2); }
    bool has(uint8_t tag) const {
        for (uint8_t i = 0; i + 2 <= n; i += 2 + b[i + 1]) if (b[i] == tag) return true;
        return false;
    }
};

// Parse the information elements of an 802.11 management frame into intel.
// `f` = frame from the MAC header on, `len` = bytes without the FCS.
static inline void devinfoFromWifi(DevInfo& di, const uint8_t* f, int len, uint8_t subtype, uint8_t rxChan) {
    di.add8(DI_FRAME, subtype);
    int ie;
    uint16_t cap = 0;
    switch (subtype) {
        case 0x08: case 0x05: ie = 24 + 12; if (len >= 36) cap = f[34] | (f[35] << 8); break;  // beacon / probe resp
        case 0x00: ie = 24 + 4;  if (len >= 26) cap = f[24] | (f[25] << 8); break;              // assoc req
        case 0x02: ie = 24 + 10; if (len >= 26) cap = f[24] | (f[25] << 8); break;              // reassoc req
        default:   ie = 24; break;                                                              // probe req
    }
    if (subtype == 0x00 || subtype == 0x02) di.add(DI_BSSID, f + 4, 6);   // addr1 = the AP
    bool rsn = false, wpa = false, sae = false, psk = false, ht = false, vht = false, he = false, wps = false, hidden = false;
    uint8_t chan = 0, rate = 0;
    uint8_t vend[15]; uint8_t nv = 0;
    bool ssidDone = false;
    while (ie + 2 <= len) {
        uint8_t id = f[ie], l = f[ie + 1];
        const uint8_t* v = f + ie + 2;
        if (ie + 2 + l > len) break;
        switch (id) {
            case 0:
                if (!ssidDone) {
                    ssidDone = true;
                    bool allZero = l > 0;
                    for (uint8_t k = 0; k < l; k++) if (v[k]) { allZero = false; break; }
                    if (allZero) hidden = true;
                    else di.add(DI_SSID, v, l > 32 ? 32 : l);
                    if (l == 0 && subtype == 0x08) hidden = true;
                }
                break;
            case 1: case 50:
                for (uint8_t k = 0; k < l; k++) { uint8_t r = v[k] & 0x7F; if (r > rate) rate = r; }
                break;
            case 3: if (l >= 1) chan = v[0]; break;
            case 7: if (l >= 2) di.add(DI_COUNTRY, v, 2); break;
            case 45: ht = true; break;
            case 191: vht = true; break;
            case 255: if (l >= 1 && v[0] == 35) he = true; break;
            case 48: {
                rsn = true;
                // AKM suites: version(2) group(4) pairwise count(2)+n*4, akm count(2)+n*4
                int p = 6;
                if (p + 2 <= l) { int pc = v[p] | (v[p + 1] << 8); p += 2 + pc * 4; }
                if (p + 2 <= l) {
                    int ac = v[p] | (v[p + 1] << 8); p += 2;
                    for (int k = 0; k < ac && p + 4 <= l; k++, p += 4) {
                        uint8_t t = v[p + 3];
                        if (t == 8 || t == 9 || t == 24 || t == 25) sae = true; else psk = true;
                    }
                }
                break;
            }
            case 221:
                if (l >= 4 && v[0] == 0x00 && v[1] == 0x50 && v[2] == 0xF2) {
                    if (v[3] == 1) wpa = true;
                    if (v[3] == 4) wps = true;
                }
                if (l >= 3 && nv < 5) {
                    bool dup = false;
                    for (uint8_t k = 0; k < nv; k++) if (!memcmp(vend + k * 3, v, 3)) dup = true;
                    if (!dup) { memcpy(vend + nv * 3, v, 3); nv++; }
                }
                break;
        }
        ie += 2 + l;
    }
    di.add8(DI_CHAN, chan ? chan : rxChan);
    if (subtype == 0x08 || subtype == 0x05) {
        uint8_t sec = 0;
        if (rsn) sec = (sae && psk) ? 5 : sae ? 4 : 3;
        else if (wpa) sec = 2;
        else if (cap & 0x0010) sec = 1;
        di.add8(DI_SEC, sec);
    }
    uint8_t wcap = (ht ? 1 : 0) | (vht ? 2 : 0) | (he ? 4 : 0) | (wps ? 8 : 0) | (hidden ? 16 : 0);
    di.add8(DI_WCAP, wcap);
    if (rate) di.add8(DI_RATE, rate);
    if (nv) di.add(DI_VENDOR, vend, nv * 3);
}

static inline void diHex(String& o, const uint8_t* p, int n) {
    static const char* hx = "0123456789abcdef";
    for (int i = 0; i < n; i++) { o += hx[p[i] >> 4]; o += hx[p[i] & 15]; }
}

static inline void diStr(String& o, const uint8_t* p, int n) {
    o += '"';
    for (int i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((uint8_t)c < 0x20 || (uint8_t)c > 0x7E) { o += '?'; }
        else o += c;
    }
    o += '"';
}

// TLV -> JSON object text ("{...}"), or "" when there is nothing.
static inline String devinfoJson(const uint8_t* b, uint8_t n) {
    if (!n) return "";
    String o = "{";
    bool first = true;
    String u16s, sdata, vendors;
    auto key = [&](const char* k) { if (!first) o += ','; first = false; o += '"'; o += k; o += "\":"; };
    for (uint8_t i = 0; i + 2 <= n; ) {
        uint8_t t = b[i], l = b[i + 1];
        const uint8_t* v = b + i + 2;
        if (i + 2 + l > n) break;
        switch (t) {
            case DI_NAME: key("name"); diStr(o, v, l); break;
            case DI_BLEFLAGS: if (l >= 1) { key("rnd"); o += (v[0] & 1) ? '1' : '0';
                                             key("conn"); o += (v[0] & 2) ? '1' : '0'; } break;
            case DI_TXPWR: if (l >= 1) { key("tx"); o += String((int8_t)v[0]); } break;
            case DI_APPEAR: if (l >= 2) { key("app"); o += String(v[0] | (v[1] << 8)); } break;
            case DI_ADVTYPE: if (l >= 1) { key("adv"); o += String(v[0]); } break;
            case DI_UUID16:
                for (uint8_t k = 0; k + 1 < l; k += 2) {
                    char s[8]; snprintf(s, sizeof(s), "\"%04x\"", v[k] | (v[k + 1] << 8));
                    if (u16s.length()) u16s += ','; u16s += s;
                }
                break;
            case DI_UUID128: key("uuid128"); o += '"'; diHex(o, v, l); o += '"'; break;
            case DI_MFR: if (l >= 2) {
                char s[8]; snprintf(s, sizeof(s), "%04x", v[0] | (v[1] << 8));
                key("cid"); o += '"'; o += s; o += '"';
                if (l > 2) { key("mfr"); o += '"'; diHex(o, v + 2, l - 2); o += '"'; }
            } break;
            case DI_SVCDATA: if (l >= 2) {
                char s[8]; snprintf(s, sizeof(s), "%04x", v[0] | (v[1] << 8));
                if (sdata.length()) sdata += ',';
                sdata += "{\"uuid\":\""; sdata += s; sdata += "\",\"data\":\""; diHex(sdata, v + 2, l - 2); sdata += "\"}";
            } break;
            case DI_SSID: key("ssid"); diStr(o, v, l); break;
            case DI_CHAN: if (l >= 1) { key("ch"); o += String(v[0]); } break;
            case DI_SEC: if (l >= 1) {
                static const char* names[] = {"open", "WEP", "WPA", "WPA2", "WPA3", "WPA2/WPA3"};
                key("sec"); o += '"'; o += v[0] < 6 ? names[v[0]] : "?"; o += '"';
            } break;
            case DI_BSSID: if (l >= 6) {
                char s[20]; snprintf(s, sizeof(s), "%02x:%02x:%02x:%02x:%02x:%02x", v[0], v[1], v[2], v[3], v[4], v[5]);
                key("bssid"); o += '"'; o += s; o += '"';
            } break;
            case DI_FRAME: if (l >= 1) {
                const char* f = v[0] == 0x04 ? "probe" : v[0] == 0x08 ? "beacon" : v[0] == 0x00 ? "assoc"
                              : v[0] == 0x02 ? "reassoc" : v[0] == 0x05 ? "probe-resp" : "mgmt";
                key("frame"); o += '"'; o += f; o += '"';
            } break;
            case DI_WCAP: if (l >= 1) {
                String s;
                if (v[0] & 4) s += "ax"; else if (v[0] & 2) s += "ac"; else if (v[0] & 1) s += "n"; else s += "bg";
                key("phy"); o += '"'; o += s; o += '"';
                if (v[0] & 8)  { key("wps"); o += '1'; }
                if (v[0] & 16) { key("hidden"); o += '1'; }
            } break;
            case DI_RATE: if (l >= 1) { key("rate"); o += String(v[0] / 2.0f, 1); } break;
            case DI_VENDOR:
                for (uint8_t k = 0; k + 2 < l; k += 3) {
                    if (vendors.length()) vendors += ',';
                    vendors += '"'; diHex(vendors, v + k, 3); vendors += '"';
                }
                break;
            case DI_COUNTRY: if (l >= 2) { key("cc"); diStr(o, v, 2); } break;
        }
        i += 2 + l;
    }
    if (u16s.length()) { key("uuids"); o += '['; o += u16s; o += ']'; }
    if (sdata.length()) { key("sdata"); o += '['; o += sdata; o += ']'; }
    if (vendors.length()) { key("vie"); o += '['; o += vendors; o += ']'; }
    o += '}';
    return first ? "" : o;
}
