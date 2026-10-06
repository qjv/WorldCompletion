`sparkfly-swamp-558.wcrp.gz` contains actual planner inputs captured in Sparkfly
Swamp (map 558), with normal reveal radius 1, 220 target squares, 36 stops and
15 mesh planes. It contains no character or account details. Mesh addresses are
normalized IDs; confirmed square history is represented only as coordinates.

Run the production planner against it:

```sh
python3 tools/replay_route.py tests/data/sparkfly-swamp-558.wcrp.gz --max-ratio 0.85
```

The regression requires all original target squares and at least 15% less
walking. The measured baseline was 429,329 game units, reduced to 335,454 (35 stops) with
the selective 3-second Balanced background budget. Timing limits can affect how far the search
gets on slower machines; `--budget-ms 60000` gives it longer to converge.

The later `sparkfly-swamp-558-rebuild.wcrp.gz` capture has 217 target
squares and 34 stops. The measured baseline was 474,064 game units; the
3-second replay reduced it to approximately 330,081 with all targets retained.

`sparkfly-swamp-current.wcrp.gz` is the user's later manual capture of the
current route. It contains 217 targets and 33 preserved stops. The captured
route was approximately 349,703 units. Use `--compare-region` to benchmark the
region/sweep experiment, existing refinement, and the hybrid on this input.

`sparkfly-swamp-detour.wcrp.gz` and `sparkfly-swamp-backtrack.wcrp.gz` capture later Bird’s Eye routes with widely spaced stops.

`sparkfly-swamp-order-213.wcrp.gz` captures nine preserved stops and 200 remaining targets. It reproduces the midpoint-only corridor bug: the 1-to-3 leg costs 41,805 units instead of approximately 9,945 when passage endpoints are included. `tests/run_tests.py` uses it to verify the shorter route remains on the mesh.

To replay the released planner configuration:

```sh
WC_PORTAL_SAMPLES=3 WC_SEARCH_SEED=1 python3 tools/replay_route.py tests/data/sparkfly-swamp-order-213.wcrp.gz --compare-covering
```

`WC_COMPARE_ORDER=1` compares the captured order with swapping points 1 and 2 and with the exact order for the same stops. `WC_DUMP_ROUTE=1` prints the resulting numbered waypoints. These diagnostic options do not interact with the game.

`sparkfly-swamp-border.wcrp.gz` captures the later Bird’s Eye border route, with 114 remaining targets. The position pass reduces the replayed coverage route from 49,350 to 47,600 units while retaining those targets. A whole-cell translation is not always safe: the diagnostic reports lost coverage before accepting any shift.
