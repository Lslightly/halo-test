#ifdef TEST
static int current_group = 0;

static int get_group_id(size_t size)
{
    if (size > MAX_SIZE)
        return -1;
    return current_group % NUM_GROUPS;
}

int main(void)
{
    char *ch = calloc(1, sizeof(char));
    free(ch);

    // Test in-group contiguity
    for (int group = 0; group < NUM_GROUPS; ++group) {
        current_group = group;
        const char *foo = "Hello, world!";
        const char *bar = "Goodbye, cruel world.";
        char *str_foo = calloc(strlen(foo) + 1, sizeof(char));
        current_group = -1;
        char *unrelated = malloc(64);
        current_group = group;
        char *str_bar = calloc(strlen(bar) + 1, sizeof(char));
        strcpy(str_foo, foo);
        strcpy(str_bar, bar);
        assert(!strcmp(str_foo, foo) && !strcmp(str_bar, bar));
        assert(str_foo + strlen(foo) + 1 == str_bar);
        free(str_foo);
        free(str_bar);
        free(unrelated);

        int *numbers[10];
        for (int i = 0; i < 10; ++i) {
            numbers[i] = malloc(sizeof(int));
            *numbers[i] = i + 1;
            if (i > 0) {
                assert((uintptr_t)numbers[i] ==
                       (uintptr_t)numbers[i - 1] + sizeof(int));
            }
        }
        for (int i = 0; i < 10; ++i)
            free(numbers[i]);
    }
    current_group = 0;

    // Test `calloc` zero-initialisation
    unsigned char *zeroes = calloc(128, sizeof(int));
    unsigned char surefire_zeroes[128] = {};
    assert(!memcmp(surefire_zeroes, zeroes, 128));

    // Test `realloc`
    unsigned char *reallocated = realloc(zeroes, 64);
    assert(reallocated);
    free(reallocated);

    // Test `posix_memalign`
    void *aligned_data;
    int ret = posix_memalign(&aligned_data, 64, 1);
    assert(!ret && aligned_data);
    free(aligned_data);

    return 0;
}
#endif
