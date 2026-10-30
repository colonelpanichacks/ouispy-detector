#include "sdcard.h"
#include <SD.h>
#include <SPI.h>
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include <dirent.h>
#include <sys/stat.h>

namespace {

constexpr char MAGIC[8] = {'O', 'S', 'P', 'K', '1', '\n', 0, 0};
constexpr int HDR = 64;
constexpr int ENTRY = 16;
constexpr int BLOCK = 256;      // index entries per sparse step: one 4 KB read finds a tile
constexpr int MAX_PACKS = 48;

struct Pack {
    char path[80];
    char layer[16];
    uint32_t n;
    uint64_t kmin, kmax;
    uint64_t *sparse;           // every BLOCK-th key, in PSRAM
    FILE *f;
};
Pack packs[MAX_PACKS];
int npacks = 0;
uint32_t ntiles = 0;
bool mounted = false;
char state[64] = "no card";
SemaphoreHandle_t lock_;
uint8_t *blk;                   // one index block
void set_state();

// lookup.ldb: the whole index in PSRAM (16 bytes a name, ~730 KB), names read on demand.
FILE *ldbF = nullptr;
uint8_t *ldbIdx = nullptr;
uint32_t ldbN = 0, ldbNames = 0;

void open_ldb(const char *path) {
    if (ldbF) fclose(ldbF), ldbF = nullptr;
    free(ldbIdx), ldbIdx = nullptr, ldbN = 0;
    FILE *f = fopen(path, "rb");
    uint8_t h[16];
    if (!f || fread(h, 1, 16, f) != 16 || memcmp(h, "OSLD1\n\0\0", 8)) { if (f) fclose(f); return; }
    uint32_t n = h[8] | h[9] << 8 | h[10] << 16 | (uint32_t)h[11] << 24;
    uint32_t names = h[12] | h[13] << 8 | h[14] << 16 | (uint32_t)h[15] << 24;
    uint8_t *idx = (uint8_t *)ps_malloc((size_t)n * 16);
    if (!idx || fread(idx, 16, n, f) != n) { fclose(f); free(idx); return; }
    ldbF = f, ldbIdx = idx, ldbN = n, ldbNames = names;
}

uint64_t rd64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = v << 8 | p[i]; return v; }
uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void open_pack(const char *path) {
    if (npacks == MAX_PACKS) return;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t h[HDR];
    if (fread(h, 1, HDR, f) != HDR || memcmp(h, MAGIC, 8)) { fclose(f); return; }
    Pack &p = packs[npacks];
    strlcpy(p.path, path, sizeof p.path);
    memcpy(p.layer, h + 8, 15);
    p.layer[15] = 0;
    p.n = rd32(h + 24);
    p.kmin = rd64(h + 28);
    p.kmax = rd64(h + 36);
    uint32_t ns = (p.n + BLOCK - 1) / BLOCK;
    p.sparse = (uint64_t *)ps_malloc(ns * 8 + 8);
    if (!p.n || !p.sparse) { fclose(f); free(p.sparse); return; }
    uint8_t e[ENTRY];
    for (uint32_t i = 0; i < ns; i++) {
        fseek(f, HDR + (long)i * BLOCK * ENTRY, SEEK_SET);
        if (fread(e, 1, ENTRY, f) != ENTRY) { fclose(f); free(p.sparse); return; }
        p.sparse[i] = rd64(e);
    }
    p.f = f;
    ntiles += p.n;
    npacks++;
}

}  // namespace

// A card put in after boot: mount it the next time something asks for the card (at most
// every 5 s; a mount attempt with no card blocks for a moment).
bool sd_ensure() {
    static uint32_t lastTry = 0;
    if (mounted) return true;
    if (lastTry && millis() - lastTry < 5000) return false;
    lastTry = millis();
    sd_begin();
    return mounted;
}

void sd_begin() {
    if (!lock_) lock_ = xSemaphoreCreateMutex();
    if (!blk) blk = (uint8_t *)ps_malloc(BLOCK * ENTRY);
    // SPI mode on its own SPI peripheral (MISO 39, CS 42, SCK 43, MOSI 44), NOT the SDMMC
    // controller: the C6 Wi-Fi runs over slot 1 of that controller, and a card on slot 0
    // beside it crashed the Wi-Fi link (esp_hosted sdio_read assert) seconds after every
    // boot. A 20 KB tile still reads in ~10 ms at 25 MHz.
    static sd_pwr_ctrl_handle_t pwr = nullptr;
    if (!pwr) {   // the slot's 3.3 V comes from on-chip LDO channel 4
        sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = 4};
        sd_pwr_ctrl_new_on_chip_ldo(&ldo, &pwr);
    }
    static SPIClass spi(FSPI);
    spi.begin(43, 39, 44, 42);
    if (!SD.begin(42, spi, 25000000, "/sdcard", 4)) {
        strlcpy(state, "no card (or not FAT32)", sizeof state);
        return;
    }
    mounted = true;
    if (DIR *d = opendir("/sdcard/maps")) {
        while (dirent *e = readdir(d)) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcasecmp(e->d_name + l - 4, ".pak")) {
                char p[80];
                snprintf(p, sizeof p, "/sdcard/maps/%s", e->d_name);
                open_pack(p);
            }
        }
        closedir(d);
    }
    open_ldb("/sdcard/db/lookup.ldb");
    set_state();
}

bool sd_ok() { return mounted; }

namespace {
void set_state() {
    snprintf(state, sizeof state, "%.1f of %.1f GB free, %d map pack%s%s",
             (SD.totalBytes() - SD.usedBytes()) / 1e9, SD.totalBytes() / 1e9, npacks, npacks == 1 ? "" : "s",
             ldbN ? ", lookup db" : "");
}

// Only plain names: letters, digits, '-', '_' and ".pak" (a map pack, into maps/) or
// ".ldb" (the lookup database, into db/) at the end. ext nullptr = the real extension.
bool part_path(const char *name, char *out, size_t cap, const char *ext) {
    size_t l = strlen(name);
    if (l < 5 || l > 60) return false;
    bool pak = !strcasecmp(name + l - 4, ".pak"), ldb = !strcasecmp(name + l - 4, ".ldb");
    if (!pak && !ldb) return false;
    for (size_t i = 0; i < l - 4; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_') return false;
    snprintf(out, cap, "/sdcard/%s/%.*s%s", pak ? "maps" : "db", (int)(l - 4), name, ext ? ext : name + l - 4);
    return true;
}
}  // namespace

long sd_part_size(const char *name) {
    char p[96];
    if (!sd_ensure() || !part_path(name, p, sizeof p, ".part")) return -1;
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : 0;
}

long sd_put(const char *name, long off, Stream &in, long len) {
    char p[96];
    if (!mounted || !part_path(name, p, sizeof p, ".part") || len < 0) return -1;
    mkdir("/sdcard/maps", 0775);
    mkdir("/sdcard/db", 0775);
    long have = sd_part_size(name);
    if (off != have) return have;   // the sender resumes from what is really there
    FILE *f = fopen(p, off ? "ab" : "wb");
    if (!f) return -1;
    static uint8_t *buf = (uint8_t *)ps_malloc(16384);
    long done = 0;
    uint32_t t0 = millis();
    while (done < len && millis() - t0 < 20000) {
        int n = in.readBytes(buf, min((long)16384, len - done));
        if (n <= 0) continue;
        if (fwrite(buf, 1, n, f) != (size_t)n) break;
        done += n;
        t0 = millis();
    }
    fclose(f);
    return sd_part_size(name);
}

bool sd_finish(const char *name) {
    char part[96], pak[96];
    if (!mounted || !part_path(name, part, sizeof part, ".part") || !part_path(name, pak, sizeof pak, nullptr)) return false;
    xSemaphoreTake(lock_, portMAX_DELAY);
    if (strstr(pak, "/db/")) {   // the lookup database
        if (ldbF) fclose(ldbF), ldbF = nullptr;
        remove(pak);
        bool ok = rename(part, pak) == 0;
        if (ok) open_ldb(pak);
        ok = ok && ldbN;
        set_state();
        xSemaphoreGive(lock_);
        return ok;
    }
    for (int i = 0; i < npacks; i++)   // replacing a pack: drop the old one first
        if (!strcmp(packs[i].path, pak)) {
            fclose(packs[i].f);
            free(packs[i].sparse);
            ntiles -= packs[i].n;
            packs[i] = packs[--npacks];
            break;
        }
    remove(pak);
    bool ok = rename(part, pak) == 0;
    int before = npacks;
    if (ok) open_pack(pak);
    ok = ok && npacks > before;
    set_state();
    xSemaphoreGive(lock_);
    return ok;
}

void sd_list(Print &out) {
    sd_ensure();
    out.print("{\"state\":\"");
    out.print(state);
    out.print("\",\"files\":[");
    int n = 0;
    if (DIR *d = opendir("/sdcard/maps")) {
        while (dirent *e = readdir(d)) {
            char p[96];
            snprintf(p, sizeof p, "/sdcard/maps/%s", e->d_name);
            struct stat st;
            if (e->d_name[0] == '.' || stat(p, &st)) continue;
            out.printf("%s{\"name\":\"%s\",\"size\":%ld}", n++ ? "," : "", e->d_name, (long)st.st_size);
        }
        closedir(d);
    }
    out.print("]}\n");
}
const char *sd_state() { return state; }
uint32_t sd_tiles() { return ntiles; }

size_t sd_tile(const char *layer, int z, long x, long y, uint8_t *buf, size_t cap) {
    if (!npacks || !blk) return 0;
    uint64_t key = (uint64_t)z << 48 | (uint64_t)x << 24 | (uint64_t)y;
    size_t got = 0;
    xSemaphoreTake(lock_, portMAX_DELAY);
    for (int i = 0; i < npacks && !got; i++) {
        Pack &p = packs[i];
        if (key < p.kmin || key > p.kmax || strcmp(p.layer, layer)) continue;
        // The block that can hold it: the last sparse key <= key.
        uint32_t ns = (p.n + BLOCK - 1) / BLOCK, lo = 0, hi = ns;
        while (hi - lo > 1) {
            uint32_t mid = (lo + hi) / 2;
            (p.sparse[mid] <= key ? lo : hi) = mid;
        }
        uint32_t first = lo * BLOCK, cnt = min((uint32_t)BLOCK, p.n - first);
        fseek(p.f, HDR + (long)first * ENTRY, SEEK_SET);
        if (fread(blk, 1, cnt * ENTRY, p.f) != cnt * ENTRY) continue;
        int a = 0, b = (int)cnt - 1;
        while (a <= b) {
            int m = (a + b) / 2;
            uint64_t k = rd64(blk + m * ENTRY);
            if (k == key) {
                uint32_t off = rd32(blk + m * ENTRY + 8), len = rd32(blk + m * ENTRY + 12);
                if (len <= cap && fseek(p.f, off, SEEK_SET) == 0 && fread(buf, 1, len, p.f) == len) got = len;
                break;
            }
            if (k < key) a = m + 1;
            else b = m - 1;
        }
    }
    xSemaphoreGive(lock_);
    return got;
}

bool sd_lookup(uint8_t table, uint32_t key, char *out, size_t cap) {
    if (!ldbN || !cap) return false;
    out[0] = 0;
    auto at = [](uint32_t i) { return (uint64_t)ldbIdx[i * 16] << 32 | rd32(ldbIdx + i * 16 + 4); };
    uint64_t want = (uint64_t)table << 32 | key;
    int32_t a = 0, b = (int32_t)ldbN - 1;
    while (a <= b) {
        int32_t m = (a + b) / 2;
        uint64_t k = at(m);
        if (k == want) {
            uint32_t off = rd32(ldbIdx + m * 16 + 8), len = min((uint32_t)cap - 1, rd32(ldbIdx + m * 16 + 12));
            xSemaphoreTake(lock_, portMAX_DELAY);
            bool ok = fseek(ldbF, ldbNames + off, SEEK_SET) == 0 && fread(out, 1, len, ldbF) == len;
            xSemaphoreGive(lock_);
            out[ok ? len : 0] = 0;
            return ok;
        }
        if (k < want) a = m + 1;
        else b = m - 1;
    }
    return false;
}

const char *sd_vendor(const char *mac) {
    // The Live list asks every frame: a small cache keeps that off the card.
    static struct { uint32_t p; char n[40]; } cache[64];
    static uint8_t next = 0;
    if (!ldbN || !mac || strlen(mac) < 8) return "";
    uint32_t b0 = strtoul(mac, nullptr, 16);
    if (b0 & 0x02) return "";   // locally administered (a random address): no vendor
    uint32_t p = b0 << 16 | (uint32_t)strtoul(mac + 3, nullptr, 16) << 8 | strtoul(mac + 6, nullptr, 16);
    for (auto &c : cache)
        if (c.p == p + 1) return c.n;
    auto &c = cache[next++ % 64];
    if (!sd_lookup(LK_OUI, p, c.n, sizeof c.n)) c.n[0] = 0;
    c.p = p + 1;   // 0 = an empty slot
    return c.n;
}
