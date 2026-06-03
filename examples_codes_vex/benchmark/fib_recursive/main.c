/* Bench: Fibonacci recursivo profundo. fib(32) = 2178309. */
#include <stdio.h>
#include <stdint.h>

static int64_t fib(int64_t n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void) {
    /* volatile evita que -O3 const-folde fib(32) a la constante 2178309. */
    volatile int64_t in = 32;
    int64_t r = fib(in);
    return (int32_t) (r & 0xFFFFFFFF);
}
