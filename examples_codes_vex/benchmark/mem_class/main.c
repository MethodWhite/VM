// mem_class: 1M heap-alloc class equiv (via malloc).
#include <stdlib.h>
typedef struct { int x; } Foo;
static int helper(int i) {
    Foo *f = (Foo*)malloc(sizeof(Foo));
    f->x = i;
    int r = f->x;
    free(f);
    return r;
}
int main(void) {
    long sum = 0; long i = 0;
    while (i < 1000000) { sum += helper(1); i++; }
    return (int)(sum & 0xFFFF);
}
