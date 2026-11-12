#include <stdio.h>

struct Node:
    int value
    struct Node *next

int sum(struct Node *n):
    int total = 0
    while (n != NULL):
        total += n->value
        n = n->next
    return total

int main(void):
    struct Node c = {3, NULL}
    struct Node b = {2, &c}
    struct Node a = {1, &b}
    printf("%d\n", sum(&a))
    return 0
