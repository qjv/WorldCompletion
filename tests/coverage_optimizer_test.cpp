#include "plugins/WorldCompletion/CoverageOptimizer.h"
#include <cassert>
#include <iostream>

using namespace completion;
struct Stop {
    Point pos;
    std::vector<uint32_t> reveals;
    bool preserved = false;
    float approach_cost = 0.f;
};
const auto metric = [](const Stop& a, const Stop& b) { return Distance(a.pos, b.pos); };
const auto order = [](std::vector<Stop> stops, const Stop& start, const Stop* finish) {
    const size_t n = stops.size();
    Costs costs(n + 1, std::vector<float>(n + 1));
    std::vector<float> terminal(n);
    for (size_t i = 0; i < n; ++i) {
        costs[n][i] = costs[i][n] = metric(start, stops[i]);
        terminal[i] = finish ? metric(stops[i], *finish) : 0.f;
        for (size_t j = 0; j < n; ++j) costs[i][j] = metric(stops[i], stops[j]);
    }
    const auto indices = OptimizeOrder(costs, terminal, 8, 4, 123, [] {});
    std::vector<Stop> result;
    for (const auto i : indices) result.push_back(stops[i]);
    return result;
};
const auto unchanged = [](const Stop& current, const Stop&, const Stop*) { return std::vector<Stop>{current}; };
void CheckCoverage(const std::vector<Stop>& result, uint32_t count)
{
    std::unordered_set<uint32_t> found;
    for (const auto& stop : result) found.insert(stop.reveals.begin(), stop.reveals.end());
    for (uint32_t i = 1; i <= count; ++i) assert(found.contains(i));
}
int main()
{
    const Stop start{{0, 0}, {}}, finish{{10, 0}, {}};
    {
        const Stop origin{{0, 0}, {}}, end{{0, 10000}, {}};
        const std::vector<Stop> border{{{3072, 5000}, {1, 2}, true}};
        const auto inward = [](const Stop& current, const Stop&, const Stop*) {
            auto valid = current; valid.pos.x = 0;
            auto incomplete = valid; incomplete.reveals = {1};
            return std::vector<Stop>{incomplete, valid};
        };
        CoverageStats stats;
        auto relaxed = RelaxCoveragePositions(border, origin, &end, metric, inward, [] {}, [] { return true; }, stats);
        assert(relaxed.size() == 1 && relaxed[0].pos.x == 0 && stats.moves == 1);
        CheckCoverage(relaxed, 2);
        auto limited = RelaxCoveragePositions(border, origin, &end, metric, inward, [] {}, [] { return false; }, stats);
        assert(limited[0].pos.x == 3072);
        const auto loses_coverage = [](const Stop& current, const Stop&, const Stop*) {
            auto invalid = current; invalid.pos.x = 0; invalid.reveals = {1};
            return std::vector<Stop>{invalid};
        };
        auto complete = RelaxCoveragePositions(border, origin, &end, metric, loses_coverage, [] {}, [] { return true; }, stats);
        assert(complete[0].pos.x == 3072);
    }
    {
        const std::vector<Stop> original{{{5, 0}, {1}}};
        const auto distant = [](const Stop& current, const Stop&, const Stop*) {
            auto worse = current;
            worse.pos = {500, 500};
            return std::vector<Stop>{worse};
        };
        size_t expensive_calls = 0;
        const auto counted = [&](const Stop& a, const Stop& b) {
            if (a.pos.x == 500 || b.pos.x == 500) ++expensive_calls;
            return metric(a, b);
        };
        CoverageStats stats;
        const auto result = ImproveCoverageRoute(original, original, start, &finish, 1, 0, 99,
            counted, distant, order, [] {}, [] { return true; }, stats);
        assert(expensive_calls == 0 && result[0].pos.x == 5 && stats.after == 10);
    }
    {
        const std::vector<Stop> original{{{0, 10}, {1}}, {{10, 10}, {2}}};
        auto pool = original;
        pool.push_back({{5, 0}, {1, 2}});
        CoverageStats stats;
        const auto result = ImproveCoverageRoute(original, pool, start, &finish, 3, 0, 99,
            metric, unchanged, order, [] {}, [] { return true; }, stats);
        CheckCoverage(result, 2);
        assert(result.size() == 1 && stats.merges == 1);
        assert(stats.after == 10.f && stats.before == 30.f);
    }
    {
        const std::vector<Stop> original{{{0, 10}, {1}}, {{10, 10}, {2}}, {{10, 20}, {3}}};
        auto pool = original;
        pool.push_back({{2, 0}, {1, 2}});
        pool.push_back({{8, 0}, {2, 3}});
        CoverageStats stats;
        const auto result = ImproveCoverageRoute(original, pool, start, &finish, 4, 0, 99,
            metric, unchanged, order, [] {}, [] { return true; }, stats);
        CheckCoverage(result, 3);
        assert(result.size() == 2 && stats.repairs && stats.after < stats.before);
    }
    {
        const std::vector<Stop> original{{{5, 5}, {1}}};
        const auto place = [](const Stop& current, const Stop&, const Stop*) {
            auto moved = current;
            moved.pos = {5, 0};
            return std::vector<Stop>{current, moved};
        };
        CoverageStats stats;
        const auto result = ImproveCoverageRoute(original, original, start, &finish, 2, 0, 99,
            metric, place, order, [] {}, [] { return true; }, stats);
        CheckCoverage(result, 1);
        assert(stats.moves == 1 && stats.after == 10.f && result[0].pos.y == 0.f);
        CoverageStats bounded;
        const auto stopped = ImproveCoverageRoute(original, original, start, &finish, 8, 12, 99,
            metric, place, order, [] {}, [] { return false; }, bounded);
        assert(stopped[0].pos.y == 5.f && bounded.before == bounded.after && bounded.budget_exhausted);
    }
    {
        const std::vector<Stop> wall{{{8, 0}, {1}}}, pool{{{8, 0}, {1}}, {{8, 4}, {1}}};
        CoverageStats stats;
        const auto result = ImproveCoverageRoute(wall, pool, start, &finish, 4, 6, 99,
            metric, unchanged, order, [] {}, [] { return true; }, stats);
        assert(result.size() == 1 && result[0].pos.y == 0.f && stats.after == 10.f);
    }
    {
        std::vector<Stop> stops;
        for (uint32_t i = 0; i < 35; ++i) stops.push_back({{static_cast<float>((i * 13) % 35), static_cast<float>((i * 7) % 11)}, {i + 1}});
        CoverageStats stats;
        const auto result = ImproveCoverageRoute(stops, stops, start, &finish, 3, 6, 99,
            metric, unchanged, order, [] {}, [] { return true; }, stats);
        CheckCoverage(result, 35);
        assert(result.size() == 35 && stats.after < stats.before * .7f);
        bool cancelled = false;
        int checks = 0;
        try {
            ImproveCoverageRoute(stops, stops, start, &finish, 3, 6, 99,
                metric, unchanged, order, [&] { if (++checks == 20) throw 1; }, [] { return true; }, stats);
        } catch (int) { cancelled = true; }
        assert(cancelled);
    }
    std::cout << "Coverage optimizer: merging, group repair, placement, 35 stops, wall travel, budgets, cancellation passed\n";
}
