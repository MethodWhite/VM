// alloc: 5M heap-alloc trivial.
#include <stdio.h>
#include <stdlib.h>
typedef struct { int x; } Foo;
static int helper(void) {
    Foo *f = (Foo*)malloc(sizeof(Foo));
    f->x = 0;
    int r = f->x;
    free(f);
    return r;
}
int main(void) {
    int i = 0;
    while (i < 5000000) { helper(); i++; }
    return i;
}
