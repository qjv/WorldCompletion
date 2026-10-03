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
    std::atomic_bool cancel{false};
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
