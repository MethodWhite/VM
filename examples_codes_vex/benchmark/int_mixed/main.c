/* Bench: aritmetica entera mixta intensiva (20M iter x 10 ops). */
#include <stdint.h>

int main(void) {
    int64_t a = 1, b = 2, c = 3, d = 5;
    volatile int32_t bound = 20000000;
    for (int32_t i = 0; i < bound; ++i) {
        a += (int64_t) i;
        b -= 1;
        c *= 3;
        d += (a ^ b);
        a &= 0xFFFFFFFF;
        b |= 1;
        c >>= 1;
        d ^= a;
        a += b;
        c += d;
    }
    return (int32_t) ((a + b + c + d) & 0xFF);
}
