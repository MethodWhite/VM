// obj_accum: objeto mutable de 4 campos, RMW + 2 escrituras condicionales en
// un loop de 20M.  gcc -O3 hace SROA del struct local -> campos en registros.
#include <stdint.h>
typedef struct { int64_t sum, cnt, mn, mx; } Stats;
int main(void) {
    Stats s; s.sum = 0; s.cnt = 0; s.mn = 2000000000; s.mx = -2000000000;
    int64_t seed = 12345, i = 0;
    while (i < 20000000) {
        seed = (seed * 1103515245 + 12345) & 2147483647;
        int64_t v = seed % 1000;
        s.sum += v;
        s.cnt += 1;
        if (v < s.mn) s.mn = v;
        if (v > s.mx) s.mx = v;
        i++;
    }
    return (int)((s.sum + s.cnt + s.mn + s.mx) % 1000000);
}
