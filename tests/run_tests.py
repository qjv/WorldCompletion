#!/usr/bin/env python3
"""Exercise the production planner on Linux with POD stand-ins for game/GUI data."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "plugins/WorldCompletion/WorldCompletion.cpp").read_text()


def block(marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if source[end:end + 1] == ";":
        end += 1
    return source[start:end]


stubs = r"""
#include <array>
#include <atomic>
#include <cassert>
#include <cfloat>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <ranges>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include "plugins/WorldCompletion/RoutePlanner.h"
#include "plugins/WorldCompletion/VisitHistory.h"
#include "plugins/WorldCompletion/PortalBarrier.h"
#include "plugins/WorldCompletion/PortalLocations.h"
namespace GW {
    struct Vec2f { float x = 0, y = 0; };
    struct GamePos { float x = 0, y = 0; uint32_t zplane = 0; };
    struct PathingTrapezoid { float XTL, XTR, YT, XBL, XBR, YB; };
    namespace Constants {
        enum class MapID { None, The_Dragons_Lair, Sorrows_Furnace, The_Undercity, Dragons_Throat, Dragons_Throat_area__What_Waits_in_Shadow, The_Deep, Urgozs_Warren, Secure_the_Refuge, Secure_the_Refuge_cinematic, Sunspear_Sanctuary_outpost, Command_Post, Moddok_Crevice, Bahdok_Caverns, Dasha_Vestibule, The_Hidden_City_of_Ahdashim, Hidden_City_of_Ahdashim_cinematic };
        enum class InstanceType { Loading, Explorable, Outpost };
        enum class SkillID : uint32_t {};
    }
    struct Skill { uint32_t name = 0; };
    struct Effect { Constants::SkillID skill_id; };
    struct EffectArray : std::vector<Effect> {
        bool available = true;
        bool valid() const { return available; }
    };
    namespace Effects {
        EffectArray effects;
        EffectArray* GetPlayerEffects() { return &effects; }
    }
    namespace SkillbarMgr {
        std::map<Constants::SkillID, Skill> skills;
        const Skill* GetSkillConstantData(Constants::SkillID id) {
            const auto found = skills.find(id);
            return found == skills.end() ? nullptr : &found->second;
        }
    }
}
struct ImRect { GW::Vec2f Min, Max; };
namespace GW {
    enum class RegionType { Outpost, Dungeon };
    enum Region { Region_Kryta, Region_Presearing, Region_DepthsOfTyria };
    enum class Continent { Tyria, RealmOfTorment };
    struct AreaInfo {
        RegionType type = RegionType::Outpost;
        Region region = Region_Kryta;
        Continent continent = Continent::Tyria;
        bool on_map = true, guild_hall = false;
        bool GetIsOnWorldMap() const { return on_map; }
        bool GetIsGuildHall() const { return guild_hall; }
    };
    namespace Map {
        bool loaded = true;
        Constants::InstanceType instance = Constants::InstanceType::Outpost;
        uint32_t id = 1000;
        AreaInfo info;
        bool GetIsMapLoaded() { return loaded; }
        Constants::InstanceType GetInstanceType() { return instance; }
        Constants::MapID GetMapID() { return static_cast<Constants::MapID>(id); }
        const AreaInfo* GetMapInfo(Constants::MapID) { return &info; }
    }
}
GW::Constants::MapID CartographyMapID() { return GW::Map::GetMapID(); }
bool GetMapBounds(const GW::AreaInfo*, ImRect& bounds) { bounds = {{0, 0}, {64, 64}}; return true; }

namespace CartographyData {
    struct Mask { int x0, y0, width, height, byte_count; const uint8_t* bits; };
}
uint8_t credit_bits = 1;
CartographyData::Mask credit_mask{0, 0, 2, 2, 1, &credit_bits};
const CartographyData::Mask* CreditableMask(const GW::AreaInfo*) { return &credit_mask; }
constexpr float kGwinchesPerWorldUnit = 96.f;
constexpr float kWorldUnitsPerCell = 32.f;
std::vector<GW::GamePos> route;
size_t route_progress = 0;
std::vector<std::vector<uint32_t>> route_waypoint_reveals;
std::vector<size_t> route_waypoint_indices;
std::vector<bool> route_waypoint_visited;
std::unordered_set<uint32_t> skipped_fog, explored_fog;
bool FogIndexExplored(uint32_t fog) { return explored_fog.contains(fog); }
bool history_dirty = false;
completion::MapVisits* fixture_visits = nullptr;
completion::MapVisits* CurrentVisits() { return fixture_visits; }
"""
markers = [
    "struct DiscoveryVisit {", "void RememberDiscovered(", "bool WaypointNeedsVisit(", "size_t CurrentRouteEnd(", "bool ReviewDiscovery(",
    "uint32_t BirdsEyeViewEffect(",
    "int FogCellX(", "int FogCellY(", "bool FogCellWithinBounds(", "bool DiscoveryCellAllowed(", "bool MaskContains(", "bool NonCompletionInterior(", "bool CompletionMapEligible(", "struct Candidate {",
    "bool Contains(", "GW::Vec2f ClosestPoint(", "GW::GamePos Centre(",
    "std::optional<completion::Portal> SharedPortal(", "size_t ClipHalfPlane(",
    "bool TrapezoidCellOverlap(", "struct Doorway {", "float DistanceToSegmentSq(",
    "bool DoorwayBlocks(", "bool CrossesDoorway(", "bool IsBeyondKnownPortal(", "bool IsExplored(", "size_t ClosestRoutePointThrough(",
    "bool PointerInArray(", "struct SnapshotPlane {", "struct BuildSnapshot {", "struct BuildResult {",
    "struct BuildCancelled {", "void BuildRoute(const BuildSnapshot &",
]
with tempfile.TemporaryDirectory(prefix="worldcompletion-tests-") as temp:
    temp = Path(temp)
    fixture = temp / "snapshot.cpp"
    fixture.write_text(stubs + "\n" + "\n\n".join(block(marker) +
                       ("\nstd::vector<DiscoveryVisit> pending_discovery;" if marker == "struct DiscoveryVisit {" else "") for marker in markers)
                       + "\n" + (ROOT / "tests/snapshot_test.cpp").read_text())
    for name, path in [("order", ROOT / "tests/route_planner_test.cpp"), ("snapshot", fixture),
                       ("history", ROOT / "tests/visit_history_test.cpp"),
                       ("navigation", ROOT / "tests/navigation_monitor_test.cpp"),
                       ("portal", ROOT / "tests/portal_barrier_test.cpp")]:
        binary = temp / name
        subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT), str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
