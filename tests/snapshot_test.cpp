static SnapshotPlane Plane(std::initializer_list<std::array<float, 4>> boxes, uintptr_t base)
{
    SnapshotPlane plane;
    for (const auto box : boxes) {
        GW::PathingTrapezoid trap{};
        trap.XTL = trap.XBL = box[0];
        trap.XTR = trap.XBR = box[2];
        trap.YB = box[1];
        trap.YT = box[3];
        plane.trapezoids.push_back(trap);
        plane.addresses.push_back(base++);
        plane.links.emplace_back();
    }
    plane.trapezoid_count = static_cast<uint32_t>(plane.trapezoids.size());
    return plane;
}

static BuildSnapshot Snapshot()
{
    BuildSnapshot snapshot;
    snapshot.width = 32;
    snapshot.height = 8;
    snapshot.bits.assign(8, ~uint32_t{0});
    snapshot.anchor = {0, 256};
    snapshot.bounds = {{0, 0}, {1024, 256}};
    snapshot.discovery_bounds = snapshot.bounds;
    snapshot.player = {500, 500, 0};
    snapshot.relaxation = 2;
    return snapshot;
}

static void Fog(BuildSnapshot& snapshot, uint32_t x, uint32_t y)
{
    snapshot.bits[y] &= ~(uint32_t{1} << x);
}

static bool OnMesh(const BuildSnapshot& snapshot, const GW::GamePos& point)
{
    if (point.zplane >= snapshot.maps.size()) return false;
    for (const auto& trap : snapshot.maps[point.zplane].trapezoids)
        if (Contains(&trap, {point.x, point.y})) return true;
    return false;
}

static void Validate(const BuildSnapshot& snapshot, const BuildResult& result)
{
    assert(result.connected);
    assert(result.waypoints.size() == result.waypoint_indices.size());
    for (size_t i = 0; i < result.waypoint_indices.size(); ++i) {
        assert(result.waypoint_indices[i] < result.route.size());
        if (i) assert(result.waypoint_indices[i - 1] < result.waypoint_indices[i]);
        const auto& point = result.route[result.waypoint_indices[i]];
        assert(point.x == result.waypoints[i].x && point.y == result.waypoints[i].y);
    }
    for (size_t i = 1; i < result.route.size(); ++i) {
        const auto a = result.route[i - 1], b = result.route[i];
        for (int step = 0; step <= 100; ++step) {
            const float t = static_cast<float>(step) / 100.f;
            assert(OnMesh(snapshot, {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.zplane}));
        }
    }
}

int main()
{
    {
        Doorway door;
        door.pos = {9000, 500};
        door.radius_sq = 1800.f * 1800.f;
        door.plane = 0;
        door.barrier = completion::Portal{{9000, -48}, {9000, 1048}};
        assert(DoorwayBlocks(door, {8000, 500}, {9500, 500}, 0));
        assert(!DoorwayBlocks(door, {8800, 100}, {8800, 900}, 0));
        assert(!DoorwayBlocks(door, {8000, 500}, {9500, 500}, 1));
        door.barrier.reset();
        assert(DoorwayBlocks(door, {8800, 100}, {8800, 900}, 0));
    }

    assert(PointerInArray(1040, 1000, 3, 20));
    assert(!PointerInArray(1060, 1000, 3, 20));
    assert(!PointerInArray(1041, 1000, 3, 20));
    assert(!PointerInArray(980, 1000, 3, 20));
    assert(!PointerInArray(0, 0, 3, 20));

    {
        assert(CompletionMapEligible()); // Cartography outpost.
        GW::Map::instance = GW::Constants::InstanceType::Explorable;
        assert(CompletionMapEligible());
        GW::Map::loaded = false;
        assert(!CompletionMapEligible());
        GW::Map::loaded = true;
        GW::Map::instance = GW::Constants::InstanceType::Loading;
        assert(!CompletionMapEligible());
        GW::Map::instance = GW::Constants::InstanceType::Outpost;
        GW::Map::info.on_map = false;
        assert(!CompletionMapEligible());
        GW::Map::info.on_map = true;
        GW::Map::info.continent = GW::Continent::RealmOfTorment;
        assert(!CompletionMapEligible());
        GW::Map::info.continent = GW::Continent::Tyria;
        GW::Map::info.type = GW::RegionType::Dungeon;
        assert(!CompletionMapEligible());
        GW::Map::info.type = GW::RegionType::Outpost;
        GW::Map::info.guild_hall = true;
        assert(!CompletionMapEligible());
        GW::Map::info.guild_hall = false;
        GW::Map::info.region = GW::Region_Presearing;
        assert(!CompletionMapEligible());
        GW::Map::info.region = GW::Region_Kryta;
        ++GW::Map::id;
        credit_bits = 0;
        assert(!CompletionMapEligible());
        ++GW::Map::id;
        credit_bits = 1;
        assert(CompletionMapEligible()); // Resumes after an excluded area.
    }

    {
        const ImRect bounds{{32, 64}, {128, 160}};
        assert(FogCellWithinBounds(bounds, 1, 2));
        assert(FogCellWithinBounds(bounds, 3, 4));
        assert(!FogCellWithinBounds(bounds, 0, 2));
        assert(!FogCellWithinBounds(bounds, 4, 2));
        assert(!FogCellWithinBounds(bounds, 1, 1));
        assert(!FogCellWithinBounds(bounds, 1, 5));
        assert(FogCellWithinBounds({{33, 65}, {127, 159}}, 1, 2));
        assert(DiscoveryCellAllowed(bounds, 0, 2, -1, 0));
        assert(!DiscoveryCellAllowed(bounds, 0, 2, -2, 0));
        assert(!DiscoveryCellAllowed(bounds, 4, 2, 2, 0));
        assert(!DiscoveryCellAllowed(bounds, 1, 1, 0, -2));
        assert(!DiscoveryCellAllowed(bounds, 1, 5, 0, 2));
    }
    {
        completion::MapVisits record;
        fixture_visits = &record;
        skipped_fog.insert(42);
        record.skipped.insert(42);
        RememberDiscovered(42);
        RememberDiscovered(9000); // Actual outside-map discoveries are kept too.
        assert(record.discovered == std::set<uint64_t>({42, 9000}));
        assert(record.skipped.empty() && skipped_fog.empty());
        assert(history_dirty);
        fixture_visits = nullptr;
        history_dirty = false;
    }
    {
        DiscoveryVisit visit{{1, 2}, 100};
        assert(!visit.Ready(2599));
        assert(visit.Ready(2600));
        visit.since = UINT32_MAX - 1000;
        assert(visit.Ready(1500));
        route = {{0, 0, 0}, {100, 0, 0}, {200, 0, 0}};
        route_waypoint_reveals = {{1, 2}, {2, 3}};
        route_waypoint_indices = {1, 2};
        route_waypoint_visited = {false, false};
        assert(CurrentRouteEnd() == 1);
        route_waypoint_visited[0] = true;
        pending_discovery.push_back({{1, 2}, 100});
        assert(CurrentRouteEnd() == 2);
        assert(!ReviewDiscovery(2599));
        assert(skipped_fog.empty() && pending_discovery.size() == 1);
        explored_fog.insert(1);
        assert(!ReviewDiscovery(2600));
        assert(skipped_fog.empty() && pending_discovery.empty()); // Square 2 still has a later visit.
        route_waypoint_visited[1] = true;
        pending_discovery.push_back({{2, 3}, 2700});
        explored_fog.insert(2);
        assert(ReviewDiscovery(5200));
        assert(skipped_fog == std::unordered_set<uint32_t>{3});
        skipped_fog.clear();
        pending_discovery.push_back({{1, 2}, 6000});
        assert(!ReviewDiscovery(6001)); // Confirmed fog retires immediately, without a pause.
        assert(pending_discovery.empty());
        route.clear();
        route_waypoint_reveals.clear();
        route_waypoint_indices.clear();
        route_waypoint_visited.clear();
        explored_fog.clear();
    }
    {
        const auto regular = static_cast<GW::Constants::SkillID>(3439);
        const auto alternate = static_cast<GW::Constants::SkillID>(9000); // Synthetic alternate ID.
        const auto unrelated = static_cast<GW::Constants::SkillID>(9001);
        auto &effects = GW::Effects::effects;
        GW::SkillbarMgr::skills = {{regular, {100}}, {alternate, {100}}, {unrelated, {101}}};
        assert(BirdsEyeViewEffect() == 0);
        effects.push_back({regular});
        assert(BirdsEyeViewEffect() == 3439);
        effects[0].skill_id = alternate;
        assert(BirdsEyeViewEffect() == 9000);
        effects[0].skill_id = unrelated;
        assert(BirdsEyeViewEffect() == 0);
        effects.clear();
        assert(BirdsEyeViewEffect() == 0);
        effects.available = false;
        assert(BirdsEyeViewEffect() == 0);
        effects.available = true;
        GW::SkillbarMgr::skills.clear();
        effects.push_back({alternate});
        assert(BirdsEyeViewEffect() == 0);
        effects.clear();
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 20000, 20000}}, 50));
        snapshot.relaxation = 0;
        Fog(snapshot, 1, 3);
        Fog(snapshot, 5, 3);
        std::atomic_bool cancelled{false};
        BuildResult original;
        BuildRoute(snapshot, original, cancelled);
        assert(original.waypoints.size() == 2);
        snapshot.previous_waypoints = original.waypoints;
        snapshot.relaxation = 8;
        for (int repeat = 0; repeat < 3; ++repeat) {
            BuildResult stable;
            BuildRoute(snapshot, stable, cancelled);
            Validate(snapshot, stable);
            assert(stable.waypoints.size() == original.waypoints.size());
            for (size_t i = 0; i < original.waypoints.size(); ++i) {
                assert(stable.waypoints[i].x == original.waypoints[i].x);
                assert(stable.waypoints[i].y == original.waypoints[i].y);
                assert(stable.waypoints[i].zplane == original.waypoints[i].zplane);
            }
        }
        Fog(snapshot, 1, 6);
        BuildResult expanded;
        BuildRoute(snapshot, expanded, cancelled);
        Validate(snapshot, expanded);
        size_t next = 0;
        for (const auto& stop : expanded.waypoints)
            if (next < original.waypoints.size() && stop.x == original.waypoints[next].x && stop.y == original.waypoints[next].y) ++next;
        assert(next == original.waypoints.size());
        assert(expanded.waypoints.size() > original.waypoints.size());
        snapshot.previous_waypoints.push_back({-9999, -9999, 0}); // Invalid old stop discarded.
        BuildResult invalid;
        BuildRoute(snapshot, invalid, cancelled);
        Validate(snapshot, invalid);
        for (const auto& stop : invalid.waypoints) assert(stop.x != -9999);
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 16000, 6000}}, 800));
        Doorway door;
        door.pos = {9000, 3000};
        door.radius_sq = 1800.f * 1800.f;
        snapshot.doorways.push_back(door);
        Fog(snapshot, 0, 6);
        Fog(snapshot, 4, 6);
        std::atomic_bool cancelled{false};
        BuildResult blocked;
        BuildRoute(snapshot, blocked, cancelled);
        Validate(snapshot, blocked);
        assert(!blocked.waypoints.empty());
        for (const auto& reveal : blocked.reveals)
            assert(std::ranges::find(reveal, 6 * 32 + 4) == reveal.end());
        for (const auto& point : blocked.route) assert(point.x < 9000);
        snapshot.doorways.clear();
        BuildResult open;
        BuildRoute(snapshot, open, cancelled);
        bool covers_far_side = false;
        for (const auto& reveal : open.reveals)
            covers_far_side |= std::ranges::find(reveal, 6 * 32 + 4) != reveal.end();
        assert(covers_far_side);
    }
    for (const float offset : {0.f, 20000.f, -20000.f}) {
        const auto plane = Plane({{offset - 1.f, offset - 1.f, offset + .25f, offset + .25f}}, 1);
        GW::Vec2f footing{};
        float area = 0.f;
        assert(TrapezoidCellOverlap(&plane.trapezoids[0], {offset, offset},
                                   {offset + 3072.f, offset + 3072.f}, footing, area));
        assert(std::abs(area - .0625f) < 1e-6f);
        assert(footing.x == offset + .125f && footing.y == offset + .125f);
        assert(Contains(&plane.trapezoids[0], footing));
        const GW::Vec2f approach{offset - 10.f, offset + .125f};
        assert(TrapezoidCellOverlap(&plane.trapezoids[0], {offset, offset},
                                   {offset + 3072.f, offset + 3072.f}, footing, area, &approach));
        assert(footing.x > offset && footing.x < offset + .125f);
        assert(Contains(&plane.trapezoids[0], footing));
    }
    {
        const auto plane = Plane({{0, 0, 3072, 3072}}, 1);
        const GW::Vec2f approach{-1000, 1536};
        GW::Vec2f footing{};
        float area = 0.f;
        assert(TrapezoidCellOverlap(&plane.trapezoids[0], {0, 0}, {3072, 3072}, footing, area, &approach));
        assert(footing.x == 192.f && footing.y == 1536.f);
    }
    std::atomic_bool cancel{false};
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 3072}}, 20));
        Fog(snapshot, 1, 7);
        completion::VisitHistory history;
        history.For("Alice", 43, snapshot.width).discovered.insert(7 * 32 + 1);
        completion::VisitHistory restored;
        assert(restored.Decode(history.Encode()));
        snapshot.confirmed_fog = restored.For("Alice", 43, snapshot.width).discovered;
        BuildResult revisit;
        BuildRoute(snapshot, revisit, cancel);
        assert(revisit.waypoints.empty());
        snapshot.confirmed_fog = restored.For("Bob", 43, snapshot.width).discovered;
        BuildResult other_character;
        BuildRoute(snapshot, other_character, cancel);
        Validate(snapshot, other_character);
        assert(other_character.waypoints.size() == 1);
        snapshot.confirmed_fog = restored.For("Alice", 44, snapshot.width).discovered;
        BuildResult other_map;
        BuildRoute(snapshot, other_map, cancel);
        assert(other_map.waypoints.size() == 1);
    }
    {
        auto snapshot = Snapshot();
        snapshot.reveal_radius = 3;
        snapshot.discovery_bounds = {{96, 96}, {128, 128}};
        snapshot.maps.push_back(Plane({{0, 0, 20000, 20000}}, 30));
        snapshot.player = {11000, 13000, 0};
        Fog(snapshot, 3, 3);
        for (const auto cell : {std::array<uint32_t, 2>{1, 3}, {5, 3}, {3, 1}, {3, 5}})
            Fog(snapshot, cell[0], cell[1]);
        BuildResult result;
        BuildRoute(snapshot, result, cancel);
        Validate(snapshot, result);
        assert(result.waypoints.size() == 1);
        assert(result.reveals[0] == std::vector<uint32_t>{3 * 32 + 3});
        assert(result.waypoints[0].x >= 9216 && result.waypoints[0].x <= 12288);
        assert(result.waypoints[0].y >= 12288 && result.waypoints[0].y <= 15360);
        snapshot.bits[3] |= uint32_t{1} << 3;
        BuildResult outside_only;
        BuildRoute(snapshot, outside_only, cancel);
        assert(outside_only.waypoints.empty());
        Fog(snapshot, 2, 3);
        BuildResult rectangle_only;
        BuildRoute(snapshot, rectangle_only, cancel);
        assert(rectangle_only.waypoints.empty());
        snapshot.include_outside = true;
        BuildResult adjacent;
        BuildRoute(snapshot, adjacent, cancel);
        Validate(snapshot, adjacent);
        assert(adjacent.waypoints.size() == 1);
        assert(adjacent.reveals[0] == std::vector<uint32_t>{3 * 32 + 2});
    }
    {
        route = {{0, 0, 0}, {100, 0, 0}, {200, 0, 0}};
        route_progress = 0;
        assert(ClosestRoutePointThrough({200, 0, 0}, 1) == 0);
        assert(ClosestRoutePointThrough({200, 0, 0}, 2) == 1);
        route_progress = 2;
        assert(ClosestRoutePointThrough({200, 0, 0}, 2) == 1);
        route.clear();
        route_progress = 0;
        assert(ClosestRoutePointThrough({}, 0) == 0);
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 3072}}, 40));
        Fog(snapshot, 3, 7);
        BuildResult normal;
        BuildRoute(snapshot, normal, cancel);
        assert(normal.waypoints.empty());
        snapshot.reveal_radius = 3;
        BuildResult compass;
        BuildRoute(snapshot, compass, cancel);
        Validate(snapshot, compass);
        assert(compass.waypoints.size() == 1);
        assert(compass.reveals[0] == std::vector<uint32_t>{7 * 32 + 3});
        snapshot.skipped.insert(7 * 32 + 3);
        BuildResult skipped;
        BuildRoute(snapshot, skipped, cancel);
        assert(skipped.waypoints.empty());
        Fog(snapshot, 2, 7);
        BuildResult partial;
        BuildRoute(snapshot, partial, cancel);
        Validate(snapshot, partial);
        assert(partial.waypoints.size() == 1);
        assert(partial.reveals[0] == std::vector<uint32_t>{7 * 32 + 2});
        snapshot.skipped.clear();
        BuildResult retry;
        BuildRoute(snapshot, retry, cancel);
        Validate(snapshot, retry);
        assert(retry.reveals[0].size() == 2);
        snapshot.reveal_radius = 1;
        BuildResult expired;
        BuildRoute(snapshot, expired, cancel);
        assert(expired.waypoints.empty());
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 6144, 3072}}, 50));
        Fog(snapshot, 2, 7);
        snapshot.relaxation = 0;
        BuildResult centered;
        BuildRoute(snapshot, centered, cancel);
        snapshot.relaxation = 8;
        BuildResult edge;
        BuildRoute(snapshot, edge, cancel);
        Validate(snapshot, centered);
        Validate(snapshot, edge);
        assert(centered.waypoints.size() == 1 && edge.waypoints.size() == 1);
        assert(centered.reveals == edge.reveals);
        const auto distance = [&](const GW::GamePos &point) {
            return hypotf(point.x - snapshot.player.x, point.y - snapshot.player.y);
        };
        assert(distance(edge.waypoints[0]) + 1000.f < distance(centered.waypoints[0]));
        assert(edge.waypoints[0].x > 3104.f && edge.waypoints[0].x < 3264.f);
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 3072}, {3072, 0, 6144, 3072}, {6144, 0, 9216, 3072}}, 100));
        snapshot.maps[0].links = {{101}, {100, 102}, {101}};
        Fog(snapshot, 3, 7);
        BuildResult result;
        BuildRoute(snapshot, result, cancel);
        Validate(snapshot, result);
        assert(result.status == "ready" && result.route.size() == 2);
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 9216}, {3072, 6144, 6144, 9216}, {6144, 0, 9216, 9216}}, 100));
        snapshot.maps[0].links = {{101}, {100, 102}, {101}};
        Fog(snapshot, 3, 7);
        BuildResult result;
        BuildRoute(snapshot, result, cancel);
        Validate(snapshot, result);
        assert(std::ranges::any_of(result.route, [](const auto& p) { return p.y >= 6144; }));
        assert(result.length > hypotf(result.route.back().x - snapshot.player.x, result.route.back().y - snapshot.player.y) + 3000.f);
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 3072}}, 100));
        snapshot.maps.push_back(Plane({{3072, 0, 9216, 3072}}, 200));
        snapshot.maps[0].links = {{200}};
        snapshot.maps[1].links = {{100}};
        Fog(snapshot, 3, 7);
        BuildResult result;
        BuildRoute(snapshot, result, cancel);
        Validate(snapshot, result);
        assert(result.route.size() >= 3);
        assert(result.route.front().zplane == 0 && result.route.back().zplane == 1);
        assert(std::ranges::any_of(result.route, [](const auto& p) { return std::fabs(p.x - 3072.f) < .01f; }));
        snapshot.maps[1].blocked = true;
        BuildResult blocked;
        BuildRoute(snapshot, blocked, cancel);
        assert(!blocked.connected);
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 3072}}, 100));
        snapshot.maps.push_back(Plane({{0, 0, 9216, 3072}}, 200));
        snapshot.maps[0].links = {{200}};
        snapshot.maps[1].links = {{100}};
        Fog(snapshot, 3, 7);
        BuildResult result;
        BuildRoute(snapshot, result, cancel);
        // A connected plane may be reached only through its declared shared boundary.
        if (result.connected) Validate(snapshot, result);
        snapshot.maps[0].links = {{}};
        snapshot.maps[1].links = {{}};
        BuildResult separated;
        BuildRoute(snapshot, separated, cancel);
        assert(!separated.connected);
        snapshot.player.zplane = 9;
        BuildRoute(snapshot, separated, cancel);
        assert(separated.status == "player_not_on_pathing");
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 3072}, {3072, 0, 9216, 3072}}, 100));
        snapshot.maps[0].links = {{101}, {100}};
        snapshot.endpoints.push_back({9000, 1500});
        snapshot.end_portal = 0;
        BuildResult result;
        BuildRoute(snapshot, result, cancel);
        Validate(snapshot, result);
        assert(result.waypoints.empty() && result.end_point);
    }
    {
        auto snapshot = Snapshot();
        snapshot.maps.push_back(Plane({{0, 0, 3072, 3072}}, 100));
        BuildResult result;
        cancel = true;
        bool threw = false;
        try { BuildRoute(snapshot, result, cancel); }
        catch (const BuildCancelled&) { threw = true; }
        assert(threw);
    }
    std::cout << "Snapshot planner: straight corridor, U detour, floor transition, blocked/separate floors, portal-only route, cancellation passed\n";
}
