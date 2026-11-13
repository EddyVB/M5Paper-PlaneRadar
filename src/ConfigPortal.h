// Settings web page. In setup mode it runs on the device's own access point
// with a captive-portal DNS, so a phone opens it right after joining. In live
// mode the same page is served on the home network (http://planeradar.local).
// AGPL-3.0.
#pragma once

#include "Settings.h"

namespace ConfigPortal {

constexpr char kHostname[] = "planeradar";

// apMode: true when the device runs its own setup WiFi (no internet).
// Scans for nearby networks, so call it before the page is needed.
void begin(Settings *settings, bool apMode);
// Call often from the main loop.
void handle();
// True once new settings were saved; the caller restarts to apply them.
bool saved();

} // namespace ConfigPortal
