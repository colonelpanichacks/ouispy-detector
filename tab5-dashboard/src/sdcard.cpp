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

void sd_begin() {
    lock_ = xSemaphoreCreateMutex();
    blk = (uint8_t *)ps_malloc(BLOCK * ENTRY);
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
    set_state();
}

bool sd_ok() { return mounted; }

namespace {
void set_state() {
    snprintf(state, sizeof state, "%.1f of %.1f GB free, %d map pack%s",
             (SD.totalBytes() - SD.usedBytes()) / 1e9, SD.totalBytes() / 1e9, npacks, npacks == 1 ? "" : "s");
}

// Only plain pack names: letters, digits, '-', '_' and one ".pak" at the end.
bool part_path(const char *name, char *out, size_t cap, const char *ext) {
    size_t l = strlen(name);
    if (l < 5 || l > 60 || strcasecmp(name + l - 4, ".pak")) return false;
    for (size_t i = 0; i < l - 4; i++)
        if (!isalnum((unsigned char)name[i]) && name[i] != '-' && name[i] != '_') return false;
    snprintf(out, cap, "/sdcard/maps/%.*s%s", (int)(l - 4), name, ext);
    return true;
}
}  // namespace

long sd_part_size(const char *name) {
    char p[96];
    if (!mounted || !part_path(name, p, sizeof p, ".part")) return -1;
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : 0;
}

long sd_put(const char *name, long off, Stream &in, long len) {
    char p[96];
    if (!mounted || !part_path(name, p, sizeof p, ".part") || len < 0) return -1;
    mkdir("/sdcard/maps", 0775);
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
    if (!mounted || !part_path(name, part, sizeof part, ".part") || !part_path(name, pak, sizeof pak, ".pak")) return false;
    xSemaphoreTake(lock_, portMAX_DELAY);
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
