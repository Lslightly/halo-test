#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <inttypes.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <elf.h>

#include "helpers.h"
#include IDENTIFY_HEADER
#include "allocate.c"
#include "test.c"

//
// This library wraps malloc, calloc, posix_memalign, aligned_alloc, realloc,
// and free to provide pool allocations to objects within HALO groups.
//
// NOTE: Although it's likely to be beneficial to runtime performance, sadly we
// can't use '__attribute__((constructor))' to set the 'real' function pointers
// as our constructor may be called before that of the target malloc library.
//

void *malloc(size_t size)
{
    int group_id = get_group_id(size);
    return group_id > -1 ? group_malloc(group_id, size) : real_malloc(size);
}

void *calloc(size_t number, size_t size)
{
    int group_id;
    volatile static int resolving = 0;
    static void *(*real_calloc)(size_t, size_t) = NULL;
    #if 1
    if (unlikely(resolving)) {
        // NOTE: We allow the very first callocs to be fulfilled from a small
        // scratch buffer as our libdl implementation calls calloc from 'dlsym'.
        return bootstrap_calloc(number, size);
    } else
    #endif
    if (unlikely(!real_calloc)) {
        resolving = 1;
        real_calloc = dlsym(RTLD_NEXT, "calloc");
        resolving = 0;
    }

    group_id = get_group_id(number * size);
    return group_id > -1 ? group_calloc(group_id, number, size)
                         : real_calloc(number, size);
}

int posix_memalign(void **ptr, size_t alignment, size_t size)
{
    int group_id = get_group_id(size);
    static int (*real_posix_memalign)(void **, size_t, size_t) = NULL;
    if (unlikely(!real_posix_memalign))
        real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    return group_id > -1 ? group_posix_memalign(group_id, ptr, alignment, size)
                         : real_posix_memalign(ptr, alignment, size);
}

void *aligned_alloc(size_t alignment, size_t size)
{
    int group_id = get_group_id(size);
    static void *(*real_aligned_alloc)(size_t, size_t) = NULL;
    if (unlikely(!real_aligned_alloc))
        real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    return group_id > -1 ? group_aligned_alloc(group_id, alignment, size)
                         : real_aligned_alloc(alignment, size);
}

void *realloc(void *ptr, size_t size)
{
    static void *(*real_realloc)(void *, size_t) = NULL;
    if (unlikely(!real_realloc))
        real_realloc = dlsym(RTLD_NEXT, "realloc");

    if (is_group_object(ptr)) {
        // NOTE: We currently assume that copying garbage after 'ptr' is always
        // safe. Since we only use a single slab at the moment, we constrain the
        // copy to the slab size such that it always will be.
        void *object = malloc(size);
        intptr_t dist_to_slab_end = globals.slab_end - (unsigned char *)ptr;
        size_t num = dist_to_slab_end > 0 ? MIN(dist_to_slab_end, size)
                                          : size;
        if (unlikely(ptr == NULL || object == NULL))
            return object;
        memcpy(object, ptr, num);
        group_free(ptr);
        return object;
    }
    return real_realloc(ptr, size);
}

void free(void *ptr)
{
    if (unlikely(ptr == NULL))
        return;
    return is_group_object(ptr) ? group_free(ptr) : real_free(ptr);
}
