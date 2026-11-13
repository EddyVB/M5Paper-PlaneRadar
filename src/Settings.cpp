// AGPL-3.0. See Settings.h.
#include "Settings.h"

#include <Preferences.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "Defaults.h"

namespace {
// Same namespace as the range and battery-mode keys in main.cpp.
constexpr char kNamespace[] = "planeradar";

void copy(char *dst, size_t len, const char *src) {
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
}

template <typename T> T clampTo(T v, T lo, T hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
} // namespace

void Settings::load() {
    copy(ssid, sizeof(ssid), WIFI_SSID);
    copy(password, sizeof(password), WIFI_PASSWORD);
    lat = RADAR_LAT;
    lon = RADAR_LON;
    metric = RADAR_DEFAULT_METRIC;
    copy(tz, sizeof(tz), RADAR_TZ);
    fetchSec = RADAR_FETCH_INTERVAL_SEC;
    batterySec = RADAR_BATTERY_INTERVAL_SEC;

    Preferences prefs;
    // Read-only open fails if the namespace doesn't exist yet (first boot).
    if (prefs.begin(kNamespace, true)) {
        if (prefs.isKey("ssid")) {
            prefs.getString("ssid", ssid, sizeof(ssid));
            prefs.getString("pass", password, sizeof(password));
        }
        // getDouble logs an error for a missing key, so check first.
        if (prefs.isKey("lat") && prefs.isKey("lon")) {
            lat = prefs.getDouble("lat", lat);
            lon = prefs.getDouble("lon", lon);
        }
        metric = prefs.getBool("metric", metric);
        if (prefs.isKey("tz")) {
            prefs.getString("tz", tz, sizeof(tz));
        }
        fetchSec = prefs.getUShort("fetchSec", fetchSec);
        batterySec = prefs.getUShort("battSec", batterySec);
        prefs.end();
    }
    sanitize();
}

void Settings::save() const {
    Preferences prefs;
    prefs.begin(kNamespace, false);
    prefs.putString("ssid", ssid);
    prefs.putString("pass", password);
    prefs.putDouble("lat", lat);
    prefs.putDouble("lon", lon);
    prefs.putBool("metric", metric);
    prefs.putString("tz", tz);
    prefs.putUShort("fetchSec", fetchSec);
    prefs.putUShort("battSec", batterySec);
    prefs.end();
}

void Settings::sanitize() {
    fetchSec = clampTo(fetchSec, kMinFetchSec, kMaxFetchSec);
    batterySec = clampTo(batterySec, kMinBatterySec, kMaxBatterySec);
    lat = clampTo(lat, -90.0, 90.0);
    lon = clampTo(lon, -180.0, 180.0);
    if (tz[0] == '\0') {
        copy(tz, sizeof(tz), RADAR_TZ);
    }
}

bool parseLatLon(const char *text, double *lat, double *lon) {
    char *end = nullptr;
    const double a = strtod(text, &end);
    if (end == text) {
        return false;
    }
    while (*end == ' ' || *end == ',' || *end == ';' || *end == '\t') {
        ++end;
    }
    const char *second = end;
    const double b = strtod(second, &end);
    if (end == second) {
        return false;
    }
    while (*end == ' ') {
        ++end;
    }
    if (*end != '\0' || a < -90 || a > 90 || b < -180 || b > 180) {
        return false;
    }
    *lat = a;
    *lon = b;
    return true;
}
