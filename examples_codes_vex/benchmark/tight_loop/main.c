/* Bench: tight loop aritmetico simple.
 * Workload: 50M iteraciones de suma acumulada.
 */
#include <stdio.h>
#include <stdint.h>

int main(void) {
    int64_t acc = 0;
    /* volatile evita que -O3 reduzca la suma a una formula cerrada. */
    volatile int32_t bound = 50000000;
    for (int32_t i = 0; i < bound; ++i) {
        acc += (int64_t) i;
    }
    return (int32_t) (acc & 0xFFFFFFFF);
}
