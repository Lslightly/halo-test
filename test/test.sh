#!/bin/bash
set -e
gcc test.c -g -O0 -no-pie -falign-functions=4096 -o test
halo baseline --jemalloc --trials 10 --pmu-events=L1-dcache-load-misses --directory ../results/test_tmp -- ./test
halo run --jemalloc --trials 10 --pmu-events=L1-dcache-load-misses --affinity-distance 128 --directory ../results/test_tmp -- ./test -- ./test
