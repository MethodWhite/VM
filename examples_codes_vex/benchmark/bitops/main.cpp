// Bench: bitops (and/or/xor/shl/shr).
#include <cstdint>
int main() {
    uint64_t a = 0xDEADBEEFCAFEBABEULL;
    uint64_t b = 0x1234567890ABCDEFULL;
    volatile int32_t bound = 30000000;
    for (int32_t i = 0; i < bound; ++i) {
        a ^= static_cast<uint64_t>(i);
        a &= 0xFFFFFFFFFFFFULL;
        a |= 0x1010101010101ULL;
        a <<= 1;
        a >>= 1;
        b += (a & 0xFFFFULL);
        b ^= (a >> 16);
        b <<= 1;
    }
    return static_cast<int32_t>((a + b) & 0xFF);
}
