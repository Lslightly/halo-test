# Heap Allocation Layout Optimiser (HALO)

This repository contains the code required to validate the primary results of:

HALO: Post-Link Heap-Layout Optimisation
Joe Savage and Timothy M. Jones
CGO 2020

Please cite this paper if you produce any work that builds upon this code or
its underlying ideas.


## License

All code in this repository is licensed under the BSD 3-clause licence found in
the `LICENSE` file.


## Structure

The top-level directory structure is roughly as follows:

- `halo-prof` contains the source code for our custom profiling tool, written
  using Intel's Pin framework, which generates an affinity graph for a target
  program by analysing its heap access patterns.
    - `utils` contains scripts used for grouping, identification, and to assist
      in applying our optimisations and extracting performance results.
- `bolt.patch` contains the patch for our custom BOLT pass, which rewrites an
  input binary to add a 'group state vector' to the `.data` segment and new
  instructions that set bits in this vector around points of of interest.
- `libhalo` contains the source code for our custom allocator, which uses
  information in the group state vector to co-locate grouped objects at runtime.
- `test` contains a simple test program that can be used to check that the
  entire pipeline is working correctly.

Please note, this work was carried out within the constraints of a nine-month
master's programme. As such, this code base was written on a relatively tight
schedule, and is very much 'research code', secondary to the content of the
paper itself. The core pipeline could really do with a good refactoring, and
currently contains its fair share of bad engineering decisions and short-term
hacks. In addition, the automation scripts we provide to aid in running
experiments are somewhat fragile and handle errors poorly, in no small part
because they were initially designed to be used only internally in a carefully
constructed software environment.

More generally, the design space of our solution has not been fully explored
due to time constraints, and we expect that better solutions than presented in
the paper may be possible (e.g. by tweaking the details of the profiling,
grouping, and allocation processes). Nonetheless, we emphasise that our paper
and this artefact are primarily about the **ideas** behind our technique, and
suggest that further improvements are largely engineering work or are left as
fruitful ground for future research.


## Prerequisites

At current, the only supported software environment is x86-64 Linux (in part,
due to limitations of BOLT at the time of development). Dependencies include
`cmake`, `ninja`, `perf`, `patchelf`, and Python 2.7, which we expect are
installed on the system prior to following the setup instructions below.

To replicate the results presented in the original paper, we recommend testing
on an x86-64 platform with similar cache parameters to the Intel Xeon® W-2195.
Due to a quirk of the current implementation, running programs must be able to
map at least 16GiB of virtual memory, over-committed or otherwise. Please note
that this requirement is somewhat artificial, stemming from the way in which our
prototype operates completely independently from the default allocator, and is
not a fundamental limitation of our technique.


## Setup

As the HALO pipeline involves a number of components developed across separate
code bases, including LLVM, BOLT, and Pin, its setup is not completely trivial.
To make this process as straightforward as possible, a list of commands follows
to establish a working HALO environment almost entirely from scratch.

To get started, simply step through the following commands in turn starting in
the root directory of the artefact (where this README file is located). If you
additionally wish for this environment to persist across sessions, simply append
the expanded form of each `export` command in the series to your login script.

```bash
# Install dependencies and set root environment variables
sudo apt-get install linux-tools-common linux-tools-generic patchelf python
pip install numpy pandas "networkx>=2.0" matplotlib
export LIBHALO_PATH="$(pwd)/libhalo"

# Clone LLVM and BOLT, applying custom patches
mkdir llvm
cd llvm
git init
git remote add origin https://github.com/llvm-mirror/llvm llvm
git fetch origin --depth 1 f137ed238db11440f03083b1c88b7ffc0f4af65e
git checkout -b llvm-bolt f137ed238db11440f03083b1c88b7ffc0f4af65e
cd tools
mkdir llvm-bolt
cd llvm-bolt
git init
git remote add origin https://github.com/facebookincubator/BOLT
git fetch origin --depth 1 e37d18ee516816dc537afe2a0dfbbaf5ae3825fc
git checkout e37d18ee516816dc537afe2a0dfbbaf5ae3825fc
git apply ../../../bolt.patch
cd ..

# Set up and build BOLT, as per the BOLT README, adding the results to your PATH
sudo apt-get install cmake ninja-build
cd ..
patch -p 1 < tools/llvm-bolt/llvm.patch
cd ..
mkdir llvm_build
cd llvm_build
cmake -G Ninja ../llvm -DLLVM_TARGETS_TO_BUILD="X86;AArch64" -DCMAKE_BUILD_TYPE=Release
ninja # here need to fix an error
export PATH="$PATH:$(pwd)/bin"
cd ..

# Download and extract Pin 3.11 and add the core 'pin' binary to your PATH
# NOTE: The original results in the paper were collected using Pin 3.7. These
# have since been reproduced with Pin 3.11, however this is the latest version
# of Pin that has been tested.
# NOTE: In the event that any linked resources succumb to link rot, please
# locate the closest available versions of the relevant resources manually.
wget https://software.intel.com/sites/landingpage/pintool/downloads/pin-3.11-97998-g7ecce2dac-gcc-linux.tar.gz
tar -xvf pin-*.tar.gz && rm pin-*.tar.gz
export PATH="$PATH:$(pwd)/$(echo pin-*)"

# Build the halo-prof Pin tool, setting related environment variables
mv halo-prof pin-*/source/tools
mkdir halo-prof
mount-halo-prof.sh
cd pin-*/source/tools/halo-prof
make
export HALO_PROF_PATH="$(pwd)"
export PATH="$PATH:$(pwd)/utils"
cd ../../../../

# Download and build jemalloc 5.1.0, adding the result to your LD_LIBRARY_PATH
# NOTE: We pass the `--disable-cxx` flag to the configure script to prevent
# jemalloc's custom C++ operators from bypassing HALO's custom allocator.
wget https://github.com/jemalloc/jemalloc/releases/download/5.1.0/jemalloc-5.1.0.tar.bz2
tar -xvf jemalloc-5.1.0.tar.bz2 && rm jemalloc-5.1.0.tar.bz2
cd jemalloc-*
mkdir build
./configure --disable-cxx --prefix="$(pwd)/build"
make
make install
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$(pwd)/build/lib"
cd ..

# Run the pipeline on a small test program to validate the setup
# NOTE: If an error is encountered at this stage, something has gone wrong!
# Unfortunately, some troubleshooting may be required.
cd test
./test.sh
```


## Running Experiments

While we are unable to release the binaries and inputs used in our evaluation
due to licensing issues, users are able to run experiments using their own
binaries and inputs by using the `halo baseline`, `halo run`, and `halo plot`
commands. These can be invoked as follows:

```bash
# Perform baseline run
# NOTE: The script will change directory to `./path/to/` before executing the
# target workload, ensuring that any input files can be easily located. Any
# relative paths in the workload command line must account for this.
halo baseline --jemalloc --trials 10 --pmu-events L1-dcache-load-misses \
              --directory results/output_dir -- ./path/to/binary --with-ref-args

# Perform optimised run (inc. generating optimised binary and allocator)
# NOTE: The script will change directory to `./path/to/` before executing the
# target workload, ensuring that any input files can be easily located. Any
# relative paths in the workload command lines must account for this.
halo run --jemalloc --trials 10 --pmu-events L1-dcache-load-misses \
         --affinity-distance 128 --directory results/output_dir    \
         -- ./path/to/binary --with-train-args                     \
         -- ./path/to/binary --with-ref-args

# Plot results
halo plot --save-as L1D.pdf --metric L1-dcache-load-misses@negimprovement \
          --y-label 'L1D Cache Miss Reduction' --group 2                  \
          --group-label jemalloc --group-label HALO --label workload_name \
          results/output_dir/baseline/results-jemalloc-*                  \
          results/output_dir/affinity-128*/results-jemalloc-*
```

For full list of available parameters, users should examine the source code of
the `halo` script in `$HALO_PROF_PATH/utils` (esp. that of the `main` function).

We also remind users of the limitations of our current prototype surrounding
multi-threaded and position-independent code. All benchmarks examined in our
paper are purely single-threaded, and were compiled with the `-g -O3 -no-pie
-falign-functions=512 -fno-unsafe-math-optimizations -fno-tree-loop-vectorize`
compiler flags.


## Troubleshooting

- The errors produced by the `halo` script tend not to be terribly descriptive.
  In the event that the script raises an exception or terminates prematurely,
  a better error message is likely to be produced by running the last executed
  command (as printed by the script) manually. Keep in mind that some commands,
  such as `pin` and `perf`, are executed by `halo` from the directory of the
  target binary (e.g. the file to be profiled or measured), and not from the
  directory in which the `halo` script was invoked.

- To prevent duplicating work between invocations of `halo`, any existing
  products in the specified output directory may be reused in running the
  pipeline and generating results files. If profiling runs are terminated
  mid-execution, they may generate premature products that will be reused in all
  future processing tasks. In such an event, the relevant subdirectory of the
  specified output directory should be deleted and generated afresh.

- As our identification pass is entirely decoupled from our binary rewriting
  pass, BOLT may occasionally emit an error roughly as follows:

  `BOLT-ERROR: new function size (0x??) is larger than maximum allowed size (0x??) for function function_name`

  This is especially likely to occur when processing small, simple programs, and
  represents the case in which BOLT cannot find any space to add the desired
  instrumentation instructions around the specified call sites. While the
  `-falign-functions` flag we recommend passing to the compiler works to
  circumvent this shortcoming in most cases, it doesn't work 100% of the time.
  This can happen, for instance, when the compiler places an important function
  last in the code segment and thus needs not pad it out to correctly align any
  code that follows it, or when the generated code happens to be perfectly
  aligned without any additional padding.

  To resolve this issue, it is typically sufficient to tweak the value passed to
  `-falign-functions`. In cases where this is not effective, one can manually
  add `nop` instructions that BOLT can overwrite, change the order in which the
  compiler generates functions somehow, or add the problematic call site to an
  `--exclude` flag passed to `halo` to ensure it cannot be used in
  identification. Of these, only the last option is suitable in cases where the
  original source code is not available, and is what we would expect a more
  sophisticated implementation to do automatically if the identification and
  rewriting steps were more tightly integrated.
