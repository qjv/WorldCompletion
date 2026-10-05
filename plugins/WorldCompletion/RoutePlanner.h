#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace completion {
    struct Point {
        float x, y;
    };
    using Portal = std::pair<Point, Point>;
    using Costs = std::vector<std::vector<float>>;

    inline float Cross(Point a, Point b)
    {
        return a.x * b.y - a.y * b.x;
    }
    inline Point Sub(Point a, Point b)
    {
        return {a.x - b.x, a.y - b.y};
    }
    inline float Distance(Point a, Point b)
    {
        return std::hypot(a.x - b.x, a.y - b.y);
    }

    inline float ShortestPortalParameter(Point before, Point after, Portal edge)
    {
        const auto [a, b] = edge;
        const double dx = b.x - a.x, dy = b.y - a.y;
        const double edge_sq = dx * dx + dy * dy;
        if (edge_sq <= 1e-8) return 0.f;
        const double ax = before.x - a.x, ay = before.y - a.y;
        const double bx = after.x - a.x, by = after.y - a.y;
        const double height_a = std::fabs(dx * ay - dy * ax);
        const double height_b = std::fabs(dx * by - dy * bx);
        const double ta = (ax * dx + ay * dy) / edge_sq;
        const double tb = (bx * dx + by * dy) / edge_sq;
        const double heights = height_a + height_b;
        return static_cast<float>(std::clamp(heights > 1e-8 ? (ta * height_b + tb * height_a) / heights : (ta + tb) * .5, 0., 1.));
    }

    inline std::optional<Portal> SharedBoundary(const Point (&a)[4], const Point (&b)[4])
    {
        for (size_t i = 0; i < 4; ++i) {
            const auto origin = a[i], edge = Sub(a[(i + 1) % 4], origin);
            const float length = std::hypot(edge.x, edge.y);
            if (length < .001f) continue;
            const Point unit{edge.x / length, edge.y / length};
            for (size_t j = 0; j < 4; ++j) {
                const auto p = Sub(b[j], origin), q = Sub(b[(j + 1) % 4], origin);
                if (std::fabs(Cross(unit, p)) > .05f || std::fabs(Cross(unit, q)) > .05f) continue;
                const float p_t = p.x * unit.x + p.y * unit.y, q_t = q.x * unit.x + q.y * unit.y;
                const float lo = std::max(0.f, std::min(p_t, q_t));
                const float hi = std::min(length, std::max(p_t, q_t));
                if (hi - lo < .001f) continue;
                return Portal{{origin.x + unit.x * lo, origin.y + unit.y * lo}, {origin.x + unit.x * hi, origin.y + unit.y * hi}};
            }
        }
        return std::nullopt;
    }

    inline bool CrossesPortalsInOrder(Point a, Point b, const std::vector<Portal> &portals, size_t begin, size_t end)
    {
        const auto direction = Sub(b, a);
        const float length_sq = direction.x * direction.x + direction.y * direction.y;
        float previous = 0.f;
        for (size_t i = begin; i < end; ++i) {
            const auto edge = Sub(portals[i].second, portals[i].first);
            const auto offset = Sub(portals[i].first, a);
            const float denominator = Cross(direction, edge);
            if (std::fabs(denominator) < .00001f) {
                const float edge_length_sq = edge.x * edge.x + edge.y * edge.y;
                if (length_sq < .00001f) {
                    if (edge_length_sq < .00001f) {
                        if (Distance(a, portals[i].first) > .001f) return false;
                    }
                    else {
                        const float t = -(offset.x * edge.x + offset.y * edge.y) / edge_length_sq;
                        if (t < 0.f || t > 1.f || std::fabs(Cross(edge, offset)) > .001f * std::sqrt(edge_length_sq)) return false;
                    }
                    continue;
                }
                if (std::fabs(Cross(direction, offset)) > .001f * std::sqrt(length_sq)) return false;
                const auto far_offset = Sub(portals[i].second, a);
                const float t0 = (offset.x * direction.x + offset.y * direction.y) / length_sq;
                const float t1 = (far_offset.x * direction.x + far_offset.y * direction.y) / length_sq;
                const float lo = std::max(previous, std::min(t0, t1));
                if (lo > std::min(1.f, std::max(t0, t1)) + .000001f) return false;
                previous = lo;
                continue;
            }
            const float t = Cross(offset, edge) / denominator;
            const float u = Cross(offset, direction) / denominator;
            if (t + .000001f < previous || t < -.000001f || t > 1.000001f || u < -.000001f || u > 1.000001f) return false;
            previous = std::max(previous, t);
        }
        return true;
    }

    inline float OrderCost(const Costs &costs, const std::vector<size_t> &order, const std::vector<float> &finish)
    {
        float total = 0.f;
        size_t previous = costs.size() - 1;
        for (const auto next : order) {
            total += costs[previous][next];
            previous = next;
        }
        return total + (order.empty() ? 0.f : finish[order.back()]);
    }

    inline std::vector<size_t> StableOrder(const Costs &costs, const std::vector<float> &finish,
                                           std::vector<size_t> optimized, std::vector<size_t> previous)
    {
        if (previous.empty()) return optimized;
        if (optimized.empty()) return previous;
        const float old_cost = OrderCost(costs, previous, finish);
        const float new_cost = OrderCost(costs, optimized, finish);
        // Preserve small ordering ties, but never lock in substantial backtracking.
        if (new_cost + std::max(1024.f, old_cost * .05f) < old_cost) return optimized;
        return previous;
    }

    template <class Check>
    std::vector<size_t> OptimizeOrder(const Costs &costs, const std::vector<float> &finish, int passes, int restarts, uint32_t seed,
                                      Check check, const std::vector<std::vector<size_t>>& spatial_seeds = {})
    {
        const size_t n = finish.size();
        if (!n) return {};
        if (n <= 15) {
            const size_t count = size_t{1} << n;
            std::vector<float> dp(count * n, std::numeric_limits<float>::infinity());
            std::vector<uint8_t> parent(count * n, 255);
            for (size_t i = 0; i < n; ++i)
                dp[(size_t{1} << i) * n + i] = costs[n][i];
            for (size_t mask = 1; mask < count; ++mask) {
                check();
                for (size_t last = 0; last < n; ++last) {
                    if (!(mask & (size_t{1} << last))) continue;
                    for (size_t next = 0; next < n; ++next) {
                        if (mask & (size_t{1} << next)) continue;
                        const size_t index = (mask | (size_t{1} << next)) * n + next;
                        const float candidate = dp[mask * n + last] + costs[last][next];
                        if (candidate >= dp[index]) continue;
                        dp[index] = candidate;
                        parent[index] = static_cast<uint8_t>(last);
                    }
                }
            }
            size_t last = 0, mask = count - 1;
            for (size_t i = 1; i < n; ++i)
                if (dp[mask * n + i] + finish[i] < dp[mask * n + last] + finish[last]) last = i;
            if (!std::isfinite(dp[mask * n + last] + finish[last])) return {};
            std::vector<size_t> order(n);
            for (size_t position = n; position-- > 0;) {
                order[position] = last;
                const auto previous = parent[mask * n + last];
                mask ^= size_t{1} << last;
                last = previous;
            }
            return order;
        }
        std::vector<size_t> best, pool(n);
        std::iota(pool.begin(), pool.end(), 0);
        size_t at = n;
        while (!pool.empty()) {
            check();
            const auto next = std::min_element(pool.begin(), pool.end(), [&](size_t a, size_t b) { return costs[at][a] < costs[at][b]; });
            at = *next;
            best.push_back(at);
            pool.erase(next);
        }
        bool symmetric = true;
        for (size_t i = 0; i < n; ++i) {
            check();
            for (size_t j = i + 1; j < n; ++j)
                if (std::fabs(costs[i][j] - costs[j][i]) > .01f) symmetric = false;
        }
        const auto improve = [&](std::vector<size_t> &order) {
            for (int pass = 0; pass < passes; ++pass) {
                bool changed = false;
                for (size_t i = 0; i + 1 < n; ++i) {
                    check();
                    const size_t before = i ? order[i - 1] : n;
                    for (size_t k = i + 1; k < n; ++k) {
                        float old_cost = costs[before][order[i]] + (k + 1 < n ? costs[order[k]][order[k + 1]] : finish[order[k]]);
                        float new_cost = costs[before][order[k]] + (k + 1 < n ? costs[order[i]][order[k + 1]] : finish[order[i]]);
                        if (!symmetric) {
                            for (size_t j = i; j < k; ++j) {
                                old_cost += costs[order[j]][order[j + 1]];
                                new_cost += costs[order[j + 1]][order[j]];
                            }
                        }
                        if (new_cost + .01f >= old_cost) continue;
                        std::reverse(order.begin() + static_cast<ptrdiff_t>(i), order.begin() + static_cast<ptrdiff_t>(k + 1));
                        changed = true;
                    }
                }
                for (size_t i = 0; i < n; ++i) {
                    check();
                    const size_t node = order[i], before = i ? order[i - 1] : n;
                    const float removed = costs[before][node] + (i + 1 < n ? costs[node][order[i + 1]] - costs[before][order[i + 1]]
                                                                           : finish[node] - (i ? finish[before] : 0.f));
                    float best_delta = -.01f;
                    size_t destination = i;
                    for (size_t j = 0; j <= n; ++j) {
                        if (j == i || j == i + 1) continue;
                        const size_t predecessor = j ? order[j - 1] : n;
                        const float added = costs[predecessor][node] + (j < n ? costs[node][order[j]] - costs[predecessor][order[j]]
                                                                              : finish[node] - finish[predecessor]);
                        if (added - removed >= best_delta) continue;
                        best_delta = added - removed;
                        destination = j;
                    }
                    if (destination == i) continue;
                    order.erase(order.begin() + static_cast<ptrdiff_t>(i));
                    order.insert(order.begin() + static_cast<ptrdiff_t>(destination > i ? destination - 1 : destination), node);
                    changed = true;
                }
                if (!changed) break;
            }
        };
        improve(best);
        float best_cost = OrderCost(costs, best, finish);
        for (auto trial : spatial_seeds) {
            check();
            if (trial.size() != n) continue;
            improve(trial);
            const float trial_cost = OrderCost(costs, trial, finish);
            if (trial_cost < best_cost) { best = std::move(trial); best_cost = trial_cost; }
        }
        if (std::ranges::any_of(finish, [](float cost) { return cost != 0.f; })) {
            std::vector<size_t> terminal_seed, remaining(n);
            std::iota(remaining.begin(), remaining.end(), 0);
            size_t next = n;
            while (!remaining.empty()) {
                check();
                const auto closest = std::min_element(remaining.begin(), remaining.end(), [&](size_t a, size_t b) {
                    return (next == n ? finish[a] : costs[next][a]) < (next == n ? finish[b] : costs[next][b]);
                });
                terminal_seed.push_back(next = *closest);
                remaining.erase(closest);
            }
            std::reverse(terminal_seed.begin(), terminal_seed.end());
            improve(terminal_seed);
            const float cost = OrderCost(costs, terminal_seed, finish);
            if (cost < best_cost) {
                best = std::move(terminal_seed);
                best_cost = cost;
            }
        }
        for (int restart = 0; restart < std::clamp(restarts, 0, 12); ++restart) {
            check();
            // Independent, reproducible seeds explore different construction
            // algorithms rather than repeatedly nudging the same local optimum.
            std::mt19937 random(seed ^ (0x9e3779b9u * static_cast<uint32_t>(restart + 1)));
            std::vector<size_t> trial, unused(n);
            std::iota(unused.begin(), unused.end(), 0);
            if (restart % 4 == 0) {
                size_t from = random() % n;
                trial.push_back(from);
                unused.erase(unused.begin() + static_cast<ptrdiff_t>(from));
                while (!unused.empty()) {
                    check();
                    const auto closest = std::min_element(unused.begin(), unused.end(),
                        [&](size_t a, size_t b) { return costs[from][a] < costs[from][b]; });
                    trial.push_back(from = *closest);
                    unused.erase(closest);
                }
            }
            else if (restart % 4 == 1) {
                // Cheapest insertion, with a shuffled node admission order.
                std::shuffle(unused.begin(), unused.end(), random);
                for (const size_t node : unused) {
                    check();
                    float best_delta = std::numeric_limits<float>::infinity();
                    size_t destination = 0;
                    for (size_t pos = 0; pos <= trial.size(); ++pos) {
                        const size_t before = pos ? trial[pos - 1] : n;
                        const float delta = costs[before][node] + (pos < trial.size()
                            ? costs[node][trial[pos]] - costs[before][trial[pos]]
                            : finish[node] - (before == n ? 0.f : finish[before]));
                        if (delta < best_delta) { best_delta = delta; destination = pos; }
                    }
                    trial.insert(trial.begin() + static_cast<ptrdiff_t>(destination), node);
                }
            }
            else if (restart % 4 == 2) {
                // Choose among the three nearest remaining stops at each step.
                size_t from = n;
                while (!unused.empty()) {
                    check();
                    const size_t choices = std::min(size_t{3}, unused.size());
                    std::partial_sort(unused.begin(), unused.begin() + static_cast<ptrdiff_t>(choices), unused.end(),
                        [&](size_t a, size_t b) { return costs[from][a] < costs[from][b]; });
                    const size_t pick = random() % choices;
                    trial.push_back(from = unused[pick]);
                    unused.erase(unused.begin() + static_cast<ptrdiff_t>(pick));
                }
            }
            else {
                trial = best;
                for (int kick = 0; kick < 3; ++kick) {
                    size_t a = random() % n, b = random() % n;
                    if (a > b) std::swap(a, b);
                    std::reverse(trial.begin() + static_cast<ptrdiff_t>(a), trial.begin() + static_cast<ptrdiff_t>(b + 1));
                }
            }
            improve(trial);
            const float cost = OrderCost(costs, trial, finish);
            if (cost >= best_cost) continue;
            best = std::move(trial);
            best_cost = cost;
        }
        return best;
    }
} // namespace completion
