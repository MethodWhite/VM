/* Bench: aritmetica f64 intensiva. 5M iter x 6 ops FP. */
#include <math.h>
#include <stdint.h>

int main(void) {
    double acc = 0.0;
    volatile int32_t bound = 5000000;
    for (int32_t i = 0; i < bound; ++i) {
        double x = (double) i;
        double s = sqrt(x);
        double a = fabs(s - 1000.0);
        double m = fmin(a, 999.0);
        double M = fmax(s, 1.0);
        double f = floor(M);
        double c = ceil(f);
        acc += m + c;
    }
    return (int32_t) ((int64_t) acc & 0xFF);
}
