#include "allocate.h"

static void *group_aligned_alloc(int group, size_t alignment, size_t req_size)
{
    req_size = MAX(req_size, 1);
    unsigned char *address;
    unsigned char *curr = groups[group].curr;
    size_t offset = TO_NEXT_ALIGNED(curr, alignment);
    size_t size = offset + req_size;
    assert(req_size <= MAX_SIZE && size < CHUNK_SIZE);
    if (unlikely(!curr || PREV_ALIGNED(curr + size, CHUNK_SIZE) > curr)) {
        curr = allocate_chunk(group);
        offset = TO_NEXT_ALIGNED(curr, alignment);
        size = offset + req_size;
    }
    address = curr + offset;

    // Update state
    unsigned char *chunk = PREV_ALIGNED(curr, CHUNK_SIZE);
    struct chunk_header *hdr = (struct chunk_header *)chunk;
    hdr->live_objects++;
    groups[group].curr = curr + size;
#ifdef STATS
    struct mem_record *record = real_malloc(sizeof(struct mem_record));
    uint64_t pages_consumed = (uint64_t)NEXT_ALIGNED(groups[group].curr - chunk,
                                                     PAGE_SIZE);
    record->address = address;
    record->size = size;
    HASH_ADD_PTR(globals.records, address, record);
    globals.live_bytes += size;
    globals.resident -= hdr->resident;
    groups[group].resident -= hdr->resident;
    hdr->resident = MAX(hdr->resident, pages_consumed);
    globals.resident += hdr->resident;
    groups[group].resident += hdr->resident;
    if (globals.resident > globals.peak_resident) {
        globals.peak_resident = globals.resident;
        globals.peak_resident_live_bytes = globals.live_bytes;
    } else if (globals.resident == globals.peak_resident) {
        globals.peak_resident_live_bytes = MIN(globals.peak_resident_live_bytes,
                                               globals.live_bytes);
    }
#endif

    // Return object
    assert(IS_ALIGNED(address, alignment));
    debug("[Group %d] Allocated %zu bytes: %p\n", group, req_size, address);
    return address;
}

static void group_free(void *address)
{
    unsigned char *chunk = PREV_ALIGNED(address, CHUNK_SIZE);
    struct chunk_header *hdr = (struct chunk_header *)chunk;

    debug("Freeing %p\n", address);
#ifdef STATS
    struct mem_record *record;
    HASH_FIND_PTR(globals.records, &address, record);
    if (record != NULL) {
        globals.live_bytes -= record->size;
        HASH_DEL(globals.records, record);
        real_free(record);
    }
#endif
    if (unlikely(--hdr->live_objects == 0)) {
        // If the chunk is currently being used, reset its bump pointer
        int group = hdr->group_id;
        assert(group < NUM_GROUPS);
        unsigned char *curr_chunk = PREV_ALIGNED(groups[group].curr,
                                                 CHUNK_SIZE);
        if (chunk == curr_chunk) {
            debug("\tResetting chunk for immediate reuse\n");
            groups[group].curr = chunk + sizeof(struct chunk_header);
            return;
        }

        // Mark the chunk as available for reuse, or otherwise unmap it
        if (globals.num_spare_chunks < MAX_SPARE_CHUNKS || !MAX_SPARE_CHUNKS) {
            debug("\tMarking chunk as available for reuse\n");
            hdr->next_spare = globals.spare_chunks;
            globals.spare_chunks = hdr;
            globals.num_spare_chunks++;
        } else {
            debug("\tReturning chunk\n");
            // NOTE: In the current design where group membership is determined
            // based on slab bounds, we can't afford to free the virtual address
            // space within slabs in case pages gets reused. Instead, we use
            // MADV_FREE to allow the OS to reclaim the physical memory.
            // This is probably bad for TLB behaviour though...
            int status = madvise(hdr, CHUNK_SIZE, MADV_FREE);
            assert(status != -1);
#ifdef STATS
            globals.live_chunks--;
            globals.resident -= hdr->resident;
            groups[group].resident -= hdr->resident;
#endif
        }
    }
}

static void *group_malloc(int group, size_t req_size)
{
    return group_aligned_alloc(group, DEFAULT_ALIGNMENT, req_size);
}

static void *group_calloc(int group, size_t number, size_t req_size)
{
    size_t size = number * req_size;
    unsigned char *ptr =  group_aligned_alloc(group, DEFAULT_ALIGNMENT, size);
    memset(ptr, 0, size);
    return ptr;
}

static int group_posix_memalign(int group, void **ptr, size_t alignment,
                                size_t req_size)
{
    *ptr = group_aligned_alloc(group, alignment, req_size);
    return 0;
}
