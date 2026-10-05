#include "plugins/WorldCompletion/CoveringTourSearch.h"
#include <cassert>
#include <iostream>
struct Stand {
    completion::Point pos;
    std::vector<uint32_t> reveals;
    bool preserved = false;
};
int main()
{
    const Stand start{{0, 0}, {}}, finish{{10, 0}, {}};
    const auto metric = [](const Stand& a, const Stand& b) { return completion::Distance(a.pos, b.pos); };
    const std::vector<Stand> old{{{0, 10}, {1}}, {{10, 10}, {2}}};
    auto pool = old;
    pool.push_back({{5, 0}, {1, 2}});
    const auto place = [](const Stand& stop, const Stand&, const Stand*) { return std::vector<Stand>{stop}; };
    completion::CoverageStats stats;
    const auto result = completion::ImproveCoveringTour(old, pool, start, &finish, 558, metric,
        [] {}, [] { return true; }, stats, place);
    assert(result.size() == 1 && result[0].reveals.size() == 2 && stats.after == 10 && stats.before == 30);
    assert(stats.merges == 1 && stats.repairs > 0);
    completion::CoverageStats limited;
    const auto unchanged = completion::ImproveCoveringTour(old, pool, start, &finish, 558, metric,
        [] {}, [] { return false; }, limited, place);
    assert(unchanged.size() == old.size() && limited.after == limited.before);
    bool cancelled = false;
    try { completion::ImproveCoveringTour(old, pool, start, &finish, 558, metric,
        [] { throw 1; }, [] { return true; }, limited, place); } catch (int) { cancelled = true; }
    assert(cancelled);
    std::vector<Stand> large;
    for (uint32_t i = 0; i < 80; ++i) large.push_back({{static_cast<float>((i * 19) % 80), static_cast<float>(i % 3)}, {i}});
    size_t work = 0;
    completion::CoverageStats wide;
    const auto repaired = completion::ImproveCoveringTour(large, large, start, &finish, 558, metric,
        [] {}, [&] { return ++work < 15000; }, wide, place);
    std::unordered_set<uint32_t> covered;
    for (const auto& stop : repaired) covered.insert(stop.reveals.begin(), stop.reveals.end());
    assert(covered.size() == 80 && wide.after <= wide.before); // Includes bits beyond the first 64.
    std::cout << "Covering search: overlapping cover, best-only publication, bitsets, work budget, cancellation passed\n";
}
