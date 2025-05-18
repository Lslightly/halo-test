#ifdef TEST
#undef CHUNK_SIZE
#define NUM_GROUPS 3
#define MAX_SIZE 4096
#define CHUNK_SIZE 8192
#define SLAB_SIZE (32ULL * (CHUNK_SIZE))
#define DEFAULT_ALIGNMENT 1
#else
#define SLAB_SIZE (16ULL * 1024ULL * 1024ULL * 1024ULL)
#define DEFAULT_ALIGNMENT 8
#endif

#ifdef STATS
#define PAGE_SIZE 4096
#define uthash_malloc(size)    real_malloc(size)
#define uthash_free(ptr, size) real_free(ptr)
#include "uthash.h"
struct mem_record {
    void *address;
    size_t size;
    UT_hash_handle hh;
};
#endif

// Chunk header layout
struct chunk_header {
    uint64_t group_id;
    uint64_t live_objects;
    struct chunk_header *next_spare;
#ifdef STATS
    uint64_t resident;
    uint64_t pad[4];
#else
    uint64_t pad[5];
#endif
} __attribute__((packed));

// Group-independent global state
static struct {
    // Spare chunk state (used to recycle unused empty chunks)
    int num_spare_chunks;              // Number of chunks available for reuse
    struct chunk_header *spare_chunks; // Linked list of spare chunks

    // Current slab state (used to allocate new chunks)
    unsigned char *slab_ptr; // Points to the next chunk in the current slab
    unsigned char *slab_end; // Points to the first byte beyond the current slab

#ifdef STATS
    // Statistics
    uint64_t live_chunks;
    uint64_t live_bytes;
    uint64_t resident;
    uint64_t peak_resident;
    uint64_t peak_resident_live_bytes;

    // Hash table of allocation records
    struct mem_record *records;
#endif
} globals;

// Per-group state
static struct {
    // Current chunk state (used to allocate objects)
    unsigned char *curr; // Pointer for bump allocation

#ifdef STATS
    // Statistics
    uint64_t resident;
#endif
} groups[NUM_GROUPS];

#define CHUNK_HDR(ptr) (struct chunk_header *)(PREV_ALIGNED(ptr, CHUNK_SIZE))
#define VALID_CHUNK(ptr) ((unsigned char *)ptr >= globals.slab_end - SLAB_SIZE && \
                          (unsigned char *)ptr <  globals.slab_end)

static int is_group_object(void *ptr)
{
    return VALID_CHUNK(ptr);
}

static void allocate_slab(void)
{
    // NOTE: Right now, we only support a single slab in order to support the
    // VALID_CHUNK macro. If we were integrated more tightly with a particular
    // allocator, this wouldn't be necessary. We could simply use the result of
    // the allocator's normal object lookup (already being done either way).
    if (unlikely(globals.slab_ptr != NULL))
        panic("attempted to allocate more than one slab\n");

    // Allocate the slab
    debug("Allocating slab...\n");
    size_t size = SLAB_SIZE + (CHUNK_SIZE - 1);
    unsigned char *base = mmap(NULL, size, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(base != MAP_FAILED);

    // Align the slab
    unsigned char *slab = NEXT_ALIGNED(base, CHUNK_SIZE);
    uintptr_t wastage = slab - base;
    if (wastage) {
        int status = munmap(base, wastage);
        assert(status != -1);
    }

    // Update state
    globals.slab_ptr = slab;
    globals.slab_end = slab + SLAB_SIZE;
    assert(IS_ALIGNED(slab, CHUNK_SIZE));
}

static unsigned char *allocate_chunk(int group)
{
    unsigned char *chunk;
    if (likely(globals.num_spare_chunks > 0)) {
        // Reuse an existing chunk if possible
        struct chunk_header *hdr = globals.spare_chunks;
        debug("Reusing existing chunk\n");
        assert(hdr != NULL);
        chunk = (unsigned char *)hdr;
        globals.spare_chunks = hdr->next_spare;
        globals.num_spare_chunks--;
    } else {
        // Allocate the chunk
        debug("Allocating chunk for group %d...\n", group);
        if (unlikely(globals.slab_ptr == globals.slab_end))
            allocate_slab();
        chunk = globals.slab_ptr;
        assert(chunk < globals.slab_end);
        assert(IS_ALIGNED(chunk, CHUNK_SIZE));

        // Update state
        globals.slab_ptr += CHUNK_SIZE;
#ifdef STATS
        globals.live_chunks++;
#endif
    }

    struct chunk_header *hdr = (struct chunk_header *)chunk;
    hdr->group_id = group;
    groups[group].curr = chunk + sizeof(struct chunk_header);
    return groups[group].curr;
}

#ifdef STATS
__attribute__((destructor))
static void print_stats()
{
    for (int group = 0; group < NUM_GROUPS; ++group)
        log("[halo-stats] group %d resident: %"PRIu64"\n", group,
            groups[group].resident);
    log("[halo-stats] final live_bytes: %"PRIu64"\n", globals.live_bytes);
    log("[halo-stats] final live_chunks: %"PRIu64"\n", globals.live_chunks);
    log("[halo-stats] final resident: %"PRIu64"\n", globals.resident);
    log("[halo-stats] peak resident: %"PRIu64"\n", globals.peak_resident);
    log("[halo-stats] peak resident live_bytes: %"PRIu64"\n",
        globals.peak_resident_live_bytes);
}
#endif
