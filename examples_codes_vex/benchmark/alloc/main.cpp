// alloc: 5M heap-alloc trivial.
#include <cstdio>
class Foo { public: int x; Foo() : x(0) {} };
static int helper() { Foo *f = new Foo(); int r = f->x; delete f; return r; }
int main() {
    int i = 0;
    while (i < 5000000) { helper(); i++; }
    return i;
}
