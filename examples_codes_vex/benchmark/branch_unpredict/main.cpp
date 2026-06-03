// Bench: branches no predecibles (xorshift) + 4 branches.
#include <cstdint>

int main() {
    uint64_t rng = 1;
    int64_t a = 0, b = 0, c = 0, d = 0;
    volatile int32_t bound = 10000000;
    for (int32_t i = 0; i < bound; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        if (rng & 1) a++; else a--;
        if (rng & 2) b++; else b--;
        if (rng & 4) c++; else c--;
        if (rng & 8) d++; else d--;
    }
    return static_cast<int32_t>((a + b + c + d) & 0xFF);
}
