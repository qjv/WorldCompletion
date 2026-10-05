#include "../plugins/WorldCompletion/RoutePlanner.h"

#include <cassert>
#include <iostream>

using namespace completion;

void TestBoundaries()
{
    const Point large[4]{{0, 10}, {10, 10}, {10, 0}, {0, 0}};
    const Point partial[4]{{10, 6}, {20, 6}, {20, 2}, {10, 2}};
    const auto edge = SharedBoundary(large, partial);
    assert(edge && std::fabs(Distance(edge->first, edge->second) - 4.f) < .001f);
    const Point gap[4]{{11, 6}, {20, 6}, {20, 2}, {11, 2}};
    assert(!SharedBoundary(large, gap));
    const Point crossing_floor[4]{{2, 12}, {8, 12}, {8, -2}, {2, -2}};
    assert(!SharedBoundary(large, crossing_floor));
    const Point touching_corner[4]{{10, 20}, {20, 20}, {20, 10}, {10, 10}};
    assert(!SharedBoundary(large, touching_corner));
    const Point slanted_a[4]{{1, 4}, {5, 4}, {4, 0}, {0, 0}};
    const Point slanted_b[4]{{4.75f, 3}, {8, 3}, {8, 1}, {4.25f, 1}};
    assert(SharedBoundary(slanted_a, slanted_b));
}

void TestCorridors()
{
    std::vector<Portal> portals{{{2, -1}, {2, 1}}, {{4, -1}, {4, 1}}};
    assert(CrossesPortalsInOrder({0, 0}, {6, 0}, portals, 0, 2));
    assert(!CrossesPortalsInOrder({0, 0}, {6, 6}, portals, 0, 2));
    std::reverse(portals.begin(), portals.end());
    assert(!CrossesPortalsInOrder({0, 0}, {6, 0}, portals, 0, 2));
    portals = {{{2, 0}, {4, 0}}, {{3, 0}, {5, 0}}};
    assert(CrossesPortalsInOrder({0, 0}, {6, 0}, portals, 0, 2));
    assert(!CrossesPortalsInOrder({0, 1}, {6, 1}, portals, 0, 2));
    portals = {{{2, -1}, {2, 1}}};
    assert(CrossesPortalsInOrder({2, 0}, {2, 0}, portals, 0, 1));
    assert(!CrossesPortalsInOrder({2, 2}, {2, 2}, portals, 0, 1));
    portals = {{{2, 0}, {2, 0}}};
    assert(CrossesPortalsInOrder({0, 0}, {4, 0}, portals, 0, 1));
    assert(!CrossesPortalsInOrder({0, 1}, {4, 1}, portals, 0, 1));
}

Costs RandomMetric(size_t n, std::mt19937& rng)
{
    Costs costs(n + 1, std::vector<float>(n + 1));
    for (size_t i = 0; i <= n; ++i)
        for (size_t j = i + 1; j <= n; ++j) costs[i][j] = costs[j][i] = 1.f + static_cast<float>(rng() % 1000);
    for (size_t k = 0; k <= n; ++k)
        for (size_t i = 0; i <= n; ++i)
            for (size_t j = 0; j <= n; ++j) costs[i][j] = std::min(costs[i][j], costs[i][k] + costs[k][j]);
    return costs;
}

void TestExactOrder()
{
    std::mt19937 rng(123);
    assert(OptimizeOrder(Costs(1, std::vector<float>(1)), {}, 8, 0, 0, [] {}).empty());
    for (size_t n = 1; n <= 8; ++n) {
        for (int fixture = 0; fixture < 10; ++fixture) {
            const auto costs = RandomMetric(n, rng);
            std::vector<float> finish(n);
            if (fixture % 2) for (auto& cost : finish) cost = static_cast<float>(rng() % 1000);
            std::vector<size_t> brute(n);
            std::iota(brute.begin(), brute.end(), 0);
            float optimal = std::numeric_limits<float>::infinity();
            do { optimal = std::min(optimal, OrderCost(costs, brute, finish)); }
            while (std::next_permutation(brute.begin(), brute.end()));
            const auto order = OptimizeOrder(costs, finish, 8, 0, 0, [] {});
            assert(std::fabs(OrderCost(costs, order, finish) - optimal) < .01f);
        }
    }
}

void TestLargeOrderAndCancellation()
{
    std::mt19937 rng(42);
    for (int fixture = 0; fixture < 30; ++fixture) {
        const size_t n = 16 + static_cast<size_t>(fixture);
        const auto costs = RandomMetric(n, rng);
        std::vector<float> finish(n, 0.f);
        if (fixture % 2) for (auto& cost : finish) cost = static_cast<float>(rng() % 1000);
        std::vector<size_t> nearest, pool(n);
        std::iota(pool.begin(), pool.end(), 0);
        size_t previous = n;
        while (!pool.empty()) {
            const auto next = std::min_element(pool.begin(), pool.end(), [&](size_t a, size_t b) { return costs[previous][a] < costs[previous][b]; });
            nearest.push_back(previous = *next);
            pool.erase(next);
        }
        auto order = OptimizeOrder(costs, finish, 16, 4, 42, [] {});
        assert(OrderCost(costs, order, finish) <= OrderCost(costs, nearest, finish) + .01f);
        assert(order == OptimizeOrder(costs, finish, 16, 4, 42, [] {}));
        std::sort(order.begin(), order.end());
        for (size_t i = 0; i < n; ++i) assert(order[i] == i);
    }
    for (size_t n : {size_t{12}, size_t{32}}) {
        bool cancelled = false;
        int checks = 0;
        try { OptimizeOrder(RandomMetric(n, rng), std::vector<float>(n), 16, 8, 0, [&] { if (++checks == 10) throw 1; }); }
        catch (int) { cancelled = true; }
        assert(cancelled);
    }
}

void TestPortalEndpoints()
{
    constexpr size_t n = 20;
    Costs costs(n + 1, std::vector<float>(n + 1));
    const auto position = [](size_t i) { return i == n ? 9.5f : static_cast<float>(i); };
    for (size_t i = 0; i <= n; ++i)
        for (size_t j = 0; j <= n; ++j) costs[i][j] = std::fabs(position(i) - position(j));
    for (const size_t endpoint : {size_t{0}, n - 1}) {
        std::vector<float> finish(n);
        for (size_t i = 0; i < n; ++i) finish[i] = 100.f * std::fabs(position(i) - position(endpoint));
        const auto order = OptimizeOrder(costs, finish, 8, 2, 42, [] {});
        assert(order.size() == n);
        assert(order.back() == endpoint);
    }
}

void TestSeedArena()
{
    std::mt19937 rng(8675309);
    bool found_improvement = false;
    for (int fixture = 0; fixture < 20; ++fixture) {
        constexpr size_t n = 24;
        const auto costs = RandomMetric(n, rng);
        std::vector<float> finish(n);
        for (auto& cost : finish) cost = static_cast<float>(rng() % 1000);
        const auto baseline = OptimizeOrder(costs, finish, 8, 0, 71, [] {});
        float best_cost = OrderCost(costs, baseline, finish);
        for (int attempts = 1; attempts <= 8; ++attempts) {
            const auto order = OptimizeOrder(costs, finish, 8, attempts, 71, [] {});
            const float cost = OrderCost(costs, order, finish);
            assert(cost <= best_cost + .01f); // More trials never discard the incumbent.
            found_improvement |= cost + .01f < OrderCost(costs, baseline, finish);
            best_cost = cost;
            assert(order == OptimizeOrder(costs, finish, 8, attempts, 71, [] {}));
            auto sorted = order;
            std::sort(sorted.begin(), sorted.end());
            for (size_t i = 0; i < n; ++i) assert(sorted[i] == i);
        }
    }
    assert(found_improvement);
}

void TestDirectedCosts()
{
    constexpr size_t n = 20;
    std::mt19937 rng(2026);
    Costs costs(n + 1, std::vector<float>(n + 1));
    for (size_t i = 0; i <= n; ++i)
        for (size_t j = 0; j <= n; ++j) if (i != j) costs[i][j] = 10.f + static_cast<float>(rng() % 1000);
    for (size_t k = 0; k <= n; ++k)
        for (size_t i = 0; i <= n; ++i)
            for (size_t j = 0; j <= n; ++j) costs[i][j] = std::min(costs[i][j], costs[i][k] + costs[k][j]);
    std::vector<float> finish(n);
    std::vector<size_t> nearest, unused(n);
    std::iota(unused.begin(), unused.end(), 0);
    size_t from = n;
    while (!unused.empty()) {
        const auto next = std::min_element(unused.begin(), unused.end(), [&](size_t a, size_t b) { return costs[from][a] < costs[from][b]; });
        nearest.push_back(from = *next);
        unused.erase(next);
    }
    const auto result = OptimizeOrder(costs, finish, 16, 8, 17, [] {});
    assert(OrderCost(costs, result, finish) <= OrderCost(costs, nearest, finish) + .01f);
}

int main()
{
    {
        std::mt19937 random(481);
        const auto coordinate = [&] { return static_cast<float>(static_cast<int>(random() % 2001) - 1000); };
        for (int sample = 0; sample < 300; ++sample) {
            const Point before{coordinate(), coordinate()}, after{coordinate(), coordinate()};
            Portal edge{{coordinate(), coordinate()}, {coordinate(), coordinate()}};
            if (sample == 0) edge.second = edge.first;
            if (sample == 1) { edge = {{0, 0}, {10, 0}}; }
            const auto objective = [&](float t) {
                const Point p{edge.first.x + t * (edge.second.x - edge.first.x), edge.first.y + t * (edge.second.y - edge.first.y)};
                return Distance(before, p) + Distance(after, p);
            };
            float lo = 0, hi = 1;
            for (int iteration = 0; iteration < 60; ++iteration) {
                const float a = (2 * lo + hi) / 3, b = (lo + 2 * hi) / 3;
                if (objective(a) < objective(b)) hi = b; else lo = a;
            }
            const float t = ShortestPortalParameter(before, after, edge);
            assert(t >= 0 && t <= 1);
            assert(objective(t) <= objective((lo + hi) * .5f) + .002f);
        }
        assert(ShortestPortalParameter({2, 0}, {8, 0}, {{0, 0}, {10, 0}}) == .5f);
        assert(ShortestPortalParameter({20, 1}, {30, -1}, {{0, 0}, {10, 0}}) == 1.f);
    }
    TestBoundaries();
    TestCorridors();
    TestExactOrder();
    TestPortalEndpoints();
    TestSeedArena();
    TestDirectedCosts();
    TestLargeOrderAndCancellation();
    std::cout << "Route planner: geometry, exact ordering, bounded refinement, determinism, cancellation passed\n";
}
