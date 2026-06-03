// Bench: Fibonacci recursivo profundo. fib(32) = 2178309.
#include <cstdint>

static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    volatile int64_t in = 32;
    int64_t r = fib(in);
    return static_cast<int32_t>(r & 0xFFFFFFFF);
}
