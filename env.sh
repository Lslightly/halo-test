#!/bin/bash
export LIBHALO_PATH="$(pwd)/libhalo"
export PATH=$(pwd)/llvm_build/bin:$PATH
export PATH=$(pwd)/$(echo pin-*):$PATH
cd pin-*/source/tools/halo-prof
export HALO_PROF_PATH="$(pwd)"
export PATH="$(pwd)/utils:$PATH"
cd ../../../../
cd jemalloc-*
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$(pwd)/build/lib"
cd ..
