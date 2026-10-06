# WorldCompletion

A [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp) plugin that plans a route through the unexplored parts of a Guild Wars area.

This project is vibecoded.

## Using it

1. Copy `WorldCompletion.dll` into your Toolbox profile’s `plugins` folder.
2. Enable it in Toolbox’s plugin settings.
3. Enter an explorable area and open the mission map with **U**.

The current leg is highlighted; later legs are faded. The route also appears on the ground. Colors, thickness, outlines, arrows, waypoint size, and the amount of ground route shown are adjustable in the plugin settings.

Bird's Eye View is detected automatically from your active effects, including alternate skill IDs with the same buff-name ID. While active, the planner uses a three-square reveal radius instead of one. The route rebuilds when the buff appears or expires; plugin settings show its status and detected skill ID.

Routing runs in eligible outposts as well as explorable areas. Areas off the world map, dungeons, guild halls, pre-Searing, the Realm of Torment, underground regions, and rectangles without cartography-credit squares automatically suspend routing. Explicit exclusions also cover non-creditable interiors such as Dragon’s Lair, Sorrow’s Furnace, the Undercity and the documented Canthan and Elonian underground areas, even when their world-map rectangles overlap green squares. Returning to an eligible area resumes it. Navigation is checked once per second for additional walkable geometry or opening connections. Stable state, buffer replacements and path closures alone do not trigger a rebuild. New terrain triggers a background extension and retries skipped targets while preserving confirmed discoveries. Stop selection favors the most still-uncovered squares, without a wall or center penalty. When coverage is equal, stop selection prefers a shorter navigable approach instead of the first map-edge cell. A change in reveal radius reassesses old stops so Bird’s Eye View can replace unnecessary wall-side visits. Useful existing waypoint positions are retained; new stops are inserted around them. Replacing the coverage set requires a saving above both 5% and 1,024 game units. Small routes also check the exact visit order using the displayed walking paths. Reordering existing stops or relaxing their positions without changing the stop count requires a saving above both 0.5% and 256 units. Completed, unreachable or redundant stops are removed. Map changes cancel pending work; mesh capture checks map identity and source buffers before reading them.

Portal blocking traces the narrowest continuous walkable passage through the portal, joining its two mesh walls with a thin barrier. Nearby travel on the same side stays available; routing and smoothing cannot cross the barrier. Tracing runs in the background and respects the portal floor. A conservative fallback remains where a reliable pair of walls cannot be found.

Discovery stops stay inside the current area's map boundaries. By default, routes focus only on the zone rectangle. Enable **Include adjacent squares outside the zone** to target its outer border too. Outside squares are targeted only from directly adjacent cells at normal reveal range; Bird's Eye View's extra range is restricted to in-zone targets.

**Coverage refinement passes** controls a background search over alternative coverage sets, stop positions and groups of 2–8 visits. It repeatedly removes spatially related stops, costly detours or route sections, then rebuilds the missing coverage using actual walking cost per newly revealed square. The search changes stand positions and order together; it can use more waypoints when that means less walking. This also applies to small Bird’s Eye routes, where a few widely spaced stops can otherwise cause long detours. Candidate routes are scored using the same smoothed navigation legs as the displayed road. Only the best complete route is published; temporary search candidates may be worse, but never replace the displayed road. A cheap position pass runs before and after the coverage search. It tries points within each reveal cell and nearby cells, preserving each stop’s uniquely covered targets while shortening its approach and departure. This allows Bird’s Eye routes to move inward when the edge targets remain covered. A final cleanup removes redundant stops when doing so does not increase walking distance. Existing stops change only for a material saving. Settings and the diagnostic log show before/after distance, cover trials, moved and merged stops, repairs, and budget exhaustion. Fast, Balanced and Thorough allow approximately 0.5, 3 and 6 seconds of refinement respectively, plus limits on new distance queries; all work runs in the planner worker. Refinement checks the largest detours first and uses straight-line lower bounds to reject uncompetitive moves before computing mesh paths. Navigation searches passage midpoints and positions near both ends, avoiding the long detours caused by midpoint-only corridor selection. Direct segments are accepted only after tracing connected portals on the same floor and checking exit barriers. Navigation rows use a bounded LRU cache. Portal smoothing uses a direct geometric solution instead of repeated numerical searches. Horizontal/vertical sweeps and border/centre-first orders supply additional construction seeds. Complex meshes need more time than flat test maps.

For route diagnostics, **Capture route replay** saves the next build's mesh, fog and planner settings to `plugins/WorldCompletion.route-replay`. The first build after loading also captures automatically. These files contain no character or account details. Run `python3 tools/replay_route.py <snapshot>` on Linux to compare the original and refined walking distance and check that no target squares were dropped. Use `WC_PORTAL_SAMPLES=3 WC_SEARCH_SEED=1 python3 tools/replay_route.py <snapshot> --compare-covering` to compare the new coverage search with the previous refinement on the same capture. Use `--budget-ms 60000` to investigate convergence beyond the in-game budget.

Stops enter reveal cells slightly deeper to allow discovery to register. Adjust **Waypoint entry margin** to change the depth. Entering a stop's reveal cell advances the route while the plugin checks discovery in the background; you do not need to pause at each marker.

Discovery confirmation gets 2.5 seconds after entering the cell, even if you keep moving. Squares that remain unrevealed are given any remaining planned visits before being skipped. Pending visits are shown in settings and are not treated as explored. This affects WorldCompletion's route; Toolbox's own green Cartographer overlay is separate.

Confirmed discoveries and skipped targets are remembered separately for each character and map in `WorldCompletion.visits`, alongside the plugin settings. On entering a zone, the plugin scans its uncovered squares and the adjacent border; new outside-zone discoveries observed while there are saved too. Walking through a square or timing out never marks it discovered. Returning to a zone avoids targeting its remembered discoveries, while roads through explored areas remain available for travel. The file uses whichever is smaller: delta-encoded square IDs or runs of consecutive IDs, packed into variable-length integers. Scans are spread across frames and writes run in the background. **Retry skipped squares** clears only failed targets, keeping confirmed discoveries.

For more than 15 stops, alternative seed orders compare nearest-neighbor routes with different starting stops, shuffled cheapest-insertion routes, randomized nearest-neighbor routes, and perturbed incumbent routes. Every trial gets the same local refinement and is scored using navigation distance plus the selected finish portal. The shortest result wins. Seeds are reproducible and the trial count is bounded by **Alternative seed orders**. Up to 15 stops use exact ordering, where extra seeds cannot improve the optimum.

Click a numbered portal marker on the mission map to finish the route there. The planner recalculates the visit order with that portal as the endpoint. Click the selected portal again to clear it.

The map toggle disables planning and the overlays. The refresh button rebuilds the route from your current position.

## How the route works

Planning runs in the background. Reading the navigation mesh and sampling terrain are spread across frames to keep the game responsive. Disabling the plugin, changing maps, or unloading cancels pending work.

The planner uses the game’s navigation mesh, blocked floors, and portal connections. It selects discovery stops, then orders them using walking costs rather than straight-line distances. Routes with up to 15 stops use exact ordering; larger routes use bounded refinement from both the player and the selected endpoint.

Overlapping visits remain separate legs, so crossing an earlier segment does not count as reaching a later stop. The active leg is drawn above future legs.

This is still experimental. Coverage selection and navigation costs are approximations, so the route is not guaranteed to be the shortest possible. Map clicks and rendering need in-game testing; the automated tests cover the planner.

## Building

The plugin builds independently of Toolbox. It uses an existing Toolbox checkout’s headers and SDK libraries; the build script does not rebuild or install the Toolbox host. Use an SDK that matches your installed Toolbox, and a Toolbox build compatible with the current Guild Wars client.

On Linux, you need Python 3, CMake 3.25+, Ninja, Wine, and an MSVC/Windows SDK toolchain. The Toolbox SDK build must contain `imgui.lib`, `GWToolboxdll/GWToolboxdll.lib`, GWCA, and the dependency headers and libraries.

```sh
./build_and_copy.sh --toolbox-root /path/to/GWToolboxpp --build-only
./build_and_copy.sh --toolbox-root /path/to/GWToolboxpp --destination /path/to/plugins
```

The DLL is written to `bin/WorldCompletion.dll`. Installation backs up the previous DLL before replacing it. Reload the plugin afterward.

For the local development setup, `./build_and_copy.sh` also copies the DLL into the configured Guild Wars plugin folder. It defaults to `~/Documents/gwtb`, or the built `~/Documents/gwtb-sep30-fix` checkout when present. The default install path is `/run/media/tulio/ssd/Games/Guild Wars/GWToolboxpp-config/TULIO/plugins`; use `--destination` for another installation.

### Compiler setup

[msvc-wine](https://github.com/mstorsjo/msvc-wine) provides Microsoft’s compiler and Windows SDK for use under Wine. Install Wine and `msitools`, then:

```sh
mkdir -p build/toolchain
git clone https://github.com/mstorsjo/msvc-wine.git build/toolchain/msvc-wine
git -C build/toolchain/msvc-wine checkout 514f8ea34842cd6d831804d0e9658d3a32870ae1
python3 build/toolchain/msvc-wine/vsdownload.py --architecture x86 x64 --dest build/toolchain/msvc
env WINEPREFIX="$PWD/build/wine-prefix" sh build/toolchain/msvc-wine/install.sh "$PWD/build/toolchain/msvc"
```

Use `--toolchain-root /path/to/msvc` for a different toolchain location. `--docker` uses an existing `gwtoolboxpp-wine-msvc` image; it does not download or build that image.

## Tests

```sh
python3 tests/run_tests.py
```

The tests run on Linux with AddressSanitizer and UndefinedBehaviorSanitizer. They cover navigation boundaries, disconnected and blocked floors, floor transitions, detours, portal endpoints, visit ordering, and cancellation.

## Credits

- [GWToolbox++ and Guild Wars Dev Hub](https://github.com/gwdevhub/GWToolboxpp): the plugin host, plugin base classes, settings support, and utilities this project builds on.
- [GWCA++ bundled with GWToolbox++](https://github.com/gwdevhub/GWToolboxpp/tree/master/Dependencies/GWCA): the game-state, navigation and effect APIs used by this build. Created by KAOS (4D 1) and HasKha; the [original standalone repository](https://github.com/GregLando113/GWCA) is archived.
- [Dear ImGui](https://github.com/ocornut/imgui): the settings interface and overlay controls.
- [MinHook](https://github.com/TsudaKageyu/minhook): the Windows hooking library linked through the Toolbox SDK and plugin framework.
- [msvc-wine](https://github.com/mstorsjo/msvc-wine): the Linux build toolchain wrappers.
- ArenaNet: Guild Wars and its game data. This project is unofficial and is not affiliated with ArenaNet or NCSOFT.

### Routing algorithms

- [Edsger W. Dijkstra, 1959](https://doi.org/10.1007/BF01386390): shortest-path search on the passage graph.
- [Michael Held and Richard M. Karp, 1962](https://doi.org/10.1137/0110015): subset dynamic programming for exact ordering of up to 15 selected stops, adapted to a player start and optional finish portal.
- [G. A. Croes, 1958](https://doi.org/10.1287/opre.6.6.791): 2-opt route improvement. Larger tours also use standard nearest-neighbor, cheapest-insertion and relocation heuristics.
- [John R. Current and David A. Schilling, 1989](https://www.ic.unicamp.br/~fusberti/problems/csp/): the covering salesman formulation, where a subset of visits covers all targets. The plugin adapts this objective to overlapping discovery footprints and an open route.
- [Stephen L. Smith and Frank Imeson, GLNS, 2017](https://ece.uwaterloo.ca/~sl2smith/GLNS/): inspiration for adaptive removal and repair, spatial and detour-based removals, and temporary exploration while retaining the best complete route. Our coverage-aware search is an adaptation, not the GLNS solver or its source code.
- [Bähnemann et al., 2019](https://arxiv.org/abs/1907.09224): inspiration for the experimental region and sweep planner. That prototype is disabled in normal routing because it performed worse on the saved captures.

The passage sampling, portal barriers, discovery checks and position relaxation are implemented for this addon. Research links and measured comparisons are in [route-strategies.md](docs/route-strategies.md).

Released under the [MIT license](LICENSE). The original Guild Wars Dev Hub copyright notice is retained.
