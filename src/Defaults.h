// First-boot defaults. config.h is optional: anything it defines is used until
// the settings are changed on the device (setup page), after which the saved
// values win. Without config.h the device starts in setup mode.
#pragma once

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

// Radar centre (decimal degrees)
#ifndef RADAR_LAT
#define RADAR_LAT 51.5072
#endif
#ifndef RADAR_LON
#define RADAR_LON -0.1276
#endif

#ifndef RADAR_RANGE_INDEX
#define RADAR_RANGE_INDEX 3
#endif

#ifndef RADAR_FETCH_INTERVAL_SEC
#define RADAR_FETCH_INTERVAL_SEC 12
#endif

// POSIX timezone for the "updated HH:MM" label (default: Central European Time).
#ifndef RADAR_TZ
#define RADAR_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#endif

#ifndef RADAR_BATTERY_INTERVAL_SEC
#define RADAR_BATTERY_INTERVAL_SEC 300
#endif

// Start in battery mode on first boot (afterwards the saved choice wins).
#ifndef RADAR_BATTERY_MODE
#define RADAR_BATTERY_MODE 0
#endif

#ifdef RADAR_UNITS_METRIC
#define RADAR_DEFAULT_METRIC true
#else
#define RADAR_DEFAULT_METRIC false
#endif
