#include "WorldCompletion.h"
#include "PortalLocations.h"
#include "RoutePlanner.h"

#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>

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

    int FogCellX(const float world_x)
    {
        return static_cast<int>(floorf(world_x / kWorldUnitsPerCell));
    }
    int FogCellY(const float world_y)
    {
        return static_cast<int>(ceilf(world_y / kWorldUnitsPerCell)) - 1;
    }

    bool MaskContains(const CartographyData::Mask *mask, const int cx, const int cy)
    {
        if (!mask) return true;
        const int x = cx - mask->x0, y = cy - mask->y0;
        if (x < 0 || y < 0 || x >= mask->width || y >= mask->height) return false;
        const size_t bit = static_cast<size_t>(y) * mask->width + x;
        return bit / 8 < static_cast<size_t>(mask->byte_count) && ((mask->bits[bit >> 3] >> (bit & 7)) & 1u) != 0;
    }

    const CartographyData::Mask *CreditableMask(const GW::AreaInfo *map_info)
    {
        if (!map_info) return nullptr;
        const int continent = static_cast<int>(map_info->continent);
        for (const auto &data : CartographyData::kContinents) {
            if (data.id == continent) return &data.creditable;
        }
        return nullptr;
    }

    struct Candidate {
        GW::GamePos pos{};
        const GW::PathingTrapezoid *trap = nullptr;
        std::vector<uint32_t> reveals;
        float footing_area = 0.f;
        int cell_x = 0;
        int cell_y = 0;
    };
    struct GroundVertex {
        float x, y, z;
        D3DCOLOR colour;
    };

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
    uint32_t route_colour = IM_COL32(255, 145, 35, 245);
    int optimization_quality = 1;
    int relaxation_passes = 8;
    int randomized_restarts = 12;
    int end_portal_index = -1;
    uint32_t end_portal_map = 0;
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
    bool addon_enabled = true;
    bool terminating = false;
    std::vector<size_t> route_waypoint_indices;
    std::vector<size_t> ground_route_indices;
    size_t ground_build_leg = 1;
    int ground_build_sample = 0;
    bool ground_building = false;
    std::string build_status = "Waiting for map";
    float build_milliseconds = 0.f;
    uint32_t active_colour = IM_COL32(80, 235, 255, 255);
    uint32_t outline_colour = IM_COL32(8, 12, 18, 230);
    float future_opacity = .3f;
    float outline_width = 1.5f;
    float waypoint_size = 5.f;
    bool active_leg_only = false;
    bool map_arrows = true;
    float ground_horizon = 6000.f;
    size_t cached_ground_first = 0, cached_ground_last = 0, cached_active_end = 0;
    void CancelBuild();
    IDirect3DStateBlock9 *route_state_block = nullptr;
    IDirect3DDevice9 *route_state_device = nullptr;
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
    std::optional<GW::GamePos> route_end_portal_point;

    GW::Constants::MapID CartographyMapID();

    void ClearRouteState()
    {
        route.clear();
        route_waypoint_indices.clear();
        ground_route_indices.clear();
        ground_building = false;
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
        ground_vertices_dirty = true;
        route_progress = 0;
        ground_progress = 0;
        route_end_portal_point.reset();
    }

    void SuspendRoute(const uint32_t now)
    {
        CancelBuild();
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

    void LogBuild(const char *state, const size_t trapezoids = 0, const size_t reachable = 0, const size_t stand_cells = 0,
                  const size_t uncovered_after_route = 0)
    {
        if (!debug_log.is_open()) return;
        std::ostringstream out;
        out << "map=" << static_cast<uint32_t>(GW::Map::GetMapID()) << " cartography_map=" << static_cast<uint32_t>(CartographyMapID())
            << " instance=" << static_cast<uint32_t>(GW::Map::GetInstanceType()) << " planner=portal_graph"
            << " state=" << state << " trapezoids=" << trapezoids << " reachable=" << reachable << " stand_cells=" << stand_cells
            << " useful=" << useful_candidates << " fog=" << unexplored_reachable << " border_fog=" << unexplored_border
            << " route=" << route.size() << " route_length=" << route_length << " waypoints=" << route_waypoints.size()
            << " uncovered_after_route=" << uncovered_after_route;
        const std::string summary = out.str();
        if (summary == last_log_summary) return;
        last_log_summary = summary;
        debug_log << GetTickCount() << ' ' << summary << '\n';
        if (const auto *player = GW::Agents::GetControlledCharacter())
            debug_log << "  player=" << player->pos.x << ',' << player->pos.y << '\n';
        debug_log.flush();
    }

    bool GetMapBounds(const GW::AreaInfo *info, ImRect &out)
    {
        if (!info) return false;
        const uint32_t *bounds = &info->icon_start_x;
        if (!bounds[0]) bounds = &info->icon_start_x_dupe;
        out = {{static_cast<float>(static_cast<int32_t>(bounds[0])), static_cast<float>(static_cast<int32_t>(bounds[1]))},
               {static_cast<float>(static_cast<int32_t>(bounds[2])), static_cast<float>(static_cast<int32_t>(bounds[3]))}};
        return out.GetWidth() > 0.f && out.GetHeight() > 0.f;
    }

    GW::Constants::MapID CartographyMapID()
    {
        const auto current = GW::Map::GetMapID();
        if (GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable) return current;
        if (current == GW::Constants::MapID::Pogahn_Passage) return GW::Constants::MapID::Gandara_the_Moon_Fortress;

        static GW::Constants::MapID cached_current = GW::Constants::MapID::None;
        static GW::Constants::MapID cached_display = GW::Constants::MapID::None;
        if (cached_current == current) return cached_display;
        cached_current = current;
        cached_display = current;

        // Missions whose playable zone belongs to a separate mission outpost
        // use that outpost's world-map bounds and placement.
        for (uint32_t id = 0; id < static_cast<uint32_t>(GW::Constants::MapID::Count); id++) {
            const auto *info = GW::Map::GetMapInfo(static_cast<GW::Constants::MapID>(id));
            if (info && (info->flags & 1) && info->GetHasMissionMapsTo() && info->mission_maps_to == static_cast<uint32_t>(current)) {
                cached_display = static_cast<GW::Constants::MapID>(id);
                break;
            }
        }
        return cached_display;
    }

    bool GameToWorld(const GW::GamePos &game, GW::Vec2f &world, const GW::Constants::MapID display_map)
    {
        const auto *map_context = GW::GetMapContext();
        const auto *info = GW::Map::GetMapInfo(display_map);
        ImRect bounds;
        if (!map_context || !GetMapBounds(info, bounds)) return false;
        const GW::Vec2f anchor = {
            bounds.Min.x - map_context->start_pos.x / kGwinchesPerWorldUnit,
            bounds.Min.y + map_context->end_pos.y / kGwinchesPerWorldUnit + 1.f,
        };
        world = {game.x / kGwinchesPerWorldUnit + anchor.x, -game.y / kGwinchesPerWorldUnit + anchor.y};
        return std::isfinite(world.x) && std::isfinite(world.y);
    }

    bool WorldToGame(const GW::Vec2f &world, GW::Vec2f &game, const GW::Constants::MapID display_map)
    {
        const auto *map_context = GW::GetMapContext();
        const auto *info = GW::Map::GetMapInfo(display_map);
        ImRect bounds;
        if (!map_context || !GetMapBounds(info, bounds)) return false;
        const GW::Vec2f anchor = {
            bounds.Min.x - map_context->start_pos.x / kGwinchesPerWorldUnit,
            bounds.Min.y + map_context->end_pos.y / kGwinchesPerWorldUnit + 1.f,
        };
        game = {(world.x - anchor.x) * kGwinchesPerWorldUnit, -(world.y - anchor.y) * kGwinchesPerWorldUnit};
        return std::isfinite(game.x) && std::isfinite(game.y);
    }

    bool Contains(const GW::PathingTrapezoid *t, const GW::Vec2f &p)
    {
        if (!t || p.y < t->YB || p.y > t->YT) return false;
        const float height = t->YT - t->YB;
        if (height <= 0.001f) return false;
        const float a = (p.y - t->YB) / height;
        const float left = t->XBL + a * (t->XTL - t->XBL);
        const float right = t->XBR + a * (t->XTR - t->XBR);
        return p.x >= left && p.x <= right;
    }

    GW::Vec2f ClosestPoint(const GW::PathingTrapezoid *trap, const GW::Vec2f &point)
    {
        if (Contains(trap, point)) return point;
        const GW::Vec2f corners[4] = {{trap->XTL, trap->YT}, {trap->XTR, trap->YT}, {trap->XBR, trap->YB}, {trap->XBL, trap->YB}};
        GW::Vec2f closest = corners[0];
        float closest_distance = FLT_MAX;
        for (size_t i = 0; i < 4; i++) {
            const auto a = corners[i], b = corners[(i + 1) % 4];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const float length_sq = dx * dx + dy * dy;
            const float t = length_sq > 0.f ? std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) / length_sq, 0.f, 1.f) : 0.f;
            const GW::Vec2f candidate{a.x + dx * t, a.y + dy * t};
            const float ox = candidate.x - point.x, oy = candidate.y - point.y;
            const float distance = ox * ox + oy * oy;
            if (distance >= closest_distance) continue;
            closest_distance = distance;
            closest = candidate;
        }
        return closest;
    }

    GW::GamePos Centre(const GW::PathingTrapezoid *t)
    {
        return {(t->XTL + t->XTR + t->XBL + t->XBR) * 0.25f, (t->YT + t->YB) * 0.5f, 0};
    }

    std::optional<completion::Portal> SharedPortal(const GW::PathingTrapezoid *from, const GW::PathingTrapezoid *to)
    {
        const completion::Point a[4] = {{from->XTL, from->YT}, {from->XTR, from->YT}, {from->XBR, from->YB}, {from->XBL, from->YB}};
        const completion::Point b[4] = {{to->XTL, to->YT}, {to->XTR, to->YT}, {to->XBR, to->YB}, {to->XBL, to->YB}};
        return completion::SharedBoundary(a, b);
    }

    size_t ClipHalfPlane(GW::Vec2f (&poly)[8], const size_t count, const int axis, const float limit, const bool keep_above)
    {
        if (!count) return 0;
        GW::Vec2f clipped[8];
        size_t out_count = 0;
        const auto coord = [axis](const GW::Vec2f &p) { return axis ? p.y : p.x; };
        const auto inside = [&](const GW::Vec2f &p) { return keep_above ? coord(p) >= limit : coord(p) <= limit; };
        for (size_t i = 0; i < count; i++) {
            const auto &a = poly[i];
            const auto &b = poly[(i + 1) % count];
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

    bool TrapezoidCellOverlap(const GW::PathingTrapezoid *trap, const GW::Vec2f &box_min, const GW::Vec2f &box_max, GW::Vec2f &footing,
                              float &overlap_area)
    {
        if (!trap || std::min(trap->XTL, trap->XBL) > box_max.x || std::max(trap->XTR, trap->XBR) < box_min.x || trap->YB > box_max.y ||
            trap->YT < box_min.y)
            return false;
        GW::Vec2f poly[8] = {{trap->XTL, trap->YT}, {trap->XTR, trap->YT}, {trap->XBR, trap->YB}, {trap->XBL, trap->YB}};
        size_t count = 4;
        count = ClipHalfPlane(poly, count, 0, box_min.x, true);
        count = ClipHalfPlane(poly, count, 0, box_max.x, false);
        count = ClipHalfPlane(poly, count, 1, box_min.y, true);
        count = ClipHalfPlane(poly, count, 1, box_max.y, false);
        if (count < 3) return false;
        float area2 = 0.f;
        GW::Vec2f centroid{};
        for (size_t i = 0; i < count; i++) {
            const auto &a = poly[i];
            const auto &b = poly[(i + 1) % count];
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

    uint32_t FileHashToFileId(const wchar_t *hash)
    {
        if (!hash) return 0;
        if (hash[0] > 0xff && hash[1] > 0xff && (hash[2] == 0 || (hash[2] > 0xff && hash[3] == 0)))
            return (hash[0] - 0xff00ff) + static_cast<uint32_t>(hash[1]) * 0xff00;
        return 0;
    }

    bool IsPortalModel(const uint32_t id)
    {
        switch (id) {
        case 0x4e6b2:
        case 0x3c5ac:
        case 0xa825:
        case 0xe723:
        case 0x858b:
        case 0x28da0:
        case 0x1c533:
        case 0x5e77a:
            return true;
        default:
            return false;
        }
    }

    std::vector<GW::Vec2f> CurrentPortalEndpoints()
    {
        std::vector<GW::Vec2f> endpoints;
        const uint32_t map_id = static_cast<uint32_t>(GW::Map::GetMapID());
        for (const auto &endpoint : world_completion_portals::locations) {
            if (endpoint.map_id == map_id) endpoints.push_back({endpoint.x, endpoint.y});
        }
        if (const auto *context = GW::GetMapContext(); context && context->props) {
            for (const auto *prop : context->props->propArray) {
                if (!prop || !prop->model_info || !IsPortalModel(FileHashToFileId(prop->model_info->model_file_name))) continue;
                const GW::Vec2f point{prop->position.x, prop->position.y};
                const bool duplicate = std::ranges::any_of(
                    endpoints, [&](const GW::Vec2f &existing) { return hypotf(existing.x - point.x, existing.y - point.y) < 1800.f; });
                if (!duplicate) endpoints.push_back(point);
            }
        }
        std::ranges::sort(endpoints, [](const GW::Vec2f &a, const GW::Vec2f &b) { return a.x == b.x ? a.y < b.y : a.x < b.x; });
        return endpoints;
    }

    std::vector<Doorway> GetBlockedDoorways(const GW::MapContext *context)
    {
        std::vector<Doorway> result;
        if (context && context->props) {
            for (const auto *prop : context->props->propArray) {
                if (!prop || !prop->model_info || !IsPortalModel(FileHashToFileId(prop->model_info->model_file_name))) continue;
                const float radius = prop->model_info->bounding_radius > 0.f ? prop->scale * prop->model_info->bounding_radius : 400.f;
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
        for (const auto &endpoint : world_completion_portals::locations) {
            if (endpoint.map_id != map_id) continue;
            const auto existing = std::ranges::find_if(result, [&](const Doorway &doorway) {
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

    float DistanceToSegmentSq(const GW::Vec2f &p, const GW::Vec2f &a, const GW::Vec2f &b)
    {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len_sq = dx * dx + dy * dy;
        const float t = len_sq > 0.f ? std::clamp(((p.x - a.x) * dx + (p.y - a.y) * dy) / len_sq, 0.f, 1.f) : 0.f;
        const float ox = a.x + dx * t - p.x, oy = a.y + dy * t - p.y;
        return ox * ox + oy * oy;
    }

    bool CrossesDoorway(const std::vector<Doorway> &doors, const GW::PathingTrapezoid *a, const GW::PathingTrapezoid *b)
    {
        const auto ca = Centre(a), cb = Centre(b);
        return std::ranges::any_of(
            doors, [&](const Doorway &door) { return DistanceToSegmentSq(door.pos, {ca.x, ca.y}, {cb.x, cb.y}) < door.radius_sq; });
    }

    bool IsBeyondKnownPortal(const GW::Vec2f &point, const GW::Vec2f &map_min, const GW::Vec2f &map_max, const uint32_t map_id)
    {
        constexpr float half_width = 4000.f;
        for (const auto &endpoint : world_completion_portals::locations) {
            if (endpoint.map_id != map_id) continue;
            const float distances[4] = {fabsf(endpoint.x - map_min.x), fabsf(endpoint.x - map_max.x), fabsf(endpoint.y - map_min.y),
                                        fabsf(endpoint.y - map_max.y)};
            const size_t side = static_cast<size_t>(std::min_element(std::begin(distances), std::end(distances)) - distances);
            const float lateral = side < 2 ? fabsf(point.y - endpoint.y) : fabsf(point.x - endpoint.x);
            if (lateral >= half_width) continue;
            if ((side == 0 && point.x < endpoint.x) || (side == 1 && point.x > endpoint.x) || (side == 2 && point.y < endpoint.y) ||
                (side == 3 && point.y > endpoint.y))
                return true;
        }
        return false;
    }

    bool IsExplored(const uint32_t *bits, const uint32_t width, const uint32_t height, const uint32_t words, const int cx, const int cy)
    {
        if (!bits || cx < 0 || cy < 0 || static_cast<uint32_t>(cx) >= width || static_cast<uint32_t>(cy) >= height) return true;
        const uint32_t word = static_cast<uint32_t>(cy) * (width >> 5) + (static_cast<uint32_t>(cx) >> 5);
        return word >= words || ((bits[word] >> (static_cast<uint32_t>(cx) & 31)) & 1u) != 0;
    }

    bool FogIndexExplored(const uint32_t fog)
    {
        const auto *world = GW::GetWorldContext();
        if (!world || !world->cartographed_areas.valid() || !fog_grid_width) return false;
        const auto *bits = reinterpret_cast<const uint32_t *>(world->cartographed_areas.m_buffer);
        const uint32_t word = fog >> 5;
        return word < world->cartographed_areas.size() && ((bits[word] >> (fog & 31)) & 1u) != 0;
    }

    void UpdateGroundRoute()
    {
        if (!ground_building) return;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        do {
            if (ground_build_leg >= route.size()) {
                ground_building = false;
                ground_vertices_dirty = true;
                return;
            }
            const auto &a = route[ground_build_leg - 1];
            const auto &b = route[ground_build_leg];
            const float dx = b.x - a.x, dy = b.y - a.y;
            const int samples = std::max(1, static_cast<int>(ceilf(hypotf(dx, dy) / 140.f)));
            const float t = static_cast<float>(ground_build_sample) / static_cast<float>(samples);
            GW::GamePos ground{a.x + dx * t, a.y + dy * t, a.zplane};
            const float z = GW::Map::QueryAltitude(&ground, 64.f);
            ground_route.push_back({ground.x, ground.y, z - 48.f});
            if (ground_build_sample == samples) {
                ground_route_indices.push_back(ground_route.size() - 1);
                ++ground_build_leg;
                ground_build_sample = 1;
            }
            else
                ++ground_build_sample;
        } while (std::chrono::steady_clock::now() < deadline);
    }

    size_t ClosestRoutePointThrough(const GW::GamePos &player, const size_t last)
    {
        size_t closest = std::min(route_progress, route.size() - 1);
        float closest_distance = FLT_MAX;
        for (size_t i = closest; i <= std::min(last, route.size() - 1); i++) {
            if (route[i].zplane != player.zplane && (!i || route[i - 1].zplane != player.zplane)) continue;
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
        for (size_t i = 0; i < route_waypoint_reveals.size(); ++i) {
            if (std::ranges::any_of(route_waypoint_reveals[i], [](const uint32_t fog) { return !FogIndexExplored(fog); }))
                return route_waypoint_indices[i];
        }
        return route.empty() ? 0 : route.size() - 1;
    }

    size_t CurrentGroundEnd()
    {
        const size_t end = CurrentRouteEnd();
        return end < ground_route_indices.size() ? ground_route_indices[end] : 0;
    }

    size_t ClosestGroundPoint(const GW::Vec3f &player)
    {
        size_t closest = std::min(ground_progress, ground_route.size() - 1);
        float closest_distance = FLT_MAX;
        const size_t last = std::min(CurrentGroundEnd(), ground_route.size() - 1);
        for (size_t i = closest; i <= last; ++i) {
            const float dx = ground_route[i].x - player.x, dy = ground_route[i].y - player.y;
            const float dz = ground_route[i].z + 48.f - player.z;
            const float distance = dx * dx + dy * dy + dz * dz;
            if (distance >= closest_distance) continue;
            closest_distance = distance;
            closest = i;
        }
        return closest;
    }

    struct SnapshotPlane {
        uint32_t trapezoid_count = 0;
        bool blocked = false;
        std::vector<GW::PathingTrapezoid> trapezoids;
        std::vector<uintptr_t> addresses;
        std::vector<std::vector<uintptr_t>> links;
    };

    struct BuildSnapshot {
        std::vector<SnapshotPlane> maps;
        std::vector<uint32_t> bits;
        std::vector<Doorway> doorways;
        std::vector<GW::Vec2f> endpoints;
        GW::GamePos player{};
        GW::Vec2f anchor{};
        ImRect bounds;
        const CartographyData::Mask *mask = nullptr;
        GW::Constants::MapID map = GW::Constants::MapID::None;
        GW::Constants::InstanceType instance = GW::Constants::InstanceType::Loading;
        uint32_t width = 0, height = 0;
        int quality = 1, relaxation = 0, restarts = 0, end_portal = -1;
        size_t capture_plane = 0, capture_trap = 0;
    };

    struct BuildResult {
        std::vector<GW::GamePos> route, waypoints;
        std::vector<std::vector<uint32_t>> reveals;
        std::vector<bool> narrow;
        std::vector<size_t> waypoint_indices;
        std::optional<GW::GamePos> end_point;
        GW::Constants::MapID map = GW::Constants::MapID::None;
        GW::Constants::InstanceType instance = GW::Constants::InstanceType::Loading;
        GW::GamePos origin{};
        uint32_t width = 0;
        int useful = 0, unexplored = 0, border = 0;
        bool connected = false;
        float length = 0.f, milliseconds = 0.f;
        std::string status;
    };

    struct BuildCancelled {};
    std::unique_ptr<BuildSnapshot> capture;
    std::future<BuildResult> build_future;
    std::shared_ptr<std::atomic_bool> build_cancel;

    void CancelBuild()
    {
        if (build_cancel) build_cancel->store(true, std::memory_order_relaxed);
        capture.reset();
    }

    bool BuildBusy()
    {
        return capture || build_future.valid();
    }

    bool BeginCapture()
    {
        const auto *maps = GW::Map::GetPathingMap();
        const auto *context = GW::GetMapContext();
        const auto *world = GW::GetWorldContext();
        const auto *player = GW::Agents::GetControlledCharacter();
        if (!maps || !maps->valid() || !context || !world || !player || !world->cartographed_areas.valid()) return false;
        auto snapshot = std::make_unique<BuildSnapshot>();
        const auto *info = GW::Map::GetMapInfo(CartographyMapID());
        if (!GetMapBounds(info, snapshot->bounds)) return false;
        snapshot->anchor = {snapshot->bounds.Min.x - context->start_pos.x / kGwinchesPerWorldUnit,
                            snapshot->bounds.Min.y + context->end_pos.y / kGwinchesPerWorldUnit + 1.f};
        snapshot->mask = CreditableMask(info);
        snapshot->map = GW::Map::GetMapID();
        snapshot->instance = GW::Map::GetInstanceType();
        snapshot->quality = optimization_quality;
        snapshot->relaxation = relaxation_passes;
        snapshot->restarts = randomized_restarts;
        snapshot->end_portal = end_portal_map == static_cast<uint32_t>(snapshot->map) ? end_portal_index : -1;
        snapshot->doorways = GetBlockedDoorways(context);
        snapshot->endpoints = CurrentPortalEndpoints();
        snapshot->maps.resize(maps->size());
        for (size_t i = 0; i < maps->size(); ++i) {
            snapshot->maps[i].trapezoid_count = (*maps)[i].trapezoid_count;
            snapshot->maps[i].blocked = context->path && i < context->path->blockedPlanes.size() && (context->path->blockedPlanes[i] & 1);
        }
        capture = std::move(snapshot);
        build_status = "Copying navigation mesh";
        return true;
    }

    bool ContinueCapture()
    {
        const auto *maps = GW::Map::GetPathingMap();
        const auto *context = GW::GetMapContext();
        if (!maps || !maps->valid() || maps->size() != capture->maps.size() || !context) {
            CancelBuild();
            return false;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        while (capture->capture_plane < maps->size()) {
            const auto plane_index = capture->capture_plane;
            const auto &source = (*maps)[plane_index];
            auto &dest = capture->maps[plane_index];
            if (source.trapezoid_count != dest.trapezoid_count || (source.trapezoid_count && !source.trapezoids)) {
                CancelBuild();
                return false;
            }
            if (capture->capture_trap == source.trapezoid_count) {
                ++capture->capture_plane;
                capture->capture_trap = 0;
                continue;
            }
            const auto *trap = &source.trapezoids[capture->capture_trap++];
            dest.trapezoids.push_back(*trap);
            for (auto &adjacent : dest.trapezoids.back().adjacent)
                adjacent = nullptr;
            dest.addresses.push_back(reinterpret_cast<uintptr_t>(trap));
            auto &links = dest.links.emplace_back();
            if (!dest.blocked) {
                for (const auto *adjacent : trap->adjacent)
                    if (adjacent) links.push_back(reinterpret_cast<uintptr_t>(adjacent));
                for (const auto index : {trap->portal_left, trap->portal_right}) {
                    if (index >= source.portal_count || !source.portals) continue;
                    const auto &portal = source.portals[index];
                    if ((portal.flags & 4) || !portal.pair || !portal.pair->trapezoids) continue;
                    if (context->path && portal.neighbor_plane < context->path->blockedPlanes.size() &&
                        (context->path->blockedPlanes[portal.neighbor_plane] & 1))
                        continue;
                    for (uint32_t i = 0; i < portal.pair->count; ++i)
                        if (portal.pair->trapezoids[i]) links.push_back(reinterpret_cast<uintptr_t>(portal.pair->trapezoids[i]));
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) return false;
        }
        const auto *world = GW::GetWorldContext();
        const auto *player = GW::Agents::GetControlledCharacter();
        if (!world || !player || !world->cartographed_areas.valid()) {
            CancelBuild();
            return false;
        }
        capture->player = player->pos;
        capture->width = world->h05B4[0];
        capture->height = world->h05B4[1];
        const auto *bits = reinterpret_cast<const uint32_t *>(world->cartographed_areas.m_buffer);
        capture->bits.assign(bits, bits + world->cartographed_areas.size());
        return true;
    }

    void BuildRoute(const BuildSnapshot &snapshot, BuildResult &result, const std::atomic_bool &cancelled)
    {
        const auto check = [&] {
            if (cancelled.load(std::memory_order_relaxed)) throw BuildCancelled{};
        };
        const auto map_id = snapshot.map;
        result.map = snapshot.map;
        result.instance = snapshot.instance;
        result.origin = snapshot.player;
        result.width = snapshot.width;
        const auto log_build = [&](const char *state, size_t = 0, size_t = 0, size_t = 0, size_t = 0) { result.status = state; };
        const int quality = std::clamp(snapshot.quality, 0, 2);
        const int two_opt_passes = std::array{8, 16, 24}[quality];
        const int relaxation_budget = snapshot.relaxation;
        const int restart_budget = snapshot.restarts;
        struct Player {
            GW::GamePos pos;
        } player_value{snapshot.player};
        const auto *player = &player_value;
        const auto *maps = &snapshot.maps;
        const uint32_t width = snapshot.width, height = snapshot.height;
        const auto *bits = snapshot.bits.data();
        const uint32_t words = static_cast<uint32_t>(snapshot.bits.size());
        if (!width || !height || (width & 31) || static_cast<uint64_t>(width / 32) * height > words) {
            log_build("invalid_carto_grid");
            return;
        }
        const auto game_to_world = [&](const GW::GamePos &game, GW::Vec2f &world, auto) {
            world = {game.x / kGwinchesPerWorldUnit + snapshot.anchor.x, -game.y / kGwinchesPerWorldUnit + snapshot.anchor.y};
            return std::isfinite(world.x) && std::isfinite(world.y);
        };
        const auto world_to_game = [&](const GW::Vec2f &world, GW::Vec2f &game, auto) {
            game = {(world.x - snapshot.anchor.x) * kGwinchesPerWorldUnit, -(world.y - snapshot.anchor.y) * kGwinchesPerWorldUnit};
            return std::isfinite(game.x) && std::isfinite(game.y);
        };
        const GW::Vec2f player_pos{player->pos.x, player->pos.y};
        const GW::PathingTrapezoid *start = nullptr;
        size_t trapezoid_count = 0;
        GW::Vec2f path_min{FLT_MAX, FLT_MAX}, path_max{-FLT_MAX, -FLT_MAX};
        for (const auto &map : *maps)
            trapezoid_count += map.trapezoid_count;
        for (const auto &map : *maps) {
            check();
            for (uint32_t i = 0; i < map.trapezoid_count; i++) {
                check();
                const auto &t = map.trapezoids[i];
                path_min.x = std::min(path_min.x, std::min(t.XTL, t.XBL));
                path_min.y = std::min(path_min.y, t.YB);
                path_max.x = std::max(path_max.x, std::max(t.XTR, t.XBR));
                path_max.y = std::max(path_max.y, t.YT);
            }
        }
        const auto find_start_on_plane = [&](const size_t plane) -> const GW::PathingTrapezoid * {
            if (plane >= maps->size() || (*maps)[plane].blocked) return nullptr;
            const auto &map = (*maps)[plane];
            for (uint32_t i = 0; i < map.trapezoid_count; i++) {
                check();
                if (Contains(&map.trapezoids[i], player_pos)) return &map.trapezoids[i];
            }
            return nullptr;
        };
        start = find_start_on_plane(player->pos.zplane);
        if (!start) {
            log_build("player_not_on_pathing", trapezoid_count);
            return;
        }

        std::unordered_set<const GW::PathingTrapezoid *> reachable;
        std::unordered_map<const GW::PathingTrapezoid *, std::vector<const GW::PathingTrapezoid *>> graph;
        std::deque<const GW::PathingTrapezoid *> queue;
        reachable.insert(start);
        queue.push_back(start);
        std::unordered_map<uintptr_t, const GW::PathingTrapezoid *> copied_traps;
        std::unordered_map<const GW::PathingTrapezoid *, size_t> planes;
        std::unordered_map<const GW::PathingTrapezoid *, std::vector<const GW::PathingTrapezoid *>> snapshot_graph;
        for (size_t plane = 0; plane < maps->size(); ++plane) {
            check();
            const auto &map = (*maps)[plane];
            for (size_t i = 0; i < map.trapezoids.size(); ++i) {
                check();
                copied_traps.emplace(map.addresses[i], &map.trapezoids[i]);
                planes.emplace(&map.trapezoids[i], plane);
            }
        }
        for (const auto &map : *maps) {
            check();
            for (size_t i = 0; i < map.trapezoids.size(); ++i) {
                check();
                for (const auto address : map.links[i]) {
                    check();
                    const auto found = copied_traps.find(address);
                    if (found != copied_traps.end() && !map.blocked && !(*maps)[planes.at(found->second)].blocked &&
                        SharedPortal(&map.trapezoids[i], found->second))
                        snapshot_graph[&map.trapezoids[i]].push_back(found->second);
                }
            }
        }
        std::vector<Doorway> path_doorways = snapshot.doorways;
        std::erase_if(path_doorways, [&](const Doorway &door) {
            const float dx = player->pos.x - door.pos.x, dy = player->pos.y - door.pos.y;
            return dx * dx + dy * dy < door.radius_sq;
        });
        const auto add_edge = [&](const GW::PathingTrapezoid *a, const GW::PathingTrapezoid *b) {
            auto &from = graph[a];
            if (std::ranges::find(from, b) == from.end()) from.push_back(b);
            auto &to = graph[b];
            if (std::ranges::find(to, a) == to.end()) to.push_back(a);
        };
        while (!queue.empty()) {
            check();
            const auto *t = queue.front();
            queue.pop_front();
            for (const auto *adjacent : snapshot_graph[t]) {
                check();
                if (CrossesDoorway(path_doorways, t, adjacent)) continue;
                add_edge(t, adjacent);
                if (reachable.insert(adjacent).second) queue.push_back(adjacent);
            }
        }

        std::map<std::pair<int, int>, Candidate> by_cell;
        std::map<std::pair<int, int>, std::vector<Candidate>> footing_options;
        const auto display_map = snapshot.map;
        const auto *creditable_mask = snapshot.mask;
        // Walk the game's pathing arrays in stable plane/trapezoid order. Iterating
        // the unordered reachable set made the retained footing for a cartography
        // cell change between rebuilds, causing waypoint 1 to jump around.
        for (size_t plane = 0; plane < maps->size(); plane++) {
            check();
            const auto &pathing_map = (*maps)[plane];
            for (uint32_t trap_index = 0; trap_index < pathing_map.trapezoid_count; trap_index++) {
                check();
                const auto *t = &pathing_map.trapezoids[trap_index];
                if (!reachable.contains(t)) continue;
                GW::Vec2f world_a, world_b;
                if (!game_to_world({std::min(t->XTL, t->XBL), t->YB, 0}, world_a, display_map) ||
                    !game_to_world({std::max(t->XTR, t->XBR), t->YT, 0}, world_b, display_map))
                    continue;
                const int cell_x0 = FogCellX(std::min(world_a.x, world_b.x));
                const int cell_x1 = FogCellX(std::max(world_a.x, world_b.x));
                const int cell_y0 = FogCellY(std::min(world_a.y, world_b.y));
                const int cell_y1 = FogCellY(std::max(world_a.y, world_b.y));
                for (int cy = cell_y0; cy <= cell_y1; cy++) {
                    check();
                    for (int cx = cell_x0; cx <= cell_x1; cx++) {
                        check();
                        GW::Vec2f game_a, game_b, footing;
                        if (!world_to_game({cx * kWorldUnitsPerCell, cy * kWorldUnitsPerCell}, game_a, display_map) ||
                            !world_to_game({(cx + 1) * kWorldUnitsPerCell, (cy + 1) * kWorldUnitsPerCell}, game_b, display_map))
                            continue;
                        const GW::Vec2f box_min{std::min(game_a.x, game_b.x), std::min(game_a.y, game_b.y)};
                        const GW::Vec2f box_max{std::max(game_a.x, game_b.x), std::max(game_a.y, game_b.y)};
                        float overlap_area = 0.f;
                        if (!TrapezoidCellOverlap(t, box_min, box_max, footing, overlap_area)) continue;
                        if (IsBeyondKnownPortal(footing, path_min, path_max, static_cast<uint32_t>(snapshot.map))) continue;
                        const auto key = std::pair{cx, cy};
                        const auto existing = by_cell.find(key);
                        Candidate candidate;
                        candidate.pos = {footing.x, footing.y, static_cast<uint32_t>(plane)};
                        candidate.trap = t;
                        candidate.footing_area = overlap_area;
                        candidate.cell_x = cx;
                        candidate.cell_y = cy;
                        for (int dy = -1; dy <= 1; dy++) {
                            check();
                            for (int dx = -1; dx <= 1; dx++) {
                                check();
                                const int fog_x = cx + dx, fog_y = cy + dy;
                                if (!IsExplored(bits, width, height, words, fog_x, fog_y) && MaskContains(creditable_mask, fog_x, fog_y))
                                    candidate.reveals.push_back(static_cast<uint32_t>(fog_y) * width + static_cast<uint32_t>(fog_x));
                            }
                        }
                        auto &options = footing_options[key];
                        if (std::ranges::none_of(options, [&](const Candidate &option) { return option.trap == t; })) {
                            options.push_back(candidate);
                            std::ranges::sort(options, {}, &Candidate::footing_area);
                            if (options.size() > 8) options.erase(options.begin());
                        }
                        if (existing == by_cell.end())
                            by_cell.emplace(key, std::move(candidate));
                        else if (existing->second.footing_area < overlap_area)
                            existing->second = std::move(candidate);
                    }
                }
            }
        }

        std::vector<Candidate> candidates;
        std::unordered_set<uint32_t> remaining;
        for (auto &[cell, candidate] : by_cell) {
            check();
            if (candidate.reveals.empty()) continue;
            for (const auto fog : candidate.reveals)
                remaining.insert(fog);
            candidates.push_back(std::move(candidate));
        }
        result.useful = static_cast<int>(candidates.size());
        result.unexplored = static_cast<int>(remaining.size());

        std::unordered_set<uint32_t> border_fog;
        const auto &map_bounds = snapshot.bounds;
        {
            const int min_x = FogCellX(map_bounds.Min.x);
            const int max_x = FogCellX(map_bounds.Max.x);
            const int min_y = FogCellY(map_bounds.Min.y);
            const int max_y = FogCellY(map_bounds.Max.y);
            for (const uint32_t fog : remaining) {
                check();
                const int x = static_cast<int>(fog % width);
                const int y = static_cast<int>(fog / width);
                if (x <= min_x + 1 || x >= max_x - 1 || y <= min_y + 1 || y >= max_y - 1) border_fog.insert(fog);
            }
        }
        result.border = static_cast<int>(border_fog.size());

        struct NavNode {
            const GW::PathingTrapezoid *a;
            const GW::PathingTrapezoid *b;
            completion::Portal edge;
            completion::Point point;
        };
        std::vector<NavNode> nodes;
        std::unordered_map<const GW::PathingTrapezoid *, std::vector<size_t>> incident;
        std::unordered_set<const GW::PathingTrapezoid *> visited;
        for (const auto &map : *maps) {
            check();
            for (const auto &trap : map.trapezoids) {
                check();
                if (!reachable.contains(&trap)) continue;
                visited.insert(&trap);
                for (const auto *next : graph[&trap]) {
                    check();
                    if (visited.contains(next)) continue;
                    const auto portal = SharedPortal(&trap, next);
                    if (!portal) continue;
                    const size_t id = nodes.size();
                    nodes.push_back(
                        {&trap, next, *portal, {(portal->first.x + portal->second.x) * .5f, (portal->first.y + portal->second.y) * .5f}});
                    incident[&trap].push_back(id);
                    incident[next].push_back(id);
                }
            }
        }
        const auto avoids_doors = [&](completion::Point a, completion::Point b) {
            return std::ranges::none_of(
                path_doorways, [&](const Doorway &door) { return DistanceToSegmentSq(door.pos, {a.x, a.y}, {b.x, b.y}) < door.radius_sq; });
        };
        struct Search {
            std::vector<float> distance;
            std::vector<size_t> parent;
        };
        using SearchKey = std::tuple<const GW::PathingTrapezoid *, float, float>;
        std::map<SearchKey, std::shared_ptr<Search>> searches;
        const auto search = [&](const GW::PathingTrapezoid *from, const GW::GamePos &point) {
            const SearchKey key{from, point.x, point.y};
            if (const auto found = searches.find(key); found != searches.end()) return found->second;
            auto row = std::make_shared<Search>();
            row->distance.assign(nodes.size(), FLT_MAX);
            row->parent.assign(nodes.size(), nodes.size());
            using Entry = std::pair<float, size_t>;
            std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
            for (const size_t id : incident[from]) {
                check();
                if (!avoids_doors({point.x, point.y}, nodes[id].point)) continue;
                row->distance[id] = completion::Distance({point.x, point.y}, nodes[id].point);
                open.emplace(row->distance[id], id);
            }
            while (!open.empty()) {
                check();
                const auto [travelled, id] = open.top();
                open.pop();
                if (travelled > row->distance[id]) continue;
                for (const auto *trap : {nodes[id].a, nodes[id].b}) {
                    check();
                    for (const size_t next : incident[trap]) {
                        check();
                        if (!avoids_doors(nodes[id].point, nodes[next].point)) continue;
                        const float value = travelled + completion::Distance(nodes[id].point, nodes[next].point);
                        if (value + .001f >= row->distance[next]) continue;
                        row->distance[next] = value;
                        row->parent[next] = id;
                        open.emplace(value, next);
                    }
                }
            }
            if (searches.size() >= 32) searches.clear();
            searches.emplace(key, row);
            return row;
        };
        const auto destination = [&](const Search &row, const GW::PathingTrapezoid *to, const GW::GamePos &point) {
            std::pair<float, size_t> best{FLT_MAX, nodes.size()};
            for (const size_t id : incident[to]) {
                check();
                if (!avoids_doors(nodes[id].point, {point.x, point.y})) continue;
                const float value = row.distance[id] + completion::Distance(nodes[id].point, {point.x, point.y});
                if (value < best.first) best = {value, id};
            }
            return best;
        };
        const auto connection_cost = [&](const Search &row, const GW::PathingTrapezoid *from, const GW::GamePos &a,
                                         const GW::PathingTrapezoid *to, const GW::GamePos &b) {
            if (from == to && avoids_doors({a.x, a.y}, {b.x, b.y})) return hypotf(b.x - a.x, b.y - a.y);
            return destination(row, to, b).first;
        };

        const auto start_row = search(start, player->pos);
        std::erase_if(candidates, [&](const Candidate& candidate) {
            const float cost = connection_cost(*start_row, start, player->pos, candidate.trap, candidate.pos);
            return !std::isfinite(cost) || cost == FLT_MAX;
        });
        remaining.clear();
        for (const auto& candidate : candidates) {
            check();
            remaining.insert(candidate.reveals.begin(), candidate.reveals.end());
        }
        std::erase_if(border_fog, [&](uint32_t fog) { return !remaining.contains(fog); });
        result.useful = static_cast<int>(candidates.size());
        result.unexplored = static_cast<int>(remaining.size());
        result.border = static_cast<int>(border_fog.size());

        // First choose a compact set of stand cells that covers all reachable fog.
        // Selection and visit order are deliberately separate: mixing distance into
        // set-cover caused the result.route to zig-zag between opposite sides of the map.
        std::vector<size_t> selected;
        while (!remaining.empty() && selected.size() < 1024) {
            check();
            size_t best = candidates.size();
            int best_value = -1;
            const bool border_pass = std::ranges::any_of(border_fog, [&](const uint32_t fog) { return remaining.contains(fog); });
            for (size_t i = 0; i < candidates.size(); i++) {
                check();
                int newly_revealed = 0;
                int border_revealed = 0;
                for (const auto fog : candidates[i].reveals) {
                    check();
                    if (!remaining.contains(fog)) continue;
                    newly_revealed++;
                    border_revealed += border_fog.contains(fog);
                }
                if (!newly_revealed) continue;
                if (border_pass && !border_revealed) continue;
                const int value = newly_revealed + border_revealed * 4;
                if (value > best_value) {
                    best_value = value;
                    best = i;
                }
            }
            if (best == candidates.size()) break;
            selected.push_back(best);
            for (const auto fog : candidates[best].reveals)
                remaining.erase(fog);
        }

        // Greedy set cover can leave an earlier stop redundant after later stops
        // overlap it. Remove any stop whose entire reveal set is still covered by
        // another selected stop, preserving at least one cover for every fog cell.
        std::unordered_map<uint32_t, uint32_t> cover_count;
        for (const size_t index : selected) {
            check();
            for (const uint32_t fog : candidates[index].reveals)
                cover_count[fog]++;
        }
        for (size_t i = selected.size(); i-- > 0;) {
            check();
            const auto &reveals = candidates[selected[i]].reveals;
            const bool redundant = std::ranges::all_of(reveals, [&](const uint32_t fog) {
                const auto found = cover_count.find(fog);
                return found != cover_count.end() && found->second > 1;
            });
            if (!redundant) continue;
            for (const uint32_t fog : reveals)
                cover_count[fog]--;
            selected.erase(selected.begin() + static_cast<ptrdiff_t>(i));
        }

        const GW::PathingTrapezoid *end_trap = nullptr;
        GW::GamePos end_point{};
        if (snapshot.end_portal >= 0 && static_cast<size_t>(snapshot.end_portal) < snapshot.endpoints.size()) {
            const auto endpoint = snapshot.endpoints[static_cast<size_t>(snapshot.end_portal)];
            float best_distance = FLT_MAX;
            for (const auto &map : *maps) {
                check();
                for (const auto &trap : map.trapezoids) {
                    check();
                    if (!reachable.contains(&trap)) continue;
                    const auto point = ClosestPoint(&trap, endpoint);
                    const GW::GamePos goal{point.x, point.y, static_cast<uint32_t>(planes.at(&trap))};
                    const float reachable_cost = connection_cost(*start_row, start, player->pos, &trap, goal);
                    if (!std::isfinite(reachable_cost) || reachable_cost == FLT_MAX) continue;
                    const float distance = hypotf(point.x - endpoint.x, point.y - endpoint.y);
                    if (distance >= best_distance) continue;
                    best_distance = distance;
                    end_trap = &trap;
                    end_point = {point.x, point.y, static_cast<uint32_t>(planes.at(&trap))};
                }
            }
        }
        const size_t target_count = selected.size();
        completion::Costs costs(target_count + 1, std::vector<float>(target_count + 1, 0.f));
        std::vector<float> finish(target_count, 0.f);
        const auto trap_at = [&](size_t i) { return i == target_count ? start : candidates[selected[i]].trap; };
        const auto point_at = [&](size_t i) { return i == target_count ? player->pos : candidates[selected[i]].pos; };
        const auto rebuild_costs = [&] {
            for (size_t i = 0; i <= target_count; ++i) {
                check();
                const auto row = search(trap_at(i), point_at(i));
                for (size_t j = i + 1; j <= target_count; ++j)
                    costs[i][j] = costs[j][i] = connection_cost(*row, trap_at(i), point_at(i), trap_at(j), point_at(j));
                if (end_trap && i < target_count) finish[i] = connection_cost(*row, trap_at(i), point_at(i), end_trap, end_point);
            }
        };
        rebuild_costs();
        auto order = completion::OptimizeOrder(costs, finish, two_opt_passes, restart_budget, static_cast<uint32_t>(map_id), check);
        const auto morph_options = candidates;
        for (int pass = 0; pass < relaxation_budget; ++pass) {
            check();
            bool improved = false;
            for (size_t step = 0; step < order.size(); ++step) {
                check();
                const size_t position = pass % 2 ? order.size() - 1 - step : step;
                auto &current = candidates[selected[order[position]]];
                for (const uint32_t fog : current.reveals)
                    --cover_count[fog];
                std::vector<uint32_t> required;
                for (const uint32_t fog : current.reveals)
                    if (!cover_count[fog]) required.push_back(fog);
                const auto *previous_trap = position ? trap_at(order[position - 1]) : start;
                const auto previous_point = position ? point_at(order[position - 1]) : player->pos;
                const auto *next_trap = position + 1 < order.size() ? trap_at(order[position + 1]) : end_trap;
                const auto next_point = position + 1 < order.size() ? point_at(order[position + 1]) : end_point;
                const auto previous_row = search(previous_trap, previous_point);
                const auto next_row = next_trap ? search(next_trap, next_point) : nullptr;
                const auto score = [&](const Candidate &option) {
                    return connection_cost(*previous_row, previous_trap, previous_point, option.trap, option.pos) +
                           (next_row ? connection_cost(*next_row, next_trap, next_point, option.trap, option.pos) : 0.f);
                };
                Candidate best = current;
                float best_cost = score(best);
                const auto consider = [&](const Candidate &option) {
                    if (!std::ranges::all_of(required,
                                             [&](uint32_t fog) { return std::ranges::find(option.reveals, fog) != option.reveals.end(); }))
                        return;
                    const float cost = score(option);
                    if (cost + .1f >= best_cost) return;
                    best_cost = cost;
                    best = option;
                    improved = true;
                };
                if (const auto found = footing_options.find({current.cell_x, current.cell_y}); found != footing_options.end())
                    for (const auto &option : found->second)
                        consider(option);
                for (const auto &option : morph_options)
                    consider(option);
                current = std::move(best);
                for (const uint32_t fog : current.reveals)
                    ++cover_count[fog];
            }
            if (!improved) break;
            rebuild_costs();
            order = completion::OptimizeOrder(costs, finish, two_opt_passes, 0, 0, check);
        }

        for (size_t position = order.size(); position-- > 0;) {
            check();
            const auto &reveals = candidates[selected[order[position]]].reveals;
            if (!std::ranges::all_of(reveals, [&](uint32_t fog) { return cover_count[fog] > 1; })) continue;
            for (const uint32_t fog : reveals)
                --cover_count[fog];
            order.erase(order.begin() + static_cast<ptrdiff_t>(position));
        }
        if (order.empty() && !end_trap) {
            log_build("nothing_reachable_to_discover");
            return;
        }
        result.route.push_back(player->pos);
        const auto append_leg = [&](const GW::PathingTrapezoid *from, const GW::PathingTrapezoid *to, const GW::GamePos &goal) {
            std::vector<size_t> path;
            if (from != to || !avoids_doors({result.route.back().x, result.route.back().y}, {goal.x, goal.y})) {
                const auto row = search(from, result.route.back());
                const auto [cost, last] = destination(*row, to, goal);
                if (last == nodes.size() || cost == FLT_MAX) return false;
                for (size_t id = last; id != nodes.size(); id = row->parent[id])
                    path.push_back(id);
                std::reverse(path.begin(), path.end());
            }
            std::vector<completion::Portal> portals{
                {{result.route.back().x, result.route.back().y}, {result.route.back().x, result.route.back().y}}};
            std::vector<GW::GamePos> raw{result.route.back()};
            raw.back().zplane = static_cast<uint32_t>(planes.at(from));
            for (size_t i = 0; i < path.size(); ++i) {
                check();
                const auto &node = nodes[path[i]];
                const GW::PathingTrapezoid *segment_trap = to;
                if (i + 1 < path.size()) {
                    const auto &next = nodes[path[i + 1]];
                    if (node.a == next.a || node.a == next.b)
                        segment_trap = node.a;
                    else if (node.b == next.a || node.b == next.b)
                        segment_trap = node.b;
                    else
                        return false;
                }
                portals.push_back(node.edge);
                raw.push_back({node.point.x, node.point.y, static_cast<uint32_t>(planes.at(segment_trap))});
            }
            portals.push_back({{goal.x, goal.y}, {goal.x, goal.y}});
            raw.push_back(goal);
            for (int pass = 0; pass < 24; ++pass) {
                check();
                float movement = 0.f;
                for (size_t step = 1; step + 1 < raw.size(); ++step) {
                    check();
                    const size_t i = pass % 2 ? raw.size() - 1 - step : step;
                    const auto [a, b] = portals[i];
                    const auto objective = [&](float t) {
                        const completion::Point point{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
                        return completion::Distance(point, {raw[i - 1].x, raw[i - 1].y}) +
                               completion::Distance(point, {raw[i + 1].x, raw[i + 1].y});
                    };
                    float low = 0.f, high = 1.f;
                    for (int iteration = 0; iteration < 24; ++iteration) {
                        check();
                        const float left = (2.f * low + high) / 3.f, right = (low + 2.f * high) / 3.f;
                        if (objective(left) <= objective(right))
                            high = right;
                        else
                            low = left;
                    }
                    const float t = (low + high) * .5f;
                    const float x = a.x + (b.x - a.x) * t, y = a.y + (b.y - a.y) * t;
                    if (!avoids_doors({raw[i - 1].x, raw[i - 1].y}, {x, y}) || !avoids_doors({x, y}, {raw[i + 1].x, raw[i + 1].y}))
                        continue;
                    movement += hypotf(x - raw[i].x, y - raw[i].y);
                    raw[i].x = x;
                    raw[i].y = y;
                }
                if (movement < .05f) break;
            }
            result.route.back().zplane = raw.front().zplane;
            size_t at = 0;
            while (at + 1 < raw.size()) {
                check();
                size_t next = at + 1;
                size_t furthest = at + 1;
                while (furthest + 1 < raw.size() && raw[furthest].zplane == raw[at].zplane)
                    ++furthest;
                for (size_t candidate = furthest; candidate > at + 1; --candidate) {
                    check();
                    if (avoids_doors({raw[at].x, raw[at].y}, {raw[candidate].x, raw[candidate].y}) &&
                        completion::CrossesPortalsInOrder({raw[at].x, raw[at].y}, {raw[candidate].x, raw[candidate].y}, portals, at + 1,
                                                          candidate)) {
                        next = candidate;
                        break;
                    }
                }
                result.route.push_back(raw[next]);
                at = next;
            }
            return true;
        };
        const auto *from = start;
        for (const size_t index : order) {
            check();
            const auto &target = candidates[selected[index]];
            if (!append_leg(from, target.trap, target.pos)) {
                result.route.clear();
                log_build("disconnected_target");
                return;
            }
            result.waypoints.push_back(target.pos);
            result.reveals.push_back(target.reveals);
            result.waypoint_indices.push_back(result.route.size() - 1);
            constexpr float cell_area = kWorldUnitsPerCell * kGwinchesPerWorldUnit * kWorldUnitsPerCell * kGwinchesPerWorldUnit;
            result.narrow.push_back(target.footing_area < cell_area * .14f);
            from = target.trap;
        }
        if (end_trap) {
            if (!append_leg(from, end_trap, end_point)) {
                result.route.clear();
                log_build("disconnected_portal");
                return;
            }
            result.end_point = end_point;
        }
        result.connected = result.route.size() > 1;
        for (size_t i = 1; i < result.route.size(); ++i)
            result.length += hypotf(result.route[i].x - result.route[i - 1].x, result.route[i].y - result.route[i - 1].y);
        log_build(remaining.empty() ? "ready" : "partial_coverage");
    }

    void PollBuild()
    {
        if (!build_future.valid() || build_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        auto result = build_future.get();
        if (!addon_enabled || terminating || build_cancel->load() || route_suspended || result.map != GW::Map::GetMapID() ||
            result.instance != GW::Map::GetInstanceType())
            return;
        build_status = result.status;
        build_milliseconds = result.milliseconds;
        if (!result.connected && result.status != "nothing_reachable_to_discover") return;
        const auto *player = GW::Agents::GetControlledCharacter();
        if (!player || player->pos.zplane != result.origin.zplane ||
            hypotf(player->pos.x - result.origin.x, player->pos.y - result.origin.y) > 1500.f) {
            recompute_requested.store(true);
            build_status = "Player moved; refreshing route";
            return;
        }
        route = std::move(result.route);
        route_waypoints = std::move(result.waypoints);
        route_waypoint_reveals = std::move(result.reveals);
        route_waypoint_narrow = std::move(result.narrow);
        route_waypoint_indices = std::move(result.waypoint_indices);
        route_end_portal_point = result.end_point;
        fog_grid_width = result.width;
        useful_candidates = result.useful;
        unexplored_reachable = result.unexplored;
        unexplored_border = result.border;
        route_connected = result.connected;
        route_length = result.length;
        route_map = result.map;
        route_instance = result.instance;
        route_progress = ground_progress = 0;
        route_waypoint_altitudes.clear();
        ground_route.clear();
        ground_route_indices.assign(1, 0);
        ground_border_vertices.clear();
        ground_core_vertices.clear();
        ground_build_leg = 1;
        ground_build_sample = 0;
        ground_building = route.size() > 1;
        ground_vertices_dirty = true;
        LogBuild(result.status.c_str());
    }

    void AdvanceBuild()
    {
        PollBuild();
        if (!capture || !ContinueCapture()) return;
        auto snapshot = std::move(capture);
        cartography_snapshot = snapshot->bits;
        build_cancel = std::make_shared<std::atomic_bool>(false);
        build_status = "Optimizing route in background";
        build_future = std::async(std::launch::async, [snapshot = std::move(snapshot), cancelled = build_cancel]() mutable {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            BuildResult result;
            const auto begin = std::chrono::steady_clock::now();
            try {
                BuildRoute(*snapshot, result, *cancelled);
            }
            catch (const BuildCancelled &) {
                result.status = "Cancelled";
            }
            catch (const std::exception &error) {
                result.status = std::string("Build failed: ") + error.what();
            }
            snapshot.reset();
            result.milliseconds = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - begin).count();
            return result;
        });
    }

    void SetEnabled(const bool enabled)
    {
        addon_enabled = enabled;
        recompute_requested.store(false);
        SuspendRoute(GetTickCount());
        build_status = enabled ? "Waiting for map" : "Disabled";
    }

    bool ProjectWorldMap(const GW::Vec2f &wm, ImVec2 &out, ImRect &clip)
    {
        auto *context = GW::Map::GetWorldMapContext();
        if (!context || !GW::UI::GetIsWorldMapShowing()) return false;
        auto *frame = GW::UI::GetFrameById(context->frame_id);
        auto *root = GW::UI::GetRootFrame();
        if (!frame || !root) return false;
        clip = {frame->position.GetContentTopLeft(root), frame->position.GetContentBottomRight(root)};
        const GW::Vec2f span = context->bottom_right - context->top_left;
        if (span.x == 0.f || span.y == 0.f) return false;
        out = {clip.Min.x + (wm.x - context->top_left.x) * clip.GetWidth() / span.x,
               clip.Min.y + (wm.y - context->top_left.y) * clip.GetHeight() / span.y};
        return true;
    }

    bool ProjectMissionMap(const GW::Vec2f &wm, ImVec2 &out, ImRect &clip)
    {
        auto *context = GW::Map::GetMissionMapContext();
        auto *root = GW::UI::GetRootFrame();
        if (!context || !context->h003c || !root) return false;
        auto *frame = GW::UI::GetFrameById(context->frame_id);
        if (!frame || !frame->IsVisible()) return false;
        clip = {frame->position.GetContentTopLeft(root), frame->position.GetContentBottomRight(root)};
        const GW::Vec2f scale = frame->position.GetViewportScale(root);
        const GW::Vec2f center = {(clip.Min.x + clip.Max.x) * 0.5f, (clip.Min.y + clip.Max.y) * 0.5f};
        const GW::Vec2f offset = wm - context->h003c->mission_map_pan_offset;
        const float zoom = GW::GetGameplayContext() ? GW::GetGameplayContext()->mission_map_zoom : 1.f;
        out = {center.x + offset.x * scale.x * zoom, center.y + offset.y * scale.y * zoom};
        return true;
    }

    bool ProjectGameWorld(const GW::Vec3f &point, ImVec2 &screen)
    {
        const auto *camera = GW::CameraMgr::GetCamera();
        const uint32_t width = GW::Render::GetViewportWidth();
        const uint32_t height = GW::Render::GetViewportHeight();
        if (!camera || !width || !height) return false;

        const auto normalize = [](const GW::Vec3f &value) {
            const float length = sqrtf(value.x * value.x + value.y * value.y + value.z * value.z);
            return length > 0.001f ? GW::Vec3f{value.x / length, value.y / length, value.z / length} : GW::Vec3f{};
        };
        const auto cross = [](const GW::Vec3f &a, const GW::Vec3f &b) {
            return GW::Vec3f{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        };
        const auto dot = [](const GW::Vec3f &a, const GW::Vec3f &b) { return a.x * b.x + a.y * b.y + a.z * b.z; };

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

    size_t GroundDrawEnd(const size_t first)
    {
        const size_t limit = active_leg_only ? std::min(CurrentGroundEnd(), ground_route.size() - 1) : ground_route.size() - 1;
        float distance = 0.f;
        size_t last = first;
        while (last < limit && distance < ground_horizon) {
            distance += hypotf(ground_route[last + 1].x - ground_route[last].x, ground_route[last + 1].y - ground_route[last].y);
            ++last;
        }
        return last;
    }

    ImU32 GroundColour(const size_t index, const bool border, const size_t active_end)
    {
        const bool active = index <= active_end;
        auto colour = ImGui::ColorConvertU32ToFloat4(border ? outline_colour : active ? active_colour : route_colour);
        if (!active) colour.w *= future_opacity;
        return ImGui::ColorConvertFloat4ToU32(colour);
    }

    void DrawGroundRoute(const float arrow_scale)
    {
        ground_segments_drawn = 0;
        if (!route_connected || ground_building || ground_route.size() < 2 || GW::UI::GetIsWorldMapShowing()) return;

        auto *draw = ImGui::GetBackgroundDrawList();
        const auto *player = GW::Agents::GetControlledCharacter();
        if (!player) return;
        const size_t first = ClosestGroundPoint({player->pos.x, player->pos.y, player->z});
        ImVec2 previous_screen;
        bool previous_visible = false;
        const size_t last = GroundDrawEnd(first), active_end = CurrentGroundEnd();
        for (size_t i = first; i <= last; i++) {
            const auto shadow = GroundColour(i, true, active_end);
            const auto orange = GroundColour(i, false, active_end);
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
                        draw->AddLine(left, tip, shadow, 2.5f + 2.f * outline_width);
                        draw->AddLine(tip, right, shadow, 2.5f + 2.f * outline_width);
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

    bool DrawOccludedGroundRoute(IDirect3DDevice9 *device, const float thickness, const float arrow_scale)
    {
        ground_segments_drawn = 0;
        if (!device || route_suspended || route_map != GW::Map::GetMapID() || route_instance != GW::Map::GetInstanceType() ||
            !route_connected || ground_building || ground_route.size() < 2 || GW::UI::GetIsWorldMapShowing())
            return false;
        const auto *player = GW::Agents::GetControlledCharacter();
        const auto *camera = GW::CameraMgr::GetCamera();
        const uint32_t width = GW::Render::GetViewportWidth();
        const uint32_t height = GW::Render::GetViewportHeight();
        if (!player || !camera || !width || !height) return false;

        using Vertex = GroundVertex;
        const size_t first = ClosestGroundPoint({player->pos.x, player->pos.y, player->z});
        if (first + 1 >= ground_route.size()) return false;
        const size_t last = GroundDrawEnd(first);
        const size_t active_end = CurrentGroundEnd();
        const auto arrows_in_segment = [&](const float length) {
            return std::max(1u, static_cast<unsigned>(ceilf(length / (180.f * arrow_scale))));
        };
        const auto build_ribbon = [&](std::vector<Vertex> &vertices, const float half_width, const bool border) {
            vertices.clear();
            vertices.reserve((last - first) * 12);
            for (int layer = 0; layer < 2; ++layer) {
                for (size_t i = first + 1; i <= last; i++) {
                    if ((i <= active_end) != (layer == 1)) continue;
                    const auto rgba = ImGui::ColorConvertU32ToFloat4(GroundColour(i, border, active_end));
                    const auto colour = D3DCOLOR_ARGB(static_cast<uint8_t>(rgba.w * 255.f), static_cast<uint8_t>(rgba.x * 255.f),
                                                      static_cast<uint8_t>(rgba.y * 255.f), static_cast<uint8_t>(rgba.z * 255.f));
                    const auto &a = ground_route[i - 1];
                    const auto &b = ground_route[i];
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
                            vertices.insert(vertices.end(), {{end_x + ox, end_y + oy, base_z, colour},
                                                             {end_x - ox, end_y - oy, base_z, colour},
                                                             {tip_x + ox, tip_y + oy, tip_z, colour},
                                                             {tip_x + ox, tip_y + oy, tip_z, colour},
                                                             {end_x - ox, end_y - oy, base_z, colour},
                                                             {tip_x - ox, tip_y - oy, tip_z, colour}});
                        };
                        append_arm(base_x + px * wing, base_y + py * wing);
                        append_arm(base_x - px * wing, base_y - py * wing);
                    }
                }
            }
        };
        const float core_width = std::clamp(thickness * 3.f, 6.f, 30.f);
        if (ground_vertices_dirty || cached_ground_thickness != thickness || cached_ground_arrow_size != arrow_scale ||
            cached_ground_colour != route_colour || cached_ground_first != first || cached_ground_last != last ||
            cached_active_end != active_end) {
            build_ribbon(ground_border_vertices, core_width + outline_width * 3.f, true);
            build_ribbon(ground_core_vertices, core_width, false);
            cached_ground_first = first;
            cached_ground_last = last;
            cached_active_end = active_end;
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
        for (size_t i = 0; i < route_waypoints.size() && i < route_waypoint_narrow.size() && i < route_waypoint_altitudes.size(); i++) {
            if ((active_leg_only && route_waypoint_indices[i] != CurrentRouteEnd()) || !route_waypoint_narrow[i] ||
                i >= route_waypoint_reveals.size() || std::ranges::all_of(route_waypoint_reveals[i], FogIndexExplored))
                continue;
            const auto &ground = route_waypoints[i];
            const float altitude = route_waypoint_altitudes[i];
            const GW::Vec3f center{ground.x, ground.y, altitude - 145.f};
            float vx = center.x - camera->position.x, vy = center.y - camera->position.y;
            const float vlen = hypotf(vx, vy);
            if (vlen < 1.f) continue;
            const float rx = -vy / vlen, ry = vx / vlen;
            constexpr float radius = 92.f;
            const auto vertex_at = [&](const float horizontal, const float vertical, const D3DCOLOR colour) {
                return Vertex{center.x + rx * horizontal, center.y + ry * horizontal, center.z - vertical, colour};
            };
            const Vertex middle{center.x, center.y, center.z, D3DCOLOR_ARGB(245, 205, 32, 32)};
            for (int edge = 0; edge < 8; edge++) {
                const float a0 = DirectX::XM_2PI * static_cast<float>(edge) / 8.f + DirectX::XM_PI / 8.f;
                const float a1 = DirectX::XM_2PI * static_cast<float>(edge + 1) / 8.f + DirectX::XM_PI / 8.f;
                stop_vertices.insert(stop_vertices.end(), {middle, vertex_at(cosf(a0) * radius, sinf(a0) * radius, middle.colour),
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
        DirectX::XMStoreFloat4x4(
            &view, DirectX::XMMatrixLookAtLH(DirectX::XMLoadFloat3(&eye), DirectX::XMLoadFloat3(&look), DirectX::XMLoadFloat3(&up)));
        DirectX::XMStoreFloat4x4(&projection, DirectX::XMMatrixPerspectiveFovLH(GW::Render::GetFieldOfView(),
                                                                                static_cast<float>(width) / static_cast<float>(height),
                                                                                46.875f, 48000.f));

        device->SetVertexShader(nullptr);
        device->SetPixelShader(nullptr);
        device->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        device->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX *>(&world));
        device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX *>(&view));
        device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX *>(&projection));
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
        const size_t vertex_offset = 0;
        const size_t core_count = ground_core_vertices.size() - vertex_offset;
        const size_t border_count = ground_border_vertices.size() - std::min(vertex_offset, ground_border_vertices.size());
        if (border_count >= 3)
            drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(border_count / 3),
                                            ground_border_vertices.data() + vertex_offset, sizeof(Vertex));
        if (drawn == D3D_OK)
            drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(core_count / 3),
                                            ground_core_vertices.data() + vertex_offset, sizeof(Vertex));
        if (drawn == D3D_OK && !stop_vertices.empty())
            drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(stop_vertices.size() / 3), stop_vertices.data(),
                                            sizeof(Vertex));
        if (drawn == D3D_OK && !stop_symbol_vertices.empty())
            drawn = device->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(stop_symbol_vertices.size() / 3),
                                            stop_symbol_vertices.data(), sizeof(Vertex));
        ground_segments_drawn = drawn == D3D_OK ? core_count / 12 : 0;

        route_state_block->Apply();
        return drawn == D3D_OK;
    }

} // namespace

DLLAPI ToolboxPlugin *ToolboxPluginInstance()
{
    static WorldCompletionPlugin instance;
    return &instance;
}

void WorldCompletionPlugin::Initialize(ImGuiContext *ctx, const ImGuiAllocFns allocator_fns, const HMODULE toolbox_dll)
{
    ToolboxUIPlugin::Initialize(ctx, allocator_fns, toolbox_dll);
    const auto on_map_lifecycle = [](GW::HookStatus *, GW::UI::UIMessage, void *, void *) {
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
    terminating = true;
    CancelBuild();
    route_suspended = true;
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

bool WorldCompletionPlugin::CanTerminate()
{
    if (build_future.valid()) {
        if (build_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
        build_future.get();
    }
    return ToolboxUIPlugin::CanTerminate();
}

void WorldCompletionPlugin::LoadSettings(const wchar_t *folder)
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
    LoadSetting("addon_enabled", addon_enabled);
    LoadSetting("show_debug_status", show_debug_status_);
    LoadSetting("show_route", show_route_);
    LoadSetting("show_ground_route", show_ground_route_);
    LoadSetting("occlude_ground_route", occlude_ground_route_);
    LoadSetting("show_numbers", show_numbers_);
    LoadSetting("show_cursor_coordinates", show_cursor_coordinates_);
    LoadSetting("route_thickness", route_thickness_);
    LoadSetting("arrow_size_percent", arrow_size_percent);
    LoadSetting("route_colour", route_colour);
    LoadSetting("active_colour", active_colour);
    LoadSetting("outline_colour", outline_colour);
    LoadSetting("future_opacity", future_opacity);
    LoadSetting("outline_width", outline_width);
    LoadSetting("waypoint_size", waypoint_size);
    LoadSetting("active_leg_only", active_leg_only);
    LoadSetting("map_arrows", map_arrows);
    LoadSetting("ground_horizon", ground_horizon);
    LoadSetting("optimization_quality", optimization_quality);
    LoadSetting("relaxation_passes", relaxation_passes);
    LoadSetting("randomized_restarts", randomized_restarts);
    LoadSetting("end_portal_index", end_portal_index);
    LoadSetting("end_portal_map", end_portal_map);
    route_thickness_ = std::clamp(route_thickness_, 1.f, 10.f);
    future_opacity = std::clamp(future_opacity, 0.f, 1.f);
    outline_width = std::clamp(outline_width, 0.f, 4.f);
    ground_horizon = std::clamp(ground_horizon, 1000.f, 20000.f);
    waypoint_size = std::clamp(waypoint_size, 2.f, 12.f);
    arrow_size_percent = std::clamp(arrow_size_percent, 10, 250);
    optimization_quality = std::clamp(optimization_quality, 0, 2);
    relaxation_passes = std::clamp(relaxation_passes, 0, 8);
    randomized_restarts = std::clamp(randomized_restarts, 0, 12);
}

void WorldCompletionPlugin::SaveSettings(const wchar_t *folder)
{
    if (folder) settings_folder_ = folder;
    SaveSetting("addon_enabled", addon_enabled);
    SaveSetting("show_debug_status", show_debug_status_);
    SaveSetting("show_route", show_route_);
    SaveSetting("show_ground_route", show_ground_route_);
    SaveSetting("occlude_ground_route", occlude_ground_route_);
    SaveSetting("show_numbers", show_numbers_);
    SaveSetting("show_cursor_coordinates", show_cursor_coordinates_);
    SaveSetting("route_thickness", route_thickness_);
    SaveSetting("arrow_size_percent", arrow_size_percent);
    SaveSetting("route_colour", route_colour);
    SaveSetting("active_colour", active_colour);
    SaveSetting("outline_colour", outline_colour);
    SaveSetting("future_opacity", future_opacity);
    SaveSetting("outline_width", outline_width);
    SaveSetting("waypoint_size", waypoint_size);
    SaveSetting("active_leg_only", active_leg_only);
    SaveSetting("map_arrows", map_arrows);
    SaveSetting("ground_horizon", ground_horizon);
    SaveSetting("optimization_quality", optimization_quality);
    SaveSetting("relaxation_passes", relaxation_passes);
    SaveSetting("randomized_restarts", randomized_restarts);
    SaveSetting("end_portal_index", end_portal_index);
    SaveSetting("end_portal_map", end_portal_map);
    ToolboxUIPlugin::SaveSettings(folder);
}

bool WorldCompletionPlugin::IsLoadedContext() const
{
    return GW::Map::GetIsMapLoaded() && GW::Map::GetInstanceType() != GW::Constants::InstanceType::Loading;
}

bool WorldCompletionPlugin::IsPogahnMissionAdaptationActive() const
{
    return IsLoadedContext() && GW::Map::GetInstanceType() == GW::Constants::InstanceType::Explorable &&
           GW::Map::GetMapID() == GW::Constants::MapID::Pogahn_Passage;
}

void WorldCompletionPlugin::Update(const float)
{
    const uint32_t now = GetTickCount();
    if (transition_event.exchange(false, std::memory_order_acq_rel)) SuspendRoute(now);
    if (!addon_enabled || terminating) {
        PollBuild();
        return;
    }
    current_instance_ = GW::Map::GetInstanceType();
    current_map_ = GW::Map::GetMapID();
    if (!GW::Map::GetIsMapLoaded() || current_instance_ != GW::Constants::InstanceType::Explorable) {
        if (!route_suspended || BuildBusy()) SuspendRoute(now);
        PollBuild();
        return;
    }
    if (!route_suspended && (current_map_ != route_map || current_instance_ != route_instance)) SuspendRoute(now);
    PollBuild();
    if (route_suspended) {
        if (pending_map != current_map_ || pending_instance != current_instance_) {
            pending_map = current_map_;
            pending_instance = current_instance_;
            resume_after = now + 1500;
            return;
        }
        if (static_cast<int32_t>(now - resume_after) < 0 || BuildBusy()) return;
        if (!BeginCapture()) {
            resume_after = now + 500;
            return;
        }
        route_map = current_map_;
        route_instance = current_instance_;
        route_suspended = false;
        last_rebuild = now;
        return;
    }
    AdvanceBuild();
    UpdateGroundRoute();
    if (!ground_building && route_waypoint_altitudes.size() < route_waypoints.size()) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
        do {
            auto point = route_waypoints[route_waypoint_altitudes.size()];
            route_waypoint_altitudes.push_back(GW::Map::QueryAltitude(&point, 64.f));
        } while (route_waypoint_altitudes.size() < route_waypoints.size() && std::chrono::steady_clock::now() < deadline);
    }
    if (!BuildBusy() && now - last_fog_check >= 500) {
        last_fog_check = now;
        const auto *world = GW::GetWorldContext();
        if (world && world->cartographed_areas.valid()) {
            const auto *current = reinterpret_cast<const uint32_t *>(world->cartographed_areas.m_buffer);
            const size_t words = world->cartographed_areas.size();
            if (cartography_snapshot.size() == words) {
                for (size_t word = 0; word < words; ++word) {
                    uint32_t newly_explored = current[word] & ~cartography_snapshot[word];
                    while (newly_explored) {
                        const uint32_t fog = static_cast<uint32_t>(word * 32 + std::countr_zero(newly_explored));
                        const bool planned = std::ranges::any_of(
                            route_waypoint_reveals, [&](const auto &reveals) { return std::ranges::find(reveals, fog) != reveals.end(); });
                        if (!planned) recompute_requested.store(true);
                        newly_explored &= newly_explored - 1;
                    }
                }
            }
            cartography_snapshot.assign(current, current + words);
        }
    }
    if (const auto *player = GW::Agents::GetControlledCharacter(); player && route.size() > 1) {
        route_progress = ClosestRoutePointThrough(player->pos, CurrentRouteEnd());
        if (!ground_building && ground_route.size() > 1) ground_progress = ClosestGroundPoint({player->pos.x, player->pos.y, player->z});
    }
    if (recompute_requested.load() && BuildBusy()) CancelBuild();
    const bool retry = route.empty() && build_status != "nothing_reachable_to_discover" && now - last_rebuild >= 3000;
    if (!BuildBusy() && now - last_rebuild >= 500 && (recompute_requested.load() || retry)) {
        recompute_requested.store(false);
        last_rebuild = now;
        BeginCapture();
    }
}

void WorldCompletionPlugin::DrawSettings()
{
    bool enabled = addon_enabled;
    if (ImGui::Checkbox("Enable World Completion", &enabled)) SetEnabled(enabled);
    ImGui::Text("%s", build_status.c_str());
    ImGui::TextWrapped("The bright route is your current leg. Faded lines are later visits, including shared roads.");
    ImGui::Separator();
    ImGui::Checkbox("Show debug status", &show_debug_status_);
    ImGui::Checkbox("Show completion route", &show_route_);
    ImGui::Checkbox("Show route on the ground", &show_ground_route_);
    ImGui::Checkbox("Occlude ground route", &occlude_ground_route_);
    ImGui::Checkbox("Show waypoint numbers", &show_numbers_);
    ImGui::Checkbox("Show map cursor coordinates", &show_cursor_coordinates_);
    ImGui::SliderFloat("Route thickness", &route_thickness_, 1.f, 10.f, "%.1f px");
    if (ImGui::Checkbox("Show only the current leg", &active_leg_only)) ground_vertices_dirty = true;
    ImGui::Checkbox("Direction arrows on maps", &map_arrows);
    if (ImGui::SliderFloat("Ground look-ahead", &ground_horizon, 1000.f, 20000.f, "%.0f game units")) ground_vertices_dirty = true;
    if (ImGui::SliderFloat("Later route opacity", &future_opacity, 0.f, 1.f, "%.2f")) ground_vertices_dirty = true;
    if (ImGui::SliderFloat("Outline width", &outline_width, 0.f, 4.f, "%.1f px")) ground_vertices_dirty = true;
    ImGui::SliderFloat("Waypoint size", &waypoint_size, 2.f, 12.f, "%.1f px");
    for (const auto &entry : {std::pair{"Current leg color", &active_colour}, std::pair{"Outline color", &outline_colour}}) {
        auto rgba = ImGui::ColorConvertU32ToFloat4(*entry.second);
        if (ImGui::ColorEdit4(entry.first, &rgba.x, ImGuiColorEditFlags_AlphaBar)) {
            *entry.second = ImGui::ColorConvertFloat4ToU32(rgba);
            ground_vertices_dirty = true;
        }
    }
    ImGui::SliderInt("Arrow size", &arrow_size_percent, 10, 250, "%d%%");
    ImVec4 colour = ImGui::ColorConvertU32ToFloat4(route_colour);
    if (ImGui::ColorEdit4("Route color", &colour.x, ImGuiColorEditFlags_AlphaBar)) {
        route_colour = ImGui::ColorConvertFloat4ToU32(colour);
        ground_vertices_dirty = true;
    }
    const char *quality_names[] = {"Fast", "Balanced", "Thorough"};
    bool optimization_changed = ImGui::Combo("Optimization quality", &optimization_quality, quality_names, IM_ARRAYSIZE(quality_names));
    optimization_changed |= ImGui::SliderInt("Relaxation passes", &relaxation_passes, 0, 8);
    optimization_changed |= ImGui::SliderInt("Randomized restarts", &randomized_restarts, 0, 12);
    if (optimization_changed) recompute_requested.store(true, std::memory_order_release);
    ImGui::TextUnformatted("Click a portal marker on the mission map to choose the route endpoint.");
}

void WorldCompletionPlugin::Draw(IDirect3DDevice9 *device)
{
    if (terminating || !IsLoadedContext() || GW::Map::GetInstanceType() != GW::Constants::InstanceType::Explorable) return;

    const float arrow_scale = static_cast<float>(arrow_size_percent) / 100.f;
    ImRect mission_clip;
    ImVec2 mission_point;
    if (ProjectMissionMap({}, mission_point, mission_clip)) {
        constexpr float button_size = 30.f;
        constexpr float button_gap = 4.f;
        ImGui::SetNextWindowPos({mission_clip.Max.x - button_size * 2.f - button_gap - 7.f, mission_clip.Max.y - button_size - 7.f});
        ImGui::SetNextWindowBgAlpha(0.f);
        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
        if (ImGui::Begin("##world_completion_mission_toggle", nullptr, flags)) {
            if (ImGui::InvisibleButton("##toggle", {button_size, button_size})) SetEnabled(!addon_enabled);
            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            auto *button_draw = ImGui::GetWindowDrawList();
            button_draw->AddRectFilled(min, max, hovered ? IM_COL32(239, 246, 248, 255) : IM_COL32(213, 226, 230, 245), 5.f);
            button_draw->AddRect(min, max, IM_COL32(20, 42, 50, 255), 5.f, 0, 2.f);
            const ImU32 icon_colour = addon_enabled ? route_colour : IM_COL32(75, 91, 98, 255);
            const ImVec2 p0{min.x + 6.f, max.y - 7.f};
            const ImVec2 p1{min.x + 11.f, min.y + 10.f};
            const ImVec2 p2{min.x + 17.f, min.y + 17.f};
            const ImVec2 p3{max.x - 6.f, min.y + 7.f};
            button_draw->AddLine(p0, p1, icon_colour, 2.5f);
            button_draw->AddLine(p1, p2, icon_colour, 2.5f);
            button_draw->AddLine(p2, p3, icon_colour, 2.5f);
            button_draw->AddCircleFilled(p0, 2.2f, icon_colour);
            button_draw->AddCircleFilled(p3, 2.2f, icon_colour);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(addon_enabled ? "Disable World Completion (stops planning and all overlays)" : "Enable World Completion");
            ImGui::SameLine(0.f, button_gap);
            ImGui::BeginDisabled(!addon_enabled);
            if (ImGui::InvisibleButton("##recompute", {button_size, button_size}))
                recompute_requested.store(true, std::memory_order_release);
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
            if (refresh_hovered) ImGui::SetTooltip("Recompute route\n%s", build_status.c_str());
            ImGui::EndDisabled();
        }
        ImGui::End();
        ImGui::PopStyleVar();


    }


    if (addon_enabled && !GW::UI::GetIsWorldMapShowing()) {
        const auto endpoints = CurrentPortalEndpoints();
        const auto map_id = static_cast<uint32_t>(GW::Map::GetMapID());
        for (size_t i = 0; i < endpoints.size(); ++i) {
            GW::Vec2f world;
            ImVec2 screen;
            ImRect clip;
            if (!GameToWorld({endpoints[i].x, endpoints[i].y, 0}, world, CartographyMapID()) ||
                !ProjectMissionMap(world, screen, clip) || !clip.Contains(screen)) continue;
            constexpr float radius = 12.f;
            const bool selected = end_portal_map == map_id && end_portal_index == static_cast<int>(i);
            ImGui::SetNextWindowPos({screen.x - radius, screen.y - radius});
            ImGui::SetNextWindowSize({radius * 2.f, radius * 2.f});
            ImGui::SetNextWindowBgAlpha(0.f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.f, 0.f});
            const auto id = "##completion_portal_" + std::to_string(i);
            constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing |
                                               ImGuiWindowFlags_NoNav;
            if (ImGui::Begin(id.c_str(), nullptr, flags)) {
                if (ImGui::InvisibleButton("##choose", {radius * 2.f, radius * 2.f})) {
                    end_portal_index = selected ? -1 : static_cast<int>(i);
                    end_portal_map = selected ? 0 : map_id;
                    recompute_requested.store(true, std::memory_order_release);
                }
                const bool hovered = ImGui::IsItemHovered();
                auto *draw = ImGui::GetForegroundDrawList();
                draw->PushClipRect(clip.Min, clip.Max, true);
                const ImU32 colour = selected ? active_colour : IM_COL32(90, 210, 255, 255);
                draw->AddCircleFilled(screen, radius, outline_colour);
                draw->AddCircle(screen, radius - 2.f, colour, 0, hovered || selected ? 3.f : 2.f);
                const auto label = std::to_string(i + 1);
                const auto size = ImGui::CalcTextSize(label.c_str());
                draw->AddText({screen.x - size.x * .5f, screen.y - size.y * .5f}, colour, label.c_str());
                if (selected) draw->AddCircle(screen, radius + 3.f, colour, 0, 2.f);
                draw->PopClipRect();
                if (hovered) ImGui::SetTooltip(selected ? "Portal %u: route endpoint\nClick to clear" :
                                                       "Portal %u: click to finish here\nReoptimizes the entire route",
                                               static_cast<unsigned>(i + 1));
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }
    }

    if (!addon_enabled || route_suspended || route_map != GW::Map::GetMapID() || route_instance != GW::Map::GetInstanceType()) return;

    if (show_ground_route_ && current_instance_ == GW::Constants::InstanceType::Explorable) {
        if (!occlude_ground_route_ || !DrawOccludedGroundRoute(device, route_thickness_, arrow_scale)) DrawGroundRoute(arrow_scale);
    }

    if (show_route_ && route_connected && route.size() > 1) {
        auto *dl = ImGui::GetBackgroundDrawList();
        ImRect clip;
        GW::Vec2f first_wm;
        ImVec2 previous;
        const auto display_map = CartographyMapID();
        if (const auto *player = GW::Agents::GetControlledCharacter(); player && GameToWorld(player->pos, first_wm, display_map)) {
            const size_t first_route_point = route_progress;
            bool world_surface = ProjectWorldMap(first_wm, previous, clip);
            if (!world_surface) world_surface = ProjectMissionMap(first_wm, previous, clip);
            if (world_surface) {
                dl->PushClipRect(clip.Min, clip.Max, true);
                const size_t active_end = CurrentRouteEnd();
                auto faded = ImGui::ColorConvertU32ToFloat4(route_colour);
                faded.w *= future_opacity;
                const ImU32 future_colour = ImGui::ColorConvertFloat4ToU32(faded);
                const auto project = [&](size_t i, ImVec2 &point) {
                    GW::Vec2f wm;
                    if (!GameToWorld(route[i], wm, display_map)) return false;
                    return GW::UI::GetIsWorldMapShowing() ? ProjectWorldMap(wm, point, clip) : ProjectMissionMap(wm, point, clip);
                };
                for (int layer = 0; layer < 2; ++layer) {
                    const bool active = layer == 1;
                    if (!active && (active_leg_only || future_opacity == 0.f)) continue;
                    const ImU32 colour = active ? active_colour : future_colour;
                    auto border = ImGui::ColorConvertU32ToFloat4(outline_colour);
                    border.w *= active ? 1.f : future_opacity;
                    const auto shadow = ImGui::ColorConvertFloat4ToU32(border);
                    for (size_t i = first_route_point + 1; i < route.size(); ++i) {
                        if ((i <= active_end) != active) continue;
                        ImVec2 a, b;
                        if (!project(i - 1, a) || !project(i, b)) continue;
                        if (std::max(a.x, b.x) < clip.Min.x || std::min(a.x, b.x) > clip.Max.x || std::max(a.y, b.y) < clip.Min.y ||
                            std::min(a.y, b.y) > clip.Max.y)
                            continue;
                        if (outline_width > 0.f) dl->AddLine(a, b, shadow, route_thickness_ + 2.f * outline_width);
                        dl->AddLine(a, b, colour, route_thickness_);
                        if (!map_arrows) continue;
                        const float dx = b.x - a.x, dy = b.y - a.y, length = hypotf(dx, dy);
                        const float size = std::max(2.f, 7.f * arrow_scale);
                        if (length < size * 2.f) continue;
                        const float ux = dx / length, uy = dy / length;
                        const float step = std::max({size * 3.f, 18.f, length / 256.f});
                        for (float distance = step * .5f; distance < length; distance += step) {
                            const ImVec2 tip{a.x + ux * distance, a.y + uy * distance};
                            if (!clip.Contains(tip)) continue;
                            const ImVec2 left{tip.x - ux * size - uy * size * .6f, tip.y - uy * size + ux * size * .6f};
                            const ImVec2 right{tip.x - ux * size + uy * size * .6f, tip.y - uy * size - ux * size * .6f};
                            if (outline_width > 0.f) {
                                dl->AddLine(left, tip, shadow, route_thickness_ + 2.f * outline_width);
                                dl->AddLine(tip, right, shadow, route_thickness_ + 2.f * outline_width);
                            }
                            dl->AddLine(left, tip, colour, route_thickness_);
                            dl->AddLine(tip, right, colour, route_thickness_);
                        }
                    }
                }
                const ImU32 colour = route_colour;
                for (size_t i = 0; i < route_waypoints.size(); i++) {
                    if (i < route_waypoint_reveals.size() && std::ranges::all_of(route_waypoint_reveals[i], FogIndexExplored)) continue;
                    GW::Vec2f wm;
                    ImVec2 screen;
                    if (!GameToWorld(route_waypoints[i], wm, display_map)) continue;
                    const bool projected =
                        GW::UI::GetIsWorldMapShowing() ? ProjectWorldMap(wm, screen, clip) : ProjectMissionMap(wm, screen, clip);
                    if (!projected) continue;
                    const bool active_stop = route_waypoint_indices[i] == active_end;
                    if (active_leg_only && !active_stop) continue;
                    const ImU32 stop_colour = active_stop ? active_colour : future_colour;
                    bool narrow_stop = i < route_waypoint_narrow.size() && route_waypoint_narrow[i];
                    if (narrow_stop && i < route_waypoint_reveals.size()) {
                        const auto *world = GW::GetWorldContext();
                        if (world && world->cartographed_areas.valid() && fog_grid_width) {
                            const auto *bits = reinterpret_cast<const uint32_t *>(world->cartographed_areas.m_buffer);
                            const uint32_t words = world->cartographed_areas.size();
                            narrow_stop = std::ranges::any_of(route_waypoint_reveals[i], [&](const uint32_t fog) {
                                return !IsExplored(bits, fog_grid_width, world->h05B4[1], words, static_cast<int>(fog % fog_grid_width),
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
                        dl->AddCircleFilled(screen, waypoint_size + outline_width, outline_colour);
                        dl->AddCircleFilled(screen, waypoint_size, stop_colour);
                        if (active_stop) dl->AddCircle(screen, waypoint_size + 3.f, active_colour, 0, 2.f);
                    }
                    if (show_numbers_) {
                        const std::string label = std::to_string(i + 1);
                        const ImVec2 label_at{screen.x + waypoint_size + 4.f, screen.y - 7.f};
                        const auto extent = ImGui::CalcTextSize(label.c_str());
                        dl->AddRectFilled({label_at.x - 2.f, label_at.y - 1.f}, {label_at.x + extent.x + 2.f, label_at.y + extent.y + 1.f},
                                          outline_colour, 2.f);
                        dl->AddText(label_at, active_stop ? active_colour : colour, label.c_str());
                    }
                }
                dl->PopClipRect();
            }
        }
    }

    if (show_cursor_coordinates_) {
        const ImVec2 mouse = ImGui::GetMousePos();
        ImRect clip;
        ImVec2 projected;
        GW::Vec2f world;
        bool hovering = false;
        if (ProjectWorldMap({}, projected, clip) && clip.Contains(mouse)) {
            const auto *context = GW::Map::GetWorldMapContext();
            const GW::Vec2f span = context->bottom_right - context->top_left;
            world = {context->top_left.x + (mouse.x - clip.Min.x) * span.x / clip.GetWidth(),
                     context->top_left.y + (mouse.y - clip.Min.y) * span.y / clip.GetHeight()};
            hovering = true;
        }
        else if (ProjectMissionMap({}, projected, clip) && clip.Contains(mouse)) {
            const auto *context = GW::Map::GetMissionMapContext();
            const auto *root = GW::UI::GetRootFrame();
            const auto *frame = context && root ? GW::UI::GetFrameById(context->frame_id) : nullptr;
            if (context && context->h003c && frame) {
                const GW::Vec2f scale = frame->position.GetViewportScale(root);
                const GW::Vec2f center{(clip.Min.x + clip.Max.x) * .5f, (clip.Min.y + clip.Max.y) * .5f};
                const float zoom = GW::GetGameplayContext() ? GW::GetGameplayContext()->mission_map_zoom : 1.f;
                if (fabsf(scale.x * zoom) > .0001f && fabsf(scale.y * zoom) > .0001f) {
                    world = {context->h003c->mission_map_pan_offset.x + (mouse.x - center.x) / (scale.x * zoom),
                             context->h003c->mission_map_pan_offset.y + (mouse.y - center.y) / (scale.y * zoom)};
                    hovering = true;
                }
            }
        }
        GW::Vec2f game;
        if (hovering && WorldToGame(world, game, CartographyMapID())) {
            char coordinates[64];
            snprintf(coordinates, sizeof(coordinates), "x %.0f  y %.0f", game.x, game.y);
            auto *foreground = ImGui::GetForegroundDrawList();
            foreground->AddText({mouse.x + 13.f, mouse.y + 13.f}, IM_COL32(0, 0, 0, 255), coordinates);
            foreground->AddText({mouse.x + 12.f, mouse.y + 12.f}, IM_COL32(255, 255, 255, 255), coordinates);
        }
    }

    if (!show_debug_status_ || !GetVisiblePtr()) return;

    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin(Name(), GetVisiblePtr(), ImGuiWindowFlags_NoCollapse)) {
        ImGui::Text("Current map: %d", static_cast<int>(current_map_));
        ImGui::Text("Instance: %s", current_instance_ == GW::Constants::InstanceType::Outpost ? "Outpost" : "Explorable");
        ImGui::Text("Pogahn mission adaptation: %s", IsPogahnMissionAdaptationActive() ? "active" : "inactive");
        ImGui::Text("Planner: %s", build_status.c_str());
        ImGui::Text("Last build: %.0f ms (background)", build_milliseconds);
        ImGui::Text("Route length: %.0f game units", route_length);
        ImGui::Text("Current leg ends at route point: %u", static_cast<unsigned>(CurrentRouteEnd()));
        ImGui::Text("Useful stand cells: %d", useful_candidates);
        ImGui::Text("Reachable fog cells: %d", unexplored_reachable);
        ImGui::Text("Route waypoints: %u", static_cast<unsigned>(route_waypoints.size()));
        ImGui::Text("Ground route points: %u", static_cast<unsigned>(ground_route.size()));
        ImGui::Text("Ground segments visible: %u", static_cast<unsigned>(ground_segments_drawn));
    }
    ImGui::End();
}
