IDENTIFY_HEADER ?= identify.h
CHUNK_SIZE ?= 1048576
MAX_SPARE_CHUNKS ?= 1
OUTPUT ?= libhalo.so
SHARED_FLAGS = -g -DIDENTIFY_HEADER='"$(IDENTIFY_HEADER)"' -DCHUNK_SIZE=$(CHUNK_SIZE) -DMAX_SPARE_CHUNKS=$(MAX_SPARE_CHUNKS)
SOURCE_FILES = $(wildcard *.c)
HEADER_FILES = $(wildcard *.h) $(IDENTIFY_HEADER)

libhalo: $(SOURCE_FILES) $(HEADER_FILES)
	gcc -shared -fPIC -O3 -DNDEBUG $(SHARED_FLAGS) libhalo.c -o $(OUTPUT) -ldl

stats: $(SOURCE_FILES) $(HEADER_FILES)
	gcc -shared -fPIC -O3 -DNDEBUG -DSTATS $(SHARED_FLAGS) libhalo.c -o $(OUTPUT) -ldl

debug: $(SOURCE_FILES) $(HEADER_FILES)
	gcc -shared -fPIC -DSTATS $(SHARED_FLAGS) libhalo.c -o $(OUTPUT) -ldl

test: $(SOURCE_FILES) $(HEADER_FILES)
	gcc -DTEST -DSTATS $(SHARED_FLAGS) libhalo.c -o test -ldl
