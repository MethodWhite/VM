/* Bench: 5M alloc + free de bloques pequenos (96 bytes). */
#include <stdlib.h>
#include <stdint.h>

static void do_iter(int32_t i) {
    uint8_t *buf = (uint8_t *) malloc(96);
    buf[0]  = (uint8_t) i;
    buf[95] = (uint8_t) (i + 95);
    free(buf);
}
int main(void) {
    volatile int32_t bound = 5000000;
    for (int32_t i = 0; i < bound; ++i) do_iter(i);
    return 42;
}
