/* ================================================================== */
// Structures and types
/* ================================================================== */

typedef UINT32 ObjectId;
typedef UINT32 AllocationContextId;
struct ObjectRecord {
    ObjectId id;
    ADDRINT addr;
};
struct AllocationRecord {
    INT32 size;
    ObjectId id;
    ObjectId predecessor, successor;
    AllocationContextId context;
    UINT32 mark;
};
struct Context {
    ObjectRecord last_object;
    UINT32 access_count;
    UINT32 mark;
};
typedef pair<ShadowStack::Chain, AllocationContextId> ChainPair;
typedef unordered_map<ShadowStack::Chain, AllocationContextId> ChainMap;
typedef ChainMap::iterator ChainMapItr;
typedef unordered_map<AllocationContextId, Context> ContextMap;
typedef ContextMap::iterator ContextMapItr;
typedef map< ADDRINT, AllocationRecord, greater<ADDRINT> > AddrMap;
typedef AddrMap::iterator AddrMapItr;

/* ===================================================================== */
// Constants
/* ===================================================================== */

#define MAX_ALLOC_CALL_SITES 65536ULL

static const char *alloc_funcs[] = { MALLOC, CALLOC, POSIX_MEMALIGN,
                                     ALIGNED_ALLOC, REALLOC, FREE };
static int alloc_funcs_nparams[] = { 1, 2, 3, 2, 2, 1 };

namespace DynAllocTracer {
/* ===================================================================== */
// Command line switches
/* ===================================================================== */

KNOB<string> KnobContextTraceOutput(KNOB_MODE_WRITEONCE, "pintool",
    "contexts-output", "contexts.txt", "specify contexts output filename");

KNOB<string> KnobInstructionLimit(KNOB_MODE_WRITEONCE, "pintool",
    "instruction-limit", "0", "specify dynamic instruction count limit");

/* ================================================================== */
// Global variables
/* ================================================================== */

static AddrMap allocations;
static ContextMap contexts;
static ChainMap chains;
static UINT64 instr_count = 0;
static UINT64 instr_limit = 0;
static ObjectId next_object_id = 1;
static AllocationContextId next_context_id = 0;
static VOID *last_allocation_dest;
static INT32 last_allocation_size;
static ofstream ContextTrace;

/* ===================================================================== */
// Helper functions
/* ===================================================================== */

bool is_alloc_func(const char *str) {
    for (size_t i = 0; i < sizeof(alloc_funcs) / sizeof(alloc_funcs[0]); ++i)
        if (!strcmp(str, alloc_funcs[i]))
            return true;
    return false;
}

vector<ChainPair> chain_pairs(void) {
    vector<ChainPair> pairs;
    pairs.reserve(chains.size());
    for (ChainMapItr it = chains.begin(); it != chains.end(); ++it)
        pairs.push_back(*it);
    return pairs;
}

bool sort_chain_pairs_by_accesses(const ChainPair &a, const ChainPair &b) {
    return (contexts[a.second].access_count > contexts[b.second].access_count);
}

static bool in_bounds(ADDRINT addr, ADDRINT base, INT32 size) {
    return (addr >= base) && (addr < ((ADDRINT)base + size));
}

static AddrMapItr get_allocation_itr(ADDRINT addr) {
    AddrMapItr lower = allocations.lower_bound(addr);
    if (lower == allocations.end() ||
        !in_bounds(addr, lower->first, lower->second.size))
    {
        return allocations.end();
    }
    return lower;
}

static AddrMapItr get_allocation_itr(ObjectRecord obj) {
    AddrMapItr it = get_allocation_itr(obj.addr);
    if (it == allocations.end() || it->second.id != obj.id)
        return allocations.end();
    return it;
}

static bool is_allocated(ADDRINT addr) {
    return get_allocation_itr(addr) == allocations.end();
}

static AllocationContextId update_allocation_context(ADDRINT addr) {
    AllocationContextId context_id;
    ShadowStack::Chain chain = ShadowStack::get_chain();
    ObjectRecord obj = { allocations[addr].id, addr };
    ChainMapItr it = chains.find(chain);

    // If there's no direct match in the chain table, try reducing the chain
    if (it == chains.end()) {
        chain = ShadowStack::reduce_chain(chain);
        it = chains.find(chain);
    }

    // Update the chain, context, and allocation tables
    allocations[addr].successor = allocations[addr].predecessor = 0;
    if (it == chains.end()) {
        Context newContext = {};

        ContextTrace << dec << "CTX " << next_context_id << ":" << endl;
        ShadowStack::print(chain, ContextTrace);
        if (__builtin_expect(next_context_id == MAX_ALLOC_CALL_SITES, 0)) {
            cerr << "ERROR: Exceeded maximum allocation call site limit\n";
            PIN_ExitApplication(1);
        }
        newContext.last_object = obj;
        context_id = next_context_id++;
        contexts[context_id] = newContext;
        chains[chain] = context_id;
    } else {
        context_id = it->second;

        // Update 'predecessor' and 'successor' allocations
        ObjectRecord prev_obj = contexts[context_id].last_object;
        AddrMapItr prev_alloc = get_allocation_itr(prev_obj);
        allocations[addr].predecessor = prev_obj.id;
        if (prev_alloc != allocations.end())
            prev_alloc->second.successor = obj.id;
        contexts[context_id].last_object = obj;
    }
    return context_id;
}

static VOID profile_allocation(ADDRINT addr, INT32 size, BOOL realloc) {
    // Only trace allocations smaller than the maximum size
    if (size > KnobMaxSize.Value()) {
        if (realloc)
            allocations.erase(addr);
        return;
    }

    // Record the allocation
    allocations[addr].size = size;
    if (!realloc)
        allocations[addr].id = next_object_id++;
    allocations[addr].context = update_allocation_context(addr);
    allocations[addr].mark = 0;
}

static VOID profile_free(AddrMapItr it) {
    allocations.erase(it);
}

/* ================================================================== */
// Analysis functions
/* ================================================================== */

VOID PIN_FAST_ANALYSIS_CALL trace_return(CHAR *name, ADDRINT addr) {
    if (__builtin_expect(!strcmp(name, POSIX_MEMALIGN), 0))
        addr = *static_cast<ADDRINT *>(last_allocation_dest);
    if (__builtin_expect((strcmp(name, REALLOC) || is_allocated(addr)) &&
                         ShadowStack::entered_main, 1))
        profile_allocation(addr, last_allocation_size, !strcmp(name, REALLOC));
}

VOID PIN_FAST_ANALYSIS_CALL trace_call1(CHAR *name, ADDRINT ip,
                                        THREADID tid, ADDRINT param1)
{
    if (!strcmp(name, MALLOC)) {
        last_allocation_size = (INT32)param1;
    } else if (!strcmp(name, FREE)) {
        ADDRINT ptr = param1;
        AddrMapItr it = allocations.find(ptr);
        if (!ShadowStack::entered_main || it == allocations.end())
            return;

        // Process the allocation
        profile_free(it);
    }
}

VOID PIN_FAST_ANALYSIS_CALL trace_call2(CHAR *name, ADDRINT ip,
                                        THREADID tid, ADDRINT param1,
                                        ADDRINT param2)
{
    if (!strcmp(name, CALLOC)) {
        // void *calloc(size_t number, size_t size)
        last_allocation_size = (INT32)(param1 * param2);
    } else if (!strcmp(name, ALIGNED_ALLOC)) {
        // void *aligned_alloc(size_t alignment, size_t size)
        last_allocation_size = (INT32)param2;
    } else if (!strcmp(name, REALLOC)) {
        // void *realloc(void *ptr, size_t size)
        last_allocation_size = (INT32)param2;
    }
}

VOID PIN_FAST_ANALYSIS_CALL trace_call3(CHAR *name, ADDRINT ip,
                                        THREADID tid, ADDRINT param1,
                                        ADDRINT param2, ADDRINT param3)
{
    if (!strcmp(name, POSIX_MEMALIGN)) {
        // int posix_memalign(void **ptr, size_t alignment, size_t size)
        last_allocation_dest = (VOID *)param1;
        last_allocation_size = (INT32)param3;
    }
}

VOID PIN_FAST_ANALYSIS_CALL trace_bbl_executed(ADDRINT num_instrs) {
    if (ShadowStack::entered_main)
        instr_count += num_instrs;
    if(__builtin_expect(instr_limit && (instr_count >= instr_limit), 0))
        PIN_ExitApplication(0);
}

/* ===================================================================== */
// Instrumentation functions
/* ===================================================================== */

VOID instrument_trace(TRACE trace, VOID *v) {
    // Track executed instruction on a per-BBL basis, primarily for calculating
    // allocation lifetimes but also for enforcing an optional limit.
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        BBL_InsertCall(bbl, IPOINT_ANYWHERE, (AFUNPTR)trace_bbl_executed,
                       IARG_FAST_ANALYSIS_CALL, IARG_UINT32, BBL_NumIns(bbl),
                       IARG_END);
    }
}

static VOID instrument_image(IMG img, VOID *v) {
    for (size_t i = 0; i < sizeof(alloc_funcs) / sizeof(alloc_funcs[0]); ++i) {
        const char *name = alloc_funcs[i];
        RTN rtn = RTN_FindByName(img, name);

        // Check whether the routine exists
        if (!RTN_Valid(rtn))
            continue;

        // Trace
        RTN_Open(rtn);
        switch (alloc_funcs_nparams[i]) {
          case 1:
            RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)trace_call1,
                           IARG_FAST_ANALYSIS_CALL, IARG_PTR, name,
                           IARG_RETURN_IP, IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0, IARG_END);
            break;
          case 2:
            RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)trace_call2,
                           IARG_FAST_ANALYSIS_CALL, IARG_PTR, name,
                           IARG_RETURN_IP, IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 1, IARG_END);
            break;
          case 3:
            RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)trace_call3,
                           IARG_FAST_ANALYSIS_CALL, IARG_PTR, name,
                           IARG_RETURN_IP, IARG_THREAD_ID,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 1,
                           IARG_FUNCARG_ENTRYPOINT_VALUE, 2, IARG_END);
            break;
        }
        if (strcmp(name, FREE)) {
            RTN_InsertCall(rtn, IPOINT_AFTER, (AFUNPTR)trace_return,
                           IARG_FAST_ANALYSIS_CALL, IARG_PTR, name,
                           IARG_FUNCRET_EXITPOINT_VALUE, IARG_END);
        }
        RTN_Close(rtn);
    }
}

static VOID finalize(INT32 code, VOID *v) {
    ContextTrace.close();
}

static void initialize(void) {
    instr_limit = strtoul(KnobInstructionLimit.Value().c_str(), NULL, 0);
    IMG_AddInstrumentFunction(instrument_image, 0);
    TRACE_AddInstrumentFunction(instrument_trace, 0);
    PIN_AddFiniFunction(finalize, 0);
    ContextTrace.open(KnobContextTraceOutput.Value().c_str());
    ContextTrace.setf(ios::showbase);
}
}
