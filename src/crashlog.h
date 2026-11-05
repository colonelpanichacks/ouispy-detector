#pragma once
// Crash record: what the last panic was (where, why, the backtrace), kept across the
// reset in RTC memory and saved to NVS on the next boot, so a node with no USB host still
// tells: it rides in its mesh status, and the home prints it in that node's RST line.
// Decode the addresses with xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <pc> <bt...>.
#include <Arduino.h>

#define CRASH_BT 6

struct CrashInfo {
    uint32_t pc, excvaddr;       // where it faulted, and the bad address (load/store faults)
    uint8_t exc, core, n;        // panic_exception_t (0 debug, 1 IWDT, 2 TWDT, 3 abort, 4 fault), core, backtrace depth
    uint32_t bt[CRASH_BT];       // caller PCs
    uint32_t boot;               // the boot count it happened in
};

// Call once at boot, after the boot count is known: picks up a crash left by the panic.
void crashlogBegin(uint32_t boot);
// The last crash this node had (also from earlier boots); false when none is recorded.
bool crashlogLast(CrashInfo& out);
// "pc=0x... exc=4 core=0 va=0x... bt=0x... 0x..." for the console.
String crashlogText(const CrashInfo& c);
