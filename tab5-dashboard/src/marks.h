// Map marks: pins (an icon, a colour, a name) and named areas, the same ones the dashboard
// map has (home-dashboard/marks.py). Made here or there; one made here offline is kept on
// the card and sent when the dashboard is back. All calls under the model lock, except
// marks_sync (poll task) and marks_load (setup).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

constexpr int MAX_MARKS = 64, MARK_PTS = 24, MARK_ICONS = 12;
extern const char *const MARK_ICON_IDS[MARK_ICONS];   // the dashboard's names, in its order

struct Mark {
    char id[25];
    char name[41];
    uint8_t kind;          // 0 pin, 1 area
    uint8_t icon;          // MARK_ICON_IDS index
    int16_t hue;           // 0-359
    double lat, lon;       // a pin; an area's centre
    uint8_t npts;
    float pts[MARK_PTS][2];
    bool pending;          // changed here, not on the dashboard yet
    bool deleted;          // deleted here, the dashboard not told yet (not drawn)
};

int marks_count();
Mark *mark_at(int i);
int mark_find(const char *id);
void mark_put(const Mark &m);            // add or replace by id; sent to the dashboard
void mark_delete(const char *id);
void mark_new_id(char *out, size_t cap);
void marks_ingest(JsonArrayConst arr);   // the dashboard's list, keeping our unsent changes
void marks_load();                       // from the card, at boot
void marks_sync();                       // poll task: send what changed, keep the card copy current
