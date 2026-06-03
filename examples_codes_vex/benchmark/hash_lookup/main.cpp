// Bench: hash lookup simulado (FNV-style int ops).
#include <cstdint>

int main() {
    uint64_t seed = 0xCAFEBABEDEADBEEFULL;
    uint64_t acc = 0;
    volatile int32_t bound = 50000000;
    for (int32_t i = 0; i < bound; ++i) {
        seed ^= static_cast<uint64_t>(i);
        seed *= 1099511628211ULL;
        seed >>= 7;
        seed |= 1ULL;
        if ((seed & 7ULL) == 0ULL) {
            acc++;
        }
    }
    return static_cast<int32_t>(acc);
}
