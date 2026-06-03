// Bench: copia secuencial byte-a-byte (1 MB x 100 iter = 100 MB).
#include <cstdint>
#include <cstdlib>

int main() {
    constexpr size_t N = 1048576;
    auto *src = static_cast<uint8_t *>(std::malloc(N));
    auto *dst = static_cast<uint8_t *>(std::malloc(N));
    for (size_t i = 0; i < N; ++i) {
        src[i] = static_cast<uint8_t>(i & 0xFF);
    }
    volatile int32_t bound = 100;
    for (int32_t it = 0; it < bound; ++it) {
        for (size_t j = 0; j < N; ++j) {
            dst[j] = src[j];
        }
    }
    int32_t r = static_cast<int32_t>(dst[1234]);
    std::free(src);
    std::free(dst);
    return r;
}
