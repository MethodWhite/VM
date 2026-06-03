/* Bench: copia secuencial byte-a-byte (1 MB x 100 iter = 100 MB). */
#include <stdlib.h>
#include <stdint.h>

int main(void) {
    const size_t N = 1048576;
    uint8_t *src = (uint8_t *) malloc(N);
    uint8_t *dst = (uint8_t *) malloc(N);
    for (size_t i = 0; i < N; ++i) {
        src[i] = (uint8_t) (i & 0xFF);
    }
    volatile int32_t bound = 100;
    for (int32_t it = 0; it < bound; ++it) {
        /* loop manual byte-a-byte para comparar con Vex (que no tiene
         * intrinsic memcpy vectorizado).  Sin restrict/builtin para
         * forzar el loop trivial. */
        for (size_t j = 0; j < N; ++j) {
            dst[j] = src[j];
        }
    }
    int32_t r = (int32_t) dst[1234];
    free(src);
    free(dst);
    return r;
}
