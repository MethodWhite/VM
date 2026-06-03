// intops_jit: 5M iter, imin/imax/abs.
#include <stdlib.h>
static long abs_l(long x) { return x < 0 ? -x : x; }
static long min_l(long a, long b) { return a < b ? a : b; }
static long max_l(long a, long b) { return a > b ? a : b; }
int main(void) {
    long acc = 0; int i = 1;
    while (i < 5000000) {
        long a = (long)i;
        long b = (long)(i + 7);
        acc += min_l(a, b);
        acc += max_l(a, b);
        acc += abs_l(a - 5000);
        i++;
    }
    return (int)(acc & 0xFFFF);
}
