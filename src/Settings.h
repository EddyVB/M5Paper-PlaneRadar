// Runtime settings, stored in NVS and editable from the setup web page.
// AGPL-3.0.
#pragma once

#include <cstddef>
#include <cstdint>

struct Settings {
    static constexpr uint16_t kMinFetchSec = 5;
    static constexpr uint16_t kMaxFetchSec = 120;
    static constexpr uint16_t kMinBatterySec = 60;
    static constexpr uint16_t kMaxBatterySec = 3600;

    char ssid[33] = {};
    char password[65] = {};
    double lat = 0;
    double lon = 0;
    bool metric = false;
    char tz[64] = {};
    uint16_t fetchSec = 12;
    uint16_t batterySec = 300;

    bool hasWifi() const { return ssid[0] != '\0'; }

    // Fills in the config.h defaults, then overrides them with saved values.
    void load();
    void save() const;
    // Clamps the intervals and coordinates into their valid ranges.
    void sanitize();
};

// Parses "48.4284, -123.3656" (also accepts spaces or ';' between them).
bool parseLatLon(const char *text, double *lat, double *lon);
