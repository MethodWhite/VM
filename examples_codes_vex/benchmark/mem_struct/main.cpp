// mem_struct: 1M iter * 3 paths.
#include <memory>
struct Punto { int x; int y; };
static int stack_struct(int base) { Punto p{base, base+1}; return p.x + p.y; }
static int heap_struct(int base) {
    auto p = std::make_unique<Punto>(Punto{base, base+1});
    return p->x + p->y;
}
static int malloc_struct(int base) {
    Punto *p = new Punto{base, base+1};
    int r = p->x + p->y;
    delete p;
    return r;
}
int main() {
    long sum = 0; long i = 0;
    while (i < 1000000) {
        sum += stack_struct(1);
        sum += heap_struct(1);
        sum += malloc_struct(1);
        i++;
    }
    return (int)(sum & 0xFFFF);
}
