// Ported from the info-orbs Plane Radar widget (PR #329, havedill/info-orbs_public,
// branch plane-radar-pr). AGPL-3.0. Changes: optional rate-limit bypass, larger cache.

#ifndef ROUTE_CLIENT_H
#define ROUTE_CLIENT_H

#include <cstddef>

class RouteClient {
public:
    static void clearCache();
    static bool isCached(const char *callsign);
    static bool formatRoute(const char *callsign, char *out, size_t outLen);
    // ignoreRateLimit skips the 4 s spacing between lookups (battery mode does
    // its few lookups back to back so it can power off sooner).
    static bool fetchIfUncached(const char *callsign, bool ignoreRateLimit = false);
    static bool prefetchOne(const char *const *callsigns, size_t count, size_t *cursor);
};

#endif
