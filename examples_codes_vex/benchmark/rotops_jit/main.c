// rotops_jit: 5M iter, rotl/rotr.
#include <stdint.h>
static uint64_t rotl_u64(uint64_t v, int n) { return (v << n) | (v >> (64 - n)); }
static uint64_t rotr_u64(uint64_t v, int n) { return (v >> n) | (v << (64 - n)); }
int main(void) {
    uint64_t acc = 0; int i = 1;
    while (i < 5000000) {
        uint64_t v = (uint64_t)i;
        acc += rotl_u64(v, 7);
        acc += rotr_u64(v, 3);
        i++;
    }
    return (int)(acc & 0xFFFF);
}
