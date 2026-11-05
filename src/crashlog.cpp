#include "crashlog.h"
#include <Preferences.h>
#include "esp_attr.h"
#include "esp_debug_helpers.h"
#include "esp_private/panic_internal.h"
#include "xtensa/xtensa_context.h"

// Linked with -Wl,--wrap=esp_panic_handler: the panic path calls us first. Only plain
// stores into RTC memory here (no locks, no flash, no heap): the system is going down.
#define CRASH_MAGIC 0x43524831u   // "CRH1"

struct CrashRtc {
    uint32_t magic;
    CrashInfo c;
};
RTC_NOINIT_ATTR static CrashRtc rtc;

static CrashInfo last;
static bool haveLast = false;

// A return address off the stack: the window bits on top, the call instruction 3 back.
static inline uint32_t IRAM_ATTR stackPc(uint32_t pc) {
    if (pc & 0x80000000u) pc = (pc & 0x3fffffffu) | 0x40000000u;
    return pc - 3;
}

extern "C" void __real_esp_panic_handler(panic_info_t* info);

extern "C" void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t* info) {
    rtc.magic = 0;
    const XtExcFrame* f = (const XtExcFrame*)info->frame;
    rtc.c.core = (uint8_t)info->core;
    rtc.c.exc = (uint8_t)info->exception;
    rtc.c.pc = f ? (uint32_t)f->pc : (uint32_t)info->addr;
    rtc.c.excvaddr = f ? (uint32_t)f->excvaddr : 0;
    rtc.c.n = 0;
    if (f) {
        esp_backtrace_frame_t fr = {(uint32_t)f->pc, (uint32_t)f->a1, (uint32_t)f->a0, f};
        while (rtc.c.n < CRASH_BT && esp_backtrace_get_next_frame(&fr)) {
            rtc.c.bt[rtc.c.n++] = stackPc(fr.pc);
            if (!fr.next_pc) break;
        }
    }
    rtc.magic = CRASH_MAGIC;
    __real_esp_panic_handler(info);
}

void crashlogBegin(uint32_t boot) {
    Preferences p;
    if (rtc.magic == CRASH_MAGIC) {
        rtc.magic = 0;
        last = rtc.c;
        last.boot = boot - 1;   // it happened in the boot before this one
        if (last.n > CRASH_BT) last.n = CRASH_BT;
        haveLast = true;
        p.begin("ouispy", false);
        p.putBytes("crash", &last, sizeof(last));
        p.end();
        return;
    }
    p.begin("ouispy", true);
    haveLast = p.getBytes("crash", &last, sizeof(last)) == sizeof(last) && last.n <= CRASH_BT;
    p.end();
}

bool crashlogLast(CrashInfo& out) {
    if (!haveLast) return false;
    out = last;
    return true;
}

String crashlogText(const CrashInfo& c) {
    char b[200];
    int w = snprintf(b, sizeof b, "pc=0x%08lX exc=%u core=%u va=0x%08lX boot=%lu bt=", (unsigned long)c.pc, c.exc, c.core,
                     (unsigned long)c.excvaddr, (unsigned long)c.boot);
    for (uint8_t i = 0; i < c.n && w < (int)sizeof b - 12; i++) w += snprintf(b + w, sizeof b - w, "%s0x%08lX", i ? " " : "", (unsigned long)c.bt[i]);
    return String(b);
}
