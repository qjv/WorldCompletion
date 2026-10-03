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
#include "plugins/WorldCompletion/PortalLocations.h"
namespace GW {
    struct Vec2f { float x = 0, y = 0; };
    struct GamePos { float x = 0, y = 0; uint32_t zplane = 0; };
    struct PathingTrapezoid { float XTL, XTR, YT, XBL, XBR, YB; };
    namespace Constants {
        enum class MapID { None };
        enum class InstanceType { Loading };
    }
}
struct ImRect { GW::Vec2f Min, Max; };
namespace CartographyData {
    struct Mask { int x0, y0, width, height, byte_count; const uint8_t* bits; };
}
constexpr float kGwinchesPerWorldUnit = 96.f;
constexpr float kWorldUnitsPerCell = 32.f;
"""
markers = [
    "int FogCellX(", "int FogCellY(", "bool MaskContains(", "struct Candidate {",
    "bool Contains(", "GW::Vec2f ClosestPoint(", "GW::GamePos Centre(",
    "std::optional<completion::Portal> SharedPortal(", "size_t ClipHalfPlane(",
    "bool TrapezoidCellOverlap(", "struct Doorway {", "float DistanceToSegmentSq(",
    "bool CrossesDoorway(", "bool IsBeyondKnownPortal(", "bool IsExplored(",
    "struct SnapshotPlane {", "struct BuildSnapshot {", "struct BuildResult {",
    "struct BuildCancelled {", "void BuildRoute(const BuildSnapshot &",
]
with tempfile.TemporaryDirectory(prefix="worldcompletion-tests-") as temp:
    temp = Path(temp)
    fixture = temp / "snapshot.cpp"
    fixture.write_text(stubs + "\n" + "\n\n".join(block(marker) for marker in markers)
                       + "\n" + (ROOT / "tests/snapshot_test.cpp").read_text())
    for name, path in [("order", ROOT / "tests/route_planner_test.cpp"), ("snapshot", fixture)]:
        binary = temp / name
        subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I", str(ROOT), str(path), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
