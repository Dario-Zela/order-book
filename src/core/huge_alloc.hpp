#pragma once

// Huge-page-backed allocator (DESIGN §11 stretch). On Linux, allocations of
// 2MB+ are 2MB-aligned and advised MADV_HUGEPAGE, letting THP back the
// arena slabs and the id-map table with 2MB pages — fewer dTLB misses on
// the two biggest hot structures. Elsewhere (macOS dev box) it falls back
// to plain allocation: the interesting measurement (perf stat dTLB-load-
// misses before/after) belongs to the canonical Linux run and is not
// claimed until made there.
//
// Standard-allocator shaped, so std::vector picks it up as a template
// argument with no changes to container logic. The alignment branch is
// deterministic in the request size, so allocate/deallocate always agree
// on which path owns a pointer.

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace ob {

inline constexpr std::size_t kHugePageBytes = 2u << 20;

template <typename T>
struct HugeAlloc {
    using value_type = T;

    HugeAlloc() = default;
    template <typename U>
    HugeAlloc(const HugeAlloc<U>&) noexcept {}

    T* allocate(std::size_t n) {
        const std::size_t bytes = n * sizeof(T);
#if defined(__linux__)
        if (bytes >= kHugePageBytes) {
            void* p = nullptr;
            if (::posix_memalign(&p, kHugePageBytes, bytes) == 0) {
                ::madvise(p, bytes, MADV_HUGEPAGE);  // advisory: failure is fine
                return static_cast<T*>(p);
            }
        }
#endif
        return static_cast<T*>(::operator new(bytes));
    }

    void deallocate(T* p, std::size_t n) noexcept {
        const std::size_t bytes = n * sizeof(T);
#if defined(__linux__)
        if (bytes >= kHugePageBytes) {
            ::free(p);  // posix_memalign pairs with free
            return;
        }
#endif
        ::operator delete(p);
        (void)bytes;
    }

    template <typename U>
    bool operator==(const HugeAlloc<U>&) const noexcept {
        return true;
    }
};

}  // namespace ob
