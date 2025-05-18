namespace DynAccessTracer {
/* ===================================================================== */
// Command line switches
/* ===================================================================== */

KNOB<INT32> KnobAffinityDistance(KNOB_MODE_WRITEONCE, "pintool",
    "affinity-distance", "1024", "maximum affinity distance in bytes");

/* ===================================================================== */
// Constants
/* ===================================================================== */

#define AFFINITY_DISTANCE (KnobAffinityDistance.Value())
#define MIN_ACCESS_SIZE      4
#define AFFINITY_QUEUE_MAX_LEN (AFFINITY_DISTANCE / MIN_ACCESS_SIZE)

/* ================================================================== */
// Global variables
/* ================================================================== */

static UINT64 access_count = 0;
static ObjectId last_touched_object = 0;
static ADDRINT last_write_addr;
static INT32 last_write_size;
struct AccessRecord {
    ObjectRecord record;
    INT32 size;
};
static struct {
    UINT64 head;
    AccessRecord *data;
} affinity_queue;
static std::map<ObjectId, std::map<ObjectId, UINT32>> affinity_graph;

/* ================================================================== */
// Helper functions
/* ================================================================== */

static bool is_coallocatable(AddrMapItr a, AddrMapItr b) {
    // Objects cannot be co-allocated with themselves
    if (a->second.id == b->second.id)
        return false;

    // Ensure that 'a' and 'b' are in allocation order
    if (b->second.id < a->second.id) {
        AddrMapItr tmp = a;
        a = b;
        b = tmp;
    }

    // Check co-allocatability
    // TODO: Make sure this works as expected around realloc
    ObjectId a_succ = a->second.successor;
    ObjectId b_pred = b->second.predecessor;
    return (!a_succ || a_succ >= b->second.id) &&
           (!b_pred || b_pred <= a->second.id);

}

static VOID process_affinity(AddrMapItr a, ObjectRecord obj) {
    AddrMapItr b = DynAllocTracer::get_allocation_itr(obj);
    if (b == DynAllocTracer::allocations.end())
        return;

    // Don't count relationships between an object and itself
    if (a->second.id == b->second.id)
        return;

    // Don't double count relationships with the same object
    if (b->second.mark == access_count)
        return;
    b->second.mark = access_count;

    // Process
    if (is_coallocatable(a, b)) {
        ObjectId tmp;
        ObjectId a_ctx = a->second.context;
        ObjectId b_ctx = b->second.context;
        if (b_ctx > a_ctx) { tmp = a_ctx; a_ctx = b_ctx; b_ctx = tmp; }
        affinity_graph[a_ctx][b_ctx]++;
    }
}

static UINT64 affinity_queue_mask_ix(UINT64 ix) {
    return ix & (AFFINITY_QUEUE_MAX_LEN - 1);
}

static VOID affinity_queue_add_access(AddrMapItr src, INT32 size) {
    ObjectRecord obj = { src->second.id, src->first };
    UINT64 ix = affinity_queue_mask_ix(affinity_queue.head++);
    affinity_queue.data[ix].record = obj;
    affinity_queue.data[ix].size = size;

    INT32 total_size = 0;
    for (UINT64 i = ix - 1; i != ix && total_size < AFFINITY_DISTANCE; --i) {
        AccessRecord prev_access;
        i = affinity_queue_mask_ix(i);
        prev_access = affinity_queue.data[i];
        if (!prev_access.record.addr)
            break;
        process_affinity(src, prev_access.record);
        total_size += prev_access.size;
    }
}

/* ===================================================================== */
// Analysis functions
/* ===================================================================== */

// NOTE: Right now we're assuming programs only touch one object per access
VOID PIN_FAST_ANALYSIS_CALL trace_access(CHAR type, ADDRINT ip, ADDRINT addr,
                                         INT32 size, BOOL prefetch)
{
    AddrMapItr it = DynAllocTracer::get_allocation_itr(addr);
    // TODO: It might be worth redefining the affinity distance parameter such
    // that *all* accesses, including non-heap accesses, count against it
    if (!ShadowStack::entered_main || it == DynAllocTracer::allocations.end())
        return;

    // Otherwise, profile this access
    // TODO: It might be worth redefining the affinity distance parameter such
    // that repeated accesses like this count against it regardless
    if (it->second.id != last_touched_object) {
        ++access_count;
        ++DynAllocTracer::contexts[it->second.context].access_count;
        affinity_queue_add_access(it, size);
        last_touched_object = it->second.id;
    }
}

VOID PIN_FAST_ANALYSIS_CALL trace_pre_write(ADDRINT addr, INT32 size) {
    last_write_addr = addr;
    last_write_size = size;
}

VOID PIN_FAST_ANALYSIS_CALL trace_write(ADDRINT ip) {
    trace_access('W', ip, last_write_addr, last_write_size, false);
}

/* ===================================================================== */
// Instrumentation functions
/* ===================================================================== */

static VOID instrument_instruction(INS ins, VOID *v)
{
    // Instrument loads (iff the load will be actually executed)
    if (INS_IsMemoryRead(ins) && INS_IsStandardMemop(ins) &&
        !INS_IsStackRead(ins) && !INS_IsIpRelRead(ins))
    {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)trace_access,
                                 IARG_FAST_ANALYSIS_CALL, IARG_UINT32, 'R',
                                 IARG_INST_PTR, IARG_MEMORYREAD_EA,
                                 IARG_MEMORYREAD_SIZE, IARG_BOOL,
                                 INS_IsPrefetch(ins), IARG_END);
    }

    if (INS_HasMemoryRead2(ins) && INS_IsStandardMemop(ins))
    {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)trace_access,
                                 IARG_FAST_ANALYSIS_CALL, IARG_UINT32, 'R',
                                 IARG_INST_PTR, IARG_MEMORYREAD2_EA,
                                 IARG_MEMORYREAD_SIZE, IARG_BOOL,
                                 INS_IsPrefetch(ins), IARG_END);
    }

    // Instrument stores (iff the store will be actually executed)
    if (INS_IsMemoryWrite(ins) && INS_IsStandardMemop(ins) &&
        !INS_IsStackWrite(ins) && !INS_IsIpRelWrite(ins))
    {
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE,
                                 (AFUNPTR)trace_pre_write,
                                 IARG_FAST_ANALYSIS_CALL, IARG_MEMORYWRITE_EA,
                                 IARG_MEMORYWRITE_SIZE, IARG_END);

        if (INS_HasFallThrough(ins))
        {
            INS_InsertCall(ins, IPOINT_AFTER, (AFUNPTR)trace_write,
                           IARG_FAST_ANALYSIS_CALL, IARG_INST_PTR, IARG_END);
        }
        if (INS_IsBranchOrCall(ins))
        {
            INS_InsertCall(ins, IPOINT_TAKEN_BRANCH, (AFUNPTR)trace_write,
                           IARG_FAST_ANALYSIS_CALL, IARG_INST_PTR, IARG_END);
        }
    }
}

static void initialize(void) {
    if ((AFFINITY_DISTANCE & (AFFINITY_DISTANCE - 1)) != 0) {
        cerr << "ERROR: affinity distance must be a power of two\n";
        PIN_ExitApplication(1);
    }

    affinity_queue.data = (AccessRecord *)calloc(AFFINITY_QUEUE_MAX_LEN,
                                                 sizeof(AccessRecord));
    if (!affinity_queue.data) {
        cerr << "ERROR: Failed to allocate affinity queue\n";
        PIN_ExitApplication(1);
    }

    INS_AddInstrumentFunction(instrument_instruction, 0);
}
}
