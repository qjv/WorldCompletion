#pragma once

#include "RoutePlanner.h"
#include <array>

namespace completion {
using WalkPolygon = std::array<Point, 4>;

// Find the continuous walkable cross-section through a portal. Testing several
// directions finds the corridor width rather than cutting along its length.
template<class Check>
std::optional<Portal> TracePortalBarrier(Point origin, const std::vector<WalkPolygon>& polygons, Check check)
{
    std::optional<Portal> best;
    double best_width = 12000.;
    for (int direction = 0; direction < 32; ++direction) {
        check();
        const double angle = direction * 3.14159265358979323846 / 32.;
        const double dx = std::cos(angle), dy = std::sin(angle);
        std::vector<std::pair<double, double>> intervals;
        for (const auto& poly : polygons) {
            check();
            double area = 0.;
            for (size_t i = 0; i < 4; ++i) {
                const auto a = poly[i], b = poly[(i + 1) % 4];
                area += (a.x - origin.x) * (b.y - origin.y) - (a.y - origin.y) * (b.x - origin.x);
            }
            if (std::abs(area) < 1e-6) continue;
            const double sign = area > 0. ? 1. : -1.;
            double lo = -1e30, hi = 1e30;
            bool valid = true;
            for (size_t i = 0; i < 4; ++i) {
                const auto a = poly[i], b = poly[(i + 1) % 4];
                const double ex = b.x - a.x, ey = b.y - a.y;
                const double constant = sign * (ex * (origin.y - a.y) - ey * (origin.x - a.x));
                const double slope = sign * (ex * dy - ey * dx);
                if (std::abs(slope) < 1e-9) { if (constant < -1e-5) valid = false; }
                else if (slope > 0.) lo = std::max(lo, -constant / slope);
                else hi = std::min(hi, -constant / slope);
            }
            if (valid && hi > lo) intervals.emplace_back(lo, hi);
        }
        std::sort(intervals.begin(), intervals.end());
        for (size_t i = 0; i < intervals.size();) {
            const double lo = intervals[i].first;
            double hi = intervals[i++].second;
            while (i < intervals.size() && intervals[i].first <= hi + .1)
                hi = std::max(hi, intervals[i++].second);
            // Require walls on both sides and reject a tangent at the mesh edge.
            if (lo > -64. || hi < 64. || -lo > 6000. || hi > 6000. || hi - lo >= best_width) continue;
            best_width = hi - lo;
            best = Portal{{static_cast<float>(origin.x + dx * (lo - 48.)), static_cast<float>(origin.y + dy * (lo - 48.))},
                          {static_cast<float>(origin.x + dx * (hi + 48.)), static_cast<float>(origin.y + dy * (hi + 48.))}};
        }
    }
    return best;
}

inline float BarrierSegmentDistanceSq(Point a, Point b, Point c, Point d)
{
    const auto cross = [](Point u, Point v, Point w) {
        return static_cast<double>(v.x - u.x) * (w.y - u.y) - static_cast<double>(v.y - u.y) * (w.x - u.x);
    };
    const double ab_c = cross(a, b, c), ab_d = cross(a, b, d), cd_a = cross(c, d, a), cd_b = cross(c, d, b);
    if (((ab_c > 0. && ab_d < 0.) || (ab_c < 0. && ab_d > 0.)) &&
        ((cd_a > 0. && cd_b < 0.) || (cd_a < 0. && cd_b > 0.))) return 0.f;
    const auto distance = [](Point p, Point u, Point v) {
        const float dx = v.x - u.x, dy = v.y - u.y, length = dx * dx + dy * dy;
        const float t = length ? std::clamp(((p.x - u.x) * dx + (p.y - u.y) * dy) / length, 0.f, 1.f) : 0.f;
        const float x = u.x + t * dx - p.x, y = u.y + t * dy - p.y;
        return x * x + y * y;
    };
    return std::min({distance(a, c, d), distance(b, c, d), distance(c, a, b), distance(d, a, b)});
}
}
