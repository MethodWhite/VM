/* Bench: state machine 8 estados (lexer-like). */
#include <stdint.h>

int main(void) {
    int32_t state = 0;
    int64_t counts = 0;
    uint64_t rng = 7;
    volatile int32_t bound = 10000000;
    for (int32_t i = 0; i < bound; ++i) {
        rng = rng * 6364136223846793005ULL;
        rng = rng + 1442695040888963407ULL;
        uint8_t b = (uint8_t) ((rng >> 33) & 0xFF);
        if (state == 0)      { state = (b < 32)  ? 1 : 2; }
        else if (state == 1) { state = (b < 64)  ? 3 : 4; }
        else if (state == 2) { state = (b < 96)  ? 5 : 6; }
        else if (state == 3) { state = 7; }
        else if (state == 4) { state = 7; }
        else if (state == 5) { state = 0; counts++; }
        else if (state == 6) { state = 0; counts++; }
        else                  { state = 0; }
    }
    return (int32_t) (counts & 0xFFFF);
}
