#ifndef NMB_MM_MALLOC_H
#define NMB_MM_MALLOC_H
#include <stdlib.h>
#include <malloc.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline void* _mm_malloc(size_t size, size_t alignment) {
    return __mingw_aligned_malloc(size, alignment);
}
static inline void _mm_free(void* ptr) {
    __mingw_aligned_free(ptr);
}
#ifdef __cplusplus
}
#endif
#endif
