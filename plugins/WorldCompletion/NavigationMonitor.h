#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace completion {
struct NavigationState {
    std::vector<uint32_t> walkable_trapezoids;
    std::vector<bool> open_portals;
};

class NavigationMonitor {
    uint32_t last_check = 0;
    bool initialized = false;
    NavigationState previous;

public:
    bool Due(const uint32_t now) const { return !initialized || now - last_check >= 1000; }
    bool Observe(NavigationState state, const uint32_t now)
    {
        bool changed = false;
        if (initialized) {
            for (size_t i = 0; i < state.walkable_trapezoids.size(); ++i)
                changed |= state.walkable_trapezoids[i] > (i < previous.walkable_trapezoids.size() ? previous.walkable_trapezoids[i] : 0);
            for (size_t i = 0; i < state.open_portals.size(); ++i)
                changed |= state.open_portals[i] && !(i < previous.open_portals.size() && previous.open_portals[i]);
        }
        previous = std::move(state);
        initialized = true;
        last_check = now;
        return changed;
    }
    void Reset()
    {
        initialized = false;
        previous = {};
    }
};
}
