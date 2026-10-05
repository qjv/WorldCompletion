#pragma once
#include "CoverageOptimizer.h"
#include <array>
#include <bit>
#include <map>

namespace completion {
// Coverage-aware destroy/repair for an open incumbent. A proposal can change its
// footing set and order together; only the best complete measured incumbent escapes
// this function. Exploring a temporary worse incumbent never changes the live road.
template<class Stop, class Metric, class Check, class Continue, class Place>
std::vector<Stop> ImproveCoveringTour(std::vector<Stop> incumbent, const std::vector<Stop>& pool,
    const Stop& start, const Stop* finish, uint32_t seed, Metric metric,
    Check check, Continue running, CoverageStats& stats, Place placements,
    int shortlist_limit = 12, int removal_size = 4, bool project = false)
{
    using Mask = std::vector<uint64_t>;
    std::unordered_map<uint32_t, size_t> dense;
    for (const auto& stop : incumbent) for (const auto fog : stop.reveals) dense.try_emplace(fog, dense.size());
    const size_t words = (dense.size() + 63) / 64;
    const auto mask_for = [&](const Stop& stop) {
        Mask bits(words);
        for (const auto fog : stop.reveals) if (const auto hit = dense.find(fog); hit != dense.end())
            bits[hit->second / 64] |= uint64_t{1} << (hit->second % 64);
        return bits;
    };
    Mask required(words, ~uint64_t{0});
    if (dense.size() % 64) required.back() = (uint64_t{1} << (dense.size() % 64)) - 1;
    std::vector<Mask> masks;
    std::vector<size_t> incidence(dense.size());
    for (const auto& stop : pool) {
        masks.push_back(mask_for(stop));
        for (const auto fog : stop.reveals) if (const auto hit = dense.find(fog); hit != dense.end()) ++incidence[hit->second];
    }
    const auto count = [](const Mask& bits) { size_t n = 0; for (const auto word : bits) n += std::popcount(word); return n; };
    const auto gain = [&](const Mask& a, const Mask& b) { size_t n = 0; for (size_t i = 0; i < words; ++i) n += std::popcount(a[i] & b[i]); return n; };
    const auto uncovered = [&](const std::vector<Stop>& visits) {
        Mask missing = required;
        for (const auto& stop : visits) {
            const auto bits = mask_for(stop);
            for (size_t i = 0; i < words; ++i) missing[i] &= ~bits[i];
        }
        return missing;
    };
    const auto cost = [&](const std::vector<Stop>& visits) {
        float total = 0;
        const Stop* previous = &start;
        for (const auto& stop : visits) { check(); total += metric(*previous, stop); previous = &stop; }
        return total + (finish ? metric(*previous, *finish) : 0.f);
    };
    const auto straight = [](const Stop& a, const Stop& b) { return std::hypot(a.pos.x - b.pos.x, a.pos.y - b.pos.y); };
    float best_cost = stats.before = cost(incumbent);
    const size_t initial_size = incumbent.size();
    stats.after = best_cost;
    if (incumbent.empty() || dense.empty()) return incumbent;
    auto working = incumbent;
    float working_cost = best_cost;
    std::mt19937 random(seed);
    std::array<float, 3> weights{1, 1, 1};
    const auto active = [&] { check(); if (running()) return true; stats.budget_exhausted = true; return false; };
    for (size_t trial = 0; trial < 160 && active(); ++trial) {
        ++stats.trials;
        const size_t n = working.size();
        if (!n) break;
        std::discrete_distribution<size_t> choose(weights.begin(), weights.end());
        const size_t method = trial < 3 ? trial : choose(random);
        const size_t remove_count = std::min(n, removal_size > 0 ? static_cast<size_t>(removal_size) : size_t{3} + random() % 7);
        std::vector<size_t> ranking(n);
        std::iota(ranking.begin(), ranking.end(), 0);
        if (method == 0) {
            const auto& centre = working[random() % n];
            std::stable_sort(ranking.begin(), ranking.end(), [&](size_t a, size_t b) { return straight(centre, working[a]) < straight(centre, working[b]); });
        } else if (method == 1) {
            std::vector<float> detour(n);
            for (size_t i = 0; i < n; ++i) {
                const auto& before = i ? working[i - 1] : start;
                const Stop* after = i + 1 < n ? &working[i + 1] : finish;
                detour[i] = metric(before, working[i]) + (after ? metric(working[i], *after) - straight(before, *after) : 0.f);
            }
            std::stable_sort(ranking.begin(), ranking.end(), [&](size_t a, size_t b) { return detour[a] > detour[b]; });
        } else {
            const size_t first = random() % n;
            std::rotate(ranking.begin(), ranking.begin() + static_cast<ptrdiff_t>(first), ranking.end());
        }
        std::vector<bool> removed(n);
        for (size_t i = 0; i < remove_count; ++i) removed[ranking[i]] = true;
        std::vector<Stop> proposal;
        for (size_t i = 0; i < n; ++i) if (!removed[i]) proposal.push_back(working[i]);
        auto missing = uncovered(proposal);
        const bool rare_first = trial % 3 == 1;
        bool repaired = true;
        for (size_t insertion = 0; count(missing) && insertion < 32 && active(); ++insertion) {
            size_t rare = dense.size();
            if (rare_first) for (size_t i = 0; i < incidence.size(); ++i)
                if ((missing[i / 64] & (uint64_t{1} << (i % 64))) && (rare == dense.size() || incidence[i] < incidence[rare])) rare = i;
            std::vector<float> edges(proposal.size() + 1);
            for (size_t i = 0; i < edges.size(); ++i) {
                const auto& before = i ? proposal[i - 1] : start;
                const Stop* after = i < proposal.size() ? &proposal[i] : finish;
                edges[i] = after ? metric(before, *after) : 0.f;
            }
            struct Insertion { size_t candidate, position, coverage; float score; };
            std::vector<Insertion> shortlist;
            for (size_t candidate = 0; candidate < pool.size(); ++candidate) {
                if (rare != dense.size() && !(masks[candidate][rare / 64] & (uint64_t{1} << (rare % 64)))) continue;
                const size_t coverage = gain(masks[candidate], missing);
                if (!coverage) continue;
                std::array<Insertion, 2> positions{{{candidate, 0, coverage, std::numeric_limits<float>::infinity()},
                    {candidate, 0, coverage, std::numeric_limits<float>::infinity()}}};
                for (size_t position = 0; position <= proposal.size(); ++position) {
                    const auto& before = position ? proposal[position - 1] : start;
                    const Stop* after = position < proposal.size() ? &proposal[position] : finish;
                    const float extra = straight(before, pool[candidate]) + (after ? straight(pool[candidate], *after) : 0.f) - edges[position];
                    const float score = extra / static_cast<float>(coverage);
                    if (score < positions[0].score) { positions[1] = positions[0]; positions[0] = {candidate, position, coverage, score}; }
                    else if (score < positions[1].score) positions[1] = {candidate, position, coverage, score};
                }
                shortlist.push_back(positions[0]);
                if (proposal.size()) shortlist.push_back(positions[1]);
            }
            if (shortlist.empty()) { repaired = false; break; }
            std::sort(shortlist.begin(), shortlist.end(),
                [](const Insertion& a, const Insertion& b) { return a.score < b.score; });
            std::map<Mask, size_t> represented;
            std::vector<Insertion> distinct;
            for (const auto& option : shortlist) {
                auto contribution = masks[option.candidate];
                for (size_t w = 0; w < words; ++w) contribution[w] &= missing[w];
                if (represented[contribution]++ >= 2) continue;
                distinct.push_back(option);
                if (distinct.size() >= static_cast<size_t>(std::clamp(shortlist_limit, 2, 64))) break;
            }
            shortlist = std::move(distinct);
            const size_t limit = shortlist.size();
            Insertion winner{};
            float best_score = std::numeric_limits<float>::infinity();
            bool found = false;
            for (size_t i = 0; i < limit && active(); ++i) {
                const auto option = shortlist[i];
                if (option.score >= best_score) continue;
                const auto& before = option.position ? proposal[option.position - 1] : start;
                const Stop* after = option.position < proposal.size() ? &proposal[option.position] : finish;
                const float extra = metric(before, pool[option.candidate]) + (after ? metric(pool[option.candidate], *after) : 0.f) - edges[option.position];
                float score = extra / static_cast<float>(option.coverage);
                if (trial % 3 == 2) score += std::fabs(score) * static_cast<float>(random() % 1000) / 5000.f;
                if (score < best_score) { best_score = score; winner = option; found = true; }
            }
            if (!found) { repaired = false; break; }
            Stop inserted = pool[winner.candidate];
            const auto& before = winner.position ? proposal[winner.position - 1] : start;
            const Stop* after = winner.position < proposal.size() ? &proposal[winner.position] : finish;
            float inserted_cost = metric(before, inserted) + (after ? metric(inserted, *after) : 0.f);
            const auto alternatives = project ? placements(inserted, before, after) : std::vector<Stop>{};
            for (size_t i = 0; i < std::min(size_t{10}, alternatives.size()) && active(); ++i) {
                const auto bits = mask_for(alternatives[i]);
                bool retains = true;
                for (size_t w = 0; w < words; ++w) retains &= !(masks[winner.candidate][w] & ~bits[w]);
                if (!retains) continue;
                const float bound = straight(before, alternatives[i]) + (after ? straight(alternatives[i], *after) : 0.f);
                if (bound + .1f >= inserted_cost) continue;
                const float distance = metric(before, alternatives[i]) + (after ? metric(alternatives[i], *after) : 0.f);
                if (distance + .1f < inserted_cost) { inserted = alternatives[i]; inserted_cost = distance; }
            }
            const auto inserted_mask = mask_for(inserted);
            proposal.insert(proposal.begin() + static_cast<ptrdiff_t>(winner.position), std::move(inserted));
            for (size_t i = 0; i < words; ++i) missing[i] &= ~inserted_mask[i];
        }
        if (!repaired || count(missing) || !active()) continue;
        const float proposal_cost = cost(proposal);
        if (!std::isfinite(proposal_cost)) continue;
        if (proposal_cost + .1f < best_cost) {
            incumbent = proposal; best_cost = proposal_cost; ++stats.repairs; weights[method] = std::min(12.f, weights[method] + 1.f);
        } else weights[method] = std::max(.25f, weights[method] * .97f);
        // Small temporary uphill moves can escape a poor cover set. Keep the
        // independently tracked incumbent for publication and reset periodically.
        if (proposal_cost < working_cost || ((random() % 8) == 0 && proposal_cost < best_cost * 1.025f)) {
            working = std::move(proposal); working_cost = proposal_cost;
        }
        if (trial % 12 == 11) { working = incumbent; working_cost = best_cost; }
    }
    // Clean up the published incumbent once, rather than paying for pruning
    // every trial or changing the search trajectory after each repair.
    std::vector<size_t> covered(dense.size());
    for (const auto& stop : incumbent) for (const auto fog : stop.reveals)
        if (const auto hit = dense.find(fog); hit != dense.end()) ++covered[hit->second];
    for (size_t i = 0; i < incumbent.size() && active();) {
        bool redundant = true;
        for (const auto fog : incumbent[i].reveals)
            if (const auto hit = dense.find(fog); hit != dense.end() && covered[hit->second] <= 1) redundant = false;
        if (!redundant) { ++i; continue; }
        const auto& before = i ? incumbent[i - 1] : start;
        const Stop* after = i + 1 < incumbent.size() ? &incumbent[i + 1] : finish;
        const float old_cost = metric(before, incumbent[i]) + (after ? metric(incumbent[i], *after) : 0.f);
        const float shortcut = after ? metric(before, *after) : 0.f;
        if (!std::isfinite(shortcut) || shortcut > old_cost + .1f) { ++i; continue; }
        for (const auto fog : incumbent[i].reveals)
            if (const auto hit = dense.find(fog); hit != dense.end()) --covered[hit->second];
        incumbent.erase(incumbent.begin() + static_cast<ptrdiff_t>(i));
        best_cost += shortcut - old_cost;
    }
    stats.after = best_cost;
    stats.merges = initial_size > incumbent.size() ? static_cast<uint32_t>(initial_size - incumbent.size()) : 0;
    return incumbent;
}
}
