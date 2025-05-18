#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct target {
    char *url;
    char *title;
    int *outbound;
};

struct target create_target(char *url, char *title, int outbound)
{
    struct target result;
    result.url = strdup(url);              // Copy 'url' to a new heap object
    result.title = strdup(title);          // Copy 'title' to a new heap object
    result.outbound = malloc(sizeof(int)); // Allocate a new 'outbound' heap object
    *result.outbound = outbound;
    return result;
}

int my_main(void)
{
    // Create targets
    struct target targets[512];
    for (int i = 0; i < sizeof(targets) / sizeof(struct target); ++i)
        targets[i] = create_target("x.com", "-", i);

    // Print target list
    for (int i = 0; i < sizeof(targets) / sizeof(struct target); ++i)
        printf("%s, %s\n", targets[i].url, targets[i].title);

    // Print average outbound links
    float outbound = 0;
    for (int i = 0; i < sizeof(targets) / sizeof(struct target); ++i)
        outbound += *targets[i].outbound;
    outbound /= sizeof(targets) / sizeof(struct target);
    printf("Average outbound links: %f\n", outbound);

    return 0;
}

int main(void)
{
    return my_main();
}
