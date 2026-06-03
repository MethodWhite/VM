// callvirt: 30M virtual-call trivial via fn-ptr.
typedef struct Counter Counter;
struct Counter { int (*inc)(Counter*); int value; };
static int inc_impl(Counter *c) { (void)c; return 1; }
int main(void) {
    Counter c; c.value = 0; c.inc = inc_impl;
    int sum = 0;
    int i = 0;
    while (i < 30000000) { sum += c.inc(&c); i++; }
    return sum;
}
