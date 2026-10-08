#include "alloc_counter.hpp"

#include <cstdlib>
#include <new>

namespace testutil {
std::atomic<std::size_t> g_allocations{0};
std::atomic<bool> g_counting{false};
}  // namespace testutil

#if !defined(_WIN32)

namespace {
void* counted_alloc(std::size_t size) {
    if (testutil::g_counting.load(std::memory_order_relaxed)) testutil::g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(size == 0 ? 1 : size);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

void* counted_aligned_alloc(std::size_t size, std::size_t alignment) {
    if (testutil::g_counting.load(std::memory_order_relaxed)) testutil::g_allocations.fetch_add(1, std::memory_order_relaxed);
    void* p = nullptr;
    if (posix_memalign(&p, alignment < sizeof(void*) ? sizeof(void*) : alignment, size == 0 ? 1 : size) != 0) throw std::bad_alloc();
    return p;
}
}  // namespace

void* operator new(std::size_t size) { return counted_alloc(size); }
void* operator new[](std::size_t size) { return counted_alloc(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return counted_alloc(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return counted_alloc(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t al) { return counted_aligned_alloc(size, static_cast<std::size_t>(al)); }
void* operator new[](std::size_t size, std::align_val_t al) { return counted_aligned_alloc(size, static_cast<std::size_t>(al)); }

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

#endif
