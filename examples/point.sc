#include <stdio.h>

struct Point:
    int x
    int y

int main(void):
    struct Point p = {.x = 1, .y = 2}
    if (p.x > 0):
        printf("%d %d\n", p.x, p.y)
    else:
        printf("empty\n")
    return 0
