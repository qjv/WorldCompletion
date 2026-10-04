# WorldCompletion

A [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp) plugin that plans a route through the unexplored parts of a Guild Wars area.

This project is vibecoded.

## Using it

1. Copy `WorldCompletion.dll` into your Toolbox profile’s `plugins` folder.
2. Enable it in Toolbox’s plugin settings.
3. Enter an explorable area and open the mission map with **U**.

The current leg is highlighted; later legs are faded. The route also appears on the ground. Colors, thickness, outlines, arrows, waypoint size, and the amount of ground route shown are adjustable in the plugin settings.

Bird's Eye View is detected automatically from your active effects, including alternate skill IDs with the same buff-name ID. While active, the planner uses a three-square reveal radius instead of one. The route rebuilds when the buff appears or expires; plugin settings show its status and detected skill ID.

Routing runs in eligible outposts as well as explorable areas. Areas off the world map, dungeons, guild halls, pre-Searing, the Realm of Torment, and rectangles without cartography-credit squares automatically suspend routing. Returning to an eligible area resumes it. Navigation is checked once per second for additional walkable geometry or opening connections. Stable state, buffer replacements and path closures alone do not trigger a rebuild. New terrain triggers a background extension and retries skipped targets while preserving confirmed discoveries. Useful existing waypoint positions and their relative order are retained; new stops are inserted around them. Completed, unreachable or redundant stops are removed. Map changes cancel pending work; mesh capture checks map identity and source buffers before reading them.

Portal blocking traces the narrowest continuous walkable passage through the portal, joining its two mesh walls with a thin barrier. Nearby travel on the same side stays available; routing and smoothing cannot cross the barrier. Tracing runs in the background and respects the portal floor. A conservative fallback remains where a reliable pair of walls cannot be found.

Discovery stops stay inside the current area's map boundaries. By default, routes focus only on the zone rectangle. Enable **Include adjacent squares outside the zone** to target its outer border too. Outside squares are targeted only from directly adjacent cells at normal reveal range; Bird's Eye View's extra range is restricted to in-zone targets.

Stops enter reveal cells slightly deeper to allow discovery to register. Adjust **Waypoint entry margin** to change the depth. Entering a stop's reveal cell advances the route while the plugin checks discovery in the background; you do not need to pause at each marker.

Discovery confirmation gets 2.5 seconds after entering the cell, even if you keep moving. Squares that remain unrevealed are given any remaining planned visits before being skipped. Pending visits are shown in settings and are not treated as explored. This affects WorldCompletion's route; Toolbox's own green Cartographer overlay is separate.

Confirmed discoveries and skipped targets are remembered separately for each character and map in `WorldCompletion.visits`, alongside the plugin settings. On entering a zone, the plugin scans its uncovered squares and the adjacent border; new outside-zone discoveries observed while there are saved too. Walking through a square or timing out never marks it discovered. Returning to a zone avoids targeting its remembered discoveries, while roads through explored areas remain available for travel. The file uses whichever is smaller: delta-encoded square IDs or runs of consecutive IDs, packed into variable-length integers. Scans are spread across frames and writes run in the background. **Retry skipped squares** clears only failed targets, keeping confirmed discoveries.

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
- [GWCA](https://github.com/GWCA/GWCA): access to Guild Wars game state, navigation data, and game APIs.
- [Dear ImGui](https://github.com/ocornut/imgui): the settings interface and overlay controls.
- [msvc-wine](https://github.com/mstorsjo/msvc-wine): the Linux build toolchain wrappers.
- ArenaNet: Guild Wars and its game data. This project is unofficial and is not affiliated with ArenaNet or NCSOFT.

Released under the [MIT license](LICENSE). The original Guild Wars Dev Hub copyright notice is retained.
