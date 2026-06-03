// Bench: tight loop aritmetico simple.
// Workload: 50M iteraciones de suma acumulada.
#include <cstdint>
#include <cstdio>

int main() {
    int64_t acc = 0;
    // volatile evita que -O3 reduzca la suma a una formula cerrada.
    volatile int32_t bound = 50000000;
    for (int32_t i = 0; i < bound; ++i) {
        acc += static_cast<int64_t>(i);
    }
    return static_cast<int32_t>(acc & 0xFFFFFFFF);
}
