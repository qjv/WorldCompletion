#!/usr/bin/env python3
"""Replay captured game inputs through the production planner on Linux."""
import argparse
import gzip
import hashlib
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("planner_fixture", ROOT / "tests/run_tests.py")
fixture = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fixture)

MAIN = r'''
#include "plugins/WorldCompletion/ReplaySnapshot.h"
#include <cstdlib>
int main(int argc, char** argv) {
    try {
        if (argc != 5) return 2;
        CartographyData::Mask mask{};
        std::vector<uint8_t> bytes;
        auto snapshot = completion::LoadReplaySnapshot<BuildSnapshot>(argv[1], mask, bytes);
        snapshot.replay_budget_ms = std::stoi(argv[2]);
        if (const auto* samples = std::getenv("WC_PORTAL_SAMPLES")) snapshot.navigation_samples = std::atoi(samples);
        std::atomic_bool cancelled{false};
        const auto dump = [&](const char* name, const BuildResult& result) {
            if (!std::getenv("WC_DUMP_ROUTE")) return;
            std::cout << name << " origin=" << snapshot.player.x << ',' << snapshot.player.y << " finish=" << snapshot.end_portal << '\n';
            for (size_t i = 0; i < result.waypoints.size(); ++i) {
                const auto& p = result.waypoints[i];
                std::cout << "stop " << i + 1 << ' ' << p.x << ' ' << p.y << " floor=" << p.zplane << " fog=";
                for (const auto fog : result.reveals[i]) std::cout << fog << ',';
                std::cout << '\n';
            }
        };
        const auto passes = snapshot.relaxation;
        std::cout << "map=" << static_cast<uint32_t>(snapshot.map)
                  << " reveal_radius=" << snapshot.reveal_radius
                  << " planes=" << snapshot.maps.size() << " passes=" << passes << '\n';
        snapshot.relaxation = 0;
        BuildResult baseline;
        auto begin = std::chrono::steady_clock::now();
        BuildRoute(snapshot, baseline, cancelled);
        const auto baseline_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        snapshot.relaxation = std::getenv("WC_ONLY_COVERING") ? 0 : passes;
        BuildResult improved;
        begin = std::chrono::steady_clock::now();
        BuildRoute(snapshot, improved, cancelled);
        const auto improved_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        if (!baseline.connected || !improved.connected) throw std::runtime_error("Replay did not produce a connected route");
        std::unordered_set<uint32_t> required, covered;
        for (const auto& fog : baseline.reveals) required.insert(fog.begin(), fog.end());
        for (const auto& fog : improved.reveals) covered.insert(fog.begin(), fog.end());
        size_t missing = 0;
        for (const auto fog : required) missing += !covered.contains(fog);
        std::cout << "baseline_length=" << baseline.length << " baseline_stops=" << baseline.waypoints.size()
                  << " improved_length=" << improved.length << " improved_stops=" << improved.waypoints.size()
                  << " required_fog=" << required.size() << " missing_fog=" << missing
                  << " moves=" << improved.refinement.moves << " merges=" << improved.refinement.merges
                  << " repairs=" << improved.refinement.repairs << " trials=" << improved.refinement.trials
                  << " budget_exhausted=" << improved.refinement.budget_exhausted << '\n';
        std::cout << "baseline_ms=" << baseline_ms << " refined_ms=" << improved_ms << '\n';
        dump("previous", improved);
        if (std::stoi(argv[4]) == 2) {
            snapshot.covering_planner = true;
            snapshot.relaxation = passes;
            const auto parameter = [](const char* name, int fallback) { const auto* value = std::getenv(name); return value ? std::atoi(value) : fallback; };
            snapshot.covering_seed = static_cast<uint32_t>(parameter("WC_SEARCH_SEED", 0));
            snapshot.covering_shortlist = parameter("WC_SHORTLIST", 12);
            snapshot.covering_removal = parameter("WC_REMOVAL", 4);
            snapshot.covering_project = parameter("WC_PROJECT", 0) != 0;
            BuildResult covering;
            begin = std::chrono::steady_clock::now();
            BuildRoute(snapshot, covering, cancelled);
            const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            std::unordered_set<uint32_t> covering_fog;
            for (const auto& fog : covering.reveals) covering_fog.insert(fog.begin(), fog.end());
            size_t dropped = 0;
            for (const auto fog : required) dropped += !covering_fog.contains(fog);
            std::cout << "covering_length=" << covering.length << " covering_stops=" << covering.waypoints.size()
                      << " covering_ms=" << ms << " covering_missing_fog=" << dropped
                      << " covering_trials=" << covering.refinement.trials << " accepted=" << covering.refinement.repairs << '\n';
            dump("covering", covering);
            if (!covering.connected || dropped) return 1;
        }
        if (std::stoi(argv[4]) == 1) {
            snapshot.region_planner = true;
            snapshot.relaxation = 0;
            BuildResult region;
            begin = std::chrono::steady_clock::now();
            BuildRoute(snapshot, region, cancelled);
            const auto region_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            std::unordered_set<uint32_t> region_fog;
            for (const auto& fog : region.reveals) region_fog.insert(fog.begin(), fog.end());
            size_t region_missing = 0;
            for (const auto fog : required) region_missing += !region_fog.contains(fog);
            std::cout << "region_length=" << region.length << " region_stops=" << region.waypoints.size()
                      << " region_ms=" << region_ms << " regions=" << region.regions
                      << " patterns=" << region.region_patterns << " region_missing_fog=" << region_missing << '\n';
            if (!region.connected || region_missing || !region.regions) return 1;
            snapshot.relaxation = passes;
            BuildResult hybrid;
            begin = std::chrono::steady_clock::now();
            BuildRoute(snapshot, hybrid, cancelled);
            const auto hybrid_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            std::unordered_set<uint32_t> hybrid_fog;
            for (const auto& fog : hybrid.reveals) hybrid_fog.insert(fog.begin(), fog.end());
            size_t hybrid_missing = 0;
            for (const auto fog : required) hybrid_missing += !hybrid_fog.contains(fog);
            std::cout << "hybrid_length=" << hybrid.length << " hybrid_stops=" << hybrid.waypoints.size()
                      << " hybrid_ms=" << hybrid_ms << " hybrid_missing_fog=" << hybrid_missing << '\n';
            if (!hybrid.connected || hybrid_missing || !hybrid.regions) return 1;
        }
        const auto max_ratio = std::stof(argv[3]);
        return missing || improved.length > baseline.length * max_ratio ? 1 : 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
'''

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--budget-ms", type=int, default=0,
                        help="Override production budget; 0 uses captured quality settings")
    parser.add_argument("--max-ratio", type=float, default=1.0,
                        help="Fail if refined distance exceeds this fraction of baseline")
    parser.add_argument("--compare-region", action="store_true",
                        help="Benchmark the region/sweep prototype without local refinement")
    parser.add_argument("--compare-covering", action="store_true",
                        help="Benchmark coverage-aware large neighborhood search")
    args = parser.parse_args()
    code = fixture.stubs + "\n" + "\n\n".join(
        fixture.block(marker) + ("\nstd::vector<DiscoveryVisit> pending_discovery;"
        if marker == "struct DiscoveryVisit {" else "") for marker in fixture.markers) + MAIN
    if os.getenv("WC_COMPARE_ORDER"):
        code = '#include <fstream>\n' + code.replace('        const auto order_stops =', r'''
        std::vector<Candidate> captured_order;
        for (const auto& point : snapshot.previous_waypoints) {
            const auto found = std::ranges::find_if(candidates, [&](const Candidate& c) {
                return c.preserved && c.pos.zplane == point.zplane && hypotf(c.pos.x - point.x, c.pos.y - point.y) < 1.f;
            });
            if (found != candidates.end()) captured_order.push_back(*found);
        }
        const auto captured_cost = [&](const std::vector<Candidate>& stops) {
            float distance = 0; const Candidate* previous = &start_stop;
            for (const auto& stop : stops) { distance += metric(*previous, stop); previous = &stop; }
            return distance + (end_trap ? metric(*previous, finish_stop) : 0.f);
        };
        if (captured_order.size() >= 3) {
            if (const auto* path = std::getenv("WC_EXPORT_MESH")) {
                std::ofstream out(path);
                for (size_t plane = 0; plane < maps->size(); ++plane)
                    for (const auto& t : (*maps)[plane].trapezoids)
                        out << "trap," << plane << ',' << t.XTL << ',' << t.XTR << ',' << t.YT << ',' << t.XBL << ',' << t.XBR << ',' << t.YB << '\n';
                out << "stop,0," << start_stop.pos.x << ',' << start_stop.pos.y << '\n';
                for (size_t i = 0; i < captured_order.size(); ++i)
                    out << "stop," << i + 1 << ',' << captured_order[i].pos.x << ',' << captured_order[i].pos.y << '\n';
                const auto export_leg = [&](const Candidate& a, const Candidate& b, const char* name) {
                    std::vector<GW::GamePos> vertices;
                    if (make_leg(a.trap, a.pos, b.trap, b.pos, vertices))
                        for (const auto& p : vertices) out << "vertex," << name << ',' << p.x << ',' << p.y << '\n';
                };
                export_leg(captured_order[0], captured_order[2], "1-3");
                export_leg(captured_order[0], captured_order[1], "1-2");
                export_leg(captured_order[1], captured_order[2], "2-3");
            }
            std::cout << "captured_order_length=" << captured_cost(captured_order) << '\n';
            const auto inspect_leg = [&](const Candidate& a, const Candidate& b, const char* label) {
                const float direct = hypotf(b.pos.x - a.pos.x, b.pos.y - a.pos.y);
                const size_t samples = static_cast<size_t>(direct / 128.f) + 2;
                size_t outside = 0, outside_all = 0, outside_raw = 0;
                for (size_t step = 0; step <= samples; ++step) {
                    const float t = static_cast<float>(step) / samples;
                    const GW::Vec2f p{a.pos.x + (b.pos.x - a.pos.x) * t, a.pos.y + (b.pos.y - a.pos.y) * t};
                    bool covered = false, any_floor = false, any_raw = false;
                    for (size_t plane = 0; plane < maps->size(); ++plane) {
                        for (const auto& trap : (*maps)[plane].trapezoids) if (Contains(&trap, p)) {
                            any_raw = true;
                            if (!(*maps)[plane].blocked) { any_floor = true; covered |= plane == a.pos.zplane; }
                            break;
                        }
                    }
                    outside += !covered;
                    outside_all += !any_floor;
                    outside_raw += !any_raw;
                }
                std::cout << "leg " << label << " straight=" << direct << " road=" << metric(a,b)
                          << " reverse_road=" << metric(b,a) << " straight_outside_mesh=" << outside << '/' << samples + 1
                          << " straight_outside_all_floors=" << outside_all
                          << " straight_outside_raw_capture=" << outside_raw
                          << " straight_crosses_barrier=" << !avoids_doors({a.pos.x,a.pos.y},{b.pos.x,b.pos.y},a.pos.zplane) << '\n';
            };
            inspect_leg(start_stop, captured_order[0], "start-1");
            inspect_leg(start_stop, captured_order[1], "start-2");
            inspect_leg(captured_order[0], captured_order[1], "1-2");
            inspect_leg(captured_order[0], captured_order[2], "1-3");
            inspect_leg(captured_order[1], captured_order[2], "2-3");
            for (size_t i = 0; i < captured_order.size(); ++i)
                std::cout << "captured_stop " << i + 1 << ' ' << captured_order[i].pos.x << ' ' << captured_order[i].pos.y << '\n';
            std::swap(captured_order[0], captured_order[1]);
            std::cout << "swapped_213_length=" << captured_cost(captured_order) << '\n';
            std::swap(captured_order[0], captured_order[1]);
        }
        const auto order_stops =''').replace('        auto coverage_pool = candidates;', r'''
        if (captured_order.size() <= 15 && !captured_order.empty()) {
            const auto exact = order_stops(captured_order, start_stop, end_trap ? &finish_stop : nullptr);
            std::cout << "exact_captured_order_length=" << captured_cost(exact) << '\n';
            for (const auto& stop : exact) {
                const auto hit = std::ranges::find_if(captured_order, [&](const Candidate& c) { return c.pos.x == stop.pos.x && c.pos.y == stop.pos.y && c.pos.zplane == stop.pos.zplane; });
                std::cout << "exact_original_point=" << std::distance(captured_order.begin(), hit) + 1 << '\n';
            }
        }
        auto coverage_pool = candidates;''')
    if os.getenv("WC_TRACE_COVERAGE"):
        code = '#include <cstdlib>\n' + code.replace('        const auto place_stop =', r'''
        if (!refined_stops.empty()) for (const auto fog : refined_stops.front().reveals) {
            float closest = std::numeric_limits<float>::infinity();
            const Candidate* best = nullptr;
            for (const auto& option : coverage_pool) {
                if (std::ranges::find(option.reveals, fog) == option.reveals.end()) continue;
                const float distance = hypotf(option.pos.x - start_stop.pos.x, option.pos.y - start_stop.pos.y);
                if (distance < closest) { closest = distance; best = &option; }
            }
            if (best) std::cout << "target " << fog << " closest=" << closest << " stand=" << best->pos.x << ',' << best->pos.y << '\n';
        }
        const auto place_stop =''')
    with tempfile.TemporaryDirectory(prefix="worldcompletion-replay-") as folder:
        cpp = Path(folder) / "replay.cpp"
        digest = hashlib.sha256(code.encode())
        for header in sorted((ROOT / "plugins/WorldCompletion").glob("*.h")):
            digest.update(header.name.encode()); digest.update(header.read_bytes())
        cache = Path(tempfile.gettempdir()) / "worldcompletion-replay-cache"
        cache.mkdir(exist_ok=True)
        binary = cache / digest.hexdigest()
        cpp.write_text(code)
        snapshot = args.snapshot.resolve()
        if snapshot.suffix == ".gz":
            decompressed = Path(folder) / "snapshot.wcrp"
            decompressed.write_bytes(gzip.decompress(snapshot.read_bytes()))
            snapshot = decompressed
        if not binary.exists():
            subprocess.run(["g++", "-std=c++20", "-O2", "-I", str(ROOT), str(cpp), "-o", str(binary)], check=True)
        mode = 2 if args.compare_covering else int(args.compare_region)
        subprocess.run([str(binary), str(snapshot), str(args.budget_ms), str(args.max_ratio), str(mode)], check=True)

if __name__ == "__main__":
    main()
