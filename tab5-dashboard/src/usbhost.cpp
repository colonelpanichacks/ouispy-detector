#include "usbhost.h"
#include <M5Unified.h>
#include <freertos/stream_buffer.h>
#include "usb/usb_host.h"

namespace {

usb_host_client_handle_t client;
usb_device_handle_t dev = nullptr;
volatile uint8_t newAddr = 0;
volatile bool gone = false;
uint8_t inEp = 0, outEp = 0;
int iface = -1;
uint16_t inMps = 64;
usb_transfer_t *inXfer = nullptr, *outXfer = nullptr;
volatile bool outBusy = false;
StreamBufferHandle_t rx;
char devName[24] = "";
SemaphoreHandle_t sendLock;

void in_done(usb_transfer_t *t) {
    if (t->status == USB_TRANSFER_STATUS_COMPLETED && t->actual_num_bytes)
        xStreamBufferSend(rx, t->data_buffer, t->actual_num_bytes, 0);   // full = drop, never block
    if (dev && t->status != USB_TRANSFER_STATUS_NO_DEVICE && t->status != USB_TRANSFER_STATUS_CANCELED)
        usb_host_transfer_submit(t);
}

void out_done(usb_transfer_t *) { outBusy = false; }

void client_cb(const usb_host_client_event_msg_t *msg, void *) {
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) newAddr = msg->new_dev.address;
    else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) gone = true;
}

void close_dev() {
    if (!dev) return;
    usb_device_handle_t d = dev;
    dev = nullptr;
    if (iface >= 0) usb_host_interface_release(client, d, iface);
    usb_host_device_close(client, d);
    iface = -1;
    devName[0] = 0;
    Serial.println("usb: node unplugged");
}

// Find the CDC data interface (class 0x0A) and its bulk IN/OUT endpoints. Nothing is sent
// on the control pipe: no line coding, and above all no DTR/RTS, which on an ESP32's
// USB-Serial-JTAG can reset the node or drop it into download mode.
void open_dev(uint8_t addr) {
    if (usb_host_device_open(client, addr, &dev) != ESP_OK) {
        dev = nullptr;
        return;
    }
    const usb_device_desc_t *dd;
    usb_host_get_device_descriptor(dev, &dd);
    const usb_config_desc_t *cfg;
    usb_host_get_active_config_descriptor(dev, &cfg);
    const uint8_t *p = (const uint8_t *)cfg;
    int cur = -1, cls = -1;
    inEp = outEp = 0;
    for (int i = 0; i + 1 < cfg->wTotalLength && p[i];) {
        uint8_t len = p[i], type = p[i + 1];
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            cur = p[i + 2];
            cls = p[i + 5];
        } else if (type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && cls == 0x0A && (p[i + 3] & 3) == 2) {
            if (p[i + 2] & 0x80) inEp = p[i + 2], inMps = p[i + 4] | (p[i + 5] << 8);
            else outEp = p[i + 2];
            iface = cur;
        }
        i += len;
    }
    snprintf(devName, sizeof devName, "%04X:%04X", dd->idVendor, dd->idProduct);
    if (!inEp || iface < 0 || usb_host_interface_claim(client, dev, iface, 0) != ESP_OK) {
        Serial.printf("usb: %s has no CDC serial, ignored\n", devName);
        iface = -1;
        close_dev();
        return;
    }
    Serial.printf("usb: node %s on interface %d (in %02X out %02X)\n", devName, iface, inEp, outEp);
    size_t sz = inMps * 8;
    if (!inXfer) usb_host_transfer_alloc(512, 0, &inXfer);
    if (!outXfer) usb_host_transfer_alloc(256, 0, &outXfer);
    inXfer->device_handle = dev;
    inXfer->bEndpointAddress = inEp;
    inXfer->callback = in_done;
    inXfer->num_bytes = min((size_t)512, sz);
    usb_host_transfer_submit(inXfer);
}

void lib_task(void *) {
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

void client_task(void *) {
    usb_host_client_config_t cc = {};
    cc.is_synchronous = false;
    cc.max_num_event_msg = 5;
    cc.async.client_event_callback = client_cb;
    usb_host_client_register(&cc, &client);
    for (;;) {
        usb_host_client_handle_events(client, pdMS_TO_TICKS(50));
        if (gone) {
            gone = false;
            close_dev();
        }
        if (newAddr && !dev) {
            uint8_t a = newAddr;
            newAddr = 0;
            open_dev(a);
        }
    }
}

}  // namespace

void usb_start() {
    rx = xStreamBufferCreate(32768, 1);
    sendLock = xSemaphoreCreateMutex();
    M5.Power.setExtOutput(true, m5::ext_port_mask_t::ext_USB);   // 5 V on the USB-A port
    usb_host_config_t hc = {};
    hc.intr_flags = ESP_INTR_FLAG_LEVEL1;
    if (usb_host_install(&hc) != ESP_OK) {
        Serial.println("usb: host install failed");
        return;
    }
    xTaskCreatePinnedToCore(lib_task, "usblib", 4096, nullptr, 5, nullptr, 1);
    xTaskCreatePinnedToCore(client_task, "usbcli", 6144, nullptr, 5, nullptr, 1);
}

bool usb_attached() { return dev != nullptr; }
const char *usb_device() { return devName; }

size_t usb_read(char *buf, size_t max, uint32_t waitMs) {
    return rx ? xStreamBufferReceive(rx, buf, max, pdMS_TO_TICKS(waitMs)) : 0;
}

bool usb_send(const char *line) {
    if (!dev || !outEp || !outXfer) return false;
    xSemaphoreTake(sendLock, portMAX_DELAY);
    for (int i = 0; outBusy && i < 100; i++) delay(2);
    size_t n = strlen(line);
    if (n > 250) n = 250;
    memcpy(outXfer->data_buffer, line, n);
    outXfer->data_buffer[n++] = '\n';
    outXfer->num_bytes = n;
    outXfer->device_handle = dev;
    outXfer->bEndpointAddress = outEp;
    outXfer->callback = out_done;
    outBusy = true;
    bool ok = usb_host_transfer_submit(outXfer) == ESP_OK;
    if (!ok) outBusy = false;
    xSemaphoreGive(sendLock);
    return ok;
}
