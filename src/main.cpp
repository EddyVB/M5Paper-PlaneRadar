#include <M5Unified.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>

#include <ctime>

#include "AdsbClient.h"
#include "ConfigPortal.h"
#include "Defaults.h"
#include "EpdRadarView.h"
#include "RadarModel.h"
#include "RouteClient.h"
#include "Settings.h"

namespace {

// Data is shown as stale after this many missed polls.
constexpr unsigned kStaleAfterPolls = 3;

// Touching the radar (left 540x540 square) cycles the range.
constexpr int kRadarTouchMaxX = 544;
// Tapping the header above the cards opens the settings screen.
constexpr int kHeaderTouchMaxY = 58;
constexpr unsigned long kSettingsScreenMs = 120000;
// Live mode: after this many failed WiFi attempts in a row, start setup mode,
// and leave it again (to retry the saved network) after kSetupTimeoutMs.
constexpr unsigned kWifiFailuresForSetup = 3;
constexpr unsigned long kSetupTimeoutMs = 10UL * 60 * 1000;
// Wait this long after the last range tap before fetching, so several quick
// taps cost one request (adsb.fi rate-limits bursts).
constexpr unsigned long kRangeFetchDelayMs = 1500;

// Card list scrolling: a swipe moves a page, a wheel step moves one card.
// The list jumps back to the top after this long without input.
constexpr int kSwipeMinPx = 40;
constexpr unsigned long kScrollResetMs = 60000;
// Spacing between on-demand route lookups for the visible cards.
constexpr unsigned long kRouteLookupSpacingMs = 4000;

// Battery mode time budgets: give up rather than drain the battery.
constexpr unsigned long kBatteryWifiTimeoutMs = 15000;
constexpr unsigned long kBatteryNtpTimeoutMs = 3000;

constexpr char kPrefsNamespace[] = "planeradar";
constexpr char kPrefsRangeKey[] = "rangeIdx";
constexpr char kPrefsBatteryKey[] = "battery";

Settings g_settings;
RadarModel g_model(RADAR_LAT, RADAR_LON, RADAR_RANGE_INDEX, RADAR_DEFAULT_METRIC);
EpdRadarView g_view;
Preferences g_prefs;
bool g_batteryMode = false;
size_t g_routeCursor = 0;
unsigned long g_lastPollMs = 0;
unsigned long g_lastSuccessMs = 0;
time_t g_lastSuccessTime = 0;
unsigned long g_rangeChangedMs = 0;
bool g_rangeFetchPending = false;
volatile bool g_ntpSynced = false;
size_t g_firstCard = 0;
unsigned long g_lastScrollMs = 0;
unsigned long g_lastRouteLookupMs = 0;
unsigned g_wifiFailures = 0;
bool g_lanPortalStarted = false;

size_t maxFirstCard() {
    const size_t n = g_model.listCount();
    return n > static_cast<size_t>(RadarModel::kDetailSlots) ? n - RadarModel::kDetailSlots : 0;
}

void clampScroll() {
    g_firstCard = std::min(g_firstCard, maxFirstCard());
}

// Returns true if the visible cards changed.
bool scrollBy(int cards) {
    const size_t before = g_firstCard;
    const long target = static_cast<long>(g_firstCard) + cards;
    g_firstCard = target < 0 ? 0 : static_cast<size_t>(target);
    clampScroll();
    g_lastScrollMs = millis();
    if (g_firstCard != before) {
        Serial.printf("scroll: cards %u-%u of %u\n", static_cast<unsigned>(g_firstCard + 1),
                      static_cast<unsigned>(std::min(g_model.listCount(), g_firstCard + RadarModel::kDetailSlots)),
                      static_cast<unsigned>(g_model.listCount()));
    }
    return g_firstCard != before;
}

// ---------------------------------------------------------------- settings

void loadSettings() {
    g_settings.load();
    g_model.configure(g_settings.lat, g_settings.lon, g_settings.metric);
    Serial.printf("settings: wifi \"%s\", centre %.5f, %.5f, %s, every %u s, battery every %u s\n",
                  g_settings.ssid, g_settings.lat, g_settings.lon, g_settings.metric ? "metric" : "imperial",
                  g_settings.fetchSec, g_settings.batterySec);
    g_prefs.begin(kPrefsNamespace, true);
    const uint8_t range = g_prefs.getUChar(kPrefsRangeKey, RADAR_RANGE_INDEX);
    g_batteryMode = g_prefs.getBool(kPrefsBatteryKey, RADAR_BATTERY_MODE != 0);
    g_prefs.end();
    g_model.setRangeIndex(range);
    Serial.printf("settings: range index %u (%.1f km outer), %s mode\n", g_model.rangeIndex(), g_model.outerKm(),
                  g_batteryMode ? "battery" : "live");
}

void saveRange() {
    g_prefs.begin(kPrefsNamespace, false);
    g_prefs.putUChar(kPrefsRangeKey, g_model.rangeIndex());
    g_prefs.end();
}

void saveBatteryMode() {
    g_prefs.begin(kPrefsNamespace, false);
    g_prefs.putBool(kPrefsBatteryKey, g_batteryMode);
    g_prefs.end();
}

// ---------------------------------------------------------------- time

// The SNTP callback runs on another task; just flag it and write the RTC from
// the main task (the RTC shares its I2C bus with the touch panel).
void onNtpSync(struct timeval *) {
    g_ntpSynced = true;
}

void startNtp() {
    g_ntpSynced = false;
    sntp_set_time_sync_notification_cb(onNtpSync);
    configTzTime(g_settings.tz, "pool.ntp.org", "time.google.com");
}

// Copy NTP time into the RTC so battery-mode wakes know the time even when
// they skip or fail the NTP sync.
void storeTimeInRtcIfSynced() {
    if (!g_ntpSynced) {
        return;
    }
    g_ntpSynced = false;
    const time_t now = time(nullptr);
    struct tm utc;
    gmtime_r(&now, &utc);
    if (M5.Rtc.isEnabled() && M5.Rtc.setDateTime(&utc)) {
        Serial.println("time: NTP synced, RTC updated");
    }
}

void restoreTimeFromRtc() {
    if (M5.Rtc.isEnabled()) {
        M5.Rtc.setSystemTimeFromRtc(); // RTC holds UTC
    }
    setenv("TZ", g_settings.tz, 1);
    tzset();
}

// ---------------------------------------------------------------- shared

EpdRadarView::Status makeStatus() {
    EpdRadarView::Status s;
    s.lastUpdate = g_lastSuccessTime;
    s.stale = g_lastSuccessMs == 0 ||
              millis() - g_lastSuccessMs > kStaleAfterPolls * g_settings.fetchSec * 1000UL;
    s.batteryPercent = M5.Power.getBatteryLevel();
    s.batteryMode = g_batteryMode;
    s.firstCard = g_firstCard;
    return s;
}

bool connectWifiBlocking(unsigned long timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_settings.ssid, g_settings.password);
    Serial.printf("wifi: connecting to %s", g_settings.ssid);
    const unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
        delay(100);
        M5.update();
        Serial.print('.');
    }
    Serial.println();
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("wifi: connection failed");
        return false;
    }
    Serial.printf("wifi: connected in %lu ms, IP %s\n", millis() - start, WiFi.localIP().toString().c_str());
    return true;
}

// ---------------------------------------------------------------- battery mode

[[noreturn]] void sleepUntilNextUpdate() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.printf("battery: sleeping %u s (awake %lu ms)\n", g_settings.batterySec, millis());
    Serial.flush();
    // On battery this cuts power and the RTC alarm turns the M5Paper back on.
    // On USB power it can't cut power, so it falls back to a timed deep sleep.
    M5.Power.timerSleep(g_settings.batterySec);
    // Only reached if timerSleep refused; deep sleep is the next best thing.
    esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(g_settings.batterySec) * 1000000ULL);
    esp_deep_sleep_start();
}

// One battery-mode update: fetch, look up routes, draw, power off.
[[noreturn]] void runBatteryCycle() {
    bool ok = false;
    if (connectWifiBlocking(kBatteryWifiTimeoutMs)) {
        startNtp();
        for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
            ok = AdsbClient::fetchUpdate(g_model.centerLat(), g_model.centerLon(), g_model.fetchRadiusKm());
        }
        if (ok) {
            g_model.update();
            // The route cache doesn't survive power-off, so look up all four now.
            const char *callsigns[RadarModel::kDetailSlots];
            const size_t count = g_model.slotCallsigns(callsigns);
            for (size_t i = 0; i < count; ++i) {
                RouteClient::fetchIfUncached(callsigns[i], true);
            }
        }
        // NTP usually lands during the fetches; wait briefly if it hasn't.
        const unsigned long ntpStart = millis();
        while (!g_ntpSynced && millis() - ntpStart < kBatteryNtpTimeoutMs) {
            delay(50);
        }
        storeTimeInRtcIfSynced();
    }

    EpdRadarView::Status status;
    status.batteryMode = true;
    status.batteryPercent = M5.Power.getBatteryLevel();
    status.lastUpdate = ok ? time(nullptr) : 0;
    status.stale = !ok;
    status.nextUpdate = time(nullptr) + g_settings.batterySec;
    g_view.render(g_model, status);
    Serial.printf("battery: %s, %u aircraft, battery %d%%\n", ok ? "updated" : "update failed",
                  static_cast<unsigned>(AdsbClient::aircraftCount()), status.batteryPercent);

    // Pressing the wheel during the wake also leaves battery mode.
    M5.update();
    if (M5.BtnB.isPressed()) {
        g_batteryMode = false;
        saveBatteryMode();
        ESP.restart();
    }
    sleepUntilNextUpdate();
}

// Holding the wheel while the device starts leaves battery mode.
bool wheelHeldAtBoot() {
    for (int i = 0; i < 5; ++i) {
        M5.update();
        delay(10);
    }
    return M5.BtnB.isPressed();
}

// ---------------------------------------------------------------- setup mode

[[noreturn]] void restartAfterSave() {
    // Give the phone a moment to receive the "Saved" page.
    const unsigned long start = millis();
    while (millis() - start < 800) {
        ConfigPortal::handle();
        delay(5);
    }
    g_view.showMessage("Settings saved", "Restarting...");
    ESP.restart();
    for (;;) {
    }
}

// Runs the device's own WiFi with the settings page until new settings are
// saved. With saved WiFi settings, a tap on the screen (or timeoutMs > 0
// passing) restarts the device to try the saved network again.
[[noreturn]] void runSetupMode(unsigned long timeoutMs) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_AP_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apSsid[24];
    snprintf(apSsid, sizeof(apSsid), "PlaneRadar-%02X%02X", mac[4], mac[5]);
    // Fresh password each time; no easily confused characters (l/1, O/0).
    static const char kChars[] = "abcdefghjkmnpqrstuvwxyz23456789";
    char apPassword[9];
    for (size_t i = 0; i + 1 < sizeof(apPassword); ++i) {
        apPassword[i] = kChars[esp_random() % (sizeof(kChars) - 1)];
    }
    apPassword[sizeof(apPassword) - 1] = '\0';
    WiFi.softAP(apSsid, apPassword);
    delay(200);
    ConfigPortal::begin(&g_settings, true);

    char url[32];
    snprintf(url, sizeof(url), "http://%s/", WiFi.softAPIP().toString().c_str());
    const bool canLeave = g_settings.hasWifi();
    g_view.showSetup(apSsid, apPassword, url,
                     canLeave ? "Tap the screen to leave setup and try the saved WiFi again."
                              : "Enter your WiFi and location on the settings page.");
    Serial.printf("setup: WiFi %s, password %s, page %s\n", apSsid, apPassword, url);

    const unsigned long start = millis();
    for (;;) {
        M5.update();
        ConfigPortal::handle();
        if (ConfigPortal::saved()) {
            restartAfterSave();
        }
        const bool tapped = M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasClicked();
        if (canLeave && (tapped || (timeoutMs > 0 && millis() - start >= timeoutMs))) {
            Serial.println("setup: leaving to retry the saved WiFi");
            ESP.restart();
        }
        delay(5);
    }
}

// Waits up to ms for a tap on the screen; true if there was one.
bool waitForTap(unsigned long ms) {
    const unsigned long start = millis();
    while (millis() - start < ms) {
        M5.update();
        if (M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasClicked()) {
            return true;
        }
        delay(10);
    }
    return false;
}

// ---------------------------------------------------------------- live mode

enum class InputAction { None, Range, Pin, Scroll, BatteryMode, Settings };

// Tap near a plane: pin it to card 1 (tap it again to unpin).
// Tap empty radar or press the side wheel: cycle the range.
// Swipe the cards up/down: scroll a page. Roll the wheel: scroll one card.
// Hold the side wheel: switch to battery mode.
// Tap the header above the cards: show the settings screen.
InputAction handleInput(char *pinnedCallsign, size_t len) {
    if (M5.BtnB.wasHold()) {
        return InputAction::BatteryMode;
    }
    if (M5.BtnA.wasClicked() && scrollBy(-1)) {
        return InputAction::Scroll;
    }
    if (M5.BtnC.wasClicked() && scrollBy(1)) {
        return InputAction::Scroll;
    }
    bool cycle = M5.BtnB.wasClicked();
    if (M5.Touch.getCount() > 0) {
        const auto &t = M5.Touch.getDetail();
        // Finger moving up shows later cards, like scrolling a phone list.
        if ((t.wasFlicked() || t.wasDragged()) && t.base_x >= kRadarTouchMaxX && abs(t.distanceY()) >= kSwipeMinPx) {
            const int page = RadarModel::kDetailSlots;
            if (scrollBy(t.distanceY() < 0 ? page : -page)) {
                return InputAction::Scroll;
            }
        }
        if (t.wasClicked() && t.x >= kRadarTouchMaxX && t.y < kHeaderTouchMaxY) {
            return InputAction::Settings;
        }
        if (t.wasClicked() && t.x < kRadarTouchMaxX) {
            const char *hit = g_view.planeAt(t.x, t.y);
            if (hit != nullptr) {
                strncpy(pinnedCallsign, hit, len - 1);
                pinnedCallsign[len - 1] = '\0';
                if (g_model.isPinned(pinnedCallsign)) {
                    g_model.setPinned(nullptr);
                    Serial.printf("pin: %s unpinned\n", pinnedCallsign);
                } else {
                    g_model.setPinned(pinnedCallsign);
                    Serial.printf("pin: %s pinned to card 1\n", pinnedCallsign);
                }
                g_model.assignSlots();
                g_firstCard = 0; // the pinned plane is card 1
                return InputAction::Pin;
            }
            cycle = true;
        }
    }
    if (!cycle) {
        return InputAction::None;
    }
    g_model.cycleRange();
    saveRange();
    Serial.printf("range: tapped -> index %u (%.1f km outer)\n", g_model.rangeIndex(), g_model.outerKm());
    g_rangeChangedMs = millis();
    g_rangeFetchPending = true;
    return InputAction::Range;
}

void connectWifiLive() {
    char line[80];
    snprintf(line, sizeof(line), "Connecting to %s...", g_settings.ssid);
    g_view.showMessage("Plane Radar", line);
    if (connectWifiBlocking(20000)) {
        g_wifiFailures = 0;
        startNtp();
        if (!g_lanPortalStarted) {
            ConfigPortal::begin(&g_settings, false);
            g_lanPortalStarted = true;
        }
        g_view.showMessage("Plane Radar", "Fetching aircraft...");
        return;
    }
    ++g_wifiFailures;
    if (g_wifiFailures >= kWifiFailuresForSetup) {
        Serial.println("wifi: giving up, starting setup mode");
        runSetupMode(kSetupTimeoutMs);
    }
    snprintf(line, sizeof(line), "WiFi failed (%u of %u) - retrying soon", g_wifiFailures, kWifiFailuresForSetup);
    g_view.showMessage("Plane Radar", line, "Tap the screen to open setup instead.");
    if (waitForTap(8000)) {
        Serial.println("setup: tapped after WiFi failure");
        runSetupMode(kSetupTimeoutMs);
    }
}

// Settings screen with a QR code for the page on the home network. The page is
// served all the time in live mode; this screen just shows where to find it.
void showSettingsScreen() {
    char url[32];
    snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());
    char altUrl[40];
    snprintf(altUrl, sizeof(altUrl), "http://%s.local/", ConfigPortal::kHostname);
    g_view.showSettingsInfo(url, altUrl);
    const unsigned long start = millis();
    while (millis() - start < kSettingsScreenMs) {
        M5.update();
        ConfigPortal::handle();
        if (ConfigPortal::saved()) {
            restartAfterSave();
        }
        if (M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasClicked()) {
            const auto &t = M5.Touch.getDetail();
            if (EpdRadarView::setupButtonHit(t.x, t.y)) {
                Serial.println("setup: started from the settings screen");
                g_view.showMessage("Setup WiFi", "Starting...");
                runSetupMode(kSetupTimeoutMs);
            }
            break;
        }
        if (M5.BtnB.wasClicked()) {
            break;
        }
        delay(10);
    }
}

// Look up at most one missing route for the cards on screen. Returns true if a
// route was fetched (so the cards are worth redrawing). Spaced out by our own
// timer too, because failed lookups don't count toward RouteClient's spacing.
bool prefetchVisibleRoutes() {
    if (g_lastRouteLookupMs != 0 && millis() - g_lastRouteLookupMs < kRouteLookupSpacingMs) {
        return false;
    }
    const char *callsigns[RadarModel::kDetailSlots];
    const size_t count = g_model.listCallsigns(g_firstCard, RadarModel::kDetailSlots, callsigns);
    bool missing = false;
    for (size_t i = 0; i < count; ++i) {
        missing = missing || !RouteClient::isCached(callsigns[i]);
    }
    if (!missing) {
        return false;
    }
    g_lastRouteLookupMs = millis();
    return RouteClient::prefetchOne(callsigns, count, &g_routeCursor);
}

} // namespace

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    Serial.begin(115200);

    if (!g_view.begin()) {
        Serial.println("view: canvas allocation failed (PSRAM?)");
    }
    loadSettings();
    restoreTimeFromRtc();

    if (!g_settings.hasWifi()) {
        Serial.println("setup: no WiFi saved");
        runSetupMode(0);
    }

    if (g_batteryMode) {
        if (wheelHeldAtBoot()) {
            g_batteryMode = false;
            saveBatteryMode();
            Serial.println("battery: wheel held at boot, switching to live mode");
            g_view.showMessage("Live mode", "Battery mode is off.");
            delay(1500);
        } else {
            runBatteryCycle();
        }
    }
    connectWifiLive();
}

void loop() {
    M5.update();
    storeTimeInRtcIfSynced();
    if (g_lanPortalStarted) {
        ConfigPortal::handle();
        if (ConfigPortal::saved()) {
            restartAfterSave();
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        connectWifiLive();
        return;
    }

    // Redraw straight away so taps feel responsive. After a range change,
    // planes beyond the old fetch radius appear with the follow-up fetch.
    char tapped[9];
    const InputAction action = handleInput(tapped, sizeof(tapped));
    if (action == InputAction::BatteryMode) {
        g_batteryMode = true;
        saveBatteryMode();
        Serial.println("battery: switching to battery mode");
        char line[64];
        snprintf(line, sizeof(line), "Updates every %u min, powered off in between.", g_settings.batterySec / 60);
        g_view.showMessage("Battery mode", line, "To exit: hold the wheel in while restarting (power or reset button).");
        delay(4000);
        g_model.setPinned(nullptr);
        runBatteryCycle();
    }
    if (action == InputAction::Settings) {
        showSettingsScreen();
        g_view.render(g_model, makeStatus());
        return;
    }
    if (action != InputAction::None) {
        const unsigned long t0 = millis();
        g_view.render(g_model, makeStatus());
        Serial.printf("radar: redrawn after tap in %lu ms\n", millis() - t0);
        // Look up a newly pinned plane's route now rather than waiting for its
        // turn in the per-poll prefetch.
        if (action == InputAction::Pin && g_model.isPinned(tapped) && RouteClient::fetchIfUncached(tapped)) {
            g_view.render(g_model, makeStatus());
        }
        return;
    }

    // Back to the top of the list after a minute without scrolling.
    if (g_firstCard > 0 && millis() - g_lastScrollMs >= kScrollResetMs) {
        g_firstCard = 0;
        g_view.render(g_model, makeStatus());
        return;
    }

    const bool rangeFetchDue = g_rangeFetchPending && millis() - g_rangeChangedMs >= kRangeFetchDelayMs;
    const bool pollDue = g_lastPollMs == 0 || millis() - g_lastPollMs >= g_settings.fetchSec * 1000UL;
    if (!rangeFetchDue && !(pollDue && !g_rangeFetchPending)) {
        // Between polls, fill in routes for the cards on screen (e.g. after scrolling).
        if (g_lastSuccessMs != 0 && prefetchVisibleRoutes()) {
            g_view.render(g_model, makeStatus());
        }
        delay(20);
        return;
    }
    g_rangeFetchPending = false;
    g_lastPollMs = millis();

    if (AdsbClient::fetchUpdate(g_model.centerLat(), g_model.centerLon(), g_model.fetchRadiusKm())) {
        g_model.update();
        clampScroll();
        prefetchVisibleRoutes();
        g_lastSuccessMs = millis();
        g_lastSuccessTime = time(nullptr);
    }

    const unsigned long t0 = millis();
    g_view.render(g_model, makeStatus());
    Serial.printf("radar: %u aircraft, render %lu ms, heap %u\n", static_cast<unsigned>(AdsbClient::aircraftCount()),
                  millis() - t0, static_cast<unsigned>(ESP.getFreeHeap()));
}
