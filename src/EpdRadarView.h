// E-ink rendering for the M5Paper (960x540 landscape). AGPL-3.0.
#pragma once

#include <M5Unified.h>
#include <ctime>

#include "RadarModel.h"

class EpdRadarView {
public:
    struct Status {
        time_t lastUpdate = 0;  // 0 = unknown
        bool stale = false;
        int batteryPercent = -1; // -1 = unknown
        bool batteryMode = false;
        time_t nextUpdate = 0;   // battery mode: when the device wakes again
        size_t firstCard = 0;    // list position shown on the top card (scrolling)
    };

    EpdRadarView();
    bool begin();
    void showMessage(const char *title, const char *line, const char *line2 = nullptr);
    void render(const RadarModel &model, const Status &status);

    // Setup mode: QR codes to join the device's WiFi and open the settings page.
    void showSetup(const char *apSsid, const char *apPassword, const char *url, const char *footer);
    // Live mode: QR code for the settings page on the home network.
    void showSettingsInfo(const char *url, const char *altUrl);
    // True if (x, y) is on the "Start setup WiFi" button of the settings screen.
    static bool setupButtonHit(int x, int y);

    // Callsign of the plane drawn nearest to (x, y) within a finger's reach,
    // or nullptr if the tap hit empty radar.
    const char *planeAt(int x, int y) const;

private:
    struct HitTarget {
        char callsign[9];
        int16_t x;
        int16_t y;
    };

    void addHitTarget(const char *callsign, int x, int y);
    void drawRadar(const RadarModel &model, size_t firstCard);
    void drawHeader(const RadarModel &model, const Status &status);
    void drawCard(const RadarModel &model, size_t pos, int x, int y, int w, int h);
    void drawScrollbar(const RadarModel &model, size_t firstCard);
    void push(bool fullRefresh);
    void drawQr(const char *text, int cx, int y, int size);

    M5Canvas m_canvas;
    unsigned m_renderCount = 0;
    HitTarget m_hits[AdsbClient::kMaxAircraft];
    size_t m_hitCount = 0;
};
