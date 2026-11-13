// AGPL-3.0. Radar geometry adapted from the info-orbs Plane Radar widget (PR #329),
// rescaled from a 240 px round TFT to a 540 px e-ink square.
#include "EpdRadarView.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "RouteClient.h"

namespace {

constexpr int kWidth = 960;
constexpr int kHeight = 540;

// Radar (left square)
constexpr int kCx = 270;
constexpr int kCy = 270;
constexpr int kOuterR = 240;
constexpr int kRingCount = 4;
constexpr int kRimR = 256;
constexpr int kRimDotR = 6;
constexpr int kNoseLen = 16;
constexpr int kTailLen = 7;
constexpr int kTailHalf = 9;
constexpr int kInsideInset = kNoseLen + kTailHalf + 1;
constexpr float kSpeedPxPerKnot = 0.16f;
constexpr int kSpeedMinPx = 4;
// A tap within this many pixels of a plane selects it.
constexpr int kHitRadiusPx = 30;

// Detail panel (right)
constexpr int kPanelX = 548;
constexpr int kPanelW = kWidth - kPanelX - 8;
constexpr int kCardTop = 66;
constexpr int kCardH = 114;
constexpr int kCardGap = 4;

// "Start setup WiFi" button on the settings screen
constexpr int kSetupBtnX = 490;
constexpr int kSetupBtnY = 372;
constexpr int kSetupBtnW = 420;
constexpr int kSetupBtnH = 72;

// Do a flashing full-quality refresh every N renders to clear e-ink ghosting.
constexpr unsigned kFullRefreshEvery = 30;

constexpr uint16_t kBlack = TFT_BLACK;
constexpr uint16_t kWhite = TFT_WHITE;
constexpr uint16_t kDark = TFT_DARKGREY;
constexpr uint16_t kLight = TFT_LIGHTGREY;

constexpr uint8_t kFlagMilitary = 1;
constexpr uint8_t kFlagInteresting = 2;
constexpr uint8_t kFlagPia = 4;
constexpr uint8_t kFlagLadd = 8;
constexpr float kKtToMph = 1.15078f;
constexpr float kKtToKmh = 1.852f;
constexpr float kMPerFt = 0.3048f;
constexpr float kDegToRad = static_cast<float>(M_PI) / 180.0f;

// The GFX fonts only cover ASCII, so fold accented Latin letters (route city
// names like "Bâle" or "Kraków") to their base letter.
constexpr char kLatin1Fold[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
                               "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
constexpr char kLatinExtAFold[] = "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Jj" "Jj"
                                  "Kkk" "LlLlLlLlLl" "NnNnNnnNn" "OoOoOoOo" "RrRrRr" "SsSsSsSs" "TtTtTt"
                                  "UuUuUuUuUuUu" "Ww" "YyY" "ZzZzZz" "s";
static_assert(sizeof(kLatin1Fold) == 65, "Latin-1 fold table must cover U+00C0..U+00FF");
static_assert(sizeof(kLatinExtAFold) == 129, "Latin Ext-A fold table must cover U+0100..U+017F");

void foldToAscii(const char *in, char *out, size_t outLen) {
    size_t o = 0;
    const auto *s = reinterpret_cast<const unsigned char *>(in);
    while (*s != 0 && o + 1 < outLen) {
        const unsigned char c = *s;
        if (c < 0x80) {
            out[o++] = static_cast<char>(c);
            ++s;
        } else if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
            const unsigned cp = ((c & 0x1F) << 6) | (s[1] & 0x3F);
            char mapped = '?';
            if (cp >= 0xC0 && cp <= 0xFF) {
                mapped = kLatin1Fold[cp - 0xC0];
            } else if (cp >= 0x100 && cp <= 0x17F) {
                mapped = kLatinExtAFold[cp - 0x100];
            }
            out[o++] = mapped;
            s += 2;
        } else {
            // 3/4-byte sequence or stray byte: skip it.
            ++s;
            while ((*s & 0xC0) == 0x80) {
                ++s;
            }
            out[o++] = '?';
        }
    }
    out[o] = '\0';
}

// Draw text with the largest font that fits maxW; truncate with "..." if none do.
void drawFitted(M5Canvas &c, const char *text, int x, int y, int maxW, const lgfx::IFont *const *fonts,
                size_t fontCount) {
    for (size_t i = 0; i < fontCount; ++i) {
        c.setFont(fonts[i]);
        if (c.textWidth(text) <= maxW) {
            c.drawString(text, x, y);
            return;
        }
    }
    char buf[96];
    strncpy(buf, text, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char tmp[100];
    for (size_t n = strlen(buf); n > 0; --n) {
        buf[n] = '\0';
        snprintf(tmp, sizeof(tmp), "%s...", buf);
        if (c.textWidth(tmp) <= maxW) {
            c.drawString(tmp, x, y);
            return;
        }
    }
}

void noseTip(int cx, int cy, float headingDeg, int *tx, int *ty) {
    const float rad = headingDeg * kDegToRad;
    *tx = cx + static_cast<int>(lroundf(sinf(rad) * kNoseLen));
    *ty = cy - static_cast<int>(lroundf(cosf(rad) * kNoseLen));
}

void drawHeadingTriangle(M5Canvas &c, int cx, int cy, float headingDeg, uint16_t color) {
    const float rad = headingDeg * kDegToRad;
    const float s = sinf(rad);
    const float co = cosf(rad);
    int tx = 0;
    int ty = 0;
    noseTip(cx, cy, headingDeg, &tx, &ty);
    const int bx = cx - static_cast<int>(lroundf(s * kTailLen));
    const int by = cy + static_cast<int>(lroundf(co * kTailLen));
    const int wx = static_cast<int>(lroundf(co * kTailHalf));
    const int wy = static_cast<int>(lroundf(s * kTailHalf));
    c.fillTriangle(tx, ty, bx + wx, by + wy, bx - wx, by - wy, color);
    c.drawTriangle(tx, ty, bx + wx, by + wy, bx - wx, by - wy, kBlack);
}

bool insideOuterRing(int x, int y) {
    const int dx = x - kCx;
    const int dy = y - kCy;
    return dx * dx + dy * dy <= kOuterR * kOuterR;
}

void clipToOuterRing(int x0, int y0, int *x1, int *y1) {
    if (insideOuterRing(*x1, *y1)) {
        return;
    }
    const int dx = *x1 - x0;
    const int dy = *y1 - y0;
    for (int step = 19; step > 0; --step) {
        const float t = step * 0.05f;
        const int px = x0 + static_cast<int>(lroundf(dx * t));
        const int py = y0 + static_cast<int>(lroundf(dy * t));
        if (insideOuterRing(px, py)) {
            *x1 = px;
            *y1 = py;
            return;
        }
    }
    *x1 = x0;
    *y1 = y0;
}

void drawSpeedVector(M5Canvas &c, int cx, int cy, float headingDeg, float trackDeg, float gsKnots) {
    if (gsKnots <= 0.0f) {
        return;
    }
    int len = static_cast<int>(gsKnots * kSpeedPxPerKnot + 0.5f);
    if (len < kSpeedMinPx) {
        len = kSpeedMinPx;
    }
    int tx = 0;
    int ty = 0;
    noseTip(cx, cy, headingDeg, &tx, &ty);
    const float rad = trackDeg * kDegToRad;
    int ex = tx + static_cast<int>(lroundf(sinf(rad) * len));
    int ey = ty - static_cast<int>(lroundf(cosf(rad) * len));
    clipToOuterRing(tx, ty, &ex, &ey);
    if (ex != tx || ey != ty) {
        c.drawWideLine(tx, ty, ex, ey, 1.2f, kDark);
    }
}

// Numbered disc matching a card to its plane. Two-digit numbers get a slightly
// bigger disc and the smaller font so they fit.
void drawBadge(M5Canvas &c, int x, int y, int r, int number, const lgfx::IFont *font, bool inverted = false) {
    if (number >= 10) {
        r += 2;
        font = &fonts::FreeSansBold9pt7b;
    }
    c.fillCircle(x, y, r, inverted ? kWhite : kBlack);
    c.setFont(font);
    c.setTextColor(inverted ? kBlack : kWhite);
    c.setTextDatum(middle_center);
    char text[4];
    snprintf(text, sizeof(text), "%d", number);
    c.drawString(text, x, y + 1);
    c.setTextColor(kBlack);
}

void drawLabel(M5Canvas &c, const char *text, int x, int y) {
    c.setFont(&fonts::FreeSans9pt7b);
    c.setTextDatum(top_left);
    const int w = c.textWidth(text);
    const int h = c.fontHeight();
    c.fillRect(x - 2, y - 1, w + 4, h + 1, kWhite);
    c.setTextColor(kBlack);
    c.drawString(text, x, y);
}

// Cogwheel icon (about 22 px across): eight teeth, a body and a hole.
void drawGear(M5Canvas &c, int cx, int cy) {
    for (int i = 0; i < 8; ++i) {
        const float a = i * 45.0f * kDegToRad;
        c.drawWideLine(cx + 6 * cosf(a), cy + 6 * sinf(a), cx + 11 * cosf(a), cy + 11 * sinf(a), 2.6f, kBlack);
    }
    c.fillCircle(cx, cy, 8, kBlack);
    c.fillCircle(cx, cy, 3, kWhite);
}

bool isSpecial(uint8_t flags) {
    return (flags & (kFlagMilitary | kFlagInteresting | kFlagPia | kFlagLadd)) != 0;
}

const char *flagsLabel(uint8_t flags) {
    if (flags & kFlagMilitary) return "MILITARY";
    if (flags & kFlagPia) return "PIA";
    if (flags & kFlagLadd) return "LADD";
    if (flags & kFlagInteresting) return "INTERESTING";
    return nullptr;
}

const char *movementLabel(const AdsbClient::Aircraft &p) {
    if (p.gsKnots < 40.0f || !p.hasBaroRate) return nullptr;
    if (p.baroRateFpm <= -250) return "Arriving";
    if (p.baroRateFpm >= 250) return "Departing";
    return nullptr;
}

// List position of an aircraft if its card is currently on screen, else -1.
int visibleCardPos(const RadarModel &model, size_t aircraftIndex, size_t firstCard) {
    const int pos = model.listPosForAircraft(aircraftIndex);
    if (pos < 0 || static_cast<size_t>(pos) < firstCard ||
        static_cast<size_t>(pos) >= firstCard + RadarModel::kDetailSlots) {
        return -1;
    }
    return pos;
}

struct PlacedPlane {
    size_t index;
    int x;
    int y;
    int distSq;
};

} // namespace

EpdRadarView::EpdRadarView() : m_canvas(&M5.Display) {}

bool EpdRadarView::begin() {
    if (M5.Display.width() < M5.Display.height()) {
        M5.Display.setRotation(M5.Display.getRotation() ^ 1);
    }
    m_canvas.setColorDepth(16);
    m_canvas.setPsram(true);
    return m_canvas.createSprite(kWidth, kHeight) != nullptr;
}

void EpdRadarView::push(bool fullRefresh) {
    M5.Display.setEpdMode(fullRefresh ? epd_mode_t::epd_quality : epd_mode_t::epd_text);
    M5.Display.startWrite();
    m_canvas.pushSprite(0, 0);
    M5.Display.endWrite();
    M5.Display.display();
}

void EpdRadarView::showMessage(const char *title, const char *line, const char *line2) {
    m_canvas.fillScreen(kWhite);
    m_canvas.setTextColor(kBlack);
    m_canvas.setTextDatum(middle_center);
    m_canvas.setFont(&fonts::FreeSansBold24pt7b);
    m_canvas.drawString(title, kWidth / 2, kHeight / 2 - 40);
    m_canvas.setFont(&fonts::FreeSans12pt7b);
    m_canvas.drawString(line, kWidth / 2, kHeight / 2 + 20);
    if (line2 != nullptr) {
        m_canvas.drawString(line2, kWidth / 2, kHeight / 2 + 56);
    }
    push(true);
    m_renderCount = 0;
}

void EpdRadarView::drawQr(const char *text, int cx, int y, int size) {
    // White quiet zone around the code, then the code itself (black on white).
    m_canvas.fillRect(cx - size / 2, y, size, size, kWhite);
    m_canvas.qrcode(text, cx - size / 2, y, size, 1, true);
}

void EpdRadarView::showSetup(const char *apSsid, const char *apPassword, const char *url, const char *footer) {
    M5Canvas &c = m_canvas;
    c.fillScreen(kWhite);
    c.setTextColor(kBlack);
    c.setTextDatum(top_center);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.drawString("Plane Radar setup", kWidth / 2, 14);

    constexpr int kQrSize = 270;
    constexpr int kQrY = 108;
    constexpr int kLeftX = kWidth / 4;
    constexpr int kRightX = kWidth * 3 / 4;
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.drawString("1. Scan to join the setup WiFi", kLeftX, 70);
    c.drawString("2. Scan to open the settings", kRightX, 70);

    // Standard WiFi QR payload; phone cameras offer to join it directly.
    char wifiQr[96];
    snprintf(wifiQr, sizeof(wifiQr), "WIFI:T:WPA;S:%s;P:%s;;", apSsid, apPassword);
    drawQr(wifiQr, kLeftX, kQrY, kQrSize);
    drawQr(url, kRightX, kQrY, kQrSize);

    char line[64];
    c.setFont(&fonts::FreeSans12pt7b);
    snprintf(line, sizeof(line), "WiFi: %s", apSsid);
    c.drawString(line, kLeftX, kQrY + kQrSize + 12);
    snprintf(line, sizeof(line), "Password: %s", apPassword);
    c.drawString(line, kLeftX, kQrY + kQrSize + 42);
    c.drawString(url, kRightX, kQrY + kQrSize + 12);
    c.drawString("(opens by itself on most phones)", kRightX, kQrY + kQrSize + 42);

    if (footer != nullptr) {
        c.setFont(&fonts::FreeSansBold9pt7b);
        c.setTextDatum(bottom_center);
        c.drawString(footer, kWidth / 2, kHeight - 10);
    }
    push(true);
    m_renderCount = 0;
}

void EpdRadarView::showSettingsInfo(const char *url, const char *altUrl) {
    M5Canvas &c = m_canvas;
    c.fillScreen(kWhite);
    c.setTextColor(kBlack);
    c.setTextDatum(top_center);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.drawString("Settings", kWidth / 2, 14);

    constexpr int kQrSize = 300;
    drawQr(url, kWidth / 4 + 20, 100, kQrSize);

    constexpr int kTextX = kWidth / 2 + 10;
    c.setTextDatum(top_left);
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.drawString("Scan with a phone on the same WiFi,", kTextX, 130);
    c.drawString("or open in a browser:", kTextX, 160);
    c.setFont(&fonts::FreeSans12pt7b);
    c.drawString(url, kTextX, 210);
    c.drawString(altUrl, kTextX, 245);
    c.setFont(&fonts::FreeSans9pt7b);
    c.drawString("WiFi, location, units, time zone, intervals.", kTextX, 290);
    c.drawString("Saving restarts the radar.", kTextX, 314);

    // Button for when the phone can't reach the page (e.g. moving to a new WiFi).
    c.fillRoundRect(kSetupBtnX, kSetupBtnY, kSetupBtnW, kSetupBtnH, 12, kBlack);
    c.setTextColor(kWhite);
    c.setTextDatum(middle_center);
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.drawString("Start setup WiFi", kSetupBtnX + kSetupBtnW / 2, kSetupBtnY + kSetupBtnH / 2);
    c.setTextColor(kBlack);
    c.setTextDatum(top_left);
    c.setFont(&fonts::FreeSans9pt7b);
    c.drawString("For a new network: the radar makes its own WiFi.", kSetupBtnX, kSetupBtnY + kSetupBtnH + 8);

    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextDatum(bottom_center);
    c.drawString("Tap anywhere else to go back to the radar", kWidth / 2, kHeight - 10);
    push(true);
    m_renderCount = 0;
}

bool EpdRadarView::setupButtonHit(int x, int y) {
    // A little slack around the button for fingers.
    return x >= kSetupBtnX - 10 && x < kSetupBtnX + kSetupBtnW + 10 && y >= kSetupBtnY - 10 &&
           y < kSetupBtnY + kSetupBtnH + 10;
}

void EpdRadarView::render(const RadarModel &model, const Status &status) {
    m_canvas.fillScreen(kWhite);
    drawRadar(model, status.firstCard);
    drawHeader(model, status);
    for (int k = 0; k < RadarModel::kDetailSlots; ++k) {
        drawCard(model, status.firstCard + k, kPanelX, kCardTop + k * (kCardH + kCardGap), kPanelW, kCardH);
    }
    drawScrollbar(model, status.firstCard);
    push(m_renderCount % kFullRefreshEvery == 0);
    ++m_renderCount;
}

void EpdRadarView::addHitTarget(const char *callsign, int x, int y) {
    if (m_hitCount >= AdsbClient::kMaxAircraft || callsign[0] == '\0') {
        return;
    }
    HitTarget &h = m_hits[m_hitCount++];
    strncpy(h.callsign, callsign, sizeof(h.callsign) - 1);
    h.callsign[sizeof(h.callsign) - 1] = '\0';
    h.x = static_cast<int16_t>(x);
    h.y = static_cast<int16_t>(y);
}

const char *EpdRadarView::planeAt(int x, int y) const {
    const HitTarget *best = nullptr;
    int bestDistSq = kHitRadiusPx * kHitRadiusPx;
    for (size_t i = 0; i < m_hitCount; ++i) {
        const int dx = m_hits[i].x - x;
        const int dy = m_hits[i].y - y;
        const int d = dx * dx + dy * dy;
        if (d <= bestDistSq) {
            bestDistSq = d;
            best = &m_hits[i];
        }
    }
    return best != nullptr ? best->callsign : nullptr;
}

void EpdRadarView::drawRadar(const RadarModel &model, size_t firstCard) {
    M5Canvas &c = m_canvas;
    c.setClipRect(0, 0, kPanelX - 4, kHeight);
    m_hitCount = 0;

    // Grid: rings (3rd = labelled range, 4th = outer edge), crosshair, centre dot
    for (int i = 1; i <= kRingCount; ++i) {
        const int r = kOuterR * i / kRingCount;
        const uint16_t col = (i >= 3) ? kBlack : kLight;
        c.drawCircle(kCx, kCy, r, col);
        c.drawCircle(kCx, kCy, r - 1, col);
    }
    c.drawFastVLine(kCx, kCy - kOuterR, 2 * kOuterR, kLight);
    c.drawFastHLine(kCx - kOuterR, kCy, 2 * kOuterR, kLight);
    c.fillCircle(kCx, kCy, 4, kBlack);

    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextColor(kBlack);
    c.setTextDatum(top_center);
    c.drawString("N", kCx, kCy - kOuterR + 6);
    c.setTextDatum(bottom_center);
    c.drawString("S", kCx, kCy + kOuterR - 4);
    c.setTextDatum(middle_left);
    c.drawString("W", kCx - kOuterR + 8, kCy);
    c.setTextDatum(middle_right);
    c.drawString("E", kCx + kOuterR - 8, kCy);

    // Ring distance labels along the north-east diagonal
    for (int i = 1; i <= kRingCount; ++i) {
        const int r = kOuterR * i / kRingCount;
        char buf[16];
        model.formatDistance(model.outerKm() * i / kRingCount, buf, sizeof(buf));
        const int lx = kCx + static_cast<int>(r * 0.7071f) + 4;
        const int ly = kCy - static_cast<int>(r * 0.7071f) - 2;
        c.setFont(i == 3 ? &fonts::FreeSansBold9pt7b : &fonts::FreeSans9pt7b);
        c.setTextDatum(bottom_left);
        c.setTextColor(i == 3 ? kBlack : kDark);
        c.drawString(buf, lx, ly);
    }
    c.setTextColor(kBlack);

    // Aircraft
    const float outerKm = model.outerKm();
    const float pxPerKm = kOuterR / outerKm;
    const float insideKm = outerKm * static_cast<float>(kOuterR - kInsideInset) / kOuterR;
    const AdsbClient::Aircraft *planes = AdsbClient::aircraftList();
    const size_t n = AdsbClient::aircraftCount();

    PlacedPlane placed[AdsbClient::kMaxAircraft];
    size_t placedCount = 0;

    for (size_t i = 0; i < n; ++i) {
        float dx = 0.0f;
        float dy = 0.0f;
        model.offsetKm(planes[i].lat, planes[i].lon, &dx, &dy);
        const float dist = sqrtf(dx * dx + dy * dy);
        if (dist <= insideKm) {
            const int x = kCx + static_cast<int>(lroundf(dx * pxPerKm));
            const int y = kCy - static_cast<int>(lroundf(dy * pxPerKm));
            placed[placedCount++] = {i, x, y, (x - kCx) * (x - kCx) + (y - kCy) * (y - kCy)};
            addHitTarget(planes[i].callsign, x, y);
        } else if (dist > 0.01f) {
            // Beyond range: hollow dot on the rim in the plane's direction.
            const float ang = atan2f(dx, dy);
            const int x = kCx + static_cast<int>(lroundf(sinf(ang) * kRimR));
            const int y = kCy - static_cast<int>(lroundf(cosf(ang) * kRimR));
            c.fillCircle(x, y, kRimDotR, kWhite);
            c.drawCircle(x, y, kRimDotR, kBlack);
            c.drawCircle(x, y, kRimDotR - 1, kBlack);
            if (model.isPinned(planes[i].callsign)) {
                for (int r = kRimDotR + 4; r <= kRimDotR + 7; ++r) {
                    c.drawCircle(x, y, r, kBlack);
                }
            }
            addHitTarget(planes[i].callsign, x, y);
            const int pos = visibleCardPos(model, i, firstCard);
            if (pos >= 0) {
                const int bx = kCx + static_cast<int>(lroundf(sinf(ang) * (kRimR - 22)));
                const int by = kCy - static_cast<int>(lroundf(cosf(ang) * (kRimR - 22)));
                drawBadge(c, bx, by, 11, pos + 1, &fonts::FreeSansBold9pt7b);
            }
        }
    }

    // Far planes first so closer ones end up on top.
    for (size_t a = 1; a < placedCount; ++a) {
        const PlacedPlane key = placed[a];
        size_t b = a;
        while (b > 0 && placed[b - 1].distSq < key.distSq) {
            placed[b] = placed[b - 1];
            --b;
        }
        placed[b] = key;
    }

    for (size_t k = 0; k < placedCount; ++k) {
        const AdsbClient::Aircraft &p = planes[placed[k].index];
        drawSpeedVector(c, placed[k].x, placed[k].y, p.noseDeg, p.trackDeg, p.gsKnots);
    }
    for (size_t k = 0; k < placedCount; ++k) {
        const AdsbClient::Aircraft &p = planes[placed[k].index];
        const bool greyed = (p.dbFlags & (kFlagLadd | kFlagPia)) != 0;
        if (p.dbFlags & (kFlagMilitary | kFlagInteresting)) {
            c.drawCircle(placed[k].x, placed[k].y, kNoseLen + 6, kBlack);
            c.drawCircle(placed[k].x, placed[k].y, kNoseLen + 5, kBlack);
        }
        if (model.isPinned(p.callsign)) {
            // Thick ring so the pinned plane stands out.
            for (int r = kNoseLen + 10; r <= kNoseLen + 14; ++r) {
                c.drawCircle(placed[k].x, placed[k].y, r, kBlack);
            }
        }
        drawHeadingTriangle(c, placed[k].x, placed[k].y, p.noseDeg, greyed ? kLight : kBlack);
    }

    // Labels last; planes whose card is on screen get a numbered badge matching
    // the card and are drawn in a second pass so they sit on top.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t k = 0; k < placedCount; ++k) {
            const AdsbClient::Aircraft &p = planes[placed[k].index];
            const int pos = visibleCardPos(model, placed[k].index, firstCard);
            if ((pos >= 0) != (pass == 1)) {
                continue;
            }
            const int x = placed[k].x;
            const int y = placed[k].y;
            if (pos >= 0) {
                drawBadge(c, x + 24, y + 18, 11, pos + 1, &fonts::FreeSansBold9pt7b);
                drawLabel(c, p.callsign, x + (pos >= 9 ? 42 : 38), y + 9);
            } else {
                drawLabel(c, p.callsign, x + 14, y + 8);
            }
        }
    }

    c.clearClipRect();
}

void EpdRadarView::drawHeader(const RadarModel &model, const Status &status) {
    M5Canvas &c = m_canvas;
    const time_t lastUpdate = status.lastUpdate;
    const bool stale = status.stale;
    c.setTextColor(kBlack);
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(top_left);
    c.drawString("Plane Radar", kPanelX, 8);

    char buf[48];
    // "14 aircraft", or "5-8 of 14 aircraft" once the list is longer than the cards
    const size_t total = model.listCount();
    if (total > static_cast<size_t>(RadarModel::kDetailSlots)) {
        const size_t last = std::min(total, status.firstCard + RadarModel::kDetailSlots);
        snprintf(buf, sizeof(buf), "%u-%u of %u aircraft", static_cast<unsigned>(status.firstCard + 1),
                 static_cast<unsigned>(last), static_cast<unsigned>(total));
    } else {
        snprintf(buf, sizeof(buf), "%u aircraft", static_cast<unsigned>(total));
    }
    c.setFont(&fonts::FreeSansBold12pt7b);
    c.setTextDatum(top_right);
    c.drawString(buf, kPanelX + kPanelW, 8);

    // Status line, e.g. "updated 14:32  next 14:37  78%"
    const auto hhmm = [](time_t t, char *out, size_t len) {
        struct tm tmv;
        if (t > 100000 && localtime_r(&t, &tmv) != nullptr) {
            strftime(out, len, "%H:%M", &tmv);
            return true;
        }
        return false;
    };
    char line[64];
    char tbuf[8];
    if (hhmm(lastUpdate, tbuf, sizeof(tbuf))) {
        snprintf(line, sizeof(line), "updated %s%s", tbuf, stale ? " - STALE" : "");
    } else {
        snprintf(line, sizeof(line), "%s", stale ? "no data - STALE" : "live");
    }
    if (status.batteryMode && hhmm(status.nextUpdate, tbuf, sizeof(tbuf))) {
        snprintf(buf, sizeof(buf), "  next %s", tbuf);
        strncat(line, buf, sizeof(line) - strlen(line) - 1);
    }
    if (status.batteryPercent >= 0) {
        snprintf(buf, sizeof(buf), "  %d%%", status.batteryPercent);
        strncat(line, buf, sizeof(line) - strlen(line) - 1);
    }
    // Bold and full black: the thin grey font was hard to read on e-ink.
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextColor(kBlack);
    c.drawString(line, kPanelX + kPanelW, 36);

    if (!status.batteryMode) {
        // Shows that tapping the header opens the settings screen.
        drawGear(c, kPanelX + 12, 46);
        c.setFont(&fonts::FreeSansBold9pt7b);
        c.setTextDatum(middle_left);
        c.drawString("Settings", kPanelX + 30, 46);
        c.setTextDatum(top_right);
    }
    if (status.batteryMode) {
        // Inverted two-line tag in the radar's empty top-left corner (outside the
        // rim) so it's obvious the screen isn't live, without covering the header.
        c.setFont(&fonts::FreeSansBold9pt7b);
        const int th = c.fontHeight();
        const int tw = c.textWidth("BATTERY") + 12;
        constexpr int kTagX = 6;
        constexpr int kTagY = 6;
        c.fillRoundRect(kTagX, kTagY, tw, 2 * th + 8, 6, kBlack);
        c.setTextColor(kWhite);
        c.setTextDatum(top_center);
        c.drawString("BATTERY", kTagX + tw / 2, kTagY + 4);
        c.drawString("MODE", kTagX + tw / 2, kTagY + 4 + th);
        c.setTextColor(kBlack);
    }

    c.drawFastHLine(kPanelX, kCardTop - 6, kPanelW, kBlack);
}

void EpdRadarView::drawScrollbar(const RadarModel &model, size_t firstCard) {
    const size_t total = model.listCount();
    if (total <= static_cast<size_t>(RadarModel::kDetailSlots)) {
        return;
    }
    // Thin track in the 8 px margin right of the cards, thumb sized to the
    // visible share of the list.
    const int trackX = kPanelX + kPanelW + 3;
    const int trackY = kCardTop;
    const int trackH = kHeight - kCardTop - 6;
    const int thumbH = std::max(24, static_cast<int>(trackH * RadarModel::kDetailSlots / total));
    const size_t maxFirst = total - RadarModel::kDetailSlots;
    const int thumbY = trackY + static_cast<int>((trackH - thumbH) * std::min(firstCard, maxFirst) / maxFirst);
    m_canvas.fillRect(trackX, trackY, 3, trackH, kLight);
    m_canvas.fillRect(trackX - 1, thumbY, 5, thumbH, kBlack);
}

void EpdRadarView::drawCard(const RadarModel &model, size_t pos, int x, int y, int w, int h) {
    M5Canvas &c = m_canvas;
    const AdsbClient::Aircraft *p = model.listAircraft(pos);

    if (p == nullptr) {
        c.drawRect(x, y, w, h, kLight);
        c.setFont(&fonts::FreeSans12pt7b);
        c.setTextColor(kDark);
        c.setTextDatum(middle_center);
        c.drawString("No aircraft", x + w / 2, y + h / 2);
        c.setTextColor(kBlack);
        return;
    }

    const bool special = isSpecial(p->dbFlags);
    for (int i = 0; i < (special ? 3 : 1); ++i) {
        c.drawRect(x + i, y + i, w - 2 * i, h - 2 * i, kBlack);
    }

    // A pinned card gets an inverted (black) header bar.
    const bool pinned = pos == 0 && model.isPinned(p->callsign);
    const uint16_t fg = pinned ? kWhite : kBlack;
    if (pinned) {
        c.fillRect(x, y, w, 44, kBlack);
    }

    drawBadge(c, x + 24, y + 22, 15, static_cast<int>(pos) + 1, &fonts::FreeSansBold12pt7b, pinned);

    c.setTextColor(fg);
    c.setTextDatum(top_left);
    c.setFont(&fonts::FreeSansBold18pt7b);
    c.drawString(p->callsign[0] != '\0' ? p->callsign : "Unknown", x + 48, y + 6);

    char buf[96];
    model.formatDistance(model.listDistanceKm(pos), buf, sizeof(buf));
    c.setFont(&fonts::FreeSans12pt7b);
    c.setTextDatum(top_right);
    c.drawString(buf, x + w - 12, y + 10);

    // Pinned tag, left of the distance
    if (pinned) {
        const int distW = c.textWidth(buf);
        c.setFont(&fonts::FreeSansBold9pt7b);
        c.setTextDatum(top_right);
        c.drawString("PINNED", x + w - 12 - distW - 14, y + 14);
    }
    c.setTextColor(kBlack);

    // Aircraft description (falls back to the ICAO type code)
    c.setTextDatum(top_left);
    // Bold and full black: the thin grey font was hard to read on e-ink.
    static const lgfx::IFont *const kDescFonts[] = {&fonts::FreeSansBold9pt7b};
    foldToAscii(p->desc[0] != '\0' ? p->desc : (p->type[0] != '\0' ? p->type : "Unknown type"), buf, sizeof(buf));
    drawFitted(c, buf, x + 12, y + 46, w - 24, kDescFonts, 1);

    // Route
    char route[64];
    static const lgfx::IFont *const kRouteFonts[] = {&fonts::FreeSans12pt7b, &fonts::FreeSans9pt7b};
    if (RouteClient::formatRoute(p->callsign, route, sizeof(route))) {
        foldToAscii(route, buf, sizeof(buf));
        drawFitted(c, buf, x + 12, y + 66, w - 24, kRouteFonts, 2);
    } else {
        // Same font and colour as a known route: light grey was hard to read.
        drawFitted(c, "route unknown", x + 12, y + 66, w - 24, kRouteFonts, 2);
    }

    // Bottom line: speed and altitude, then a flag tag or arriving/departing hint
    // AdsbClient formats altitude as "<n> ft" (or "GND"); convert it for metric.
    char alt[16];
    const size_t altLen = strlen(p->alt);
    if (model.metric() && altLen > 3 && strcmp(p->alt + altLen - 3, " ft") == 0) {
        snprintf(alt, sizeof(alt), "%ld m", lroundf(atoi(p->alt) * kMPerFt));
    } else {
        snprintf(alt, sizeof(alt), "%s", altLen > 0 ? p->alt : "-");
    }
    if (model.metric()) {
        snprintf(buf, sizeof(buf), "%d km/h    %s", static_cast<int>(lroundf(p->gsKnots * kKtToKmh)), alt);
    } else {
        snprintf(buf, sizeof(buf), "%d mph    %s", static_cast<int>(lroundf(p->gsKnots * kKtToMph)), alt);
    }
    c.setFont(&fonts::FreeSansBold9pt7b);
    c.setTextDatum(top_left);
    c.drawString(buf, x + 12, y + 92);

    const char *tag = flagsLabel(p->dbFlags);
    if (tag != nullptr) {
        const int tw = c.textWidth(tag) + 12;
        const int th = c.fontHeight() + 2;
        c.fillRect(x + w - 12 - tw, y + h - 8 - th, tw, th, kBlack);
        c.setTextColor(kWhite);
        c.setTextDatum(middle_center);
        c.drawString(tag, x + w - 12 - tw / 2, y + h - 8 - th / 2);
        c.setTextColor(kBlack);
    } else if (const char *mv = movementLabel(*p)) {
        c.setTextDatum(top_right);
        c.setTextColor(kDark);
        c.drawString(mv, x + w - 12, y + 92);
        c.setTextColor(kBlack);
    }
}
