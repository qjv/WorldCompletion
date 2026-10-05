# Coverage route strategies

The objective is walking distance while revealing every reachable target square.
Stop count and visual straightness alone are insufficient: a long sweep may be
necessary, while a short-looking line may cross a wall or miss fog.

The current planner first selects a coverage set and then orders its stops. This
can lock the search into a poor selection even after the ordering improves.

## Research and adaptation

- [Bähnemann et al., 2019](https://arxiv.org/abs/1907.09224) compare sweep
  combinations across obstacle-separated regions and account for connections
  between them. For this plugin, partition the reachable mesh into corridors and
  open regions, generate both sweep orientations and entry/exit choices, then
  optimize their connections to the selected portal. Discovery squares need
  coverage by the reveal footprint; they do not all need to be walked through.
- [Karapetyan and Gutin, 2012](https://arxiv.org/abs/1005.5525) study efficient
  local search for generalized TSP. Adaptation here means changing the footing
  and its order together. Our reveal sets overlap, so this is not a direct GTSP
  implementation: every proposed replacement must retain the complete target
  union.
- Border-first and centre-first are candidate constructions, not universal
  rules. A perimeter loop can reveal the edges efficiently, but interior sweeps
  may already reveal the same squares. Compare both with horizontal/vertical
  sweeps and different reveal-width offsets using actual mesh distance.

## Experiments

`SpatialSeeds.h` builds horizontal/vertical alternating sweeps with two offsets,
their reverse orders, and border/centre-first ring orders. Cheap navigation
estimates rank them; only four receive ordering refinement. Every accepted
coverage repair still uses measured, smoothed road length.

Reveal-width lattice covers were also tested. Different offsets changed which
cells were visited, with greedy repairs for targets lacking suitable lattice
footing. The prototype consumed search time without improving both saved
Sparkfly cases sufficiently, so it was removed.

Replacing smoothed distances with portal-midpoint estimates for repair ordering
was also tested. It improved the newer capture but regressed the original one.
Use cheap bounds to reject losers, while retaining actual road costs for local
ordering and final acceptance.

## Reducing cost

Portal smoothing formerly used 24 ternary iterations per portal per sweep. The
minimum two-segment distance through a fixed line segment has a closed-form
reflection solution, clamped to the segment. Computing that point directly cuts
the work of each trial without replacing navigation with straight-line guesses.
Portal barrier checks and floor-aware corridor validation remain necessary.

Further work should compare total build time, walking length, coverage union,
repeated corridors and changes to preserved waypoints on both saved captures.
Only then should region decomposition or coverage along moving segments replace
the current stop-based model. Moving discovery also needs evidence about reveal
latency; assuming immediate revelation could recreate the missed-square bug.

## Region/sweep prototype

`RegionCoveragePlanner.h` implements a discrete adaptation of the paper's
architecture: scan the valid stand-cell grid and split regions at interval
splits/merges; generate sweep directions, reveal-width offsets and reversed
entry/exit patterns; assign fog targets to regions with a valid footing; and
jointly optimize region order and pattern choice. Pattern choice for a fixed
region order uses dynamic programming; bounded 2-opt searches region orders.
Distances are measured on the captured mesh, and every target is checked after
building the road. Floors remain separate.

This is not the publication's exact polygon decomposition or its external
memetic GTSP solver. A discovery cell containing a small obstacle may still be
walkable, so the discrete decomposition can merge geometry that an exact mesh
polygon decomposition would separate. That limits what these results establish.

The prototype is disabled in the game. Run:

```sh
python3 tools/replay_route.py tests/data/sparkfly-swamp-current.wcrp.gz --compare-region
```

The comparison measures the existing refined planner, pure region sweeps, and
region sweeps followed by 0.5 seconds of the existing local repair. Compiler
time is excluded; binaries are cached by generated source and header content.

On the latest capture, the current planner took about 3.09 seconds and produced
a 326,621-unit road. Region sweeps took 1.43 seconds but produced 459,120 units;
the hybrid took 1.92 seconds and produced 375,823 units. All retained 217 targets.
On the first capture, the corresponding distances were 335,454, 459,228 and
368,721 units, with times 3.10, 1.55 and 2.04 seconds. All retained 220 targets.
Timing varies with machine load and affects bounded local search results.

The second capture likewise produced 329,169 units in 3.10 seconds with the
current planner, 459,166 units in 1.41 seconds with region sweeps, and 371,602
units in 1.93 seconds with the hybrid. All retained its 217 targets.

The experiment builds faster but produces longer roads on these cases. It is
not enabled by default and does not justify replacing the installed planner.

## Recommended next experiment: covering-route large neighborhood search

The [covering salesman formulation](https://www.ic.unicamp.br/~fusberti/problems/csp/)
chooses a subset of vertices whose coverage sets cover every target and
minimizes travel over that subset. This is a closer fit to the plugin's overlapping
reveal squares than obligating it to finish every geometric region in one sweep.
Our adaptation must be an open route from the player to the optional selected
portal, with floor-aware mesh travel, rather than the formulation's closed tour.

[Smith and Imeson, GLNS](https://ece.uwaterloo.ca/~sl2smith/GLNS/) use adaptive
large neighborhood search to repeatedly destroy and reconstruct tours. Their
GTSP solver is not a direct replacement here: it uses clusters, whereas one of
our stand positions can satisfy multiple overlapping discovery requirements.
The useful adaptation is its search structure, with explicit coverage validation.

The current repairs remove consecutive route windows and reconstruct them
mostly by coverage gain. Proposed changes:

1. Compress the remaining fog IDs into a dense index and store candidate reveal
   sets as bitsets. Keep a reverse index of valid footings for each target.
2. Remove 4–10 stops chosen by spatial proximity, overlapping coverage, or their
   contribution to long detours. Their route indices need not be consecutive.
3. Keep the surviving route and compute only targets that lost their final cover.
4. Reinsert footings according to extra mesh walking distance divided by newly
   covered targets, not their distance from the player. Prioritize rare targets
   and targets whose second-best insertion is much worse than their best.
5. Consider changing both footing and order, and allow more stops if they yield
   less walking. Stop count is not the objective. Local 2-opt and relocation can
   polish the repaired candidate.
6. Retain the best complete route independently of the exploration state. A
   bounded experimental search may temporarily explore worse complete routes to
   escape local minima, but never publish them. Preserve existing waypoints
   unless the final saving meets the stability threshold.
7. Give more attempts to removal/repair operators that deliver the greatest
   saving per millisecond. Use cached mesh legs, straight-line lower bounds and
   small insertion shortlists to bound expensive path evaluations.

Acceptance requires complete target coverage and shorter actual rendered-road
distance. Compare all three saved captures at the same time budget, with several
fixed random seeds and total build timing. The coverage-aware implementation now beats the previous planner on all three saved Sparkfly captures. It uses four-stop removals, a twelve-entry shortlist diversified by newly covered targets, dense coverage bitsets, adaptive removal weights, and a bounded 160-trial search. A fixed seed of 1 gives repeatable operator choices without map-specific tuning. Final cleanup removes redundant stops; existing routes require at least a 5% or 1,024-unit saving before their stops are replaced.

Measured with the same Balanced budget on the saved production mesh:

| Capture | Previous search | Covering search | Previous / covering time | Targets retained |
| --- | ---: | ---: | --- | ---: |
| Current | 326,938 | 276,778 | 3.10 / 2.31 s | 217 / 217 |
| First | 335,454 | 264,289 | 3.10 / 2.62 s | 220 / 220 |
| Rebuild | 330,081 | 294,102 | 3.12 / 3.10 s | 217 / 217 |

Timing and the point reached before a deadline vary with machine load. These results demonstrate shorter complete routes, not global optimality. Six seeds were tested on the current capture; smaller shortlists of six and eight and a larger shortlist of sixteen produced worse routes than twelve. Restricting point projection to the repair loop also consumed the budget without useful improvements, so it remains disabled. The publication-inspired region sweep prototype remains disabled because its measured routes were longer.

Crediting discovery along moving route segments could save additional stops,
but is a separate experiment. It must respect actual reveal latency and the
confirmed fog state before relying on those segments for coverage.


## Corridor selection regression

A later Bird’s Eye capture exposed a problem below the tour optimizer: the midpoint-only navigation graph sent point 1 to point 3 through a western corridor, costing 41,805 units. Searching near both passage endpoints as well reduces that leg to 9,945 units on the same captured mesh. With the corrected walking metric, visiting points 2, 1, 3 is shorter by about 23,000 units; the old metric incorrectly rejected that order. The mesh contains the nearby route, although the perfectly straight chord crosses its edge. This was a corridor selection failure, not evidence that the area was unreachable.

The regression fixture is `sparkfly-swamp-order-213.wcrp.gz`. The test isolates the 1-to-3 leg, requires a connected route below 12,000 units, and checks sampled road segments remain inside the captured polygons. Small tours also receive an exact ordering pass scored with rendered legs before coverage repairs consume the time budget. Reordering existing positions has a smaller stability threshold than replacing the stand positions.
