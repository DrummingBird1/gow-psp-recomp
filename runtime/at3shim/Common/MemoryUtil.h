/* Stand-in for PPSSPP's Common/MemoryUtil.h (only the aligned allocator at3_standalone uses). */
#pragma once
#include <cstdlib>
#ifdef _WIN32
#include <malloc.h>
#endif

inline void *AllocateAlignedMemory(size_t size, size_t alignment) {
#ifdef _WIN32
    return _aligned_malloc(size, alignment);
#else
    void *p = nullptr;
    return posix_memalign(&p, alignment, size) == 0 ? p : nullptr;
#endif
}

inline void FreeAlignedMemory(void *p) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
}
