#include "allocation_probe.hpp"
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
std::atomic<bool> track_allocations{false};
std::atomic<std::uint64_t> allocations{0};
void record() noexcept {if(track_allocations.load(std::memory_order_relaxed))allocations.fetch_add(1,std::memory_order_relaxed);}
void* allocate(std::size_t n) {record();if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* allocate_aligned(std::size_t n,std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if(auto* p=_aligned_malloc(n?n:1,alignment))return p;
#else
    if(n>std::numeric_limits<std::size_t>::max()-(alignment-1))throw std::bad_alloc();
    const auto rounded=((n?n:1)+alignment-1)/alignment*alignment;
    if(auto* p=std::aligned_alloc(alignment,rounded))return p;
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
void* operator new(std::size_t n){return allocate(n);}
void* operator new[](std::size_t n){return allocate(n);}
void* operator new(std::size_t n,const std::nothrow_t&) noexcept{try{return allocate(n);}catch(...){return nullptr;}}
void* operator new[](std::size_t n,const std::nothrow_t&) noexcept{try{return allocate(n);}catch(...){return nullptr;}}
void operator delete(void* p) noexcept{std::free(p);}
void operator delete[](void* p) noexcept{std::free(p);}
void operator delete(void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept{std::free(p);}
void operator delete(void* p,std::size_t) noexcept{std::free(p);}
void operator delete[](void* p,std::size_t) noexcept{std::free(p);}
void* operator new(std::size_t n,std::align_val_t a){return allocate_aligned(n,static_cast<std::size_t>(a));}
void* operator new[](std::size_t n,std::align_val_t a){return allocate_aligned(n,static_cast<std::size_t>(a));}
void* operator new(std::size_t n,std::align_val_t a,const std::nothrow_t&) noexcept{try{return allocate_aligned(n,static_cast<std::size_t>(a));}catch(...){return nullptr;}}
void* operator new[](std::size_t n,std::align_val_t a,const std::nothrow_t&) noexcept{try{return allocate_aligned(n,static_cast<std::size_t>(a));}catch(...){return nullptr;}}
void operator delete(void* p,std::align_val_t) noexcept{free_aligned(p);}
void operator delete[](void* p,std::align_val_t) noexcept{free_aligned(p);}
void operator delete(void* p,std::align_val_t,const std::nothrow_t&) noexcept{free_aligned(p);}
void operator delete[](void* p,std::align_val_t,const std::nothrow_t&) noexcept{free_aligned(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{free_aligned(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{free_aligned(p);}
namespace allocation_probe {
void start() noexcept{allocations.store(0,std::memory_order_relaxed);track_allocations.store(true,std::memory_order_relaxed);}
std::uint64_t stop() noexcept{track_allocations.store(false,std::memory_order_relaxed);return allocations.load(std::memory_order_relaxed);}
}

void test_ir();
void test_storage();
void test_runtime();
void test_preimage();
void test_encoded_ranges();
void test_vector();
void test_predicate();
void test_adaptive();
void test_decode();
void test_ranges();
void test_wordcode();
void test_frontier();
void test_residual();
void test_boolean_proof();
void test_frontier_runtime();
void test_codegen();
void test_portfolio();
int main() {
    try {
        test_ir();std::cout<<"PASS exact arithmetic, interval certificates, compiler and parser\n";
        test_storage();std::cout<<"PASS storage, progressive prefixes and malformed files\n";
        test_preimage();std::cout<<"PASS backward predicate compilation and budget fallback\n";
        test_encoded_ranges();std::cout<<"PASS direct encoded range execution\n";
        test_runtime();std::cout<<"PASS differential execution, SIMD, halos and allocation discipline\n";
        test_decode();std::cout<<"PASS portable transpose decoding, tails, channels and basis bits\n";
        test_vector();std::cout<<"PASS tiled SIMD execution, liveness slots, aliases and compact channels\n";
        test_predicate();std::cout<<"PASS exact multi-input predicate lowering and encoded/dense masks\n";
        test_adaptive();std::cout<<"PASS prepared adaptive selection, forced paths and resource contracts\n";
        test_ranges();std::cout<<"PASS global row ranges, original halos, unaligned masks and allocation discipline\n";
        test_wordcode();std::cout<<"PASS exact bitplane circuits, liveness, thresholds and ranged execution\n";
        test_frontier();std::cout<<"PASS certified prefix bounds, shared forests and canonical plan validation\n";
        test_boolean_proof();std::cout<<"PASS bounded Boolean proofs, care equivalence and exhaustion\n";
        test_residual();std::cout<<"PASS per-atom carry residuals, suffix-only circuits, liveness and bounds\n";
        test_frontier_runtime();std::cout<<"PASS frontier runtime, exact residuals, ranges and allocation discipline\n";
        test_codegen();std::cout<<"PASS native code emission and generated source contracts\n";
        test_portfolio();std::cout<<"PASS bounded portfolio calibration, selection and resource contracts\n";
        std::cout<<"All Suffice tests passed.\n";return 0;
    } catch(const std::exception& e) {allocation_probe::stop();std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
