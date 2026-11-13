// AGPL-3.0. See RadarModel.h.
#include "RadarModel.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr float kKmPerMile = 1.609344f;
constexpr float kKmPerDegLat = 111.32f;
// Labelled-ring presets: miles for imperial, km for metric. Metric has two extra
// wider steps because 25 km covers a much smaller area than 25 miles.
constexpr float kImperialPresets[] = {5.0f, 10.0f, 15.0f, 25.0f};
constexpr float kMetricPresets[] = {5.0f, 10.0f, 15.0f, 25.0f, 40.0f, 50.0f};
static_assert(sizeof(kMetricPresets) / sizeof(float) <= RadarModel::kMaxRangePresets, "too many presets");
// Fetch a bit beyond the outer ring so edge dots can show approaching planes.
constexpr float kFetchMarginFactor = 1.15f;
} // namespace

RadarModel::RadarModel(double centerLat, double centerLon, uint8_t rangeIndex, bool metric)
    : m_lat(centerLat), m_lon(centerLon), m_cosLat(cosf(static_cast<float>(centerLat) * static_cast<float>(M_PI) / 180.0f)),
      m_rangeIndex(0), m_metric(metric) {
    setRangeIndex(rangeIndex);
}

void RadarModel::configure(double centerLat, double centerLon, bool metric) {
    m_lat = centerLat;
    m_lon = centerLon;
    m_cosLat = cosf(static_cast<float>(centerLat) * static_cast<float>(M_PI) / 180.0f);
    m_metric = metric;
    setRangeIndex(m_rangeIndex);
}

size_t RadarModel::rangePresetCount() const {
    return m_metric ? sizeof(kMetricPresets) / sizeof(float) : sizeof(kImperialPresets) / sizeof(float);
}

void RadarModel::setRangeIndex(uint8_t index) {
    m_rangeIndex = index < rangePresetCount() ? index : 1;
}

void RadarModel::cycleRange() {
    m_rangeIndex = static_cast<uint8_t>((m_rangeIndex + 1) % rangePresetCount());
}

float RadarModel::ringKm() const {
    return m_metric ? kMetricPresets[m_rangeIndex] : kImperialPresets[m_rangeIndex] * kKmPerMile;
}

float RadarModel::outerKm() const {
    return ringKm() * 4.0f / 3.0f;
}

float RadarModel::fetchRadiusKm() const {
    return outerKm() * kFetchMarginFactor;
}

void RadarModel::offsetKm(float lat, float lon, float *dxKm, float *dyKm) const {
    *dxKm = static_cast<float>(lon - m_lon) * kKmPerDegLat * m_cosLat;
    *dyKm = static_cast<float>(lat - m_lat) * kKmPerDegLat;
}

float RadarModel::distanceKm(float lat, float lon) const {
    float dx = 0.0f;
    float dy = 0.0f;
    offsetKm(lat, lon, &dx, &dy);
    return sqrtf(dx * dx + dy * dy);
}

void RadarModel::formatDistance(float km, char *buf, size_t len) const {
    const float v = m_metric ? km : km / kKmPerMile;
    const char *unit = m_metric ? "km" : "mi";
    if (v < 10.0f) {
        snprintf(buf, len, "%.1f %s", v, unit);
    } else {
        snprintf(buf, len, "%d %s", static_cast<int>(lroundf(v)), unit);
    }
}

void RadarModel::update() {
    const size_t n = AdsbClient::aircraftCount();
    const AdsbClient::Aircraft *planes = AdsbClient::aircraftList();

    m_sortedCount = 0;
    for (size_t i = 0; i < n && m_sortedCount < AdsbClient::kMaxAircraft; ++i) {
        m_sorted[m_sortedCount++] = {i, distanceKm(planes[i].lat, planes[i].lon)};
    }
    for (size_t i = 1; i < m_sortedCount; ++i) {
        const Sorted key = m_sorted[i];
        size_t j = i;
        while (j > 0 && m_sorted[j - 1].distKm > key.distKm) {
            m_sorted[j] = m_sorted[j - 1];
            --j;
        }
        m_sorted[j] = key;
    }

    assignSlots();
}

void RadarModel::setPinned(const char *callsign) {
    if (callsign == nullptr) {
        m_pinned[0] = '\0';
        return;
    }
    strncpy(m_pinned, callsign, sizeof(m_pinned) - 1);
    m_pinned[sizeof(m_pinned) - 1] = '\0';
}

bool RadarModel::isPinned(const char *callsign) const {
    return callsign != nullptr && m_pinned[0] != '\0' && strncmp(m_pinned, callsign, sizeof(m_pinned)) == 0;
}

int RadarModel::findRank(const char *callsign) const {
    const AdsbClient::Aircraft *planes = AdsbClient::aircraftList();
    for (size_t i = 0; i < m_sortedCount; ++i) {
        if (strncmp(planes[m_sorted[i].index].callsign, callsign, 9) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RadarModel::assignSlots() {
    const AdsbClient::Aircraft *planes = AdsbClient::aircraftList();

    if (m_pinned[0] != '\0') {
        if (findRank(m_pinned) < 0) {
            m_pinned[0] = '\0'; // pinned plane left the area
        } else if (strncmp(m_slotCallsign[0], m_pinned, sizeof(m_pinned)) != 0) {
            // If the pinned plane is on another card, swap so the plane it
            // displaces from card 1 stays visible.
            for (int s = 1; s < kDetailSlots; ++s) {
                if (strncmp(m_slotCallsign[s], m_pinned, sizeof(m_pinned)) == 0) {
                    memcpy(m_slotCallsign[s], m_slotCallsign[0], sizeof(m_slotCallsign[s]));
                    break;
                }
            }
            memcpy(m_slotCallsign[0], m_pinned, sizeof(m_pinned));
        }
    }

    for (int slot = 0; slot < kDetailSlots; ++slot) {
        const int rank = resolveSlotRank(slot);
        m_slotRank[slot] = rank;
        if (rank >= 0) {
            strncpy(m_slotCallsign[slot], planes[m_sorted[rank].index].callsign, sizeof(m_slotCallsign[slot]) - 1);
            m_slotCallsign[slot][sizeof(m_slotCallsign[slot]) - 1] = '\0';
        } else {
            m_slotCallsign[slot][0] = '\0';
        }
    }
    buildList();
}

int RadarModel::resolveSlotRank(int slot) const {
    if (m_sortedCount == 0) {
        return -1;
    }
    const AdsbClient::Aircraft *planes = AdsbClient::aircraftList();

    // Keep the aircraft this slot showed last time, if it's still around.
    if (m_slotCallsign[slot][0] != '\0') {
        for (size_t i = 0; i < m_sortedCount; ++i) {
            if (strncmp(planes[m_sorted[i].index].callsign, m_slotCallsign[slot], sizeof(m_slotCallsign[slot])) == 0) {
                return static_cast<int>(i);
            }
        }
    }

    // Otherwise take the closest aircraft no other slot is showing.
    for (size_t i = 0; i < m_sortedCount; ++i) {
        const char *callsign = planes[m_sorted[i].index].callsign;
        bool used = false;
        for (int s = 0; s < kDetailSlots; ++s) {
            if (s != slot && m_slotCallsign[s][0] != '\0' &&
                strncmp(m_slotCallsign[s], callsign, sizeof(m_slotCallsign[s])) == 0) {
                used = true;
                break;
            }
        }
        if (!used) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void RadarModel::buildList() {
    m_listCount = 0;
    bool used[AdsbClient::kMaxAircraft] = {};
    for (int s = 0; s < kDetailSlots; ++s) {
        const int rank = m_slotRank[s];
        if (rank >= 0 && static_cast<size_t>(rank) < m_sortedCount && !used[rank]) {
            m_list[m_listCount++] = static_cast<uint8_t>(rank);
            used[rank] = true;
        }
    }
    for (size_t r = 0; r < m_sortedCount; ++r) {
        if (!used[r]) {
            m_list[m_listCount++] = static_cast<uint8_t>(r);
        }
    }
}

const AdsbClient::Aircraft *RadarModel::listAircraft(size_t pos) const {
    if (pos >= m_listCount) {
        return nullptr;
    }
    return &AdsbClient::aircraftList()[m_sorted[m_list[pos]].index];
}

float RadarModel::listDistanceKm(size_t pos) const {
    return pos < m_listCount ? m_sorted[m_list[pos]].distKm : 0.0f;
}

int RadarModel::listPosForAircraft(size_t aircraftIndex) const {
    for (size_t p = 0; p < m_listCount; ++p) {
        if (m_sorted[m_list[p]].index == aircraftIndex) {
            return static_cast<int>(p);
        }
    }
    return -1;
}

size_t RadarModel::listCallsigns(size_t first, size_t count, const char **out) const {
    size_t n = 0;
    for (size_t p = first; p < first + count && p < m_listCount; ++p) {
        out[n++] = listAircraft(p)->callsign;
    }
    return n;
}

const AdsbClient::Aircraft *RadarModel::slotAircraft(int slot) const {
    const int rank = m_slotRank[slot];
    if (rank < 0 || static_cast<size_t>(rank) >= m_sortedCount) {
        return nullptr;
    }
    return &AdsbClient::aircraftList()[m_sorted[rank].index];
}

float RadarModel::slotDistanceKm(int slot) const {
    const int rank = m_slotRank[slot];
    return rank >= 0 ? m_sorted[rank].distKm : 0.0f;
}

int RadarModel::slotForAircraft(size_t aircraftIndex) const {
    for (int s = 0; s < kDetailSlots; ++s) {
        const int rank = m_slotRank[s];
        if (rank >= 0 && m_sorted[rank].index == aircraftIndex) {
            return s;
        }
    }
    return -1;
}

size_t RadarModel::slotCallsigns(const char **out) const {
    size_t count = 0;
    for (int s = 0; s < kDetailSlots; ++s) {
        const AdsbClient::Aircraft *p = slotAircraft(s);
        if (p != nullptr) {
            out[count++] = p->callsign;
        }
    }
    return count;
}
