#pragma once
#include "RoutePlanner.h"
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace completion {
struct CoverageGridCell { int x, y; uint32_t floor; };
struct RegionCoverageResult {
    std::vector<size_t> order;
    size_t regions = 0, patterns = 0;
    float estimated_length = std::numeric_limits<float>::infinity();
};

// Discrete adaptation of BCD + one sweep pattern per region (GTSP). The grid
// partitions valid stand cells, not unexplored cells: uncovered fog is then
// assigned to a region with a valid reveal footing. Floors remain separate.
// Navigation, cancellation and a work budget are supplied by the caller.
template<class Stop, class Metric, class Check, class Continue>
RegionCoverageResult PlanRegionCoverage(const std::vector<Stop>& stops,
    const std::vector<CoverageGridCell>& walkable, int radius, uint32_t width,
    const Stop& start, const Stop* finish, Metric metric, Check check, Continue running)
{
    using Key = std::tuple<uint32_t, int, int>;
    using Fog = std::unordered_set<uint32_t>;
    RegionCoverageResult best;
    if (stops.empty() || walkable.empty() || !width || radius < 0) return best;
    const int stride = 2 * radius + 1;
    std::map<std::pair<size_t, size_t>, float> distances;
    const auto leg = [&](size_t a, size_t b) {
        const auto key = std::pair{a, b};
        if (const auto hit = distances.find(key); hit != distances.end()) return hit->second;
        const Stop& from = a == stops.size() ? start : stops[a];
        const Stop& to = b == stops.size() ? *finish : stops[b];
        const float value = metric(from, to);
        distances.emplace(key, value);
        return value;
    };
    for (int decomposition_axis = 0; decomposition_axis < 2 && running(); ++decomposition_axis) {
        struct Run { uint32_t floor; int scan, lo, hi; size_t region = 0; };
        std::map<std::pair<uint32_t, int>, std::set<int>> lines;
        for (const auto p : walkable) lines[{p.floor, decomposition_axis ? p.x : p.y}].insert(decomposition_axis ? p.y : p.x);
        std::map<Key, size_t> membership;
        std::vector<Run> previous;
        size_t region_count = 0;
        for (const auto& [line, across] : lines) {
            check();
            if (!running()) return best;
            std::vector<Run> current;
            for (const auto cell : across) {
                if (current.empty() || cell > current.back().hi + 1) current.push_back({line.first, line.second, cell, cell});
                else current.back().hi = cell;
            }
            const auto overlaps = [](const Run& a, const Run& b) {
                return a.floor == b.floor && b.scan == a.scan + 1 && a.lo <= b.hi && b.lo <= a.hi;
            };
            for (auto& run : current) {
                const Run* parent = nullptr;
                size_t parents = 0;
                for (const auto& old : previous) if (overlaps(old, run)) { parent = &old; ++parents; }
                size_t children = 0;
                if (parents == 1) for (const auto& child : current) children += overlaps(*parent, child);
                run.region = parents == 1 && children == 1 ? parent->region : region_count++;
                for (int cell = run.lo; cell <= run.hi; ++cell)
                    membership[{run.floor, decomposition_axis ? run.scan : cell, decomposition_axis ? cell : run.scan}] = run.region;
            }
            previous = std::move(current);
        }
        if (!region_count || region_count > 128) continue;
        std::vector<std::vector<size_t>> choices(region_count);
        std::vector<size_t> region_for(stops.size(), region_count);
        for (size_t i = 0; i < stops.size(); ++i) {
            const auto& stop = stops[i];
            const auto found = membership.find({stop.pos.zplane, stop.cell_x, stop.cell_y});
            if (found == membership.end()) continue;
            choices[found->second].push_back(i); region_for[i] = found->second;
        }
        std::map<uint32_t, std::pair<float, size_t>> assignments;
        for (size_t i = 0; i < stops.size(); ++i) if (region_for[i] != region_count) {
            for (const auto fog : stops[i].reveals) {
                const float distance = std::hypot(static_cast<float>(stops[i].cell_x) - fog % width,
                    static_cast<float>(stops[i].cell_y) - fog / width);
                const auto old = assignments.find(fog);
                if (old == assignments.end() || distance < old->second.first) assignments[fog] = {distance, region_for[i]};
            }
        }
        std::vector<Fog> targets(region_count);
        for (const auto& [fog, assignment] : assignments) targets[assignment.second].insert(fog);
        struct Pattern { std::vector<size_t> visits; float cost; };
        std::vector<std::vector<Pattern>> groups;
        for (size_t region = 0; region < region_count && running(); ++region) {
            if (targets[region].empty()) continue;
            std::vector<Pattern> patterns;
            for (int axis = 0; axis < 2 && running(); ++axis) for (int offset = 0; offset < stride && running(); ++offset) {
                auto uncovered = targets[region];
                std::vector<size_t> visits;
                // Prefer a reveal-width-spaced lane, then repair its uncovered
                // edge targets using any valid footing in this same region.
                for (int stage = 0; stage < 2; ++stage) while (!uncovered.empty()) {
                    check();
                    if (!running()) return best;
                    size_t winner = stops.size(), gain_best = 0;
                    for (const auto i : choices[region]) {
                        const int cell = axis ? stops[i].cell_x : stops[i].cell_y;
                        if (stage == 0 && (cell % stride + stride) % stride != offset) continue;
                        size_t gain = 0;
                        for (const auto fog : stops[i].reveals) gain += uncovered.contains(fog);
                        if (gain > gain_best) { winner = i; gain_best = gain; }
                    }
                    if (winner == stops.size()) break;
                    visits.push_back(winner);
                    for (const auto fog : stops[winner].reveals) uncovered.erase(fog);
                }
                if (!uncovered.empty()) continue;
                for (int side = 0; side < 2; ++side) {
                    auto sweep = visits;
                    std::stable_sort(sweep.begin(), sweep.end(), [&](size_t a, size_t b) {
                        const int lane_a = axis ? stops[a].cell_x : stops[a].cell_y;
                        const int lane_b = axis ? stops[b].cell_x : stops[b].cell_y;
                        const int band_a = static_cast<int>(std::floor(static_cast<float>(lane_a - offset) / stride));
                        const int band_b = static_cast<int>(std::floor(static_cast<float>(lane_b - offset) / stride));
                        if (band_a != band_b) return band_a < band_b;
                        const int across_a = axis ? stops[a].cell_y : stops[a].cell_x;
                        const int across_b = axis ? stops[b].cell_y : stops[b].cell_x;
                        return (band_a % 2 != 0) ^ (side != 0) ? across_a > across_b : across_a < across_b;
                    });
                    for (int reverse = 0; reverse < 2; ++reverse) {
                        if (reverse) std::reverse(sweep.begin(), sweep.end());
                        float internal = 0;
                        for (size_t i = 1; i < sweep.size(); ++i) internal += leg(sweep[i - 1], sweep[i]);
                        if (std::isfinite(internal)) patterns.push_back({sweep, internal});
                    }
                }
            }
            // Keep a bounded set of alternative entry/exit patterns per region.
            std::stable_sort(patterns.begin(), patterns.end(), [](const Pattern& a, const Pattern& b) { return a.cost < b.cost; });
            std::vector<Pattern> unique;
            for (auto& pattern : patterns) {
                if (std::ranges::any_of(unique, [&](const Pattern& p) { return p.visits == pattern.visits; })) continue;
                unique.push_back(std::move(pattern));
                if (unique.size() == 32) break;
            }
            if (unique.empty()) { groups.clear(); break; }
            groups.push_back(std::move(unique));
        }
        if (groups.empty() || !running()) continue;
        // For each region order, dynamic programming jointly chooses its sweep
        // direction, entry and exit. Region order itself uses bounded 2-opt.
        const auto evaluate = [&](const std::vector<size_t>& order, std::vector<size_t>* visits_path) {
            std::vector<std::vector<float>> values(order.size());
            std::vector<std::vector<size_t>> parent(order.size());
            for (size_t i = 0; i < order.size(); ++i) {
                check();
                if (!running()) return std::numeric_limits<float>::infinity();
                const auto& group = groups[order[i]];
                values[i].assign(group.size(), std::numeric_limits<float>::infinity()); parent[i].resize(group.size());
                for (size_t b = 0; b < group.size(); ++b) {
                    if (!running()) return std::numeric_limits<float>::infinity();
                    if (!i) values[i][b] = leg(stops.size(), group[b].visits.front()) + group[b].cost;
                    else for (size_t a = 0; a < values[i - 1].size(); ++a) {
                        if (!running()) return std::numeric_limits<float>::infinity();
                        const float cost = values[i - 1][a] + leg(groups[order[i - 1]][a].visits.back(), group[b].visits.front()) + group[b].cost;
                        if (cost < values[i][b]) { values[i][b] = cost; parent[i][b] = a; }
                    }
                }
            }
            size_t last = 0;
            float cost_best = std::numeric_limits<float>::infinity();
            for (size_t i = 0; i < values.back().size(); ++i) {
                const float cost = values.back()[i] + (finish ? leg(groups[order.back()][i].visits.back(), stops.size()) : 0.f);
                if (cost < cost_best) { cost_best = cost; last = i; }
            }
            if (visits_path && std::isfinite(cost_best)) {
                std::vector<size_t> patterns(order.size());
                for (size_t i = order.size(); i-- > 0;) { patterns[i] = last; if (i) last = parent[i][last]; }
                visits_path->clear();
                for (size_t i = 0; i < order.size(); ++i) {
                    const auto& visits = groups[order[i]][patterns[i]].visits;
                    visits_path->insert(visits_path->end(), visits.begin(), visits.end());
                }
            }
            return cost_best;
        };
        std::vector<size_t> order, unused(groups.size());
        std::iota(unused.begin(), unused.end(), 0);
        size_t previous_stop = stops.size();
        while (!unused.empty() && running()) {
            size_t winner = unused.front(), exit = groups[winner][0].visits.back();
            float minimum = std::numeric_limits<float>::infinity();
            for (const auto region : unused) for (const auto& pattern : groups[region]) {
                const float score = leg(previous_stop, pattern.visits.front()) + pattern.cost;
                if (score < minimum) { minimum = score; winner = region; exit = pattern.visits.back(); }
            }
            order.push_back(winner); std::erase(unused, winner); previous_stop = exit;
        }
        if (!unused.empty()) continue;
        std::vector<size_t> visits_path;
        float cost = evaluate(order, &visits_path);
        const auto retain = [&] {
            if (cost >= best.estimated_length || visits_path.empty()) return;
            best.order = visits_path; best.estimated_length = cost; best.regions = groups.size(); best.patterns = 0;
            for (const auto& group : groups) best.patterns += group.size();
        };
        retain();
        for (int pass = 0; pass < 4 && running(); ++pass) {
            bool changed = false;
            for (size_t i = 0; i < order.size() && running(); ++i) for (size_t j = i + 1; j < order.size() && running(); ++j) {
                auto trial = order;
                std::reverse(trial.begin() + static_cast<ptrdiff_t>(i), trial.begin() + static_cast<ptrdiff_t>(j + 1));
                const float candidate = evaluate(trial, nullptr);
                if (candidate + .1f >= cost) continue;
                order = std::move(trial); cost = candidate; evaluate(order, &visits_path); retain(); changed = true;
            }
            if (!changed) break;
        }
    }
    return best;
}
}
