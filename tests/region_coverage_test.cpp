#include "plugins/WorldCompletion/RegionCoveragePlanner.h"
#include <cassert>
#include <iostream>

struct Stand {
    struct Position { float x, y; uint32_t zplane; } pos;
    int cell_x, cell_y;
    std::vector<uint32_t> reveals;
};

int main()
{
    std::vector<Stand> stops;
    std::vector<completion::CoverageGridCell> grid;
    // A hole splits the scan into separate intervals, then joins them again.
    for (int y = 0; y < 7; ++y) for (int x = 0; x < 7; ++x) {
        if (x == 3 && y > 0 && y < 6) continue;
        grid.push_back({x, y, 0});
        Stand stop{{static_cast<float>(x), static_cast<float>(y), 0}, x, y, {}};
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx)
            if (x + dx >= 0 && x + dx < 7 && y + dy >= 0 && y + dy < 7)
                stop.reveals.push_back(static_cast<uint32_t>((y + dy) * 32 + x + dx));
        stops.push_back(std::move(stop));
    }
    const Stand start{{0, 0, 0}, 0, 0, {}}, finish{{6, 6, 0}, 6, 6, {}};
    const auto metric = [](const Stand& a, const Stand& b) { return std::hypot(a.pos.x - b.pos.x, a.pos.y - b.pos.y); };
    const auto result = completion::PlanRegionCoverage(stops, grid, 1, 32, start, &finish,
        metric, [] {}, [] { return true; });
    assert(!result.order.empty() && result.regions > 1 && result.patterns >= result.regions);
    std::unordered_set<uint32_t> required, covered;
    for (const auto& stop : stops) required.insert(stop.reveals.begin(), stop.reveals.end());
    float measured = 0;
    Stand previous = start;
    for (const auto i : result.order) {
        assert(i < stops.size());
        covered.insert(stops[i].reveals.begin(), stops[i].reveals.end());
        measured += metric(previous, stops[i]); previous = stops[i];
    }
    measured += metric(previous, finish);
    assert(required == covered && std::fabs(measured - result.estimated_length) < .001f);
    const auto bounded = completion::PlanRegionCoverage(stops, grid, 1, 32, start, &finish,
        metric, [] {}, [] { return false; });
    assert(bounded.order.empty());
    for (const size_t limit : {size_t{80}, size_t{300}, size_t{1000}}) {
        size_t checks = 0;
        const auto interrupted = completion::PlanRegionCoverage(stops, grid, 1, 32, start, &finish,
            metric, [] {}, [&] { return ++checks <= limit; });
        if (interrupted.order.empty()) continue;
        std::unordered_set<uint32_t> retained;
        for (const auto i : interrupted.order) retained.insert(stops[i].reveals.begin(), stops[i].reveals.end());
        assert(retained == required); // Never return an unfinished region cover.
    }
    {
        const std::vector<Stand> floors{{{0, 0, 0}, 0, 0, {0}}, {{0, 0, 1}, 0, 0, {100}}};
        const std::vector<completion::CoverageGridCell> cells{{0, 0, 0}, {0, 0, 1}};
        const auto separate = completion::PlanRegionCoverage(floors, cells, 1, 32, start, &finish,
            metric, [] {}, [] { return true; });
        assert(separate.regions == 2 && separate.order.size() == 2);
    }
    bool cancelled = false;
    try { completion::PlanRegionCoverage(stops, grid, 1, 32, start, &finish,
        metric, [] { throw 1; }, [] { return true; }); } catch (int) { cancelled = true; }
    assert(cancelled);
    std::cout << "Region coverage: obstacle splits, sweep choice, coverage, terminal costs, budget, cancellation passed\n";
}
