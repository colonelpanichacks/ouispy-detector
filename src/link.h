// Mesh link, the `meshlink` build only (pio run -e meshlink): a node for the phone. It
// joins the mesh as a remote and scans nothing, so the phone's Bluetooth link never takes
// radio time from a scanner; the phone (the app) is the dashboard on top of it.
//
// Its console is carried both ways over Bluetooth LE, Nordic UART style, as well as USB:
//   service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
//   RX      6E400002-...  write: lines in, "\n"-terminated (the same verbs as USB)
//   TX      6E400003-...  notify: every line the console prints, chunked to the MTU
// Bonded pairing with a 6-digit PIN (random at first boot, kept in NVS; console "blepin"
// shows or sets it, "blebonds clear" forgets every paired phone). Nothing is sent and no
// line is taken until the phone has paired.
//
// Every Serial use in src/ goes through LinkSerial in this build (build_src_flags
// force-includes this header), so each line reaches USB and the phone alike.
#pragma once
#ifdef __cplusplus   // force-included into C files too (odid_wifi.c): nothing for them
#include <Arduino.h>

#ifdef MESHLINK_NODE
constexpr bool linkOn() { return true; }

class LinkOut : public Stream {
public:
    void begin(unsigned long baud);
    void setRxBufferSize(size_t n);
    void setTxBufferSize(size_t n);
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* b, size_t n) override;
    int available() override;
    int read() override;
    int peek() override;
    void flush();
    int availableForWrite();
    operator bool() const;   // a host on USB, or a paired phone listening
    using Print::write;
};
extern LinkOut LinkSerial;
#ifndef LINK_NO_SERIAL_MACRO
#define Serial LinkSerial
#endif

// Bluetooth up (before WiFi, like the BLE scanner): GATT server, advertising, sender task.
void linkBegin();
// Console: "blepin", "blepin <6 digits>", "blebonds clear", "link". True when ours.
bool linkConsole(const String& verb, const String& arg);
// A paired phone is subscribed.
bool linkPhone();
#else
constexpr bool linkOn() { return false; }
inline void linkBegin() {}
inline bool linkConsole(const String&, const String&) { return false; }
inline bool linkPhone() { return false; }
#endif
#endif  // __cplusplus
