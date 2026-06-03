// Bench: aritmetica f64 intensiva. 5M iter x 6 ops FP.
#include <cmath>
#include <cstdint>

int main() {
    double acc = 0.0;
    volatile int32_t bound = 5000000;
    for (int32_t i = 0; i < bound; ++i) {
        double x = static_cast<double>(i);
        double s = std::sqrt(x);
        double a = std::fabs(s - 1000.0);
        double m = std::fmin(a, 999.0);
        double M = std::fmax(s, 1.0);
        double f = std::floor(M);
        double c = std::ceil(f);
        acc += m + c;
    }
    return static_cast<int32_t>(static_cast<int64_t>(acc) & 0xFF);
}
