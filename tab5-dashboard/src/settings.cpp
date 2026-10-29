#include "settings.h"
#include <Preferences.h>

Settings S;

static const char *NS = "tab5dash";

void settings_load() {
    Preferences p;
    p.begin(NS, true);
    S.ssid = p.getString("ssid", "");
    S.pass = p.getString("pass", "");
    S.host = p.getString("host", "");
    S.port = p.getUShort("port", 5077);
    S.token = p.getString("token", "");
    S.otaPass = p.getString("otapass", "");
    S.tz = p.getString("tz", S.tz);
    S.hostname = p.getString("hostname", S.hostname);
    S.flip = p.getBool("flip", false);
    S.bright = p.getUChar("bright", 70);
    S.sound = p.getBool("sound", true);
    S.muted = p.getString("muted", "");
    S.source = p.getUChar("source", SRC_AUTO);
    p.end();
}

void settings_save() {
    Preferences p;
    p.begin(NS, false);
    p.putString("ssid", S.ssid);
    p.putString("pass", S.pass);
    p.putString("host", S.host);
    p.putUShort("port", S.port);
    p.putString("token", S.token);
    p.putString("otapass", S.otaPass);
    p.putString("tz", S.tz);
    p.putString("hostname", S.hostname);
    p.putBool("flip", S.flip);
    p.putUChar("bright", S.bright);
    p.putBool("sound", S.sound);
    p.putString("muted", S.muted);
    p.putUChar("source", S.source);
    p.end();
}

bool settings_ready() {
    // USB only needs nothing; otherwise Wi-Fi (a blank host = discover the dashboard).
    return S.source == SRC_USB || S.ssid.length() > 0;
}

bool wifi_wanted() { return S.ssid.length() > 0; }

static String masked(const String &s) {
    return s.length() ? String("(set, ") + s.length() + " chars)" : String("(empty)");
}

static void show() {
    Serial.printf("ssid     %s\n", S.ssid.c_str());
    Serial.printf("pass     %s\n", masked(S.pass).c_str());
    Serial.printf("host     %s\n", S.host.c_str());
    Serial.printf("port     %u\n", S.port);
    Serial.printf("token    %s\n", masked(S.token).c_str());
    Serial.printf("otapass  %s\n", masked(S.otaPass).c_str());
    Serial.printf("tz       %s\n", S.tz.c_str());
    Serial.printf("hostname %s\n", S.hostname.c_str());
    Serial.printf("source   %s\n", S.source == SRC_USB ? "usb" : S.source == SRC_NETWORK ? "network" : "auto");
    Serial.printf("flip %d  bright %u  sound %d  muted %s\n", S.flip, S.bright, S.sound,
                  S.muted.length() ? S.muted.c_str() : "-");
}

bool settings_serial_line(const String &raw) {
    String line = raw;
    line.trim();
    if (line == "show") {
        show();
        return true;
    }
    if (line == "reboot") {
        Serial.println("rebooting");
        Serial.flush();
        ESP.restart();
        return true;
    }
    if (line == "clear") {
        Preferences p;
        p.begin(NS, false);
        p.clear();
        p.end();
        Serial.println("settings cleared, rebooting");
        Serial.flush();
        ESP.restart();
        return true;
    }
    if (!line.startsWith("set ")) return false;
    String rest = line.substring(4);
    int sp = rest.indexOf(' ');
    String key = sp < 0 ? rest : rest.substring(0, sp);
    String val = sp < 0 ? String() : rest.substring(sp + 1);
    if (key == "ssid") S.ssid = val;
    else if (key == "pass") S.pass = val;
    else if (key == "host") S.host = val;
    else if (key == "port") S.port = val.toInt() ? val.toInt() : 5077;
    else if (key == "token") S.token = val;
    else if (key == "otapass") S.otaPass = val;
    else if (key == "tz") S.tz = val;
    else if (key == "hostname") S.hostname = val.length() ? val : String("ouispy-tab5");
    else if (key == "flip") S.flip = val.toInt() != 0;
    else if (key == "bright") S.bright = constrain(val.toInt(), 5, 100);
    else if (key == "sound") S.sound = val.toInt() != 0;
    else if (key == "source") S.source = val == "usb" ? SRC_USB : val == "network" ? SRC_NETWORK : SRC_AUTO;
    else {
        Serial.printf("unknown key '%s' (ssid pass host port token otapass tz hostname flip bright sound source)\n",
                      key.c_str());
        return true;
    }
    settings_save();
    Serial.printf("ok: %s saved (send 'reboot' to apply network changes)\n", key.c_str());
    return true;
}
