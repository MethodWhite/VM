// callvirt_hot: 10M virtual-call hot with state.
typedef struct C C;
struct C { int (*inc)(C*, int); int n; };
static int inc_impl(C *c, int d) { c->n += d; return c->n; }
int main(void) {
    C c; c.n = 0; c.inc = inc_impl;
    int sum = 0; int i = 0;
    while (i < 10000000) { sum = c.inc(&c, 1); i++; }
    return sum;
}
