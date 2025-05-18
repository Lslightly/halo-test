#include <iostream>
#include <fstream>
#include <unistd.h>
#include <stdlib.h>
#include <algorithm>
#include <limits.h>
#include <unordered_map>
#include <map>
#include <set>
#include "pin.H"

using namespace std;

/* ===================================================================== */
// Command line switches
/* ===================================================================== */

KNOB<string> KnobLocalityGraphTGFOutput(KNOB_MODE_WRITEONCE, "pintool",
    "tgf-output", "locality.tgf", "specify TGF output filename");
KNOB<INT32> KnobMaxSize(KNOB_MODE_WRITEONCE, "pintool", "max-object-size",
    "4096", "maximum size of co-allocatable objects");

/* ===================================================================== */
// Includes
/* ===================================================================== */

#include "ShadowStack.h"
#include "DynAllocTracer.h"
#include "DynAccessTracer.h"

/* ===================================================================== */
// Helper functions
/* ===================================================================== */

static void write_tgf(vector<ChainPair> &contexts) {
    ofstream LocalityGraph;
    LocalityGraph.open(KnobLocalityGraphTGFOutput.Value().c_str());
    LocalityGraph.setf(ios::showbase);

    // Write nodes
    for (vector<ChainPair>::iterator it = contexts.begin();
         it != contexts.end(); ++it)
    {
        Context c = DynAllocTracer::contexts[it->second];
        if (!c.mark)
            continue;
        LocalityGraph << it->second << " " << c.access_count << "\n";
    }
    LocalityGraph << "#\n";

    // Write edges
    for (size_t i = 0; i < contexts.size(); ++i) {
        UINT32 i_mark = DynAllocTracer::contexts[i].mark;
        for (size_t j = 0; i_mark && j <= i; ++j) {
            UINT32 weight = DynAccessTracer::affinity_graph[i][j];
            UINT32 j_mark = DynAllocTracer::contexts[j].mark;
            if (j_mark && weight)
                LocalityGraph << i << " " << j << " " << weight << "\n";
        }
    }
    LocalityGraph.close();
}

/* ===================================================================== */
// Analysis functions
/* ===================================================================== */

static VOID thread_end(THREADID tid, const CONTEXT *ctxt, INT32 code, VOID *v) {
    cerr << "Finished after executing " << DynAllocTracer::instr_count;
    cerr << " instructions." << endl;
    if (code != 0)
        return;

    // Sort allocation contexts by access frequency
    vector<ChainPair> contexts = DynAllocTracer::chain_pairs();
    sort(contexts.begin(), contexts.end(),
         DynAllocTracer::sort_chain_pairs_by_accesses);

    // Mark popular nodes
    UINT32 accesses = 0;
    UINT64 threshold = (UINT32)(((double)DynAccessTracer::access_count) * 0.9);
    for (vector<ChainPair>::iterator it = contexts.begin();
         it != contexts.end(); ++it)
    {
        Context *c = &DynAllocTracer::contexts[it->second];

        // Stop marking nodes as popular once the access threshold is reached
        c->mark = 1;
        accesses += c->access_count;
        if (accesses >= threshold)
            break;
    }

    write_tgf(contexts);
    cerr << "Generated locality graph accounting for " << accesses << " out of "
         << DynAccessTracer::access_count << " unique object accesses" << endl;
}

/* ===================================================================== */
// Entry point
/* ===================================================================== */

int main(int argc, char *argv[]) {
    // Initialize PIN library. Print help message if -h(elp) is specified
    // in the command line or the command line is invalid 
    PIN_InitSymbols();
    if (PIN_Init(argc,argv)) {
        cerr << KNOB_BASE::StringKnobSummary() << endl;
        return -1;
    }

    // Initialize PIN tool
    cout << showbase;
    ShadowStack::initialize();
    DynAllocTracer::initialize();
    DynAccessTracer::initialize();

    // Set up instrumentation functions and analysis callbacks
    PIN_AddThreadFiniFunction(thread_end, NULL);

    // Start the program, never returns
    PIN_StartProgram();
    
    return 0;
}
