// Bench: suma de array i32 (100K x 200 pasadas).
#include <cstdint>
#include <vector>
int main() {
    std::vector<int32_t> arr(100000);
    for (int32_t i = 0; i < 100000; ++i) arr[i] = i;
    int64_t sum = 0;
    volatile int32_t passes = 200;
    for (int32_t p = 0; p < passes; ++p)
        for (int32_t i = 0; i < 100000; ++i) sum += static_cast<int64_t>(arr[i]);
    return static_cast<int32_t>(sum & 0xFF);
}
