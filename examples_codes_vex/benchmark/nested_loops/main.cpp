// Bench: nested loops (matrix-like).  500 x 500 x 200 = 50M ops.
#include <cstdint>
int main() {
    int64_t sum = 0;
    volatile int32_t bi = 500, bj = 500, bk = 200;
    for (int32_t i = 0; i < bi; ++i)
        for (int32_t j = 0; j < bj; ++j)
            for (int32_t k = 0; k < bk; ++k)
                sum += static_cast<int64_t>(i + j + k);
    return static_cast<int32_t>(sum & 0xFF);
}
