/* ===================================================================== */
// Imports
/* ===================================================================== */

using namespace std::tr1;

/* ===================================================================== */
// Constants
/* ===================================================================== */

#define LONGJMP "__longjmp"

#if defined(TARGET_MAC)
#define MALLOC "_malloc"
#define CALLOC "_calloc"
#define POSIX_MEMALIGN "_posix_memalign"
#define ALIGNED_ALLOC "_aligned_alloc"
#define REALLOC "_realloc"
#define FREE "_free"
#else
#define MALLOC "malloc"
#define CALLOC "calloc"
#define POSIX_MEMALIGN "posix_memalign"
#define ALIGNED_ALLOC "aligned_alloc"
#define REALLOC "realloc"
#define FREE "free"
#endif

namespace ShadowStack {
/* ===================================================================== */
// Command line switches
/* ===================================================================== */

KNOB<INT32> KnobMaxStackDepth(KNOB_MODE_WRITEONCE, "pintool",
    "max-stack-depth", "0", "maximum stack depth");

/* ================================================================== */
// Structures and types
/* ================================================================== */

struct CallSite {
    ADDRINT site;
    RTN rtn;
    bool operator==(const CallSite& rhs) const {
        return (site == rhs.site) && (rtn == rhs.rtn);
    }
    size_t hash(void) const {
        return (site << 32) | RTN_Id(rtn);
    }
};
typedef std::vector<CallSite> Chain;
typedef std::vector<CallSite>::reverse_iterator ChainItr;
}

namespace std {
    template<typename T> struct hash< vector<T> > {
        size_t operator()(vector<T> const &v) const {
            size_t h = 0;
            for (size_t i = 0; i < v.size(); ++i)
                h ^= std::hash<T>()(v[i]) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    template<> struct hash<ShadowStack::CallSite> {
        size_t operator()(ShadowStack::CallSite const &v) const {
            return v.hash();
        }
    };
}


namespace ShadowStack {
/* ================================================================== */
// Global variables
/* ================================================================== */

static UINT64 thread_count = 0;
static UINT64 signal_depth = 0;
static bool entered_main = false;
static ADDRINT last_stub_call_site = 0;
static vector<RTN> ext_traceable_routines;
static Chain chain;

/* ================================================================== */
// Helper functions
/* ================================================================== */

static VOID print(Chain chain, ostream &stream) {
    ChainItr it = chain.rbegin();
    while (it != chain.rend()) {
        stream << "\t" << (RTN_Valid(it->rtn) ? RTN_Name(it->rtn) : "UNKNOWN");
        stream << " from " << std::hex << it->site << "\n";
        ++it;
    }
    stream << flush;
}

#if 0
static VOID print_compact(Chain chain, ostream &stream, int n) {
    ChainItr it = chain.rbegin();
    if (it != chain.rend()) ++it;
    while (n && it != chain.rend()) {
        stream << RTN_Name(it->rtn);
        if (--n && ++it != chain.rend()) stream << ", ";
    }
    stream << flush;
}
#endif

static bool is_ext_traceable_rtn(RTN rtn) {
    for (size_t i = 0; i < ext_traceable_routines.size(); ++i)
        if (rtn == ext_traceable_routines[i])
            return true;
    return false;
}

static int is_stub_rtn(RTN rtn, IMG img) {
    if (!RTN_Valid(rtn) || !IMG_IsMainExecutable(img))
        return 0;

    const char *sec_name = SEC_Name(RTN_Sec(rtn)).c_str();
    const char *rtn_name = RTN_Name(rtn).c_str();
    char *at = strrchr(rtn_name, '@');
    if (!strcmp(sec_name, "__stubs") || (at && !strcmp(at, "@plt")))
        return 1; // User code calls these directly
    else if (!strcmp(sec_name, "__stub_helper") || !strcmp(sec_name, ".plt"))
        return 2; // Resolution functions deeper in the stack
    return 0;
}

// Return the current chain (constrained by KnobMaxStackDepth)
static Chain get_chain() {
    size_t n = KnobMaxStackDepth.Value();
    if (n > 0) {
        Chain result(chain.end() - std::min(chain.size(), n), chain.end());
        return result;
    }
    return chain;
}

// Reduce a chain such that for any duplicate calls, only the most recent copies
// are kept
static Chain reduce_chain(Chain chain) {
    static unordered_map<CallSite, bool> seen;

    ChainItr it = chain.rbegin();
    while (it != chain.rend()) {
        if (seen[*it]) {
            // Laughably, this appears to be the best way to erase an
            // element from a vector using a reverse iterator pre-C++11.
            ChainItr j = it;
            it = ChainItr(chain.erase((++j).base()));
        } else {
            seen[*it] = true;
            ++it;
        }
    }
    seen.clear();
    return chain;
}

/* ===================================================================== */
// Analysis functions
/* ===================================================================== */

static bool should_trace_branch(RTN rtn, ADDRINT target) {
    return RTN_Valid(rtn) && (IMG_IsMainExecutable(IMG_FindByAddress(target)) ||
                              is_ext_traceable_rtn(rtn));
}

static VOID PIN_FAST_ANALYSIS_CALL trace_stub_call(ADDRINT src) {
    last_stub_call_site = src;
}

static VOID PIN_FAST_ANALYSIS_CALL trace_call(ADDRINT src, ADDRINT sp,
                                              RTN rtn)
{
    // If this call is being traced but wasn't in the main executable and
    // doesn't have a corresponding call site, it must have gone through a stub.
    if (!src) {
        src = last_stub_call_site;
        last_stub_call_site = 0;
    }

    // Don't trace repeated calls or calls before 'main'
    if (!ShadowStack::entered_main || ShadowStack::chain.back().rtn == rtn)
        return;

    // Don't trace any routines called within externally traceable routines
    if (is_ext_traceable_rtn(ShadowStack::chain.back().rtn))
        return;

    // Trace the call
    CallSite s = { src, rtn };
    ShadowStack::chain.push_back(s);
}

static VOID PIN_FAST_ANALYSIS_CALL trace_indirect_call(ADDRINT src, ADDRINT sp,
                                                       ADDRINT target)
{
    if (ShadowStack::entered_main) {
        PIN_LockClient();
        RTN rtn = RTN_FindByAddress(target);
        bool traceable = should_trace_branch(rtn, target);
        PIN_UnlockClient();
        if (traceable)
            trace_call(src, sp, rtn);
    }
}

static VOID PIN_FAST_ANALYSIS_CALL trace_return(ADDRINT ip, ADDRINT sp,
                                                ADDRINT ret)
{
    if (ShadowStack::entered_main) {
        PIN_LockClient();
        RTN rtn = RTN_FindByAddress(ret);
        PIN_UnlockClient();
        if (!RTN_Valid(rtn))
            return;

        for (long i = (long)ShadowStack::chain.size() - 1; i >= 0; --i) {
            if (ShadowStack::chain[i].rtn == rtn) {
                ShadowStack::chain.resize(i + 1);
                return;
            }
        }

        // Deal with external tracable routines called from library functions...
        // we probably should have dealt with this by just adding all calls to
        // the shadow stack and then reducing them out, but never mind.
        if (is_ext_traceable_rtn(ShadowStack::chain.back().rtn))
            ShadowStack::chain.pop_back();
    }
}

static VOID trace_signal(THREADID threadIndex,  CONTEXT_CHANGE_REASON reason,
                         const CONTEXT *ctxtFrom, CONTEXT *ctxtTo, INT32 info,
                         VOID *v)
{
    switch(reason) {
    case CONTEXT_CHANGE_REASON_SIGNAL:
        // NOTE: At current, signals don't contribute to the call site chain
        ++signal_depth;
        break;
    case CONTEXT_CHANGE_REASON_SIGRETURN:
        --signal_depth;
        break;
    case CONTEXT_CHANGE_REASON_FATALSIGNAL:
        break;
    case CONTEXT_CHANGE_REASON_APC:
    case CONTEXT_CHANGE_REASON_EXCEPTION:
    case CONTEXT_CHANGE_REASON_CALLBACK:
        // TODO: If we want proper Windows support, need to implement these...
        break;
    }
}


static VOID trace_main(THREADID tid, RTN rtn)
{
    entered_main = true;
    if (chain.empty()) {
        CallSite s = { 0, rtn };
        chain.push_back(s);
    }
}

static VOID trace_thread_start(THREADID threadIndex, CONTEXT *ctxt, INT32 flags,
                               VOID *v)
{
    ++thread_count;
    ASSERT(thread_count == 1, "This PIN tool does not support multi-threading");
}

/* ===================================================================== */
// Instrumentation functions
/* ===================================================================== */

static VOID instrument_image(IMG img, VOID *v) {
    // Instrument the entry point
    RTN rtn = RTN_FindByName(img, "main");
    if (!RTN_Valid(rtn))
        rtn = RTN_FindByName(img, "_main");
    if (RTN_Valid(rtn)) {
        RTN_Open(rtn);
        RTN_InsertCall(rtn, IPOINT_BEFORE, (AFUNPTR)trace_main,
                       IARG_THREAD_ID, IARG_PTR, rtn, IARG_END);
        RTN_Close(rtn);
    }

    // Keep track of external traceable routines
    rtn = RTN_FindByName(img, LONGJMP);
    if (RTN_Valid(rtn)) ext_traceable_routines.push_back(rtn);
    rtn = RTN_FindByName(img, MALLOC);
    if (RTN_Valid(rtn)) ext_traceable_routines.push_back(rtn);
    rtn = RTN_FindByName(img, CALLOC);
    if (RTN_Valid(rtn)) ext_traceable_routines.push_back(rtn);
    rtn = RTN_FindByName(img, POSIX_MEMALIGN);
    if (RTN_Valid(rtn)) ext_traceable_routines.push_back(rtn);
    rtn = RTN_FindByName(img, ALIGNED_ALLOC);
    if (RTN_Valid(rtn)) ext_traceable_routines.push_back(rtn);
    rtn = RTN_FindByName(img, REALLOC);
    if (RTN_Valid(rtn)) ext_traceable_routines.push_back(rtn);
    rtn = RTN_FindByName(img, FREE);
    if (RTN_Valid(rtn)) ext_traceable_routines.push_back(rtn);
}

static VOID instrument_trace(TRACE trace, VOID *v) {
    // Instrument cross-function control flow instructions
    // NOTE: We don't explicitly deal with setjmp/longjmp at the moment, though
    // do add 'longjmp' to chains to identify these cases
    // NOTE: We don't explicitly deal with exceptions at the moment, though they
    // work okay in many situations just out of the box
    for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)) {
        INS tail = BBL_InsTail(bbl);
        ADDRINT site = INS_Address(tail);
        RTN rtn = TRACE_Rtn(trace);
        IMG img = IMG_FindByAddress(RTN_Address(rtn));

        site = (IMG_Valid(img) &&
                IMG_IsMainExecutable(img)) ? (site - IMG_LoadOffset(img)) : 0;
        if (INS_IsRet(tail)) {
            INS_InsertPredicatedCall(tail, IPOINT_BEFORE, (AFUNPTR)trace_return,
                                     IARG_FAST_ANALYSIS_CALL, IARG_REG_VALUE,
                                     IARG_INST_PTR, REG_STACK_PTR,
                                     IARG_BRANCH_TARGET_ADDR, IARG_END);
        } else if (INS_IsDirectBranchOrCall(tail)) {
            ADDRINT target = INS_DirectBranchOrCallTargetAddress(tail);
            RTN target_rtn = RTN_FindByAddress(target);
            int stub_routine_type = is_stub_rtn(target_rtn, img);
            if (stub_routine_type > 0) {
                if (stub_routine_type == 1) {
                    INS_InsertPredicatedCall(tail, IPOINT_BEFORE,
                                             (AFUNPTR)trace_stub_call,
                                             IARG_FAST_ANALYSIS_CALL, IARG_PTR,
                                             site, IARG_END);
                }
            } else if (should_trace_branch(target_rtn, target)) {
                INS_InsertPredicatedCall(tail, IPOINT_BEFORE,
                                         (AFUNPTR)trace_call,
                                         IARG_FAST_ANALYSIS_CALL, IARG_ADDRINT,
                                         site, IARG_REG_VALUE, REG_STACK_PTR,
                                         IARG_PTR, target_rtn, IARG_END);
            }
        } else if (INS_IsIndirectBranchOrCall(tail) && !is_stub_rtn(rtn, img)) {
            INS_InsertPredicatedCall(tail, IPOINT_BEFORE,
                                     (AFUNPTR)trace_indirect_call,
                                     IARG_FAST_ANALYSIS_CALL, IARG_ADDRINT,
                                     site, IARG_REG_VALUE, REG_STACK_PTR,
                                     IARG_BRANCH_TARGET_ADDR, IARG_END);
        }
    }
}

static void initialize(void) {
    IMG_AddInstrumentFunction(instrument_image, 0);
    TRACE_AddInstrumentFunction(instrument_trace, 0);
    PIN_AddContextChangeFunction(trace_signal, 0);
    PIN_AddThreadStartFunction(trace_thread_start, 0);
}
}
