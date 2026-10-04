#include "plugins/WorldCompletion/NavigationMonitor.h"
#include <cassert>
#include <iostream>
int main()
{
    completion::NavigationMonitor monitor;
    assert(monitor.Due(0));
    assert(!monitor.Observe({{100, 0}, {true, false}}, 0));
    assert(!monitor.Due(999));
    assert(monitor.Due(1000));
    assert(!monitor.Observe({{100, 0}, {true, false}}, 1000));
    assert(monitor.Observe({{100, 20}, {true, false}}, 2000));
    assert(!monitor.Observe({{100, 20}, {true, false}}, 3000));
    assert(!monitor.Observe({{100, 0}, {false, false}}, 4000)); // Closures do not rebuild.
    assert(monitor.Observe({{100, 20}, {true, false}}, 5000));
    assert(monitor.Observe({{100, 20, 10}, {true, false}}, 6000));
    assert(monitor.Observe({{100, 20, 10}, {true, true}}, 7000));
    monitor.Reset();
    assert(!monitor.Observe({{9}, {}}, UINT32_MAX - 499));
    assert(!monitor.Due(499));
    assert(monitor.Due(500));
    std::cout << "Navigation monitor: expansion only, stable state, closures, timing, reset passed\n";
}
