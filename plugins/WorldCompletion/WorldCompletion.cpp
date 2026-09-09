#include "WorldCompletion.h"
#include "PortalLocations.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <numeric>
#include <random>
#include <DirectXMath.h>
#include <queue>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <GWCA/Context/MapContext.h>
#include <GWCA/Context/GameplayContext.h>
#include <GWCA/Context/WorldContext.h>
#include <GWCA/GameEntities/Agent.h>
#include <GWCA/GameEntities/Camera.h>
#include <GWCA/GameEntities/Map.h>
#include <GWCA/GameEntities/Pathing.h>
#include <GWCA/Managers/AgentMgr.h>
#include <GWCA/Managers/CameraMgr.h>
#include <GWCA/Managers/MapMgr.h>
#include <GWCA/Managers/RenderMgr.h>
#include <GWCA/Managers/UIMgr.h>

#include <Widgets/CartographyData.h>

namespace {
    constexpr float kGwinchesPerWorldUnit = 96.f;
    constexpr float kWorldUnitsPerCell = 32.f;

    int FogCellX(const float world_x) { return static_cast<int>(floorf(world_x / kWorldUnitsPerCell)); }
    int FogCellY(const float world_y) { return static_cast<int>(ceilf(world_y / kWorldUnitsPerCell)) - 1; }

    bool MaskContains(const CartographyData::Mask* mask, const int cx, const int cy)
    {
        if (!mask) return true;
        const int x = cx - mask->x0, y = cy - mask->y0;
        if (x < 0 || y < 0 || x >= mask->width || y >= mask->height) return false;
        const size_t bit = static_cast<size_t>(y) * mask->width + x;
        return bit / 8 < static_cast<size_t>(mask->byte_count)
            && ((mask->bits[bit >> 3] >> (bit & 7)) & 1u) != 0;
    }

    const CartographyData::Mask* CreditableMask(const GW::AreaInfo* map_info)
    {
        if (!map_info) return nullptr;
        const int continent = static_cast<int>(map_info->continent);
        for (const auto& data : CartographyData::kContinents) {
            if (data.id == continent) return &data.creditable;
        }
        return nullptr;
    }

    struct Candidate {
        GW::GamePos pos{};
        const GW::PathingTrapezoid* trap = nullptr;
        std::vector<uint32_t> reveals;
        float footing_area = 0.f;
        int cell_x = 0;
        int cell_y = 0;
    };
    struct GroundVertex { float x, y, z; D3DCOLOR colour; };

    std::vector<GW::GamePos> route;
    std::vector<GW::GamePos> route_waypoints;
    std::vector<std::vector<uint32_t>> route_waypoint_reveals;
    std::vector<bool> route_waypoint_narrow;
    std::vector<float> route_waypoint_altitudes;
    std::vector<GW::Vec3f> ground_route;
    uint32_t fog_grid_width = 0;
    GW::Constants::MapID route_map = GW::Constants::MapID::None;
    GW::Constants::InstanceType route_instance = GW::Constants::InstanceType::Loading;
    GW::Constants::MapID pending_map = GW::Constants::MapID::None;
    GW::Constants::InstanceType pending_instance = GW::Constants::InstanceType::Loading;
    std::atomic_bool route_suspended = true;
    std::atomic_bool transition_event = false;
    std::atomic_bool recompute_requested = false;
    int arrow_size_percent = 100;
    uint32_t route_colour = 0xF52391FFu;
    int optimization_quality = 1;
    int relaxation_passes = 8;
    int randomized_restarts = 12;
    uint32_t resume_after = 0;
    uint32_t last_rebuild = 0;
    uint32_t last_fog_check = 0;
    int useful_candidates = 0;
    int unexplored_reachable = 0;
    int unexplored_border = 0;
    size_t ground_segments_drawn = 0;
    bool route_connected = false;
    float route_length = 0.f;
    std::ofstream debug_log;
    std::string last_log_summary;
    int route_strategy = -1;
    int chosen_planner = -1;
    IDirect3DStateBlock9* route_state_block = nullptr;
    IDirect3DDevice9* route_state_device = nullptr;
    GW::HookEntry map_lifecycle_hook;
    std::vector<uint32_t> cartography_snapshot;
    std::vector<GroundVertex> ground_border_vertices;
    std::vector<GroundVertex> ground_core_vertices;
    float cached_ground_thickness = -1.f;
    float cached_ground_arrow_size = -1.f;
    uint32_t cached_ground_colour = 0;
    bool ground_vertices_dirty = true;
    size_t route_progress = 0;
    size_t ground_progress = 0;

    GW::Constants::MapID CartographyMapID();

    void ClearRouteState()
    {
        route.clear();
        route_waypoints.clear();
        route_waypoint_reveals.clear();
        route_waypoint_narrow.clear();
        route_waypoint_altitudes.clear();
        ground_route.clear();
        ground_border_vertices.clear();
        ground_core_vertices.clear();
        cartography_snapshot.clear();
        route_connected = false;
        route_map = GW::Constants::MapID::None;
        route_instance = GW::Constants::InstanceType::Loading;
        chosen_planner = -1;
        route_strategy = -1;
        ground_vertices_dirty = true;
        route_progress = 0;
        ground_progress = 0;
    }

    void SuspendRoute(const uint32_t now)
    {
        ClearRouteState();
        if (route_state_block) {
            route_state_block->Release();
            route_state_block = nullptr;
        }
        route_state_device = nullptr;
        route_suspended = true;
        pending_map = GW::Constants::MapID::None;
        pending_instance = GW::Constants::InstanceType::Loading;
        resume_after = now + 1500;
    }

    void LogBuild(const char* state, const size_t trapezoids = 0, const size_t reachable = 0,
                  const size_t stand_cells = 0, const size_t uncovered_after_route = 0)
    {
        if (!debug_log.is_open()) return;
        std::ostringstream out;
        out << "map=" << static_cast<uint32_t>(GW::Map::GetMapID())
            << " cartography_map=" << static_cast<uint32_t>(CartographyMapID())
            << " instance=" << static_cast<uint32_t>(GW::Map::GetInstanceType())
            << " planner=" << route_strategy
            << " state=" << state
            << " trapezoids=" << trapezoids
            << " reachable=" << reachable
            << " stand_cells=" << stand_cells
            << " useful=" << useful_candidates
            << " fog=" << unexplored_reachable
            << " border_fog=" << unexplored_border
            << " route=" << route.size()
            << " route_length=" << route_length
            << " waypoints=" << route_waypoints.size()
            << " uncovered_after_route=" << uncovered_after_route;
        const std::string summary = out.str();
        if (summary == last_log_summary) return;
        last_log_summary = summary;
        debug_log << GetTickCount() << ' ' << summary << '\n';
        if (const auto* player = GW::Agents::GetControlledCharacter())
            debug_log << "  player=" << player->pos.x << ',' << player->pos.y << '\n';
        for (size_t i = 0; i < route_waypoints.size(); i++) {
            debug_log << "  waypoint=" << i + 1 << " game=" << route_waypoints[i].x << ',' << route_waypoints[i].y << " reveals=";
            if (i < route_waypoint_reveals.size() && fog_grid_width) {
                for (const uint32_t fog : route_waypoint_reveals[i])
                    debug_log << (fog % fog_grid_width) << ',' << (fog / fog_grid_width) << ';';
            }
            debug_log << '\n';
        }
        for (size_t i = 0; i < route.size(); i++)
            debug_log << "  route_point=" << i << " game=" << route[i].x << ',' << route[i].y << '\n';
        debug_log.flush();
    }

    bool GetMapBounds(const GW::AreaInfo* info, ImRect& out)
    {
        if (!info) return false;
        const uint32_t* bounds = &info->icon_start_x;
        if (!bounds[0]) bounds = &info->icon_start_x_dupe;
        out = {{static_cast<float>(static_cast<int32_t>(bounds[0])), static_cast<float>(static_cast<int32_t>(bounds[1]))},
               {static_cast<float>(static_cast<int32_t>(bounds[2])), static_cast<float>(static_cast<int32_t>(bounds[3]))}};
        return out.GetWidth() > 0.f && out.GetHeight() > 0.f;
    }

    GW::Constants::MapID CartographyMapID()
    {
        const auto current = GW::Map::GetMapID();
        if (GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable) return current;
        if (current == GW::Constants::MapID::Pogahn_Passage)
            return GW::Constants::MapID::Gandara_the_Moon_Fortress;

        static GW::Constants::MapID cached_current = GW::Constants::MapID::None;
        static GW::Constants::MapID cached_display = GW::Constants::MapID::None;
        if (cached_current == current) return cached_display;
        cached_current = current;
        cached_display = current;

        // Missions whose playable zone belongs to a separate mission outpost
        // use that outpost's world-map bounds and placement.
        for (uint32_t id = 0; id < static_cast<uint32_t>(GW::Constants::MapID::Count); id++) {
            const auto* info = GW::Map::GetMapInfo(static_cast<GW::Constants::MapID>(id));
            if (info && (info->flags & 1) && info->GetHasMissionMapsTo()
                && info->mission_maps_to == static_cast<uint32_t>(current)) {
                cached_display = static_cast<GW::Constants::MapID>(id);
                break;
            }
        }
        return cached_display;
    }

    bool GameToWorld(const GW::GamePos& game, GW::Vec2f& world, const GW::Constants::MapID display_map)
    {
        const auto* map_context = GW::GetMapContext();
        const auto* info = GW::Map::GetMapInfo(display_map);
        ImRect bounds;
        if (!map_context || !GetMapBounds(info, bounds)) return false;
        const GW::Vec2f anchor = {
            bounds.Min.x - map_context->start_pos.x / kGwinchesPerWorldUnit,
            bounds.Min.y + map_context->end_pos.y / kGwinchesPerWorldUnit + 1.f,
        };
        world = {game.x / kGwinchesPerWorldUnit + anchor.x,
                 -game.y / kGwinchesPerWorldUnit + anchor.y};
        return std::isfinite(world.x) && std::isfinite(world.y);
    }

    bool WorldToGame(const GW::Vec2f& world, GW::Vec2f& game, const GW::Constants::MapID display_map)
    {
        const auto* map_context = GW::GetMapContext();
        const auto* info = GW::Map::GetMapInfo(display_map);
        ImRect bounds;
        if (!map_context || !GetMapBounds(info, bounds)) return false;
        const GW::Vec2f anchor = {
            bounds.Min.x - map_context->start_pos.x / kGwinchesPerWorldUnit,
            bounds.Min.y + map_context->end_pos.y / kGwinchesPerWorldUnit + 1.f,
        };
        game = {(world.x - anchor.x) * kGwinchesPerWorldUnit,
                -(world.y - anchor.y) * kGwinchesPerWorldUnit};
        return std::isfinite(game.x) && std::isfinite(game.y);
    }

    bool Contains(const GW::PathingTrapezoid* t, const GW::Vec2f& p)
    {
        if (!t || p.y < t->YB || p.y > t->YT) return false;
        const float height = t->YT - t->YB;
        if (height <= 0.001f) return false;
        const float a = (p.y - t->YB) / height;
        const float left = t->XBL + a * (t->XTL - t->XBL);
        const float right = t->XBR + a * (t->XTR - t->XBR);
        return p.x >= left && p.x <= right;
    }

    GW::GamePos Centre(const GW::PathingTrapezoid* t)
    {
        return {(t->XTL + t->XTR + t->XBL + t->XBR) * 0.25f, (t->YT + t->YB) * 0.5f, 0};
    }

    GW::GamePos TransitionPoint(const GW::PathingTrapezoid* from, const GW::PathingTrapezoid* to)
    {
        const GW::Vec2f a[4] = {{from->XTL, from->YT}, {from->XTR, from->YT},
                                 {from->XBR, from->YB}, {from->XBL, from->YB}};
        const GW::Vec2f b[4] = {{to->XTL, to->YT}, {to->XTR, to->YT},
                                 {to->XBR, to->YB}, {to->XBL, to->YB}};
        const auto points_match = [](const GW::Vec2f& lhs, const GW::Vec2f& rhs) {
            return fabsf(lhs.x - rhs.x) < 1.f && fabsf(lhs.y - rhs.y) < 1.f;
        };
        for (size_t i = 0; i < 4; i++) {
            const auto& a0 = a[i];
            const auto& a1 = a[(i + 1) % 4];
            for (size_t j = 0; j < 4; j++) {
                const auto& b0 = b[j];
                const auto& b1 = b[(j + 1) % 4];
                if (!((points_match(a0, b0) && points_match(a1, b1))
                    || (points_match(a0, b1) && points_match(a1, b0)))) continue;
                return {(a0.x + a1.x) * 0.5f, (a0.y + a1.y) * 0.5f, 0};
            }
        }
        const auto ca = Centre(from), cb = Centre(to);
        return {(ca.x + cb.x) * 0.5f, (ca.y + cb.y) * 0.5f, 0};
    }

    size_t ClipHalfPlane(GW::Vec2f (&poly)[8], const size_t count, const int axis,
                         const float limit, const bool keep_above)
    {
        if (!count) return 0;
        GW::Vec2f clipped[8];
        size_t out_count = 0;
        const auto coord = [axis](const GW::Vec2f& p) { return axis ? p.y : p.x; };
        const auto inside = [&](const GW::Vec2f& p) { return keep_above ? coord(p) >= limit : coord(p) <= limit; };
        for (size_t i = 0; i < count; i++) {
            const auto& a = poly[i];
            const auto& b = poly[(i + 1) % count];
            const bool a_inside = inside(a), b_inside = inside(b);
            if (a_inside && out_count < 8) clipped[out_count++] = a;
            if (a_inside == b_inside) continue;
            const float da = coord(a) - limit;
            const float t = da / (da - (coord(b) - limit));
            if (out_count < 8) clipped[out_count++] = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
        }
        std::copy_n(clipped, out_count, poly);
        return out_count;
    }

    bool TrapezoidCellOverlap(const GW::PathingTrapezoid* trap, const GW::Vec2f& box_min,
                              const GW::Vec2f& box_max, GW::Vec2f& footing, float& overlap_area)
    {
        if (!trap || std::min(trap->XTL, trap->XBL) > box_max.x || std::max(trap->XTR, trap->XBR) < box_min.x
            || trap->YB > box_max.y || trap->YT < box_min.y) return false;
        GW::Vec2f poly[8] = {{trap->XTL, trap->YT}, {trap->XTR, trap->YT},
                             {trap->XBR, trap->YB}, {trap->XBL, trap->YB}};
        size_t count = 4;
        count = ClipHalfPlane(poly, count, 0, box_min.x, true);
        count = ClipHalfPlane(poly, count, 0, box_max.x, false);
        count = ClipHalfPlane(poly, count, 1, box_min.y, true);
        count = ClipHalfPlane(poly, count, 1, box_max.y, false);
        if (count < 3) return false;
        float area2 = 0.f;
        GW::Vec2f centroid{};
        for (size_t i = 0; i < count; i++) {
            const auto& a = poly[i];
            const auto& b = poly[(i + 1) % count];
            const float cross = a.x * b.y - b.x * a.y;
            area2 += cross;
            centroid.x += (a.x + b.x) * cross;
            centroid.y += (a.y + b.y) * cross;
        }
        if (fabsf(area2) < 1e-3f) return false;
        footing = {centroid.x / (3.f * area2), centroid.y / (3.f * area2)};
        overlap_area = fabsf(area2) * 0.5f;
        return true;
    }

    struct Doorway {
        GW::Vec2f pos{};
        float radius_sq = 0.f;
    };

    uint32_t FileHashToFileId(const wchar_t* hash)
    {
        if (!hash) return 0;
        if (hash[0] > 0xff && hash[1] > 0xff && (hash[2] == 0 || (hash[2] > 0xff && hash[3] == 0)))
            return (hash[0] - 0xff00ff) + static_cast<uint32_t>(hash[1]) * 0xff00;
        return 0;
    }

    bool IsPortalModel(const uint32_t id)
    {
        switch (id) {
            case 0x4e6b2: case 0x3c5ac: case 0xa825: case 0xe723:
            case 0x858b: case 0x28da0: case 0x1c533: case 0x5e77a: return true;
            default: return false;
        }
    }

    std::vector<Doorway> GetBlockedDoorways(const GW::MapContext* context)
    {
        std::vector<Doorway> result;
        if (context && context->props) {
            for (const auto* prop : context->props->propArray) {
                if (!prop || !prop->model_info || !IsPortalModel(FileHashToFileId(prop->model_info->model_file_name))) continue;
                const float radius = prop->model_info->bounding_radius > 0.f
                    ? prop->scale * prop->model_info->bounding_radius : 400.f;
                result.push_back({{prop->position.x, prop->position.y}, radius * radius});
            }
        }

        // Some Factions exits do not expose one of the known travel-portal prop
        // models in the live prop array. Use the embedded inter-map endpoint data
        // as a conservative fallback, so pathing cannot leak through those exits.
        // Connection coordinates are spawn points just beyond the actual threshold,
        // not the centre of the door prop. Cover the whole transition throat so an
        // adjacency edge cannot slip between the endpoint and the visible portal.
        constexpr float endpoint_radius = 1800.f;
        const uint32_t map_id = static_cast<uint32_t>(GW::Map::GetMapID());
        for (const auto& endpoint : world_completion_portals::locations) {
            if (endpoint.map_id != map_id) continue;
            const auto existing = std::ranges::find_if(result, [&](const Doorway& doorway) {
                return hypotf(doorway.pos.x - endpoint.x, doorway.pos.y - endpoint.y) < endpoint_radius;
            });
            if (existing == result.end()) {
                result.push_back({{endpoint.x, endpoint.y}, endpoint_radius * endpoint_radius});
            }
            else {
                // A matching live prop often reports only the model cylinder,
                // which is much narrower than the complete zoning threshold.
                existing->pos = {endpoint.x, endpoint.y};
                existing->radius_sq = std::max(existing->radius_sq, endpoint_radius * endpoint_radius);
            }
        }
        return result;
    }

    float DistanceToSegmentSq(const GW::Vec2f& p, const GW::Vec2f& a, const GW::Vec2f& b)
    {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len_sq = dx * dx + dy * dy;
        const float t = len_sq > 0.f
            ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq, 0.f, 1.f) : 0.f;
        const float ox = a.x + dx * t - p.x, oy = a.y + dy * t - p.y;
        return ox * ox + oy * oy;
    }

    bool CrossesDoorway(const std::vector<Doorway>& doors, const GW::PathingTrapezoid* a,
                        const GW::PathingTrapezoid* b)
    {
        const auto ca = Centre(a), cb = Centre(b);
        return std::ranges::any_of(doors, [&](const Doorway& door) {
            return DistanceToSegmentSq(door.pos, {ca.x, ca.y}, {cb.x, cb.y}) < door.radius_sq;
        });
    }

    bool IsBeyondKnownPortal(const GW::Vec2f& point, const GW::Vec2f& map_min,
                             const GW::Vec2f& map_max, const uint32_t map_id)
    {
        constexpr float half_width = 4000.f;
        for (const auto& endpoint : world_completion_portals::locations) {
            if (endpoint.map_id != map_id) continue;
            const float distances[4] = {fabsf(endpoint.x - map_min.x), fabsf(endpoint.x - map_max.x),
                                        fabsf(endpoint.y - map_min.y), fabsf(endpoint.y - map_max.y)};
            const size_t side = static_cast<size_t>(std::min_element(std::begin(distances), std::end(distances)) - distances);
            const float lateral = side < 2 ? fabsf(point.y - endpoint.y) : fabsf(point.x - endpoint.x);
            if (lateral >= half_width) continue;
            if ((side == 0 && point.x < endpoint.x) || (side == 1 && point.x > endpoint.x)
                || (side == 2 && point.y < endpoint.y) || (side == 3 && point.y > endpoint.y)) return true;
        }
        return false;
    }

    bool IsExplored(const uint32_t* bits, const uint32_t width, const uint32_t height,
                    const uint32_t words, const int cx, const int cy)
    {
        if (!bits || cx < 0 || cy < 0 || static_cast<uint32_t>(cx) >= width || static_cast<uint32_t>(cy) >= height) return true;
        const uint32_t word = static_cast<uint32_t>(cy) * (width >> 5) + (static_cast<uint32_t>(cx) >> 5);
        return word >= words || ((bits[word] >> (static_cast<uint32_t>(cx) & 31)) & 1u) != 0;
    }

    bool FogIndexExplored(const uint32_t fog)
    {
        const auto* world = GW::GetWorldContext();
        if (!world || !world->cartographed_areas.valid() || !fog_grid_width) return false;
        const auto* bits = reinterpret_cast<const uint32_t*>(world->cartographed_areas.m_buffer);
        const uint32_t word = fog >> 5;
        return word < world->cartographed_areas.size() && ((bits[word] >> (fog & 31)) & 1u) != 0;
    }

    void RebuildGroundRoute()
    {
        ground_route.clear();
        ground_vertices_dirty = true;
        if (route.size() < 2) return;
        const auto* maps = GW::Map::GetPathingMap();
        float previous_z = 0.f;
        bool have_previous_z = false;
        for (size_t leg = 1; leg < route.size(); leg++) {
            const auto& a = route[leg - 1];
            const auto& b = route[leg];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const int samples = std::max(1, static_cast<int>(ceilf(sqrtf(dx * dx + dy * dy) / 140.f)));
            for (int sample = leg == 1 ? 0 : 1; sample <= samples; sample++) {
                const float t = static_cast<float>(sample) / static_cast<float>(samples);
                GW::GamePos ground{a.x + dx * t, a.y + dy * t, a.zplane};
                float z = GW::Map::QueryAltitude(&ground, 64.f);
                float best_distance = have_previous_z ? fabsf(z - previous_z) : FLT_MAX;
                if (maps) {
                    for (uint32_t plane = 1; plane < maps->size(); plane++) {
                        GW::GamePos probe{ground.x, ground.y, plane};
                        const float candidate_z = GW::Map::QueryAltitude(&probe, 64.f);
                        if (candidate_z == 0.f) continue;
                        const float candidate_distance = have_previous_z
                            ? fabsf(candidate_z - previous_z)
                            : fabsf(candidate_z - z);
                        if (candidate_distance >= best_distance) continue;
                        best_distance = candidate_distance;
                        z = candidate_z;
                    }
                }
                previous_z = z;
                have_previous_z = true;
                ground_route.push_back({ground.x, ground.y, z - 48.f});
            }
        }
    }

    size_t ClosestRoutePointThrough(const GW::Vec2f& player, const size_t last)
    {
        size_t closest = std::min(route_progress, route.size() - 1);
        float closest_distance = FLT_MAX;
        for (size_t i = closest; i <= std::min(last, route.size() - 1); i++) {
            const float dx = route[i].x - player.x, dy = route[i].y - player.y;
            const float distance = dx * dx + dy * dy;
            if (distance >= closest_distance) continue;
            closest_distance = distance;
            closest = i;
        }
        return closest;
    }

    size_t CurrentRouteEnd()
    {
        const auto unfinished = std::ranges::find_if(route_waypoint_reveals, [](const auto& reveals) {
            return std::ranges::any_of(reveals, [](const uint32_t fog) { return !FogIndexExplored(fog); });
        });
        if (unfinished == route_waypoint_reveals.end()) return route.size() - 1;
        const auto waypoint = static_cast<size_t>(unfinished - route_waypoint_reveals.begin());
        const auto& target = route_waypoints[waypoint];
        const auto found = std::ranges::find_if(route.begin() + static_cast<ptrdiff_t>(route_progress), route.end(),
            [&](const GW::GamePos& point) { return point.x == target.x && point.y == target.y; });
        return found == route.end() ? route.size() - 1 : static_cast<size_t>(found - route.begin());
    }

    size_t ClosestGroundPoint(const GW::Vec2f& player)
    {
        size_t closest = std::min(ground_progress, ground_route.size() - 1);
        float closest_distance = FLT_MAX;
        const auto route_end = CurrentRouteEnd();
        const auto& target = route[route_end];
        size_t last = closest;
        float target_distance = FLT_MAX;
        for (size_t i = closest; i < ground_route.size(); i++) {
            const float dx = ground_route[i].x - target.x, dy = ground_route[i].y - target.y;
            const float distance = dx * dx + dy * dy;
            if (distance >= target_distance) continue;
            target_distance = distance;
            last = i;
        }
        for (size_t i = closest; i <= last; i++) {
            const float dx = ground_route[i].x - player.x, dy = ground_route[i].y - player.y;
            const float distance = dx * dx + dy * dy;
            if (distance >= closest_distance) continue;
            closest_distance = distance;
            closest = i;
        }
        return closest;
    }

    void BuildRoute()
    {
        const int quality = std::clamp(optimization_quality, 0, 2);
        const int two_opt_passes = std::array{8, 16, 32}[quality];
        const int relocation_passes = std::array{2, 6, 12}[quality];
        route.clear();
        route_waypoints.clear();
        route_waypoint_reveals.clear();
        route_waypoint_narrow.clear();
        route_waypoint_altitudes.clear();
        ground_route.clear();
        route_connected = false;
        route_length = 0.f;
        route_progress = 0;
        ground_progress = 0;
        useful_candidates = 0;
        unexplored_reachable = 0;
        unexplored_border = 0;
        route_map = GW::Map::GetMapID();
        route_instance = GW::Map::GetInstanceType();
        if (!GW::Map::GetIsMapLoaded() || GW::Map::GetInstanceType() == GW::Constants::InstanceType::Loading) {
            LogBuild("not_loaded");
            return;
        }

        const auto* player = GW::Agents::GetControlledCharacter();
        const auto* world = GW::GetWorldContext();
        auto* maps = GW::Map::GetPathingMap();
        if (!player || !world || !maps || !maps->valid() || !world->cartographed_areas.valid()) {
            LogBuild("missing_context");
            return;
        }
        const uint32_t width = world->h05B4[0], height = world->h05B4[1];
        fog_grid_width = width;
        if (!width || !height || (width & 31)) {
            LogBuild("invalid_carto_grid");
            return;
        }
        const auto* bits = reinterpret_cast<const uint32_t*>(world->cartographed_areas.m_buffer);
        const uint32_t words = world->cartographed_areas.size();

        const GW::Vec2f player_pos{player->pos.x, player->pos.y};
        const GW::PathingTrapezoid* start = nullptr;
        size_t start_plane = 0;
        size_t trapezoid_count = 0;
        GW::Vec2f path_min{FLT_MAX, FLT_MAX}, path_max{-FLT_MAX, -FLT_MAX};
        for (const auto& map : *maps) trapezoid_count += map.trapezoid_count;
        for (const auto& map : *maps) {
            for (uint32_t i = 0; i < map.trapezoid_count; i++) {
                const auto& t = map.trapezoids[i];
                path_min.x = std::min(path_min.x, std::min(t.XTL, t.XBL));
                path_min.y = std::min(path_min.y, t.YB);
                path_max.x = std::max(path_max.x, std::max(t.XTR, t.XBR));
                path_max.y = std::max(path_max.y, t.YT);
            }
        }
        const auto find_start_on_plane = [&](const size_t plane) -> const GW::PathingTrapezoid* {
            if (plane >= maps->size()) return nullptr;
            const auto& map = (*maps)[plane];
            for (uint32_t i = 0; i < map.trapezoid_count; i++) {
                if (Contains(&map.trapezoids[i], player_pos)) return &map.trapezoids[i];
            }
            return nullptr;
        };
        start_plane = player->pos.zplane;
        start = find_start_on_plane(start_plane);
        for (size_t plane = 0; plane < maps->size(); plane++) {
            if (start || plane == player->pos.zplane) continue;
            start = find_start_on_plane(plane);
            if (start) start_plane = plane;
            if (start) break;
        }
        if (!start) {
            LogBuild("player_not_on_pathing", trapezoid_count);
            return;
        }

        std::unordered_set<const GW::PathingTrapezoid*> reachable;
        std::unordered_map<const GW::PathingTrapezoid*, std::vector<const GW::PathingTrapezoid*>> graph;
        struct TrapRef { const GW::PathingTrapezoid* trap; size_t plane; };
        std::deque<TrapRef> queue;
        reachable.insert(start);
        queue.push_back({start, start_plane});
        const auto* map_context = GW::GetMapContext();
        const auto* path_context = map_context ? map_context->path : nullptr;
        // Match the Cartographer/Vanquish reachability rule: travel thresholds
        // divide the navmesh, except for the threshold on which we spawned.
        std::vector<Doorway> path_doorways = GetBlockedDoorways(map_context);
        std::erase_if(path_doorways, [&](const Doorway& door) {
            const float dx = player->pos.x - door.pos.x, dy = player->pos.y - door.pos.y;
            return dx * dx + dy * dy < door.radius_sq;
        });
        const auto add_edge = [&](const GW::PathingTrapezoid* a, const GW::PathingTrapezoid* b) {
            auto& from = graph[a];
            if (std::ranges::find(from, b) == from.end()) from.push_back(b);
            auto& to = graph[b];
            if (std::ranges::find(to, a) == to.end()) to.push_back(a);
        };
        while (!queue.empty()) {
            const auto [t, plane] = queue.front();
            queue.pop_front();
            for (const auto* adjacent : t->adjacent) {
                if (!adjacent) continue;
                if (CrossesDoorway(path_doorways, t, adjacent)) continue;
                add_edge(t, adjacent);
                if (reachable.insert(adjacent).second) queue.push_back({adjacent, plane});
            }
            if (plane >= maps->size()) continue;
            const auto& pathing_map = (*maps)[plane];
            const auto expand_portal = [&](const uint16_t portal_index) {
                if (portal_index >= pathing_map.portal_count) return;
                const auto& portal = pathing_map.portals[portal_index];
                if ((portal.flags & 0x04) || !portal.pair) return;
                const size_t target_plane = portal.neighbor_plane;
                if (path_context && target_plane < path_context->blockedPlanes.size()
                    && (path_context->blockedPlanes[target_plane] & 1)) return;
                for (uint32_t i = 0; i < portal.pair->count; i++) {
                    const auto* target = portal.pair->trapezoids[i];
                    if (!target) continue;
                    if (CrossesDoorway(path_doorways, t, target)) continue;
                    add_edge(t, target);
                    if (reachable.insert(target).second) queue.push_back({target, target_plane});
                }
            };
            expand_portal(t->portal_left);
            expand_portal(t->portal_right);
        }

        std::map<std::pair<int, int>, Candidate> by_cell;
        std::map<std::pair<int, int>, std::vector<Candidate>> footing_options;
        const auto display_map = CartographyMapID();
        const auto* display_map_info = GW::Map::GetMapInfo(display_map);
        const auto* creditable_mask = CreditableMask(display_map_info);
        // Walk the game's pathing arrays in stable plane/trapezoid order. Iterating
        // the unordered reachable set made the retained footing for a cartography
        // cell change between rebuilds, causing waypoint 1 to jump around.
        for (size_t plane = 0; plane < maps->size(); plane++) {
            const auto& pathing_map = (*maps)[plane];
            for (uint32_t trap_index = 0; trap_index < pathing_map.trapezoid_count; trap_index++) {
                const auto* t = &pathing_map.trapezoids[trap_index];
                if (!reachable.contains(t)) continue;
            GW::Vec2f world_a, world_b;
            if (!GameToWorld({std::min(t->XTL, t->XBL), t->YB, 0}, world_a, display_map)
                || !GameToWorld({std::max(t->XTR, t->XBR), t->YT, 0}, world_b, display_map)) continue;
            const int cell_x0 = FogCellX(std::min(world_a.x, world_b.x));
            const int cell_x1 = FogCellX(std::max(world_a.x, world_b.x));
            const int cell_y0 = FogCellY(std::min(world_a.y, world_b.y));
            const int cell_y1 = FogCellY(std::max(world_a.y, world_b.y));
            for (int cy = cell_y0; cy <= cell_y1; cy++) {
                for (int cx = cell_x0; cx <= cell_x1; cx++) {
                    GW::Vec2f game_a, game_b, footing;
                    if (!WorldToGame({cx * kWorldUnitsPerCell, cy * kWorldUnitsPerCell}, game_a, display_map)
                        || !WorldToGame({(cx + 1) * kWorldUnitsPerCell, (cy + 1) * kWorldUnitsPerCell}, game_b, display_map)) continue;
                    const GW::Vec2f box_min{std::min(game_a.x, game_b.x), std::min(game_a.y, game_b.y)};
                    const GW::Vec2f box_max{std::max(game_a.x, game_b.x), std::max(game_a.y, game_b.y)};
                    float overlap_area = 0.f;
                    if (!TrapezoidCellOverlap(t, box_min, box_max, footing, overlap_area)) continue;
                    if (IsBeyondKnownPortal(footing, path_min, path_max,
                                            static_cast<uint32_t>(GW::Map::GetMapID()))) continue;
                    const auto key = std::pair{cx, cy};
                    const auto existing = by_cell.find(key);
                    if (existing != by_cell.end() && existing->second.footing_area >= overlap_area) continue;
                    Candidate candidate;
                    candidate.pos = {footing.x, footing.y, static_cast<uint32_t>(plane)};
                    candidate.trap = t;
                    candidate.footing_area = overlap_area;
                    candidate.cell_x = cx;
                    candidate.cell_y = cy;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const int fog_x = cx + dx, fog_y = cy + dy;
                            if (!IsExplored(bits, width, height, words, fog_x, fog_y)
                                && MaskContains(creditable_mask, fog_x, fog_y))
                                candidate.reveals.push_back(static_cast<uint32_t>(fog_y) * width + static_cast<uint32_t>(fog_x));
                        }
                    }
                    auto& options = footing_options[key];
                    if (std::ranges::none_of(options, [&](const Candidate& option) { return option.trap == t; })) {
                        options.push_back(candidate);
                        std::ranges::sort(options, {}, &Candidate::footing_area);
                        if (options.size() > 8) options.erase(options.begin());
                    }
                    if (existing == by_cell.end()) by_cell.emplace(key, std::move(candidate));
                    else existing->second = std::move(candidate);
                }
            }
            }
        }

        std::vector<Candidate> candidates;
        std::unordered_set<uint32_t> remaining;
        for (auto& [cell, candidate] : by_cell) {
            if (candidate.reveals.empty()) continue;
            for (const auto fog : candidate.reveals) remaining.insert(fog);
            candidates.push_back(std::move(candidate));
        }
        useful_candidates = static_cast<int>(candidates.size());
        unexplored_reachable = static_cast<int>(remaining.size());

        std::unordered_set<uint32_t> border_fog;
        ImRect map_bounds;
        if (GetMapBounds(display_map_info, map_bounds)) {
            const int min_x = FogCellX(map_bounds.Min.x);
            const int max_x = FogCellX(map_bounds.Max.x);
            const int min_y = FogCellY(map_bounds.Min.y);
            const int max_y = FogCellY(map_bounds.Max.y);
            for (const uint32_t fog : remaining) {
                const int x = static_cast<int>(fog % width);
                const int y = static_cast<int>(fog / width);
                if (x <= min_x + 1 || x >= max_x - 1 || y <= min_y + 1 || y >= max_y - 1)
                    border_fog.insert(fog);
            }
        }
        unexplored_border = static_cast<int>(border_fog.size());

        // First choose a compact set of stand cells that covers all reachable fog.
        // Selection and visit order are deliberately separate: mixing distance into
        // set-cover caused the route to zig-zag between opposite sides of the map.
        std::vector<size_t> selected;
        while (!remaining.empty() && selected.size() < 1024) {
            size_t best = candidates.size();
            int best_value = -1;
            const bool border_pass = std::ranges::any_of(border_fog, [&](const uint32_t fog) { return remaining.contains(fog); });
            for (size_t i = 0; i < candidates.size(); i++) {
                int newly_revealed = 0;
                int border_revealed = 0;
                for (const auto fog : candidates[i].reveals) {
                    if (!remaining.contains(fog)) continue;
                    newly_revealed++;
                    border_revealed += border_fog.contains(fog);
                }
                if (!newly_revealed) continue;
                if (border_pass && !border_revealed) continue;
                const int value = newly_revealed + border_revealed * 4;
                if (value > best_value) { best_value = value; best = i; }
            }
            if (best == candidates.size()) break;
            selected.push_back(best);
            for (const auto fog : candidates[best].reveals) remaining.erase(fog);
        }

        // Greedy set cover can leave an earlier stop redundant after later stops
        // overlap it. Remove any stop whose entire reveal set is still covered by
        // another selected stop, preserving at least one cover for every fog cell.
        std::unordered_map<uint32_t, uint32_t> cover_count;
        for (const size_t index : selected) {
            for (const uint32_t fog : candidates[index].reveals) cover_count[fog]++;
        }
        for (size_t i = selected.size(); i-- > 0;) {
            const auto& reveals = candidates[selected[i]].reveals;
            const bool redundant = std::ranges::all_of(reveals, [&](const uint32_t fog) {
                const auto found = cover_count.find(fog);
                return found != cover_count.end() && found->second > 1;
            });
            if (!redundant) continue;
            for (const uint32_t fog : reveals) cover_count[fog]--;
            selected.erase(selected.begin() + static_cast<ptrdiff_t>(i));
        }

        const auto find_leg = [&](const GW::PathingTrapezoid* from, const GW::PathingTrapezoid* to,
                                  const GW::GamePos* origin) {
            std::vector<const GW::PathingTrapezoid*> leg;
            std::unordered_map<const GW::PathingTrapezoid*, const GW::PathingTrapezoid*> parent;
            std::unordered_map<const GW::PathingTrapezoid*, float> distance;
            using QueueEntry = std::pair<float, const GW::PathingTrapezoid*>;
            std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<>> open;
            parent.emplace(from, nullptr);
            distance.emplace(from, 0.f);
            open.emplace(0.f, from);
            const auto heuristic = [&](const GW::PathingTrapezoid* at) {
                const auto a = Centre(at), b = Centre(to);
                const float dx = b.x - a.x, dy = b.y - a.y;
                return sqrtf(dx * dx + dy * dy);
            };
            while (!open.empty()) {
                const auto [priority, at] = open.top();
                open.pop();
                const auto known = distance.find(at);
                if (known == distance.end() || priority > known->second + heuristic(at) + 0.01f) continue;
                if (at == to) break;
                if (const auto it = graph.find(at); it != graph.end()) {
                    for (const auto* next : it->second) {
                        if (!reachable.contains(next)) continue;
                        const auto a = Centre(at), b = Centre(next);
                        float edge_cost = hypotf(b.x - a.x, b.y - a.y);
                        if (at == from && origin) {
                            const auto crossing = TransitionPoint(at, next);
                            edge_cost = hypotf(crossing.x - origin->x, crossing.y - origin->y)
                                + hypotf(b.x - crossing.x, b.y - crossing.y);
                        }
                        const float candidate = known->second + edge_cost;
                        const auto old = distance.find(next);
                        if (old != distance.end() && candidate + 0.01f >= old->second) continue;
                        distance[next] = candidate;
                        parent[next] = at;
                        open.emplace(candidate + heuristic(next), next);
                    }
                }
            }
            if (!parent.contains(to)) return leg;
            for (const auto* at = to; at && at != from; at = parent[at]) leg.push_back(at);
            std::reverse(leg.begin(), leg.end());
            return leg;
        };
        const auto connection_cost = [&](const GW::PathingTrapezoid*, const GW::GamePos& from_point,
                                         const GW::PathingTrapezoid*, const GW::GamePos& to_point) {
            return hypotf(to_point.x - from_point.x, to_point.y - from_point.y);
        };

        const size_t target_count = selected.size();
        std::vector<std::vector<float>> costs(target_count + 1, std::vector<float>(target_count + 1, FLT_MAX));
        const auto trap_at = [&](const size_t i) { return i == target_count ? start : candidates[selected[i]].trap; };
        const auto point_at = [&](const size_t i) { return i == target_count ? player->pos : candidates[selected[i]].pos; };
        for (size_t i = 0; i <= target_count; i++) {
            costs[i][i] = 0.f;
            for (size_t j = i + 1; j <= target_count; j++) {
                const float cost = connection_cost(trap_at(i), point_at(i), trap_at(j), point_at(j));
                costs[i][j] = costs[j][i] = cost;
            }
        }

        std::vector<size_t> order_pool(target_count);
        std::iota(order_pool.begin(), order_pool.end(), 0);
        std::vector<size_t> order;
        size_t current_index = target_count;
        while (!order_pool.empty()) {
            const auto nearest = std::ranges::min_element(order_pool, [&](const size_t a, const size_t b) {
                return costs[current_index][a] < costs[current_index][b];
            });
            current_index = *nearest;
            order.push_back(current_index);
            order_pool.erase(nearest);
        }

        // Remove graph-distance backtracks with repeated open-path 2-opt passes.
        for (int pass = 0; pass < two_opt_passes; pass++) {
            bool improved = false;
            for (size_t i = 0; i + 1 < order.size(); i++) {
                const size_t before = i ? order[i - 1] : target_count;
                for (size_t k = i + 1; k < order.size(); k++) {
                    const float old_cost = costs[before][order[i]]
                        + (k + 1 < order.size() ? costs[order[k]][order[k + 1]] : 0.f);
                    const float new_cost = costs[before][order[k]]
                        + (k + 1 < order.size() ? costs[order[i]][order[k + 1]] : 0.f);
                    if (new_cost + 1.f >= old_cost) continue;
                    std::reverse(order.begin() + i, order.begin() + k + 1);
                    improved = true;
                }
            }
            if (!improved) break;
        }

        // 2-opt cannot move a single badly placed stop across the route. Follow it
        // with bounded best-relocation passes, scoring against actual graph travel
        // distance. This is the repair phase after the coarse covering route.
        const auto order_cost = [&](const std::vector<size_t>& candidate_order) {
            float total = 0.f;
            size_t previous = target_count;
            for (const size_t next : candidate_order) {
                total += costs[previous][next];
                previous = next;
            }
            return total;
        };
        if (order.size() <= 128) {
            float best_cost = order_cost(order);
            for (int pass = 0; pass < relocation_passes; pass++) {
                std::vector<size_t> best_order = order;
                float pass_cost = best_cost;
                for (size_t from_index = 0; from_index < order.size(); from_index++) {
                    for (size_t to_index = 0; to_index < order.size(); to_index++) {
                        if (from_index == to_index) continue;
                        auto trial = order;
                        const size_t moved = trial[from_index];
                        trial.erase(trial.begin() + static_cast<ptrdiff_t>(from_index));
                        trial.insert(trial.begin() + static_cast<ptrdiff_t>(to_index), moved);
                        const float trial_cost = order_cost(trial);
                        if (trial_cost + 1.f >= pass_cost) continue;
                        pass_cost = trial_cost;
                        best_order = std::move(trial);
                    }
                }
                if (pass_cost + 1.f >= best_cost) break;
                order = std::move(best_order);
                best_cost = pass_cost;
            }
        }

        // Jointly choose the footing for each selected cartography cell against
        // its neighbours in the finished sweep. A cell can overlap several
        // disconnected-looking corridor edges; choosing by area alone creates
        // avoidable trips into the far side of that cell.
        for (int pass = 0; pass < relaxation_passes; pass++) {
            bool improved = false;
            for (size_t iteration = 0; iteration < order.size(); iteration++) {
                const size_t position = pass % 2 ? order.size() - 1 - iteration : iteration;
                Candidate& current = candidates[selected[order[position]]];
                const auto options = footing_options.find({current.cell_x, current.cell_y});
                if (options == footing_options.end() || options->second.size() < 2) continue;
                const GW::PathingTrapezoid* previous_trap = position
                    ? candidates[selected[order[position - 1]]].trap : start;
                const GW::GamePos previous_point = position
                    ? candidates[selected[order[position - 1]]].pos : player->pos;
                const bool has_next = position + 1 < order.size();
                const GW::PathingTrapezoid* next_trap = has_next
                    ? candidates[selected[order[position + 1]]].trap : nullptr;
                const GW::GamePos next_point = has_next
                    ? candidates[selected[order[position + 1]]].pos : GW::GamePos{};
                const auto score = [&](const Candidate& option) {
                    float value = connection_cost(previous_trap, previous_point, option.trap, option.pos);
                    if (has_next) value += connection_cost(option.trap, option.pos, next_trap, next_point);
                    return value;
                };
                float best_score = score(current);
                const Candidate* best = &current;
                for (const Candidate& option : options->second) {
                    const float option_score = score(option);
                    if (option_score + 1.f >= best_score) continue;
                    best_score = option_score;
                    best = &option;
                }
                if (best == &current) continue;
                current = *best;
                improved = true;
            }
            if (!improved) break;
        }

        // Elastic-band pass: a selected stop is not tied to its original stand
        // square. Slide it to any reachable candidate that preserves every uniquely
        // covered fog cell, minimizing the two neighbouring navmesh legs. This lets
        // a slightly angled through-route collect a square instead of making a
        // perpendicular visit and returning to the same corridor.
        const auto morph_options = candidates;
        for (int pass = 0; pass < relaxation_passes; pass++) {
            bool improved = false;
            for (size_t iteration = 0; iteration < order.size(); iteration++) {
                const size_t position = pass % 2 ? order.size() - 1 - iteration : iteration;
                Candidate& current = candidates[selected[order[position]]];
                for (const uint32_t fog : current.reveals) cover_count[fog]--;
                std::vector<uint32_t> required;
                for (const uint32_t fog : current.reveals) {
                    if (cover_count[fog] == 0) required.push_back(fog);
                }
                const GW::PathingTrapezoid* previous_trap = position
                    ? candidates[selected[order[position - 1]]].trap : start;
                const GW::GamePos previous_point = position
                    ? candidates[selected[order[position - 1]]].pos : player->pos;
                const bool has_next = position + 1 < order.size();
                const GW::PathingTrapezoid* next_trap = has_next
                    ? candidates[selected[order[position + 1]]].trap : nullptr;
                const GW::GamePos next_point = has_next
                    ? candidates[selected[order[position + 1]]].pos : GW::GamePos{};
                const auto score = [&](const Candidate& option) {
                    float value = connection_cost(previous_trap, previous_point, option.trap, option.pos);
                    if (has_next) value += connection_cost(option.trap, option.pos, next_trap, next_point);
                    return value;
                };
                float best_score = score(current);
                const Candidate* best = &current;
                for (const Candidate& option : morph_options) {
                    const bool preserves_coverage = std::ranges::all_of(required, [&](const uint32_t fog) {
                        return std::ranges::find(option.reveals, fog) != option.reveals.end();
                    });
                    if (!preserves_coverage) continue;
                    const float option_score = score(option);
                    if (option_score + 1.f >= best_score) continue;
                    best_score = option_score;
                    best = &option;
                }
                if (best != &current) {
                    current = *best;
                    improved = true;
                }
                for (const uint32_t fog : current.reveals) cover_count[fog]++;
            }
            if (!improved) break;
        }

        // Footing optimization can move a stop onto a different corridor. The old
        // cost matrix then describes positions that no longer exist, which was
        // producing sequences such as 7 -> past 9 -> 8 -> back to 9. Recompute
        // graph costs for the final footings and optimize the order once more.
        for (size_t i = 0; i <= target_count; i++) {
            costs[i][i] = 0.f;
            for (size_t j = i + 1; j <= target_count; j++) {
                const float cost = connection_cost(trap_at(i), point_at(i), trap_at(j), point_at(j));
                costs[i][j] = costs[j][i] = cost;
            }
        }
        for (int pass = 0; pass < two_opt_passes; pass++) {
            bool improved = false;
            for (size_t i = 0; i + 1 < order.size(); i++) {
                const size_t before = i ? order[i - 1] : target_count;
                for (size_t k = i + 1; k < order.size(); k++) {
                    const float old_cost = costs[before][order[i]]
                        + (k + 1 < order.size() ? costs[order[k]][order[k + 1]] : 0.f);
                    const float new_cost = costs[before][order[k]]
                        + (k + 1 < order.size() ? costs[order[i]][order[k + 1]] : 0.f);
                    if (new_cost + 1.f >= old_cost) continue;
                    std::reverse(order.begin() + i, order.begin() + k + 1);
                    improved = true;
                }
            }
            if (!improved) break;
        }
        if (order.size() <= 128) {
            float best_cost = order_cost(order);
            for (int pass = 0; pass < relocation_passes; pass++) {
                std::vector<size_t> best_order = order;
                float pass_cost = best_cost;
                for (size_t from_index = 0; from_index < order.size(); from_index++) {
                    for (size_t to_index = 0; to_index < order.size(); to_index++) {
                        if (from_index == to_index) continue;
                        auto trial = order;
                        const size_t moved = trial[from_index];
                        trial.erase(trial.begin() + static_cast<ptrdiff_t>(from_index));
                        trial.insert(trial.begin() + static_cast<ptrdiff_t>(to_index), moved);
                        const float trial_cost = order_cost(trial);
                        if (trial_cost + 1.f >= pass_cost) continue;
                        pass_cost = trial_cost;
                        best_order = std::move(trial);
                    }
                }
                if (pass_cost + 1.f >= best_cost) break;
                order = std::move(best_order);
                best_cost = pass_cost;
            }
        }

        // A single nearest-neighbour seed plus local edits gets trapped when several
        // discovery stops sit on branches of the same corridor. Search many complete
        // rotations and optimize each one, then retain the shortest open route.
        if (order.size() >= 4 && order.size() <= 64) {
            std::vector<size_t> global_best = order;
            float global_best_cost = order_cost(global_best);
            std::mt19937 rng(0x57435254u ^ static_cast<uint32_t>(route_map));
            for (int restart = 0; restart < randomized_restarts; restart++) {
                auto trial = order;
                std::shuffle(trial.begin(), trial.end(), rng);
                for (int pass = 0; pass < two_opt_passes / 2; pass++) {
                    bool improved = false;
                    for (size_t i = 0; i + 1 < trial.size(); i++) {
                        const size_t before = i ? trial[i - 1] : target_count;
                        for (size_t k = i + 1; k < trial.size(); k++) {
                            const float old_cost = costs[before][trial[i]]
                                + (k + 1 < trial.size() ? costs[trial[k]][trial[k + 1]] : 0.f);
                            const float new_cost = costs[before][trial[k]]
                                + (k + 1 < trial.size() ? costs[trial[i]][trial[k + 1]] : 0.f);
                            if (new_cost + .1f >= old_cost) continue;
                            std::reverse(trial.begin() + i, trial.begin() + k + 1);
                            improved = true;
                        }
                    }
                    if (!improved) break;
                }
                for (int pass = 0; pass < relocation_passes / 2; pass++) {
                    bool improved = false;
                    float trial_cost = order_cost(trial);
                    for (size_t from_index = 0; from_index < trial.size(); from_index++) {
                        for (size_t to_index = 0; to_index < trial.size(); to_index++) {
                            if (from_index == to_index) continue;
                            auto moved_order = trial;
                            const size_t moved = moved_order[from_index];
                            moved_order.erase(moved_order.begin() + static_cast<ptrdiff_t>(from_index));
                            moved_order.insert(moved_order.begin() + static_cast<ptrdiff_t>(to_index), moved);
                            const float moved_cost = order_cost(moved_order);
                            if (moved_cost + .1f >= trial_cost) continue;
                            trial = std::move(moved_order);
                            trial_cost = moved_cost;
                            improved = true;
                        }
                    }
                    if (!improved) break;
                }
                const float trial_cost = order_cost(trial);
                if (trial_cost + .1f >= global_best_cost) continue;
                global_best = std::move(trial);
                global_best_cost = trial_cost;
            }
            order = std::move(global_best);
        }

        std::vector<std::vector<size_t>> planner_orders;
        planner_orders.push_back(order); // multi-start elastic route
        {
            std::vector<size_t> pool(target_count);
            std::iota(pool.begin(), pool.end(), 0);
            std::vector<size_t> insertion;
            while (!pool.empty()) {
                float best_delta = FLT_MAX;
                size_t best_pool = 0, best_position = 0;
                for (size_t p = 0; p < pool.size(); p++) {
                    const size_t node = pool[p];
                    for (size_t position = 0; position <= insertion.size(); position++) {
                        const size_t before = position ? insertion[position - 1] : target_count;
                        float delta = costs[before][node];
                        if (position < insertion.size())
                            delta += costs[node][insertion[position]] - costs[before][insertion[position]];
                        if (delta >= best_delta) continue;
                        best_delta = delta;
                        best_pool = p;
                        best_position = position;
                    }
                }
                insertion.insert(insertion.begin() + static_cast<ptrdiff_t>(best_position), pool[best_pool]);
                pool.erase(pool.begin() + static_cast<ptrdiff_t>(best_pool));
            }
            planner_orders.push_back(std::move(insertion));
        }
        {
            const size_t root = target_count;
            std::vector<size_t> parent(target_count, root);
            std::vector<float> key(target_count, FLT_MAX);
            std::vector<bool> in_tree(target_count, false);
            for (size_t i = 0; i < target_count; i++) key[i] = costs[root][i];
            for (size_t count = 0; count < target_count; count++) {
                size_t node = target_count;
                for (size_t i = 0; i < target_count; i++)
                    if (!in_tree[i] && (node == target_count || key[i] < key[node])) node = i;
                if (node == target_count) break;
                in_tree[node] = true;
                for (size_t i = 0; i < target_count; i++) {
                    if (!in_tree[i] && costs[node][i] < key[i]) {
                        key[i] = costs[node][i];
                        parent[i] = node;
                    }
                }
            }
            std::vector<std::vector<size_t>> children(target_count + 1);
            for (size_t i = 0; i < target_count; i++) children[parent[i]].push_back(i);
            for (auto& branch : children)
                std::ranges::sort(branch, [&](const size_t a, const size_t b) { return key[a] < key[b]; });
            std::vector<size_t> mst_near;
            const auto visit = [&](this auto&& self, const size_t node) -> void {
                if (node != root) mst_near.push_back(node);
                for (const size_t child : children[node]) self(child);
            };
            visit(root);
            planner_orders.push_back(std::move(mst_near));
            for (auto& branch : children) std::reverse(branch.begin(), branch.end());
            std::vector<size_t> mst_far;
            const auto visit_far = [&](this auto&& self, const size_t node) -> void {
                if (node != root) mst_far.push_back(node);
                for (const size_t child : children[node]) self(child);
            };
            visit_far(root);
            planner_orders.push_back(std::move(mst_far));
        }
        {
            std::vector<size_t> clockwise(target_count);
            std::iota(clockwise.begin(), clockwise.end(), 0);
            const GW::Vec2f sweep_center{(path_min.x + path_max.x) * .5f, (path_min.y + path_max.y) * .5f};
            const auto angle = [&](const size_t i) {
                const auto p = point_at(i);
                return atan2f(p.y - sweep_center.y, p.x - sweep_center.x);
            };
            std::ranges::sort(clockwise, [&](const size_t a, const size_t b) { return angle(a) < angle(b); });
            auto reversed = clockwise;
            std::reverse(reversed.begin(), reversed.end());
            planner_orders.push_back(std::move(clockwise));
            planner_orders.push_back(std::move(reversed));
        }
        // Axis sweeps are useful for long, folded maps where radial ordering
        // alternates between opposite sides of the same corridor.
        for (const bool by_y : {false, true}) {
            std::vector<size_t> sweep(target_count);
            std::iota(sweep.begin(), sweep.end(), 0);
            std::ranges::sort(sweep, [&](const size_t a, const size_t b) {
                const auto pa = point_at(a), pb = point_at(b);
                return by_y ? pa.y < pb.y : pa.x < pb.x;
            });
            planner_orders.push_back(sweep);
            std::reverse(sweep.begin(), sweep.end());
            planner_orders.push_back(std::move(sweep));
        }

        const auto improve = [&](std::vector<size_t>& candidate_order) {
            for (int pass = 0; pass < two_opt_passes; pass++) {
                bool improved = false;
                for (size_t i = 0; i + 1 < candidate_order.size(); i++) {
                    const size_t before = i ? candidate_order[i - 1] : target_count;
                    for (size_t k = i + 1; k < candidate_order.size(); k++) {
                        const float old_cost = costs[before][candidate_order[i]]
                            + (k + 1 < candidate_order.size() ? costs[candidate_order[k]][candidate_order[k + 1]] : 0.f);
                        const float new_cost = costs[before][candidate_order[k]]
                            + (k + 1 < candidate_order.size() ? costs[candidate_order[i]][candidate_order[k + 1]] : 0.f);
                        if (new_cost + .1f >= old_cost) continue;
                        std::reverse(candidate_order.begin() + i, candidate_order.begin() + k + 1);
                        improved = true;
                    }
                }
                if (!improved) break;
            }
        };
        for (auto& planner_order : planner_orders) improve(planner_order);
        if (chosen_planner < 0 || static_cast<size_t>(chosen_planner) >= planner_orders.size()) {
            chosen_planner = 0;
            float shortest = order_cost(planner_orders[0]);
            for (size_t i = 1; i < planner_orders.size(); i++) {
                const float candidate_cost = order_cost(planner_orders[i]);
                if (candidate_cost + .1f >= shortest) continue;
                shortest = candidate_cost;
                chosen_planner = static_cast<int>(i);
            }
        }
        route_strategy = chosen_planner;
        order = std::move(planner_orders[static_cast<size_t>(chosen_planner)]);

        // Expand each optimized waypoint leg through the reachable trapezoid graph.
        // Every rendered segment therefore represents an actual adjacent transition.
        const GW::PathingTrapezoid* from = start;
        if (order.empty()) {
            LogBuild("nothing_reachable_to_discover", trapezoid_count, reachable.size(), by_cell.size(), remaining.size());
            return;
        }
        route.push_back(player->pos);
        for (const size_t ordered_index : order) {
            const Candidate* target = &candidates[selected[ordered_index]];
            const auto leg = find_leg(from, target->trap, &route.back());
            if (from != target->trap && leg.empty()) {
                route.clear();
                route_waypoints.clear();
                route_waypoint_reveals.clear();
                route_waypoint_narrow.clear();
                route_waypoint_altitudes.clear();
                LogBuild("disconnected_target", trapezoid_count, reachable.size(), by_cell.size(), remaining.size());
                return;
            }
            route_waypoints.push_back(target->pos);
            route_waypoint_reveals.push_back(target->reveals);
            constexpr float cell_area = kWorldUnitsPerCell * kGwinchesPerWorldUnit
                * kWorldUnitsPerCell * kGwinchesPerWorldUnit;
            route_waypoint_narrow.push_back(target->footing_area < cell_area * .14f);
            GW::GamePos waypoint_ground = target->pos;
            route_waypoint_altitudes.push_back(GW::Map::QueryAltitude(&waypoint_ground, 64.f));
            std::vector<GW::GamePos> raw{route.back()};
            const GW::PathingTrapezoid* previous_trap = from;
            for (const auto* at : leg) {
                raw.push_back(TransitionPoint(previous_trap, at));
                previous_trap = at;
            }
            raw.push_back(target->pos);

            route.insert(route.end(), raw.begin() + 1, raw.end());
            from = target->trap;
        }
        route_connected = !route_waypoints.empty();
        for (size_t i = 1; i < route.size(); i++)
            route_length += hypotf(route[i].x - route[i - 1].x, route[i].y - route[i - 1].y);
        RebuildGroundRoute();
        LogBuild("ready", trapezoid_count, reachable.size(), by_cell.size(), remaining.size());
    }

    bool ProjectWorldMap(const GW::Vec2f& wm, ImVec2& out, ImRect& clip)
    {
        auto* context = GW::Map::GetWorldMapContext();
        if (!context || !GW::UI::GetIsWorldMapShowing()) return false;
        auto* frame = GW::UI::GetFrameById(context->frame_id);
        auto* root = GW::UI::GetRootFrame();
        if (!frame || !root) return false;
        clip = {frame->position.GetContentTopLeft(root), frame->position.GetContentBottomRight(root)};
        const GW::Vec2f span = context->bottom_right - context->top_left;
        if (span.x == 0.f || span.y == 0.f) return false;
        out = {clip.Min.x + (wm.x - context->top_left.x) * clip.GetWidth() / span.x,
               clip.Min.y + (wm.y - context->top_left.y) * clip.GetHeight() / span.y};
        return true;
    }

    bool ProjectMissionMap(const GW::Vec2f& wm, ImVec2& out, ImRect& clip)
    {
        auto* context = GW::Map::GetMissionMapContext();
        auto* root = GW::UI::GetRootFrame();
        if (!context || !context->h003c || !root) return false;
        auto* frame = GW::UI::GetFrameById(context->frame_id);
        if (!frame || !frame->IsVisible()) return false;
        clip = {frame->position.GetContentTopLeft(root), frame->position.GetContentBottomRight(root)};
        const GW::Vec2f scale = frame->position.GetViewportScale(root);
        const GW::Vec2f center = {(clip.Min.x + clip.Max.x) * 0.5f, (clip.Min.y + clip.Max.y) * 0.5f};
        const GW::Vec2f offset = wm - context->h003c->mission_map_pan_offset;
        const float zoom = GW::GetGameplayContext() ? GW::GetGameplayContext()->mission_map_zoom : 1.f;
        out = {center.x + offset.x * scale.x * zoom, center.y + offset.y * scale.y * zoom};
        return true;
    }

    bool ProjectGameWorld(const GW::Vec3f& point, ImVec2& screen)
    {
        const auto* camera = GW::CameraMgr::GetCamera();
        const uint32_t width = GW::Render::GetViewportWidth();
        const uint32_t height = GW::Render::GetViewportHeight();
        if (!camera || !width || !height) return false;

        const auto normalize = [](const GW::Vec3f& value) {
            const float length = sqrtf(value.x * value.x + value.y * value.y + value.z * value.z);
            return length > 0.001f ? GW::Vec3f{value.x / length, value.y / length, value.z / length} : GW::Vec3f{};
        };
        const auto cross = [](const GW::Vec3f& a, const GW::Vec3f& b) {
            return GW::Vec3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        };
        const auto dot = [](const GW::Vec3f& a, const GW::Vec3f& b) {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        };

        const GW::Vec3f forward = normalize(camera->look_at_target - camera->position);
        const GW::Vec3f right = normalize(cross({0.f, 0.f, -1.f}, forward));
        const GW::Vec3f up = cross(forward, right);
        const GW::Vec3f relative = point - camera->position;
        const float view_z = dot(relative, forward);
        if (view_z < 20.f) return false;
        const float tan_half_fov = tanf(GW::Render::GetFieldOfView() * 0.5f);
        if (tan_half_fov < 0.001f) return false;
        const float aspect = static_cast<float>(width) / static_cast<float>(height);
        const float ndc_x = dot(relative, right) / (view_z * tan_half_fov * aspect);
        const float ndc_y = dot(relative, up) / (view_z * tan_half_fov);
        screen = {(ndc_x + 1.f) * 0.5f * width, (1.f - ndc_y) * 0.5f * height};
        return true;
    }

    void DrawGroundRoute(const float arrow_scale)
    {
        ground_segments_drawn = 0;
        if (!route_connected || route_waypoints.empty() || ground_route.size() < 2 || GW::UI::GetIsWorldMapShowing()) return;

        auto* draw = ImGui::GetBackgroundDrawList();
        const ImU32 shadow = IM_COL32(35, 12, 0, 190);
        const ImU32 orange = route_colour;
        const auto* player = GW::Agents::GetControlledCharacter();
        if (!player) return;
        const size_t first = ClosestGroundPoint({player->pos.x, player->pos.y});
        ImVec2 previous_screen;
        bool previous_visible = false;
        for (size_t i = first; i < ground_route.size(); i++) {
            ImVec2 current_screen;
            const bool visible = ProjectGameWorld(ground_route[i], current_screen);
            if (visible && previous_visible) {
                const float dx = current_screen.x - previous_screen.x, dy = current_screen.y - previous_screen.y;
                const float length = hypotf(dx, dy);
                if (length > 8.f) {
                    const float ux = dx / length, uy = dy / length;
                    const float px = -uy, py = ux;
                    const float arrow_length = 10.f * arrow_scale;
                    const float arrow_width = 6.f * arrow_scale;
                    for (float start = 0.f; start < length; start += arrow_length) {
                        const float end = std::min(start + arrow_length, length);
                        if (end - start < arrow_length * .35f) break;
                        const ImVec2 tip{previous_screen.x + ux * end, previous_screen.y + uy * end};
                        const ImVec2 base{previous_screen.x + ux * start, previous_screen.y + uy * start};
                        const ImVec2 left{base.x + px * arrow_width, base.y + py * arrow_width};
                        const ImVec2 right{base.x - px * arrow_width, base.y - py * arrow_width};
                        draw->AddLine(left, tip, shadow, 6.f);
                        draw->AddLine(tip, right, shadow, 6.f);
                        draw->AddLine(left, tip, orange, 2.5f);
                        draw->AddLine(tip, right, orange, 2.5f);
                        ground_segments_drawn++;
                    }
                }
            }
            previous_screen = current_screen;
            previous_visible = visible;
        }
    }

    bool DrawOccludedGroundRoute(IDirect3DDevice9* device, const float thickness, const float arrow_scale)
    {
        ground_segments_drawn = 0;
        if (!device || route_suspended || route_map != GW::Map::GetMapID()
            || route_instance != GW::Map::GetInstanceType() || !route_connected
            || route_waypoints.empty() || ground_route.size() < 2
            || GW::UI::GetIsWorldMapShowing()) return false;
        const auto* player = GW::Agents::GetControlledCharacter();
        const auto* camera = GW::CameraMgr::GetCamera();
        const uint32_t width = GW::Render::GetViewportWidth();
        const uint32_t height = GW::Render::GetViewportHeight();
        if (!player || !camera || !width || !height) return false;

        using Vertex = GroundVertex;
        const size_t first = ClosestGroundPoint({player->pos.x, player->pos.y});
        if (first + 1 >= ground_route.size()) return false;
        const auto arrows_in_segment = [&](const float length) {
            return std::max(1u, static_cast<unsigned>(ceilf(length / (180.f * arrow_scale))));
        };
        const auto build_ribbon = [&](std::vector<Vertex>& vertices, const float half_width, const D3DCOLOR colour) {
            vertices.clear();
            vertices.reserve((ground_route.size() - 1) * 12);
            for (size_t i = 1; i < ground_route.size(); i++) {
                const auto& a = ground_route[i - 1];
                const auto& b = ground_route[i];
                const float dx = b.x - a.x, dy = b.y - a.y;
                const float length = hypotf(dx, dy);
                if (length < .1f) continue;
                const float ux = dx / length, uy = dy / length;
                const float px = -uy, py = ux;
                const unsigned arrow_count = arrows_in_segment(length);
                const float step = length / static_cast<float>(arrow_count);
                const float arrow_length = step;
                for (unsigned arrow = 0; arrow < arrow_count; arrow++) {
                    const float start = step * static_cast<float>(arrow);
                    const float end = step * static_cast<float>(arrow + 1);
                    const float tip_x = a.x + ux * end, tip_y = a.y + uy * end;
                    const float base_x = a.x + ux * start, base_y = a.y + uy * start;
                    const float base_z = a.z + (b.z - a.z) * (start / length);
                    const float tip_z = a.z + (b.z - a.z) * (end / length);
                    const float wing = arrow_length * .48f;
                    const auto append_arm = [&](const float end_x, const float end_y) {
                        const float arm_dx = tip_x - end_x, arm_dy = tip_y - end_y;
                        const float arm_len = hypotf(arm_dx, arm_dy);
                        const float ox = -arm_dy / arm_len * half_width, oy = arm_dx / arm_len * half_width;
                        vertices.insert(vertices.end(), {
                            {end_x + ox, end_y + oy, base_z, colour}, {end_x - ox, end_y - oy, base_z, colour},
                            {tip_x + ox, tip_y + oy, tip_z, colour}, {tip_x + ox, tip_y + oy, tip_z, colour},
                            {end_x - ox, end_y - oy, base_z, colour}, {tip_x - ox, tip_y - oy, tip_z, colour}});
                    };
                    append_arm(base_x + px * wing, base_y + py * wing);
                    append_arm(base_x - px * wing, base_y - py * wing);
                }
            }
        };
        const float core_width = std::clamp(thickness * 3.f, 6.f, 18.f);
        if (ground_vertices_dirty || cached_ground_thickness != thickness || cached_ground_arrow_size != arrow_scale
            || cached_ground_colour != route_colour) {
            const auto alpha = static_cast<uint8_t>(route_colour >> 24);
            const auto blue = static_cast<uint8_t>(route_colour >> 16);
            const auto green = static_cast<uint8_t>(route_colour >> 8);
            const auto red = static_cast<uint8_t>(route_colour);
            build_ribbon(ground_border_vertices, core_width + 5.f, D3DCOLOR_ARGB(220, 35, 12, 0));
            build_ribbon(ground_core_vertices, core_width, D3DCOLOR_ARGB(alpha, red, green, blue));
            cached_ground_thickness = thickness;
            cached_ground_arrow_size = arrow_scale;
            cached_ground_colour = route_colour;
            ground_vertices_dirty = false;
        }
        if (ground_core_vertices.empty()) return false;
        // Billboard positions change with the camera, but retaining capacity avoids
        // allocator traffic in this per-frame rendering path.
        static std::vector<Vertex> stop_vertices;
        static std::vector<Vertex> stop_symbol_vertices;
        stop_vertices.clear();
        stop_symbol_vertices.clear();
        stop_vertices.reserve(route_waypoints.size() * 24);
        stop_symbol_vertices.reserve(route_waypoints.size() * 12);
        for (size_t i = 0; i < route_waypoints.size() && i < route_waypoint_narrow.size()
                           && i < route_waypoint_altitudes.size(); i++) {
            if (!route_waypoint_narrow[i] || i >= route_waypoint_reveals.size()
                || std::ranges::all_of(route_waypoint_reveals[i], FogIndexExplored)) continue;
            const auto& ground = route_waypoints[i];
            const float altitude = route_waypoint_altitudes[i];
            const GW::Vec3f center{ground.x, ground.y, altitude - 145.f};
            float vx = center.x - camera->position.x, vy = center.y - camera->position.y;
            const float vlen = hypotf(vx, vy);
            if (vlen < 1.f) continue;
            const float rx = -vy / vlen, ry = vx / vlen;
            constexpr float radius = 92.f;
            const auto vertex_at = [&](const float horizontal, const float vertical, const D3DCOLOR colour) {
                return Vertex{center.x + rx * horizontal, center.y + ry * horizontal,
                              center.z - vertical, colour};
            };
            const Vertex middle{center.x, center.y, center.z, D3DCOLOR_ARGB(245, 205, 32, 32)};
            for (int edge = 0; edge < 8; edge++) {
                const float a0 = DirectX::XM_2PI * static_cast<float>(edge) / 8.f + DirectX::XM_PI / 8.f;
                const float a1 = DirectX::XM_2PI * static_cast<float>(edge + 1) / 8.f + DirectX::XM_PI / 8.f;
                stop_vertices.insert(stop_vertices.end(), {middle,
                    vertex_at(cosf(a0) * radius, sinf(a0) * radius, middle.colour),
                    vertex_at(cosf(a1) * radius, sinf(a1) * radius, middle.colour)});
            }
            const auto append_quad = [&](const float left, const float right, const float bottom, const float top) {
                const D3DCOLOR white = D3DCOLOR_ARGB(255, 255, 250, 235);
                const Vertex bl = vertex_at(left, bottom, white), br = vertex_at(right, bottom, white);
                const Vertex tl = vertex_at(left, top, white), tr = vertex_at(right, top, white);
                stop_symbol_vertices.insert(stop_symbol_vertices.end(), {tl, bl, tr, tr, bl, br});
            };
            append_quad(-10.f, 10.f, -5.f, 45.f);
            append_quad(-11.f, 11.f, -46.f, -24.f);
        }

        if (route_state_device != device) {
            if (route_state_block) route_state_block->Release();
            route_state_block = nullptr;
            route_state_device = device;
        }
        if (!route_state_block && device->CreateStateBlock(D3DSBT_ALL, &route_state_block) != D3D_OK) return false;
        if (!route_state_block || route_state_block->Capture() != D3D_OK) return false;

        DirectX::XMFLOAT4X4 world{}, view{}, projection{};
        DirectX::XMStoreFloat4x4(&world, DirectX::XMMatrixIdentity());
        const DirectX::XMFLOAT3 eye{camera->position.x, camera->position.y, camera->position.z};
        const DirectX::XMFLOAT3 look{camera->look_at_target.x, camera->look_at_target.y, camera->look_at_target.z};
        const DirectX::XMFLOAT3 up{0.f, 0.f, -1.f};
        DirectX::XMStoreFloat4x4(&view, DirectX::XMMatrixLookAtLH(
            DirectX::XMLoadFloat3(&eye), DirectX::XMLoadFloat3(&look), DirectX::XMLoadFloat3(&up)));
        DirectX::XMStoreFloat4x4(&projection, DirectX::XMMatrixPerspectiveFovLH(
            GW::Render::GetFieldOfView(), static_cast<float>(width) / static_cast<float>(height), 46.875f, 48000.f));

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        device->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&world));
        device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&view));
        device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&projection));
        device->SetTexture(0, nullptr);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        HRESULT drawn = D3D_OK;
        size_t vertex_offset = 0;
        for (size_t i = 1; i < first && i < ground_route.size(); i++) {
            const float dx = ground_route[i].x - ground_route[i - 1].x;
            const float dy = ground_route[i].y - ground_route[i - 1].y;
            const float length = hypotf(dx, dy);
            if (length >= .1f) vertex_offset += arrows_in_segment(length) * 12;
        }
        vertex_offset = std::min(vertex_offset, ground_core_vertices.size());
        const size_t core_count = ground_core_vertices.size() - vertex_offset;
        const size_t border_count = ground_border_vertices.size() - std::min(vertex_offset, ground_border_vertices.size());
        if (border_count >= 3) drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
            static_cast<UINT>(border_count / 3), ground_border_vertices.data() + vertex_offset, sizeof(Vertex));
        if (drawn == D3D_OK) drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
            static_cast<UINT>(core_count / 3), ground_core_vertices.data() + vertex_offset, sizeof(Vertex));
        if (drawn == D3D_OK && !stop_vertices.empty()) drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
            static_cast<UINT>(stop_vertices.size() / 3), stop_vertices.data(), sizeof(Vertex));
        if (drawn == D3D_OK && !stop_symbol_vertices.empty()) drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST,
            static_cast<UINT>(stop_symbol_vertices.size() / 3), stop_symbol_vertices.data(), sizeof(Vertex));
        ground_segments_drawn = drawn == D3D_OK ? core_count / 12 : 0;

        route_state_block->Apply();
        return drawn == D3D_OK;
    }

}

DLLAPI ToolboxPlugin* ToolboxPluginInstance()
{
    static WorldCompletionPlugin instance;
    return &instance;
}

void WorldCompletionPlugin::Initialize(ImGuiContext* ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    const auto on_map_lifecycle = [](GW::HookStatus*, GW::UI::UIMessage, void*, void*) {
        // kMapChange arrives before GW starts replacing map-owned buffers. Stop
        // rendering immediately; Update performs the actual cleanup safely.
        route_suspended.store(true, std::memory_order_release);
        transition_event.store(true, std::memory_order_release);
    };
    GW::UI::RegisterUIMessageCallback(&map_lifecycle_hook, GW::UI::UIMessage::kMapChange, on_map_lifecycle, -0x4000);
    GW::UI::RegisterUIMessageCallback(&map_lifecycle_hook, GW::UI::UIMessage::kMapLoaded, on_map_lifecycle, -0x4000);
    GW::UI::RegisterUIMessageCallback(&map_lifecycle_hook, GW::UI::UIMessage::kTravel, on_map_lifecycle, -0x4000);
}

void WorldCompletionPlugin::SignalTerminate()
{
    // Overlay/path callbacks will be removed here once the host bridge is
    // added. Keeping the lifecycle hook in place prevents teardown leaks.
    GW::UI::RemoveUIMessageCallback(&map_lifecycle_hook);
    recompute_requested.store(false, std::memory_order_release);
    if (!settings_folder_.empty()) SaveSettings(settings_folder_.c_str());
    ToolboxUIPlugin::SignalTerminate();
    debug_log.close();
    if (route_state_block) {
        route_state_block->Release();
        route_state_block = nullptr;
    }
    route_state_device = nullptr;
}

void WorldCompletionPlugin::LoadSettings(const wchar_t* folder)
{
    settings_folder_ = folder ? folder : L"";
    ToolboxUIPlugin::LoadSettings(folder);
    debug_log.close();
    last_log_summary.clear();
    const std::filesystem::path log_path = std::filesystem::path(folder) / L"WorldCompletion.log";
    debug_log.open(log_path, std::ios::out | std::ios::trunc);
    if (debug_log.is_open()) {
        debug_log << "World Completion standalone diagnostics\n";
        debug_log << "Orange waypoints are stand positions; they can be explored when they reveal adjacent fog.\n";
        debug_log.flush();
    }
    LoadSetting("show_debug_status", show_debug_status_);
    LoadSetting("show_route", show_route_);
    LoadSetting("show_ground_route", show_ground_route_);
    LoadSetting("occlude_ground_route", occlude_ground_route_);
    LoadSetting("show_numbers", show_numbers_);
    LoadSetting("route_thickness", route_thickness_);
    LoadSetting("arrow_size_percent", arrow_size_percent);
    LoadSetting("route_colour", route_colour);
    LoadSetting("optimization_quality", optimization_quality);
    LoadSetting("relaxation_passes", relaxation_passes);
    LoadSetting("randomized_restarts", randomized_restarts);
    arrow_size_percent = std::clamp(arrow_size_percent, 10, 250);
    optimization_quality = std::clamp(optimization_quality, 0, 2);
    relaxation_passes = std::clamp(relaxation_passes, 0, 24);
    randomized_restarts = std::clamp(randomized_restarts, 0, 32);
}

void WorldCompletionPlugin::SaveSettings(const wchar_t* folder)
{
    if (folder) settings_folder_ = folder;
    SaveSetting("show_debug_status", show_debug_status_);
    SaveSetting("show_route", show_route_);
    SaveSetting("show_ground_route", show_ground_route_);
    SaveSetting("occlude_ground_route", occlude_ground_route_);
    SaveSetting("show_numbers", show_numbers_);
    SaveSetting("route_thickness", route_thickness_);
    SaveSetting("arrow_size_percent", arrow_size_percent);
    SaveSetting("route_colour", route_colour);
    SaveSetting("optimization_quality", optimization_quality);
    SaveSetting("relaxation_passes", relaxation_passes);
    SaveSetting("randomized_restarts", randomized_restarts);
    ToolboxUIPlugin::SaveSettings(folder);
}

bool WorldCompletionPlugin::IsLoadedContext() const
{
    return GW::Map::GetIsMapLoaded()
        && GW::Map::GetInstanceType() != GW::Constants::InstanceType::Loading;
}

bool WorldCompletionPlugin::IsPogahnMissionAdaptationActive() const
{
    return IsLoadedContext()
        && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable
        && GW::Map::GetMapID() == GW::Constants::MapID::Pogahn_Passage;
}

void WorldCompletionPlugin::Update(const float)
{
    const uint32_t now = GetTickCount();
    if (transition_event.exchange(false, std::memory_order_acq_rel)) {
        SuspendRoute(now);
        return;
    }
    current_instance_ = GW::Map::GetInstanceType();
    if (!GW::Map::GetIsMapLoaded() || current_instance_ == GW::Constants::InstanceType::Loading) {
        if (!route_suspended || pending_map != GW::Constants::MapID::None) SuspendRoute(now);
        return;
    }
    if (current_instance_ == GW::Constants::InstanceType::Outpost) {
        if (!route_suspended || pending_map != GW::Constants::MapID::None) SuspendRoute(now);
        return;
    }
    current_map_ = GW::Map::GetMapID();

    if (route_suspended) {
        if (pending_map != current_map_ || pending_instance != current_instance_) {
            pending_map = current_map_;
            pending_instance = current_instance_;
            resume_after = now + 1500;
            return;
        }
        if (static_cast<int32_t>(now - resume_after) < 0) return;
        const auto* player = GW::Agents::GetControlledCharacter();
        const auto* world = GW::GetWorldContext();
        const auto* maps = GW::Map::GetPathingMap();
        if (!player || !world || !maps || !maps->valid() || !world->cartographed_areas.valid()) {
            resume_after = now + 500;
            return;
        }
        route_suspended = false;
        last_rebuild = now;
        BuildRoute();
        if (world->cartographed_areas.valid()) {
            const auto* current = reinterpret_cast<const uint32_t*>(world->cartographed_areas.m_buffer);
            cartography_snapshot.assign(current, current + world->cartographed_areas.size());
        }
        return;
    }

    if (current_map_ != route_map || current_instance_ != route_instance) {
        SuspendRoute(now);
        pending_map = current_map_;
        pending_instance = current_instance_;
        resume_after = now + 1500;
        return;
    }
    bool unexpected_discovery = false;
    if (now - last_fog_check >= 500) {
        last_fog_check = now;
        const auto* world = GW::GetWorldContext();
        if (world && world->cartographed_areas.valid()) {
            const auto* current = reinterpret_cast<const uint32_t*>(world->cartographed_areas.m_buffer);
            const size_t words = world->cartographed_areas.size();
            if (cartography_snapshot.size() != words) {
                cartography_snapshot.assign(current, current + words);
            }
            else {
                bool changed = false;
                for (size_t word = 0; word < words; word++) {
                    uint32_t newly_explored = current[word] & ~cartography_snapshot[word];
                    while (newly_explored) {
                        const uint32_t bit = std::countr_zero(newly_explored);
                        const uint32_t fog = static_cast<uint32_t>(word * 32 + bit);
                        const bool planned = std::ranges::any_of(route_waypoint_reveals, [&](const auto& reveals) {
                            return std::ranges::find(reveals, fog) != reveals.end();
                        });
                        unexpected_discovery |= !planned;
                        newly_explored &= newly_explored - 1;
                        changed = true;
                    }
                }
                if (changed) {
                    ground_vertices_dirty = true;
                }
                cartography_snapshot.assign(current, current + words);
            }
        }
    }
    if (const auto* player = GW::Agents::GetControlledCharacter(); player && route.size() > 1) {
        route_progress = ClosestRoutePointThrough({player->pos.x, player->pos.y}, CurrentRouteEnd());
        if (ground_route.size() > 1) ground_progress = ClosestGroundPoint({player->pos.x, player->pos.y});
    }
    const bool transient_retry = route.empty() && now - last_rebuild >= 3000;
    const bool manual_recompute = recompute_requested.exchange(false, std::memory_order_acq_rel);
    if (unexpected_discovery || transient_retry || manual_recompute) {
        last_rebuild = now;
        BuildRoute();
        if (const auto* world = GW::GetWorldContext(); world && world->cartographed_areas.valid()) {
            const auto* current = reinterpret_cast<const uint32_t*>(world->cartographed_areas.m_buffer);
            cartography_snapshot.assign(current, current + world->cartographed_areas.size());
        }
    }
}

void WorldCompletionPlugin::DrawSettings()
{
    ImGui::Checkbox("Show debug status", &show_debug_status_);
    ImGui::Checkbox("Show completion route", &show_route_);
    ImGui::Checkbox("Show route on the ground", &show_ground_route_);
    ImGui::Checkbox("Occlude ground route", &occlude_ground_route_);
    ImGui::Checkbox("Show waypoint numbers", &show_numbers_);
    ImGui::SliderFloat("Route thickness", &route_thickness_, 1.f, 6.f, "%.1f px");
    ImGui::SliderInt("Arrow size", &arrow_size_percent, 10, 250, "%d%%");
    ImVec4 colour = ImGui::ColorConvertU32ToFloat4(route_colour);
    if (ImGui::ColorEdit4("Route color", &colour.x, ImGuiColorEditFlags_AlphaBar)) {
        route_colour = ImGui::ColorConvertFloat4ToU32(colour);
        ground_vertices_dirty = true;
    }
    const char* quality_names[] = {"Fast", "Balanced", "Thorough"};
    bool optimization_changed = ImGui::Combo("Optimization quality", &optimization_quality,
                                             quality_names, IM_ARRAYSIZE(quality_names));
    optimization_changed |= ImGui::SliderInt("Relaxation passes", &relaxation_passes, 0, 24);
    optimization_changed |= ImGui::SliderInt("Randomized restarts", &randomized_restarts, 0, 32);
    if (optimization_changed) recompute_requested.store(true, std::memory_order_release);
}

void WorldCompletionPlugin::Draw(IDirect3DDevice9* device)
{
    if (route_suspended || !IsLoadedContext() || route_map != GW::Map::GetMapID()
        || route_instance != GW::Map::GetInstanceType()) return;

    const float arrow_scale = static_cast<float>(arrow_size_percent) / 100.f;
    if (show_ground_route_ && current_instance_ == GW::Constants::InstanceType::Explorable) {
        if (!occlude_ground_route_ || !DrawOccludedGroundRoute(device, route_thickness_, arrow_scale)) DrawGroundRoute(arrow_scale);
    }

    ImRect mission_clip;
    ImVec2 mission_point;
    if (ProjectMissionMap({}, mission_point, mission_clip)) {
        constexpr float button_size = 30.f;
        constexpr float button_gap = 4.f;
        ImGui::SetNextWindowPos({mission_clip.Max.x - button_size * 2.f - button_gap - 7.f,
                                 mission_clip.Max.y - button_size - 7.f});
        ImGui::SetNextWindowBgAlpha(0.f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
        if (ImGui::Begin("##world_completion_mission_toggle", nullptr, flags)) {
            if (ImGui::InvisibleButton("##toggle", {button_size, button_size})) show_route_ = !show_route_;
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            auto* button_draw = ImGui::GetWindowDrawList();
            button_draw->AddRectFilled(min, max, hovered ? IM_COL32(239, 246, 248, 255) : IM_COL32(213, 226, 230, 245), 5.f);
            button_draw->AddRect(min, max, IM_COL32(20, 42, 50, 255), 5.f, 0, 2.f);
            const ImU32 icon_colour = show_route_ ? route_colour : IM_COL32(75, 91, 98, 255);
            const ImVec2 p0{min.x + 6.f, max.y - 7.f};
            const ImVec2 p1{min.x + 11.f, min.y + 10.f};
            const ImVec2 p2{min.x + 17.f, min.y + 17.f};
            const ImVec2 p3{max.x - 6.f, min.y + 7.f};
            button_draw->AddLine(p0, p1, icon_colour, 2.5f);
            button_draw->AddLine(p1, p2, icon_colour, 2.5f);
            button_draw->AddLine(p2, p3, icon_colour, 2.5f);
            button_draw->AddCircleFilled(p0, 2.2f, icon_colour);
            button_draw->AddCircleFilled(p3, 2.2f, icon_colour);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle the world-completion route on this map");
            ImGui::SameLine(0.f, button_gap);
            if (ImGui::InvisibleButton("##recompute", {button_size, button_size})) recompute_requested.store(true, std::memory_order_release);
            const bool refresh_hovered = ImGui::IsItemHovered();
            const ImVec2 refresh_min = ImGui::GetItemRectMin();
            const ImVec2 refresh_max = ImGui::GetItemRectMax();
            button_draw->AddRectFilled(refresh_min, refresh_max,
                                       refresh_hovered ? IM_COL32(239, 246, 248, 255) : IM_COL32(213, 226, 230, 245), 5.f);
            button_draw->AddRect(refresh_min, refresh_max, IM_COL32(20, 42, 50, 255), 5.f, 0, 2.f);
            const ImVec2 center{(refresh_min.x + refresh_max.x) * .5f, (refresh_min.y + refresh_max.y) * .5f};
            button_draw->PathArcTo(center, 8.f, -2.8f, .5f, 16);
            button_draw->PathStroke(route_colour, 0, 2.5f);
            button_draw->AddTriangleFilled({center.x + 8.f, center.y - 4.f}, {center.x + 11.f, center.y + 2.f},
                                           {center.x + 4.f, center.y + 1.f}, route_colour);
            if (refresh_hovered) ImGui::SetTooltip("Recompute the world-completion route");
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }

    if (show_route_ && route_connected && !route_waypoints.empty() && route.size() > 1) {
        auto* dl = ImGui::GetBackgroundDrawList();
        ImRect clip;
        GW::Vec2f first_wm;
        ImVec2 previous;
        const auto display_map = CartographyMapID();
        if (const auto* player = GW::Agents::GetControlledCharacter(); player && GameToWorld(player->pos, first_wm, display_map)) {
            const size_t first_route_point = route_progress;
            bool world_surface = ProjectWorldMap(first_wm, previous, clip);
            if (!world_surface) world_surface = ProjectMissionMap(first_wm, previous, clip);
            if (world_surface) {
                dl->PushClipRect(clip.Min, clip.Max, true);
                const ImU32 colour = route_colour;
                const bool draw_world_arrows = GW::UI::GetIsWorldMapShowing();
                for (size_t i = first_route_point; i < route.size(); i++) {
                    GW::Vec2f wm;
                    ImVec2 screen;
                    if (!GameToWorld(route[i], wm, display_map)) continue;
                    const bool projected = GW::UI::GetIsWorldMapShowing()
                        ? ProjectWorldMap(wm, screen, clip)
                        : ProjectMissionMap(wm, screen, clip);
                    if (!projected) continue;
                    if (!draw_world_arrows) {
                        dl->AddLine(previous, screen, colour, route_thickness_);
                        previous = screen;
                        continue;
                    }
                    const float dx = screen.x - previous.x, dy = screen.y - previous.y;
                    const float length = hypotf(dx, dy);
                    if (length > 7.f) {
                        const float ux = dx / length, uy = dy / length;
                        const float px = -uy, py = ux;
                        const float arrow_size = std::clamp(route_thickness_ * 3.f, 7.f, 14.f) * arrow_scale;
                        for (float start = 0.f; start < length; start += arrow_size) {
                            const float end = std::min(start + arrow_size, length);
                            if (end - start < arrow_size * .35f) break;
                            const ImVec2 tip{previous.x + ux * end, previous.y + uy * end};
                            const ImVec2 base{previous.x + ux * start, previous.y + uy * start};
                            dl->AddLine({base.x + px * arrow_size * .6f, base.y + py * arrow_size * .6f},
                                        tip, colour, route_thickness_);
                            dl->AddLine(tip,
                                        {base.x - px * arrow_size * .6f, base.y - py * arrow_size * .6f},
                                        colour, route_thickness_);
                        }
                    }
                    previous = screen;
                }
                for (size_t i = 0; i < route_waypoints.size(); i++) {
                    if (i < route_waypoint_reveals.size()
                        && std::ranges::all_of(route_waypoint_reveals[i], FogIndexExplored)) continue;
                    GW::Vec2f wm;
                    ImVec2 screen;
                    if (!GameToWorld(route_waypoints[i], wm, display_map)) continue;
                    const bool projected = GW::UI::GetIsWorldMapShowing()
                        ? ProjectWorldMap(wm, screen, clip)
                        : ProjectMissionMap(wm, screen, clip);
                    if (!projected) continue;
                    bool narrow_stop = i < route_waypoint_narrow.size() && route_waypoint_narrow[i];
                    if (narrow_stop && i < route_waypoint_reveals.size()) {
                        const auto* world = GW::GetWorldContext();
                        if (world && world->cartographed_areas.valid() && fog_grid_width) {
                            const auto* bits = reinterpret_cast<const uint32_t*>(world->cartographed_areas.m_buffer);
                            const uint32_t words = world->cartographed_areas.size();
                            narrow_stop = std::ranges::any_of(route_waypoint_reveals[i], [&](const uint32_t fog) {
                                return !IsExplored(bits, fog_grid_width, world->h05B4[1], words,
                                                   static_cast<int>(fog % fog_grid_width),
                                                   static_cast<int>(fog / fog_grid_width));
                            });
                        }
                    }
                    if (narrow_stop) {
                        dl->AddNgonFilled(screen, 8.f, IM_COL32(210, 35, 35, 245), 8);
                        dl->AddNgon(screen, 8.f, IM_COL32(255, 245, 225, 255), 8, 2.f);
                        dl->AddText({screen.x - 2.f, screen.y - 7.f}, IM_COL32_WHITE, "!");
                    }
                    else {
                        dl->AddCircleFilled(screen, 4.f, colour);
                    }
                    if (show_numbers_) {
                        const std::string label = std::to_string(i + 1);
                        dl->AddText({screen.x + 6.f, screen.y - 7.f}, colour, label.c_str());
                    }
                }
                dl->PopClipRect();
            }
        }
    }

    if (!show_debug_status_ || !GetVisiblePtr()) return;

    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin(Name(), GetVisiblePtr(), ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Current map: %d", static_cast<int>(current_map_));
        ImGui::Text("Instance: %s",
            current_instance_ == GW::Constants::InstanceType::Outpost ? "Outpost" : "Explorable");
        ImGui::Text("Pogahn mission adaptation: %s",
            IsPogahnMissionAdaptationActive() ? "active" : "inactive");
        ImGui::Text("Useful stand cells: %d", useful_candidates);
        ImGui::Text("Reachable fog cells: %d", unexplored_reachable);
        ImGui::Text("Route waypoints: %u", static_cast<unsigned>(route_waypoints.size()));
        ImGui::Text("Ground route points: %u", static_cast<unsigned>(ground_route.size()));
        ImGui::Text("Ground segments visible: %u", static_cast<unsigned>(ground_segments_drawn));
    }
    ImGui::End();
}
