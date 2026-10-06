# WorldCompletion

A [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp) plugin that plans walking routes through unexplored Guild Wars areas. This project is vibecoded.

## Install

1. Download `WorldCompletion.dll` from the [latest release](https://github.com/qjv/WorldCompletion/releases/latest).
2. Copy it into your Toolbox profile’s `plugins` folder and enable it in Toolbox.
3. Open the mission map with **U** in an eligible area.

## Features

- Routes on the mission map and ground, with adjustable colors, lines and waypoint markers.
- Automatic Bird’s Eye View detection and discovery history per character and map.
- Clickable finish portals and background planning when new terrain becomes walkable.
- Routing in eligible outposts and explorable areas; automatic suspension in non-completion zones.

The map toggle disables the addon. Planning is experimental; routes are not guaranteed to be optimal.

## Documentation

See the [full guide](docs/guide.md) for settings, discovery behavior, standalone builds, testing and route diagnostics. [Route strategies](docs/route-strategies.md) covers the research and replay results.

## Credits

Built on [GWToolbox++](https://github.com/gwdevhub/GWToolboxpp), its [bundled GWCA++](https://github.com/gwdevhub/GWToolboxpp/tree/master/Dependencies/GWCA), [Dear ImGui](https://github.com/ocornut/imgui) and [MinHook](https://github.com/TsudaKageyu/minhook). Linux builds use [msvc-wine](https://github.com/mstorsjo/msvc-wine). The [original GWCA++ repository](https://github.com/GregLando113/GWCA) is archived.

Routing uses [Dijkstra](https://doi.org/10.1007/BF01386390), [Held–Karp](https://doi.org/10.1137/0110015), [Croes’ 2-opt](https://doi.org/10.1287/opre.6.6.791), the [covering salesman formulation](https://www.ic.unicamp.br/~fusberti/problems/csp/) and ideas from [Smith and Imeson’s GLNS](https://ece.uwaterloo.ca/~sl2smith/GLNS/). [Full algorithm credits](docs/guide.md#routing-algorithms) explain the adaptations and experimental sweep work.

Guild Wars belongs to ArenaNet. This project is unofficial. Released under the [MIT license](LICENSE).
