#include <atomic>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {
std::atomic<long long> allocation_countdown{-1};
void checkpoint() {
    auto remaining = allocation_countdown.load(std::memory_order_relaxed);
    if (remaining < 0) return;
    if (remaining == 0) {
        allocation_countdown.store(-1, std::memory_order_relaxed);
        throw std::bad_alloc();
    }
    allocation_countdown.store(remaining - 1, std::memory_order_relaxed);
}
void* allocate(std::size_t n) {
    checkpoint();
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* allocate_aligned(std::size_t n, std::size_t alignment) {
    checkpoint();
#if defined(_MSC_VER)
    if (auto* p = _aligned_malloc(n ? n : 1, alignment)) return p;
#else
    if (n > std::numeric_limits<std::size_t>::max() - (alignment - 1)) throw std::bad_alloc();
    const auto rounded = ((n ? n : 1) + alignment - 1) / alignment * alignment;
    if (auto* p = std::aligned_alloc(alignment, rounded)) return p;
#endif
    throw std::bad_alloc();
}
void free_aligned(void* p) noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
}
namespace catalog_test_alloc {
void fail_after(long long count) noexcept { allocation_countdown.store(count, std::memory_order_relaxed); }
void reset() noexcept { allocation_countdown.store(-1, std::memory_order_relaxed); }
}
void* operator new(std::size_t n) { return allocate(n); }
void* operator new[](std::size_t n) { return allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { try { return allocate(n); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { try { return allocate(n); } catch (...) { return nullptr; } }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void* operator new(std::size_t n, std::align_val_t a) { return allocate_aligned(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocate_aligned(n, static_cast<std::size_t>(a)); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return allocate_aligned(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; } }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { try { return allocate_aligned(n, static_cast<std::size_t>(a)); } catch (...) { return nullptr; } }
void operator delete(void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { free_aligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { free_aligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { free_aligned(p); }

void test_catalog();
int main() {
    try {
        test_catalog();
        std::cout << "All catalog lifecycle and cache tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        catalog_test_alloc::reset();
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
