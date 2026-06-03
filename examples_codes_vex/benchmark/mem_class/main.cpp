// mem_class: 1M heap-alloc class.
class Foo { public: int x; Foo(int v) : x(v) {} };
static int helper(int i) {
    Foo *f = new Foo(i);
    int r = f->x;
    delete f;
    return r;
}
int main() {
    long sum = 0; long i = 0;
    while (i < 1000000) { sum += helper(1); i++; }
    return (int)(sum & 0xFFFF);
}
