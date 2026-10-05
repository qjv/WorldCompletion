#pragma once
#include "RoutePlanner.h"

namespace completion {
// Coverage-specific construction seeds. These only propose orders; the caller
// remains responsible for coverage, navigation and measured-road acceptance.
inline std::vector<std::vector<size_t>> SpatialSeeds(const std::vector<Point>& cells,
    int stride, const Costs& costs, const std::vector<float>& finish)
{
    if (cells.size() <= 15 || cells.size() > 128) return {};
    Point lo = cells.front(), hi = lo;
    for (const auto p : cells) {
        lo.x = std::min(lo.x, p.x); lo.y = std::min(lo.y, p.y);
        hi.x = std::max(hi.x, p.x); hi.y = std::max(hi.y, p.y);
    }
    const Point centre{(lo.x + hi.x) * .5f, (lo.y + hi.y) * .5f};
    std::vector<std::vector<size_t>> seeds;
    for (int axis = 0; axis < 2; ++axis) for (int phase = 0; phase < 2; ++phase) {
        std::vector<size_t> order(cells.size());
        std::iota(order.begin(), order.end(), 0);
        const auto lane = [&](size_t i) {
            return static_cast<int>(std::floor(((axis ? cells[i].x - lo.x : cells[i].y - lo.y)
                + phase * stride * .5f) / std::max(1, stride)));
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const int row_a = lane(a), row_b = lane(b);
            if (row_a != row_b) return row_a < row_b;
            const float across_a = axis ? cells[a].y : cells[a].x;
            const float across_b = axis ? cells[b].y : cells[b].x;
            return row_a % 2 ? across_a > across_b : across_a < across_b;
        });
        seeds.push_back(order);
        std::reverse(order.begin(), order.end()); seeds.push_back(std::move(order));
    }
    for (int centre_first = 0; centre_first < 2; ++centre_first) for (int reverse = 0; reverse < 2; ++reverse) {
        std::vector<size_t> order(cells.size());
        std::iota(order.begin(), order.end(), 0);
        const auto ring = [&](size_t i) {
            const auto p = cells[i];
            const float depth = std::min({p.x - lo.x, hi.x - p.x, p.y - lo.y, hi.y - p.y});
            return static_cast<int>(depth / std::max(1, stride));
        };
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const int ring_a = ring(a), ring_b = ring(b);
            if (ring_a != ring_b) return centre_first ? ring_a > ring_b : ring_a < ring_b;
            const float angle_a = std::atan2(cells[a].y - centre.y, cells[a].x - centre.x);
            const float angle_b = std::atan2(cells[b].y - centre.y, cells[b].x - centre.x);
            return reverse ? angle_a > angle_b : angle_a < angle_b;
        });
        seeds.push_back(std::move(order));
    }
    // Refine only the best four estimates. Generating the other patterns needs
    // no new navigation queries and does not increase the expensive arena size.
    std::stable_sort(seeds.begin(), seeds.end(), [&](const auto& a, const auto& b) {
        return OrderCost(costs, a, finish) < OrderCost(costs, b, finish);
    });
    seeds.resize(std::min(size_t{4}, seeds.size()));
    return seeds;
}
}
