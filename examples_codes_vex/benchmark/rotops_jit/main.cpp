// rotops_jit: 5M iter, rotl/rotr.
#include <cstdint>
class RotOps {
public:
    uint64_t run_hot(int n) {
        uint64_t acc = 0; int i = 1;
        while (i < n) {
            uint64_t v = (uint64_t)i;
            acc += (v << 7) | (v >> 57);
            acc += (v >> 3) | (v << 61);
            i++;
        }
        return acc;
    }
};
int main() {
    uint64_t r = RotOps().run_hot(5000000);
    return (int)(r & 0xFFFF);
}
