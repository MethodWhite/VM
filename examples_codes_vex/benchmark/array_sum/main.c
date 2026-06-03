/* Bench: suma de array i32 (100K x 200 pasadas). */
#include <stdlib.h>
#include <stdint.h>
int main(void) {
    int32_t *arr = (int32_t *) malloc(100000 * sizeof(int32_t));
    for (int32_t i = 0; i < 100000; ++i) arr[i] = i;
    int64_t sum = 0;
    volatile int32_t passes = 200;
    for (int32_t p = 0; p < passes; ++p)
        for (int32_t i = 0; i < 100000; ++i) sum += (int64_t) arr[i];
    free(arr);
    return (int32_t)(sum & 0xFF);
}
