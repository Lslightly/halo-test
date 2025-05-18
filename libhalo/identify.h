#ifndef IDENTIFY_H
#define IDENTIFY_H
#ifndef TEST

#define NUM_GROUPS    1
#define MAX_SIZE   4096

static int get_group_id(size_t size)
{
    if (size > MAX_SIZE)
        return -1;

    panic("error: could not find a valid implementation of 'get_group_id'\n");
    return -1;
}

#endif
#endif
