# WorldCompletion

WorldCompletion is an experimental [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp) plugin for Guild Wars. It builds a route through the still-unexplored portions of the current explorable area and displays that route as directional arrows on the world map, mission map, minimap, and game world.

## Features

- Recomputes the route as cartography progress changes.
- Keeps route progress monotonic through intersections and overlapping roads.
- Draws a gap-free minimap line plus adjustable, end-to-end directional arrows on the world map and terrain.
- Provides arrow sizing from 10% to 250% and a configurable route color.
- Provides optional depth-tested ground rendering so terrain can occlude arrows.
- Includes adjacent minimap controls for visibility and explicit recomputation.
- Marks narrow mandatory stops and omits completed reveal targets.
- Suspends route state during travel and map transitions.
- Uses bounded optimization passes to avoid long loading stalls.
- Exposes fast, balanced, and thorough optimization modes, relaxation passes, and randomized restart counts.
- Saves appearance and optimizer settings explicitly when the plugin unloads.

## Installation

Download `WorldCompletion.dll` from the latest release and copy it to your GWToolbox plugin directory. For the configuration used during development, that is:

```text
GWToolboxpp-config/<profile>/plugins/WorldCompletion.dll
```

Restart Guild Wars/GWToolbox or reload the plugin. This repository contains only the plugin; it does not replace or update GWToolbox itself.

## Building

The source is arranged under `plugins/WorldCompletion` so it can be copied into a GWToolbox++ checkout. Add this entry to `cmake/gwtoolboxdll_plugins.cmake` alongside the other plugin declarations:

```cmake
add_tb_plugin(WorldCompletion)
```

Then configure GWToolbox++ normally and build the `WorldCompletion` target. The released binary is built as 32-bit Windows `RelWithDebInfo` using the project's Wine/MSVC Docker toolchain.

## Methodology

The plugin reads Guild Wars' live cartography bitfield and pathing trapezoids through GWCA. It maps unexplored fog cells to reachable standing candidates, favors candidates that reveal multiple required cells, and reduces redundant stops while preserving coverage.

Route construction uses the game's pathing-plane and portal connectivity. A bounded shortest-path search creates traversable legs between selected stops. The visit order is refined with open-path 2-opt and relocation passes. Alternating forward and reverse relaxation sweeps choose only local footing alternatives for the same cartography cell. Their objective combines adjacent-leg distance with lateral deviation from the neighboring-stop chord, so additional passes converge toward straighter placements instead of drifting between distant reveal candidates. The settings control the pass budget and randomized search breadth.

Progress is constrained to the current unfinished target rather than chosen globally from the nearest route geometry. This prevents crossings and shared road sections from skipping later targets. The ground route samples terrain altitude and renders contiguous chevrons with independently interpolated Z intervals. With occlusion enabled, those triangles are depth-tested against the game scene.

Final route geometry retains the ordered transitions between adjacent pathing trapezoids. Optimization is limited to stop selection and ordering, so recomputation—manual or automatic—cannot replace a graph-valid leg with a straight XY shortcut across separate terrain surfaces.

Expensive search stages use user-bounded iteration counts and cached geometry. Route computation is triggered by meaningful map/cartography changes or the recompute button rather than every frame. Travel, map-change, and map-loaded messages suspend the route and release Direct3D state before map-owned resources change.

## Credits

- [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp) and the Guild Wars Dev Hub contributors for the plugin host, build system, utilities, and original MIT license.
- [GWCA](https://github.com/GWCA/GWCA) contributors for the Guild Wars client API and game data access.
- [Dear ImGui](https://github.com/ocornut/imgui) contributors for overlay drawing and settings controls.
- ArenaNet for Guild Wars. This is an unofficial community project and is not affiliated with or endorsed by ArenaNet or NCSOFT.

## License

MIT. See [LICENSE](LICENSE). The retained license credits Guild Wars Dev Hub, reflecting the GWToolbox++ codebase in which this plugin was developed.

## Status

This software interacts with live game and rendering state and should be treated as experimental. Back up your GWToolbox configuration before testing new releases.
