// mem_struct: 1M iter * 3 paths (stack, heap, malloc).
#include <stdlib.h>
typedef struct { int x; int y; } Punto;
static int stack_struct(int base) {
    Punto p; p.x = base; p.y = base + 1;
    return p.x + p.y;
}
static int heap_struct(int base) {
    Punto *p = (Punto*)malloc(sizeof(Punto));
    p->x = base; p->y = base + 1;
    int r = p->x + p->y;
    free(p);
    return r;
}
static int malloc_struct(int base) {
    Punto *p = (Punto*)malloc(8);
    p->x = base; p->y = base + 1;
    int r = p->x + p->y;
    free(p);
    return r;
}
int main(void) {
    long sum = 0; long i = 0;
    while (i < 1000000) {
        sum += stack_struct(1);
        sum += heap_struct(1);
        sum += malloc_struct(1);
        i++;
    }
    return (int)(sum & 0xFFFF);
}
