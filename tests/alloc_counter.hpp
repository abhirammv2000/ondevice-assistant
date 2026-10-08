// Counts calls to operator new, so a test can state "this path does not allocate" and have it checked.
// The replacement operators are in alloc_counter.cpp. Counting is off except inside a ScopedAllocCount.
#pragma once

#include <atomic>
#include <cstddef>

namespace testutil {

extern std::atomic<std::size_t> g_allocations;
extern std::atomic<bool> g_counting;

class ScopedAllocCount {
public:
    ScopedAllocCount() : before_(g_allocations.load()) { g_counting.store(true); }
    ~ScopedAllocCount() { g_counting.store(false); }
    std::size_t count() const { return g_allocations.load() - before_; }

private:
    std::size_t before_;
};

}  // namespace testutil
