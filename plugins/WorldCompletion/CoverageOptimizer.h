#pragma once

#include "RoutePlanner.h"
#include <unordered_map>
#include <unordered_set>

namespace completion {
struct CoverageStats {
    float before = 0.f, after = 0.f;
    uint32_t trials = 0, moves = 0, merges = 0, repairs = 0, passes = 0;
    bool budget_exhausted = false;
};

// Cheap position polishing comes before changing the cover set. Keep each
// stop's uniquely assigned targets, but allow other reveal cells and points
// within them when the complete walking path becomes shorter.
template<class Stop, class Metric, class Place, class Check, class Continue>
std::vector<Stop> RelaxCoveragePositions(std::vector<Stop> stops, const Stop& start,
    const Stop* finish, Metric metric, Place placements, Check check, Continue running, CoverageStats& stats)
{
    const auto straight = [](const Stop& a, const Stop& b) { return std::hypot(a.pos.x - b.pos.x, a.pos.y - b.pos.y); };
    for (size_t i = 0; i < stops.size() && running(); ++i) {
        check();
        std::unordered_set<uint32_t> unique(stops[i].reveals.begin(), stops[i].reveals.end());
        for (size_t j = 0; j < stops.size(); ++j) if (i != j)
            for (const auto fog : stops[j].reveals) unique.erase(fog);
        const Stop& before = i ? stops[i - 1] : start;
        const Stop* after = i + 1 < stops.size() ? &stops[i + 1] : finish;
        float best_cost = metric(before, stops[i]) + (after ? metric(stops[i], *after) : 0.f);
        const float old_cost = best_cost;
        Stop best = stops[i];
        auto options = placements(stops[i], before, after);
        std::erase_if(options, [&](const Stop& option) {
            return !std::ranges::all_of(unique, [&](uint32_t fog) { return std::ranges::find(option.reveals, fog) != option.reveals.end(); });
        });
        const auto bound = [&](const Stop& option) { return straight(before, option) + (after ? straight(option, *after) : 0.f); };
        std::stable_sort(options.begin(), options.end(), [&](const Stop& a, const Stop& b) { return bound(a) < bound(b); });
        std::vector<Stop> shortlist;
        for (const auto& option : options) {
            if (bound(option) + .1f >= best_cost) break;
            if (std::ranges::any_of(shortlist, [&](const Stop& other) { return straight(option, other) < 768.f; })) continue;
            shortlist.push_back(option);
            if (shortlist.size() == 8) break;
        }
        for (const auto& option : shortlist) {
            check();
            if (!running()) break;
            if (bound(option) + .1f >= best_cost) continue;
            const float approach = metric(before, option);
            if (approach + (after ? straight(option, *after) : 0.f) + .1f >= best_cost || !running()) continue;
            const float distance = approach + (after ? metric(option, *after) : 0.f);
            if (distance + .1f < best_cost) { best = option; best_cost = distance; }
        }
        if (best_cost + .1f < old_cost) { stops[i] = std::move(best); ++stats.moves; }
    }
    return stops;
}

// All candidates already have valid footing and reveal sets. Geometry, actual
// walking distance, ordering, cancellation and runtime limits remain caller-owned.
template<class Stop, class Metric, class Place, class Order, class Check, class Continue>
std::vector<Stop> ImproveCoverageRoute(std::vector<Stop> route, const std::vector<Stop>& pool,
    const Stop& start, const Stop* finish, int passes, int seeds, uint32_t seed,
    Metric metric, Place placements, Order ordering, Check check, Continue keep_going, CoverageStats& stats)
{
    using FogSet = std::unordered_set<uint32_t>;
    const auto covered = [](const std::vector<Stop>& stops) {
        FogSet fog;
        for (const auto& stop : stops) fog.insert(stop.reveals.begin(), stop.reveals.end());
        return fog;
    };
    const FogSet required = covered(route);
    const auto cost = [&](const std::vector<Stop>& stops, const Stop& before, const Stop* after) {
        float distance = 0.f;
        const Stop* previous = &before;
        for (const auto& stop : stops) { check(); distance += metric(*previous, stop); previous = &stop; }
        return distance + (after ? metric(*previous, *after) : 0.f);
    };
    const auto complete = [&](const std::vector<Stop>& stops) {
        const auto fog = covered(stops);
        return std::ranges::all_of(required, [&](uint32_t cell) { return fog.contains(cell); });
    };
    float best_cost = stats.before = cost(route, start, finish);
    const auto lower_bound = [](const Stop& a, const Stop& b) {
        return std::hypot(a.pos.x - b.pos.x, a.pos.y - b.pos.y);
    };
    const auto running = [&] {
        check();
        if (keep_going()) return true;
        stats.budget_exhausted = true;
        return false;
    };
    const bool has_preserved = std::ranges::any_of(route, [](const Stop& stop) { return stop.preserved; });
    const auto accept = [&](std::vector<Stop> trial, const float margin) {
        if (!complete(trial)) return false;
        const float trial_cost = cost(trial, start, finish);
        if (!std::isfinite(trial_cost) || trial_cost + margin >= best_cost) return false;
        route = std::move(trial);
        best_cost = trial_cost;
        return true;
    };
    const auto construct = [&](const std::vector<Stop>& choices, FogSet uncovered, int strategy, uint32_t trial_seed, size_t limit) {
        std::mt19937 random(trial_seed);
        std::vector<Stop> stops;
        while (!uncovered.empty() && stops.size() < limit && running()) {
            float best_score = -1.f;
            const Stop* best = nullptr;
            for (const auto& option : choices) {
                check();
                size_t gain = 0;
                for (const auto cell : option.reveals) gain += uncovered.contains(cell);
                if (!gain) continue;
                float score = static_cast<float>(gain);
                if (strategy == 1) score /= std::sqrt(1.f + option.approach_cost / 3072.f);
                // Jitter breaks coverage ties and supplies independent cover sets.
                if (strategy == 2) score *= .75f + static_cast<float>(random() % 1000) / 2000.f;
                if (score <= best_score) continue;
                best_score = score;
                best = &option;
            }
            if (!best) break;
            stops.push_back(*best);
            for (const auto cell : best->reveals) uncovered.erase(cell);
        }
        if (!uncovered.empty()) stops.clear();
        return stops;
    };
    if (passes <= 0 || route.empty()) { stats.after = best_cost; return route; }
    for (int pass = 0; pass < std::clamp(passes, 0, 8) && running(); ++pass) {
        bool improved = false;
        ++stats.passes;
        // Existing legs are cached. Spend the budget on the greatest detours
        // first, rather than exhausting it near the beginning of the route.
        std::vector<size_t> priority(route.size());
        std::iota(priority.begin(), priority.end(), 0);
        std::vector<float> detours(route.size());
        for (size_t i = 0; i < route.size(); ++i) {
            const auto& before = i ? route[i - 1] : start;
            const Stop* after = i + 1 < route.size() ? &route[i + 1] : finish;
            detours[i] = metric(before, route[i]) + (after ? metric(route[i], *after) - lower_bound(before, *after) : 0.f);
        }
        std::stable_sort(priority.begin(), priority.end(), [&](size_t a, size_t b) { return detours[a] > detours[b]; });
        // Coordinate descent also samples alternative cells/footings covering the
        // stop's unique targets. Preserved stops move only for a material saving.
        const size_t move_count = route.size();
        for (size_t step = 0; step < move_count && running(); ++step) {
            const size_t i = priority[(step + static_cast<size_t>(pass) * move_count) % priority.size()];
            FogSet unique(route[i].reveals.begin(), route[i].reveals.end());
            for (size_t j = 0; j < route.size(); ++j)
                if (j != i) for (const auto cell : route[j].reveals) unique.erase(cell);
            const Stop before = i ? route[i - 1] : start;
            const Stop* after = i + 1 < route.size() ? &route[i + 1] : finish;
            const float old_leg = metric(before, route[i]) + (after ? metric(route[i], *after) : 0.f);
            Stop best = route[i];
            float best_leg = old_leg;
            const float margin = route[i].preserved ? std::max(1024.f, old_leg * .05f) : .1f;
            const auto consider = [&](const Stop& option) {
                if (!std::ranges::all_of(unique, [&](uint32_t cell) {
                    return std::ranges::find(option.reveals, cell) != option.reveals.end();
                })) return;
                const float bound = lower_bound(before, option) + (after ? lower_bound(option, *after) : 0.f);
                if (bound + margin >= old_leg || bound + .1f >= best_leg) return;
                const float approach = metric(before, option);
                if (approach + (after ? lower_bound(option, *after) : 0.f) + .1f >= best_leg) return;
                const float distance = approach + (after ? metric(option, *after) : 0.f);
                if (distance + .1f < best_leg) { best = option; best_leg = distance; }
            };
            for (const auto& option : placements(route[i], before, after)) { if (!running()) break; consider(option); }
            if (best_leg + margin < old_leg) {
                route[i] = std::move(best);
                best_cost = cost(route, start, finish);
                ++stats.moves;
                improved = true;
            }
        }

        // Destroy/rebuild contiguous windows. This combines deletion, merging,
        // different cover choices and exact ordering within up to eight visits.
        for (const size_t width : {size_t{2}, size_t{3}, size_t{5}, size_t{8}}) {
            if (width > route.size()) continue;
            std::vector<size_t> windows(route.size() - width + 1);
            std::iota(windows.begin(), windows.end(), 0);
            std::vector<float> excess(windows.size());
            for (const auto first : windows) {
                const auto& before = first ? route[first - 1] : start;
                const Stop* after = first + width < route.size() ? &route[first + width] : finish;
                std::vector<Stop> section(route.begin() + first, route.begin() + first + width);
                excess[first] = cost(section, before, after) - (after ? lower_bound(before, *after) : 0.f);
            }
            std::stable_sort(windows.begin(), windows.end(), [&](size_t a, size_t b) { return excess[a] > excess[b]; });
            for (const size_t first : windows) {
                if (!running()) break;
                if (first + width > route.size()) continue;
                FogSet needed;
                for (size_t i = first; i < first + width; ++i)
                    needed.insert(route[i].reveals.begin(), route[i].reveals.end());
                for (size_t i = 0; i < route.size(); ++i)
                    if (i < first || i >= first + width) for (const auto cell : route[i].reveals) needed.erase(cell);
                const Stop before = first ? route[first - 1] : start;
                const Stop* after = first + width < route.size() ? &route[first + width] : finish;
                std::vector<Stop> choices;
                for (const auto& option : pool) {
                    check();
                    if (std::ranges::any_of(option.reveals, [&](uint32_t cell) { return needed.contains(cell); })) choices.push_back(option);
                }
                for (size_t i = first; i < first + width; ++i) choices.push_back(route[i]);
                bool preserved = false;
                std::vector<Stop> old(route.begin() + first, route.begin() + first + width);
                for (const auto& stop : old) preserved |= stop.preserved;
                const float old_cost = cost(old, before, after);
                for (int strategy = 0; strategy < 3 && running(); ++strategy) {
                    auto replacement = construct(choices, needed, strategy, seed + static_cast<uint32_t>(first + pass * 97 + strategy), width);
                    if (!needed.empty() && replacement.empty()) continue;
                    if (!running()) break;
                    replacement = ordering(std::move(replacement), before, after);
                    const float replacement_cost = cost(replacement, before, after);
                    const float margin = preserved ? std::max(1024.f, old_cost * .05f) : .1f;
                    if (!std::isfinite(replacement_cost) || replacement_cost + margin >= old_cost) continue;
                    auto trial = route;
                    trial.erase(trial.begin() + first, trial.begin() + first + width);
                    trial.insert(trial.begin() + first, replacement.begin(), replacement.end());
                    if (accept(std::move(trial), margin)) {
                        stats.merges += static_cast<uint32_t>(width - replacement.size());
                        ++stats.repairs;
                        improved = true;
                        break;
                    }
                }
            }
        }
        if (running() && improved) {
            auto reordered = ordering(route, start, finish);
            const float margin = has_preserved ? std::max(1024.f, best_cost * .05f) : .1f;
            accept(std::move(reordered), margin);
        }
        if (!improved) break;
    }
    // Compare complete covers, not just permutations of a single set of stops.
    for (int trial = 0; trial < std::clamp(seeds, 0, 12) && running(); ++trial) {
        auto alternative = construct(pool, required, trial % 3, seed + 0x9e3779b9u * (trial + 1), 1024);
        if (alternative.empty() || alternative.size() > 128 || !running()) continue;
        alternative = ordering(std::move(alternative), start, finish);
        ++stats.trials;
        const float margin = has_preserved ? std::max(1024.f, best_cost * .05f) : .1f;
        const size_t old_size = route.size();
        if (accept(std::move(alternative), margin)) {
            if (route.size() < old_size) stats.merges += static_cast<uint32_t>(old_size - route.size());
            ++stats.repairs;
        }
    }

    stats.after = best_cost;
    return route;
}
}
