// Display-independent radar state: projection, distance sorting and the
// detail-slot pinning logic from the info-orbs Plane Radar widget (PR #329).
// AGPL-3.0.
#pragma once

#include <cstddef>
#include <cstdint>

#include "AdsbClient.h"

class RadarModel {
public:
    static constexpr int kDetailSlots = 4;
    static constexpr size_t kMaxRangePresets = 6;

    struct Sorted {
        size_t index;
        float distKm;
    };

    RadarModel(double centerLat, double centerLon, uint8_t rangeIndex, bool metric);
    // Change the centre and units (after loading settings). Keeps the range
    // index if it's valid for the new units.
    void configure(double centerLat, double centerLon, bool metric);

    // Range: the labelled 3rd ring is the preset; the outer (4th) ring is 4/3 of it.
    // Presets: 5/10/15/25 mi, or 5/10/15/25/40/50 km when metric.
    uint8_t rangeIndex() const { return m_rangeIndex; }
    size_t rangePresetCount() const;
    void setRangeIndex(uint8_t index);
    void cycleRange();
    float ringKm() const;
    float outerKm() const;
    float fetchRadiusKm() const;
    bool metric() const { return m_metric; }

    double centerLat() const { return m_lat; }
    double centerLon() const { return m_lon; }

    // East (dx) / north (dy) offset from the radar centre in km.
    void offsetKm(float lat, float lon, float *dxKm, float *dyKm) const;
    float distanceKm(float lat, float lon) const;
    void formatDistance(float km, char *buf, size_t len) const;

    // Call after AdsbClient::fetchUpdate() succeeds.
    void update();

    // Pin an aircraft to slot 0 (card 1) until it leaves the fetch area.
    // nullptr clears the pin. Call assignSlots() afterwards to apply it now.
    void setPinned(const char *callsign);
    bool isPinned(const char *callsign) const;
    bool hasPin() const { return m_pinned[0] != '\0'; }
    void assignSlots();

    size_t sortedCount() const { return m_sortedCount; }
    const Sorted &sorted(size_t i) const { return m_sorted[i]; }

    // Detail slots stay pinned to the same aircraft between polls.
    const AdsbClient::Aircraft *slotAircraft(int slot) const;
    float slotDistanceKm(int slot) const;
    int slotForAircraft(size_t aircraftIndex) const;
    size_t slotCallsigns(const char **out) const;

    // Scrollable card list: the four pinned slots first (so the top of the list
    // looks the same as before), then every other aircraft by distance.
    size_t listCount() const { return m_listCount; }
    const AdsbClient::Aircraft *listAircraft(size_t pos) const;
    float listDistanceKm(size_t pos) const;
    int listPosForAircraft(size_t aircraftIndex) const;
    size_t listCallsigns(size_t first, size_t count, const char **out) const;

private:
    int resolveSlotRank(int slot) const;
    int findRank(const char *callsign) const;
    void buildList();

    double m_lat;
    double m_lon;
    float m_cosLat;
    uint8_t m_rangeIndex;
    bool m_metric;

    Sorted m_sorted[AdsbClient::kMaxAircraft];
    size_t m_sortedCount = 0;
    char m_slotCallsign[kDetailSlots][9] = {};
    char m_pinned[9] = {};
    int m_slotRank[kDetailSlots] = {-1, -1, -1, -1};
    uint8_t m_list[AdsbClient::kMaxAircraft] = {}; // ranks into m_sorted
    size_t m_listCount = 0;
};
