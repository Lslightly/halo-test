#define likely(x)   __builtin_expect((x), 1)
#define unlikely(x) __builtin_expect((x), 0)

#define log(...) fprintf(stderr, __VA_ARGS__)
#define panic(...) do { fprintf(stderr, __VA_ARGS__); exit(1); } while (0)
#ifdef NDEBUG
#define debug(...)
#define assert(x)
#else
#define debug(...) log(__VA_ARGS__)
#define assert(x)  do { if (!(x)) panic("%s:%d: Assertion `%s` failed.\n", __FILE__, __LINE__, #x); } while (0)
#endif

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define HIGH_BIT(type) (1 << (sizeof(type) * CHAR_BIT) - 1)
#define NEXT_ALIGNED(x, align) (unsigned char *)(((uintptr_t)(x) + ((align) - 1)) & ~(uintptr_t)((align) - 1))
#define PREV_ALIGNED(x, align) (unsigned char *)((uintptr_t)(x) & ~(uintptr_t)((align) - 1))
#define TO_NEXT_ALIGNED(x, align) (NEXT_ALIGNED(x, align) - (x))
#define IS_ALIGNED(x, align) (((uintptr_t)(x) & ((align) - 1)) == 0)

#define BOOTSTRAP_ALIGNMENT 16

static void *real_malloc(size_t size)
{
    static void *(*next_malloc)(size_t) = NULL;
    if (unlikely(!next_malloc))
        next_malloc = dlsym(RTLD_NEXT, "malloc");
    return next_malloc(size);
}

static void real_free(void *ptr)
{
    static void (*next_free)(void *) = NULL;
    if (unlikely(!next_free))
        next_free = dlsym(RTLD_NEXT, "free");
    return next_free(ptr);
}

static void *bootstrap_calloc(size_t number, size_t req_size)
{
    static unsigned char scratch[64];
    static unsigned char *ptr = scratch;
    unsigned char *address;
    size_t offset = TO_NEXT_ALIGNED(ptr, BOOTSTRAP_ALIGNMENT);
    size_t size = offset + (number * req_size);
    if (ptr + size > ptr + sizeof(scratch))
        return NULL;
    address = ptr + offset;
    ptr = ptr + size;
    return ptr;
}

static uint64_t *get_group_state(void)
{
    // Get the full path of the current binary
    char path[512] = {};
    if (readlink("/proc/self/exe", path, 511) < 0)
        panic("failed to read binary path\n");

    // Open the file
    uint8_t *bin;
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        panic("failed to open file\n");
    if (fstat(fd, &st) < 0)
        panic("failed to fstat file\n");
    bin = (uint8_t *)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (bin == MAP_FAILED)
        panic("failed to mmap file\n");

    // Parse ELF header
    Elf64_Ehdr *hdr = (Elf64_Ehdr *)bin;
    if (hdr->e_ident[EI_CLASS] != ELFCLASS64)
        panic("expected ELFCLASS64\n");

    // Parse sections
    uint64_t *halo_state_addr = 0;
    Elf64_Half num_sections = hdr->e_shnum;
    Elf64_Shdr *sections = (Elf64_Shdr *)(bin + hdr->e_shoff);
    Elf64_Shdr *strtab = sections + hdr->e_shstrndx;
    char *section_names = (char *)(bin + strtab->sh_offset);
    for (int i = 0; i < num_sections; ++i) {
        char *name = &section_names[sections[i].sh_name];
        if (!strcmp(name, ".data.halo_state")) {
            halo_state_addr = (uint64_t *)sections[i].sh_addr;
            break;
        }
    }
    if (!halo_state_addr)
        panic("failed to find .data.halo_state in: %s\n", path);

    return halo_state_addr;
}
