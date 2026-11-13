// AGPL-3.0. See ConfigPortal.h.
#include "ConfigPortal.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

namespace ConfigPortal {
namespace {

constexpr int kMaxNetworks = 15;

struct TzOption {
    const char *label;
    const char *posix;
};

constexpr TzOption kZones[] = {
    {"Central Europe (Brussels, Paris, Berlin)", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"UK, Ireland, Portugal", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Eastern Europe (Helsinki, Athens)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Moscow, Istanbul", "MSK-3"},
    {"UTC", "UTC0"},
    {"US Eastern", "EST5EDT,M3.2.0,M11.1.0"},
    {"US Central", "CST6CDT,M3.2.0,M11.1.0"},
    {"US Mountain", "MST7MDT,M3.2.0,M11.1.0"},
    {"Arizona", "MST7"},
    {"US Pacific", "PST8PDT,M3.2.0,M11.1.0"},
    {"Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Hawaii", "HST10"},
    {"India", "IST-5:30"},
    {"China, Singapore, Perth", "CST-8"},
    {"Japan, Korea", "JST-9"},
    {"Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
    {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
};

WebServer g_server(80);
DNSServer g_dns;
Settings *g_settings = nullptr;
bool g_apMode = false;
bool g_saved = false;
String g_networks[kMaxNetworks];
int g_networkCount = 0;

String esc(const char *s) {
    String out;
    for (; *s != '\0'; ++s) {
        switch (*s) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += *s;
        }
    }
    return out;
}

void scanNetworks() {
    g_networkCount = 0;
    const int n = WiFi.scanNetworks();
    for (int i = 0; i < n && g_networkCount < kMaxNetworks; ++i) {
        const String ssid = WiFi.SSID(i);
        bool dup = ssid.isEmpty();
        for (int j = 0; j < g_networkCount && !dup; ++j) {
            dup = g_networks[j] == ssid;
        }
        if (!dup) {
            g_networks[g_networkCount++] = ssid;
        }
    }
    WiFi.scanDelete();
    Serial.printf("portal: %d networks found\n", g_networkCount);
}

const char kStyle[] PROGMEM = R"CSS(
body{font-family:system-ui,sans-serif;margin:0;padding:16px;background:#f4f4f4;color:#111}
main{max-width:520px;margin:auto}
h1{font-size:1.5em;margin:.2em 0 .6em}
fieldset{border:1px solid #ccc;border-radius:10px;background:#fff;margin:0 0 14px;padding:10px 14px 14px}
legend{font-weight:700;padding:0 6px}
label{display:block;margin:10px 0 4px;font-weight:600}
input,select,button{font-size:16px;width:100%;box-sizing:border-box;padding:10px;border:1px solid #999;border-radius:8px;background:#fff}
button{background:#111;color:#fff;border:0;font-weight:700;margin-top:6px}
button.alt{background:#e6e6e6;color:#111}
.row{display:flex;gap:8px}.row>*{flex:1}
small{color:#555;display:block;margin-top:4px}
.err{background:#fdd;border:1px solid #c00;padding:10px;border-radius:8px}
.ok{background:#dfd;border:1px solid #080;padding:10px;border-radius:8px}
)CSS";

String pageStart(const char *title) {
    String h;
    h.reserve(9000);
    h += F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content='width=device-width,initial-scale=1'><title>");
    h += title;
    h += F("</title><style>");
    h += FPSTR(kStyle);
    h += F("</style></head><body><main>");
    return h;
}

void sendForm(const char *error, int code) {
    const Settings &s = *g_settings;
    String h = pageStart("Plane Radar settings");
    h += F("<h1>Plane Radar settings</h1>");
    if (error != nullptr) {
        h += F("<p class=err>");
        h += esc(error);
        h += F("</p>");
    }
    h += F("<form method=post action=/save>");

    // WiFi
    h += F("<fieldset><legend>WiFi</legend><label for=ssid>Network</label>"
           "<input id=ssid name=ssid list=nets autocomplete=off autocapitalize=none required maxlength=32 value=\"");
    h += esc(s.ssid);
    h += F("\"><datalist id=nets>");
    for (int i = 0; i < g_networkCount; ++i) {
        h += F("<option value=\"");
        h += esc(g_networks[i].c_str());
        h += F("\">");
    }
    h += F("</datalist><small>Pick a network found nearby or type its name.</small>"
           "<label for=pass>Password</label><input id=pass name=pass type=password maxlength=64 "
           "autocomplete=off autocapitalize=none placeholder=\"");
    h += s.password[0] != '\0' ? F("(unchanged)") : F("(none)");
    h += F("\"><small><input type=checkbox style='width:auto' onclick=\"pass.type=this.checked?'text':'password'\"> "
           "Show password. Leave empty to keep the saved one.</small></fieldset>");

    // Location
    char loc[48];
    snprintf(loc, sizeof(loc), "%.5f, %.5f", s.lat, s.lon);
    h += F("<fieldset><legend>Radar centre</legend><label for=loc>Latitude, longitude</label>"
           "<input id=loc name=loc required inputmode=decimal value=\"");
    h += loc;
    h += F("\"><small>In Google Maps, long-press your spot and copy the numbers shown, "
           "e.g. <i>51.29033, 4.56542</i>.</small><div class=row>");
    if (!g_apMode) {
        h += F("<button type=button class=alt onclick=geo()>Guess from internet</button>");
    }
    h += F("<button type=button class=alt onclick=\"open('https://www.google.com/maps?q='+"
           "encodeURIComponent(loc.value))\">Show on map</button></div></fieldset>");

    // Display
    h += F("<fieldset><legend>Display</legend><label for=units>Units</label><select id=units name=units>"
           "<option value=imperial");
    h += s.metric ? "" : " selected";
    h += F(">Miles, mph, feet</option><option value=metric");
    h += s.metric ? " selected" : "";
    h += F(">Kilometres, km/h, metres</option></select><label for=tz>Time zone</label><select id=tz name=tz>");
    bool known = false;
    for (const auto &z : kZones) {
        const bool sel = strcmp(z.posix, s.tz) == 0;
        known = known || sel;
        h += F("<option value=\"");
        h += z.posix;
        h += '"';
        h += sel ? " selected>" : ">";
        h += z.label;
        h += F("</option>");
    }
    h += F("<option value=custom");
    h += known ? ">" : " selected>";
    h += F("Other (enter below)</option></select><input name=tzc placeholder='POSIX TZ string' value=\"");
    h += known ? String() : esc(s.tz);
    h += F("\"><small>Used for the \"updated HH:MM\" time.</small></fieldset>");

    // Timing
    h += F("<fieldset><legend>Updates</legend><div class=row><div><label for=fetch>Live: every (s)</label>"
           "<input id=fetch name=fetch type=number min=");
    h += Settings::kMinFetchSec;
    h += F(" max=");
    h += Settings::kMaxFetchSec;
    h += F(" value=");
    h += s.fetchSec;
    h += F("></div><div><label for=batt>Battery: every (min)</label><input id=batt name=batt type=number min=");
    h += Settings::kMinBatterySec / 60;
    h += F(" max=");
    h += Settings::kMaxBatterySec / 60;
    h += F(" value=");
    h += s.batterySec / 60;
    h += F("></div></div><small>10-15 s suits e-ink in live mode. Longer battery intervals last longer.</small>"
           "</fieldset><button>Save and restart</button></form>");

    h += F("<script>function geo(){fetch('/geoip').then(r=>r.json()).then(j=>{if(j.status=='success')"
           "{loc.value=j.lat.toFixed(5)+', '+j.lon.toFixed(5);alert('Near '+j.city+'. Check it on the map, "
           "this guess can be kilometres off.')}else alert('Could not guess the location.')})"
           ".catch(()=>alert('Could not guess the location.'))}</script></main></body></html>");
    g_server.send(code, "text/html", h);
}

void handleSave() {
    Settings s = *g_settings;
    const String ssid = g_server.arg("ssid");
    const String pass = g_server.arg("pass");
    if (ssid.isEmpty() || ssid.length() >= sizeof(s.ssid)) {
        sendForm("Enter a WiFi network name (up to 32 characters).", 400);
        return;
    }
    if (pass.length() >= sizeof(s.password) || (!pass.isEmpty() && pass.length() < 8)) {
        sendForm("A WiFi password has 8 to 64 characters.", 400);
        return;
    }
    if (!parseLatLon(g_server.arg("loc").c_str(), &s.lat, &s.lon)) {
        sendForm("Enter the location as latitude, longitude, e.g. 51.29033, 4.56542.", 400);
        return;
    }
    String tz = g_server.arg("tz");
    if (tz == "custom") {
        tz = g_server.arg("tzc");
        tz.trim();
    }
    if (tz.isEmpty() || tz.length() >= sizeof(s.tz)) {
        sendForm("Choose a time zone, or enter a POSIX TZ string such as CET-1CEST,M3.5.0,M10.5.0/3.", 400);
        return;
    }

    // An empty password keeps the saved one, unless the network changed.
    const bool sameNetwork = ssid == g_settings->ssid;
    strncpy(s.ssid, ssid.c_str(), sizeof(s.ssid) - 1);
    s.ssid[sizeof(s.ssid) - 1] = '\0';
    if (!pass.isEmpty() || !sameNetwork) {
        strncpy(s.password, pass.c_str(), sizeof(s.password) - 1);
        s.password[sizeof(s.password) - 1] = '\0';
    }
    s.metric = g_server.arg("units") == "metric";
    strncpy(s.tz, tz.c_str(), sizeof(s.tz) - 1);
    s.tz[sizeof(s.tz) - 1] = '\0';
    s.fetchSec = static_cast<uint16_t>(g_server.arg("fetch").toInt());
    s.batterySec = static_cast<uint16_t>(g_server.arg("batt").toInt() * 60);
    s.sanitize();
    s.save();
    *g_settings = s;
    Serial.printf("portal: saved (wifi %s, %.5f, %.5f, %s, every %u s, battery %u s)\n", s.ssid, s.lat, s.lon,
                  s.metric ? "metric" : "imperial", s.fetchSec, s.batterySec);

    String h = pageStart("Saved");
    h += F("<h1>Saved</h1><p class=ok>The radar restarts now and connects to <b>");
    h += esc(s.ssid);
    h += F("</b>.</p><p>If it can't connect, it shows the setup screen again after about a minute.</p>");
    if (g_apMode) {
        h += F("<p>Your phone leaves the setup WiFi by itself; you can close this page.</p>");
    }
    h += F("</main></body></html>");
    g_server.send(200, "text/html", h);
    g_saved = true;
}

// Device-side IP geolocation (http only; ip-api.com needs no key).
void handleGeoIp() {
    HTTPClient http;
    http.setTimeout(5000);
    if (http.begin("http://ip-api.com/json/?fields=status,lat,lon,city") && http.GET() == 200) {
        g_server.send(200, "application/json", http.getString());
    } else {
        g_server.send(502, "application/json", "{\"status\":\"fail\"}");
    }
    http.end();
}

// Captive portal: every unknown URL (phone connectivity checks included)
// is redirected to the form, which makes phones pop up the sign-in page.
void handleNotFound() {
    if (g_apMode) {
        g_server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
        g_server.send(302, "text/plain", "");
    } else {
        g_server.send(404, "text/plain", "Not found");
    }
}

} // namespace

void begin(Settings *settings, bool apMode) {
    g_settings = settings;
    g_apMode = apMode;
    g_saved = false;
    scanNetworks();

    if (apMode) {
        g_dns.setErrorReplyCode(DNSReplyCode::NoError);
        g_dns.start(53, "*", WiFi.softAPIP());
    } else if (MDNS.begin(kHostname)) {
        MDNS.addService("http", "tcp", 80);
    }
    g_server.on("/", HTTP_GET, [] { sendForm(nullptr, 200); });
    g_server.on("/save", HTTP_POST, handleSave);
    g_server.on("/geoip", HTTP_GET, handleGeoIp);
    g_server.onNotFound(handleNotFound);
    g_server.begin();
    Serial.printf("portal: serving on %s\n", (apMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
}

void handle() {
    if (g_apMode) {
        g_dns.processNextRequest();
    }
    g_server.handleClient();
}

bool saved() {
    return g_saved;
}

} // namespace ConfigPortal
