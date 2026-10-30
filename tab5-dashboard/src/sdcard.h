// The microSD card: offline map packs and lookup databases, so the Tab5 works away from
// the dashboard. FAT32 (this build's FATFS has no exFAT), SPI mode, mounted at /sdcard.
//
//   /sdcard/maps/*.pak     map tile packs (home-dashboard/tilepack.py builds them)
//   /sdcard/db/lookup.ldb  vendor / Bluetooth names (home-dashboard/lookupdb.py builds it)
//
// A pack: a 64-byte header, a sorted index of {u64 key, u32 offset, u32 length} (key =
// z << 48 | x << 24 | y) and the JPEG tiles. Packs stay under 2 GB (FAT32 caps a file at 4 GB, and fseek
// takes a signed 32-bit offset here), so a deep area is several files.
#pragma once
#include <Arduino.h>

void sd_begin();
bool sd_ok();
const char *sd_state();          // "no card", "12.3 of 29.7 GB free, 3 map packs"...
uint32_t sd_tiles();             // tiles in every mounted pack
// A tile's JPEG from the packs into buf; its length, 0 = not on the card. Any task.
size_t sd_tile(const char *layer, int z, long x, long y, uint8_t *buf, size_t cap);

// Files written onto the card elsewhere (or as <name>.part, then sd_finish renames and mounts
// them). Pushing them in over Wi-Fi crashed the Tab5 (see net.cpp /sd/put).
long sd_part_size(const char *name);                       // bytes so far, -1 = bad name or no card
long sd_put(const char *name, long off, Stream &in, long len);   // the new size, -1 on error
bool sd_finish(const char *name);
void sd_list(Print &out);                                  // the maps folder as JSON

// Offline device intel from lookup.ldb. Tables as in lookupdb.py.
enum : uint8_t { LK_OUI = 1, LK_CID = 2, LK_UUID = 3, LK_APPEAR = 4 };
bool sd_lookup(uint8_t table, uint32_t key, char *out, size_t cap);   // false: unknown
const char *sd_vendor(const char *mac);   // OUI vendor of a MAC ("" if unknown or random); cached
