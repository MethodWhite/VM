/* Bench: hash lookup simulado (FNV-style int ops). */
#include <stdint.h>

int main(void) {
    uint64_t seed = 0xCAFEBABEDEADBEEFULL;
    uint64_t acc = 0;
    volatile int32_t bound = 50000000;
    for (int32_t i = 0; i < bound; ++i) {
        seed ^= (uint64_t) i;
        seed *= 1099511628211ULL;
        seed >>= 7;
        seed |= 1ULL;
        if ((seed & 7ULL) == 0ULL) {
            acc++;
        }
    }
    return (int32_t) acc;
}
