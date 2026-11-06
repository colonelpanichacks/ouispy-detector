// Mesh link: the console over Bluetooth LE for the phone (see link.h).
#ifdef MESHLINK_NODE
#undef Serial   // here Serial is the USB port itself
#include "link.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_random.h>
#include "mesh.h"

LinkOut LinkSerial;

static const char* NUS_SVC = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX  = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_TX  = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";

// Lines out to the phone. Only whole lines: one that doesn't fit is dropped entire
// (the phone misses it, as a busy USB host would), never cut in half.
#define TX_RING 16384
static uint8_t txBuf[TX_RING];
static volatile size_t txHead = 0, txTail = 0;   // head: next write, tail: next send
static size_t txLineStart = 0;                   // where the line being written began
static bool txDropping = false;
static portMUX_TYPE txMux = portMUX_INITIALIZER_UNLOCKED;

// Lines in from the phone; read() hands them out only up to the last whole line.
#define RX_RING 4096
static uint8_t rxBuf[RX_RING];
static volatile size_t rxHead = 0, rxTail = 0, rxLines = 0;
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

static NimBLECharacteristic* txChr = nullptr;
static volatile uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool secured = false, subscribed = false;
static volatile uint16_t mtu = 23;
static uint32_t pin = 0;

static bool phoneReady() { return connHandle != BLE_HS_CONN_HANDLE_NONE && secured && subscribed; }
bool linkPhone() { return phoneReady(); }

static size_t txUsed() { return (txHead + TX_RING - txTail) % TX_RING; }

static void txPut(const uint8_t* b, size_t n) {
    if (!phoneReady()) return;
    portENTER_CRITICAL(&txMux);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (!txDropping) {
            size_t next = (txHead + 1) % TX_RING;
            if (next == txTail) {   // full: take back this line's start and skip the rest of it
                txHead = txLineStart;
                txDropping = true;
            } else {
                txBuf[txHead] = c;
                txHead = next;
            }
        }
        if (c == '\n') {
            txDropping = false;
            txLineStart = txHead;
        }
    }
    portEXIT_CRITICAL(&txMux);
}

static void txClear() {
    portENTER_CRITICAL(&txMux);
    txHead = txTail = txLineStart = 0;
    txDropping = false;
    portEXIT_CRITICAL(&txMux);
}

// Sends what's queued as notifications, as fast as the link drains them.
static void txTask(void*) {
    uint8_t chunk[244];
    for (;;) {
        if (!phoneReady() || !txChr) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        size_t room = mtu > 3 ? mtu - 3 : 20;
        if (room > sizeof chunk) room = sizeof chunk;
        size_t n = 0;
        portENTER_CRITICAL(&txMux);
        // only bytes of lines already finished (or a full chunk of a long one)
        size_t avail = (txLineStart + TX_RING - txTail) % TX_RING;
        if (avail == 0 && txUsed() >= room) avail = txUsed();
        for (size_t t = txTail; n < room && n < avail; t = (t + 1) % TX_RING) chunk[n++] = txBuf[t];
        portEXIT_CRITICAL(&txMux);
        if (!n) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
        struct os_mbuf* om = ble_hs_mbuf_from_flat(chunk, n);
        int rc = om ? ble_gattc_notify_custom(connHandle, txChr->getHandle(), om) : BLE_HS_ENOMEM;
        if (rc == 0) {
            portENTER_CRITICAL(&txMux);
            txTail = (txTail + n) % TX_RING;
            portEXIT_CRITICAL(&txMux);
        } else {
            vTaskDelay(pdMS_TO_TICKS(8));   // out of buffers: let the link drain
        }
    }
}

class ServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer*, ble_gap_conn_desc* d) override {
        connHandle = d->conn_handle;
        secured = d->sec_state.encrypted && d->sec_state.authenticated;
        subscribed = false;
        mtu = 23;
        txClear();
        NimBLEDevice::startSecurity(d->conn_handle);   // a bonded phone re-encrypts, a new one pairs
        // a short interval while connected: lines reach the phone without waiting
        NimBLEDevice::getServer()->updateConnParams(d->conn_handle, 6, 12, 0, 400);
    }
    void onDisconnect(NimBLEServer*, ble_gap_conn_desc*) override {
        connHandle = BLE_HS_CONN_HANDLE_NONE;
        secured = subscribed = false;
        txClear();
    }
    void onMTUChange(uint16_t m, ble_gap_conn_desc*) override { mtu = m; }
    void onAuthenticationComplete(ble_gap_conn_desc* d) override {
        secured = d->sec_state.encrypted && d->sec_state.authenticated;
        if (!secured) NimBLEDevice::getServer()->disconnect(d->conn_handle);
    }
    uint32_t onPassKeyRequest() override { return pin; }
};

class TxCB : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, ble_gap_conn_desc*, uint16_t v) override {
        subscribed = (v & 1) != 0;
        if (!subscribed) txClear();
    }
};

class RxCB : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, ble_gap_conn_desc* d) override {
        if (!(d->sec_state.encrypted && d->sec_state.authenticated)) return;
        NimBLEAttValue v = c->getValue();
        portENTER_CRITICAL(&rxMux);
        for (size_t i = 0; i < v.length(); i++) {
            uint8_t ch = v.data()[i];
            if (ch == '\r') continue;
            size_t next = (rxHead + 1) % RX_RING;
            if (next == rxTail) break;   // full: the rest is lost (a line that long is not a command)
            rxBuf[rxHead] = ch;
            rxHead = next;
            if (ch == '\n') rxLines++;
        }
        portEXIT_CRITICAL(&rxMux);
    }
};

static uint32_t loadPin() {
    Preferences p;
    p.begin("link", false);
    uint32_t v = p.getUInt("pin", 0);
    if (v < 100000 || v > 999999) {
        v = 100000 + esp_random() % 900000;
        p.putUInt("pin", v);
    }
    p.end();
    return v;
}

void linkBegin() {
    pin = loadPin();
    char id[7];
    meshSelfIdHex(id);
    String name = String("OUI-SPY Link ") + id;
    NimBLEDevice::init(name.c_str());
    NimBLEDevice::setMTU(247);
    NimBLEDevice::setSecurityAuth(true, true, true);          // bonded, MITM, secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);   // the phone types our PIN
    NimBLEDevice::setSecurityPasskey(pin);

    NimBLEServer* s = NimBLEDevice::createServer();
    s->setCallbacks(new ServerCB());
    s->advertiseOnDisconnect(true);
    NimBLEService* svc = s->createService(NUS_SVC);
    txChr = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
    txChr->setCallbacks(new TxCB());
    NimBLECharacteristic* rx = svc->createCharacteristic(
        NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::WRITE_ENC |
                    NIMBLE_PROPERTY::WRITE_AUTHEN);
    rx->setCallbacks(new RxCB());
    svc->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(NUS_SVC);
    adv->setScanResponse(true);
    adv->start();

    xTaskCreatePinnedToCore(txTask, "linktx", 4096, nullptr, 2, nullptr, 1);
    Serial.printf("[LINK] Bluetooth up as \"%s\" (pair with the PIN: console blepin)\n", name.c_str());
}

bool linkConsole(const String& verb, const String& arg) {
    if (verb == "blepin") {
        String a = arg;
        a.trim();
        if (a.length()) {
            long v = a.toInt();
            if (a.length() != 6 || v < 100000 || v > 999999) {
                Serial.println("ERR blepin: 6 digits, not starting with 0");
                return true;
            }
            pin = (uint32_t)v;
            Preferences p;
            p.begin("link", false);
            p.putUInt("pin", pin);
            p.end();
            NimBLEDevice::setSecurityPasskey(pin);
        }
        LinkSerial.printf("OK blepin %06lu\n", (unsigned long)pin);
        return true;
    }
    if (verb == "blebonds") {
        if (arg == "clear") {
            NimBLEDevice::deleteAllBonds();
            LinkSerial.println("OK blebonds cleared (every phone pairs again)");
        } else {
            LinkSerial.printf("OK blebonds %d\n", NimBLEDevice::getNumBonds());
        }
        return true;
    }
    if (verb == "link") {
        LinkSerial.printf("OK link phone=%d secured=%d subscribed=%d mtu=%u queued=%u\n",
                          connHandle != BLE_HS_CONN_HANDLE_NONE, (int)secured, (int)subscribed,
                          (unsigned)mtu, (unsigned)txUsed());
        return true;
    }
    return false;
}

// ---- LinkSerial: USB as before, plus the phone ----
void LinkOut::begin(unsigned long baud) { Serial.begin(baud); }
void LinkOut::setRxBufferSize(size_t n) { Serial.setRxBufferSize(n); }
void LinkOut::setTxBufferSize(size_t n) { Serial.setTxBufferSize(n); }

size_t LinkOut::write(uint8_t c) { return write(&c, 1); }

size_t LinkOut::write(const uint8_t* b, size_t n) {
    txPut(b, n);
    if (Serial) Serial.write(b, n);
    return n;
}

int LinkOut::available() {
    int u = Serial.available();
    if (u > 0) return u;
    if (!rxLines) return 0;
    return (int)((rxHead + RX_RING - rxTail) % RX_RING);
}

int LinkOut::read() {
    if (Serial.available() > 0) return Serial.read();
    int c = -1;
    portENTER_CRITICAL(&rxMux);
    if (rxLines && rxTail != rxHead) {
        c = rxBuf[rxTail];
        rxTail = (rxTail + 1) % RX_RING;
        if (c == '\n') rxLines--;
    }
    portEXIT_CRITICAL(&rxMux);
    return c;
}

int LinkOut::peek() {
    if (Serial.available() > 0) return Serial.peek();
    return rxLines && rxTail != rxHead ? rxBuf[rxTail] : -1;
}

void LinkOut::flush() { if (Serial) Serial.flush(); }

int LinkOut::availableForWrite() {
    if (Serial) return Serial.availableForWrite();
    return phoneReady() ? (int)(TX_RING - 1 - txUsed()) : 0;
}

LinkOut::operator bool() const { return (bool)Serial || phoneReady(); }
#endif
