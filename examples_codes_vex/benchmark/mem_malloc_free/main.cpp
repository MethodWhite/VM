// Bench: 5M alloc + free de bloques pequenos (96 bytes).
#include <cstdint>
#include <cstdlib>
static void do_iter(int32_t i) {
    auto *buf = static_cast<uint8_t *>(std::malloc(96));
    buf[0]  = static_cast<uint8_t>(i);
    buf[95] = static_cast<uint8_t>(i + 95);
    std::free(buf);
}
int main() {
    volatile int32_t bound = 5000000;
    for (int32_t i = 0; i < bound; ++i) do_iter(i);
    return 42;
}
