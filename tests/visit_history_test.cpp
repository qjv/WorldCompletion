#include <cassert>
#include <iostream>
#include "plugins/WorldCompletion/VisitHistory.h"

int main()
{
    completion::VisitHistory history;
    auto &alice = history.For("Alice", 43, 1024);
    for (uint32_t i = 0; i < 1000; ++i) alice.discovered.insert(i);
    alice.skipped = {2000, 3000};
    auto bytes = history.Encode();
    assert(bytes.size() < 40); // A consecutive run does not take one entry per square.
    completion::VisitHistory loaded;
    assert(loaded.Decode(bytes));
    assert(loaded.For("Alice", 43, 1024).discovered == alice.discovered);
    assert(loaded.For("Alice", 43, 1024).skipped == alice.skipped);
    assert(loaded.For("Bob", 43, 1024).discovered.empty());
    assert(loaded.For("Alice", 44, 1024).discovered.empty());
    history.For("Bob", 43, 1024).discovered.insert(42);
    history.For("Alice", 44, 1024).discovered.insert(UINT32_MAX);
    bytes = history.Encode();
    assert(loaded.Decode(bytes));
    assert(loaded.For("Bob", 43, 1024).discovered.contains(42));
    assert(!loaded.For("Bob", 43, 1024).discovered.contains(43));
    assert(loaded.For("Alice", 44, 1024).discovered.contains(UINT32_MAX));
    for (size_t n = 0; n < bytes.size(); ++n) {
        assert(!loaded.Decode(std::span<const uint8_t>(bytes.data(), n)));
        assert(loaded.For("Bob", 43, 1024).discovered.contains(42));
    }
    bytes.push_back(0);
    assert(!loaded.Decode(bytes));
    assert(loaded.For("Alice", 43, 2048).discovered.empty()); // A changed grid cannot reuse old indices.
    completion::VisitHistory sparse;
    for (uint32_t i = 0; i < 100; ++i) sparse.For("Alice", 43, 1024).discovered.insert(i * 3);
    assert(sparse.Encode().size() < 130);
    assert(loaded.Decode(sparse.Encode()));
    assert(loaded.For("Alice", 43, 1024).discovered == sparse.For("Alice", 43, 1024).discovered);
    auto saving = sparse;
    sparse.For("Alice", 43, 1024).discovered.insert(9000);
    assert(!saving.For("Alice", 43, 1024).discovered.contains(9000));
    assert(sparse.For("Alice", 43, 1024).discovered.contains(9000));
    std::cout << "Visit history: compressed runs/deltas, characters, maps, grid changes, corrupt files passed\n";
}
