#include "plugins/WorldCompletion/PortalBarrier.h"
#include <cassert>
#include <iostream>

int main()
{
    using namespace completion;
    const auto check = [] {};
    std::vector<WalkPolygon> corridor{
        WalkPolygon{Point{0, 1000}, {5000, 1000}, {5000, 0}, {0, 0}},
        WalkPolygon{Point{5000, 1000}, {10000, 1000}, {10000, 0}, {5000, 0}}
    };
    const auto barrier = TracePortalBarrier({9000, 500}, corridor, check);
    assert(barrier);
    assert(std::abs(Distance(barrier->first, barrier->second) - 1096.f) < 1.f);
    assert(BarrierSegmentDistanceSq({8000, 500}, {9500, 500}, barrier->first, barrier->second) == 0.f);
    assert(BarrierSegmentDistanceSq({8800, 100}, {8800, 900}, barrier->first, barrier->second) > 48.f * 48.f);
    assert(BarrierSegmentDistanceSq({9000, 500}, {9000, 500}, barrier->first, barrier->second) == 0.f);
    const auto rotate = [](Point p) { return Point{.70710678f * (p.x - p.y), .70710678f * (p.x + p.y)}; };
    for (auto& poly : corridor) for (auto& p : poly) p = rotate(p);
    const auto diagonal = TracePortalBarrier(rotate({9000, 500}), corridor, check);
    assert(diagonal);
    assert(std::abs(Distance(diagonal->first, diagonal->second) - 1096.f) < 1.f);
    assert(BarrierSegmentDistanceSq(rotate({8000, 500}), rotate({9500, 500}), diagonal->first, diagonal->second) == 0.f);
    std::vector<WalkPolygon> open{WalkPolygon{Point{-20000, 20000}, {20000, 20000}, {20000, -20000}, {-20000, -20000}}};
    assert(!TracePortalBarrier({0, 0}, open, check));
    assert(!TracePortalBarrier({0, 0}, {}, check));
    std::cout << "Portal barriers: corridor walls, adjacent polygons, diagonal passage, crossing, parallel travel, fallback passed\n";
}
