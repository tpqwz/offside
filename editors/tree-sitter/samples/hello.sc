#include <stdio.h>

struct Point:
    int x
    int y

int main(void):
    struct Point p = {1, 2}
    if (p.x > 0):
        printf("n = %d\n", p.x)
    else:
        printf("zero\n")
    int t = 1 > 0 ?
        2 :
        3
    error:
        return t
