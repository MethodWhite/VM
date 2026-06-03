// intops_jit: 5M iter, imin/imax/abs.
#include <algorithm>
#include <cstdlib>
class IntOps {
public:
    long run_hot(int n) {
        long acc = 0; int i = 1;
        while (i < n) {
            long a = (long)i;
            long b = (long)(i + 7);
            acc += std::min(a, b);
            acc += std::max(a, b);
            acc += std::abs(a - 5000);
            i++;
        }
        return acc;
    }
};
int main() {
    IntOps op;
    long r = op.run_hot(5000000);
    return (int)(r & 0xFFFF);
}
