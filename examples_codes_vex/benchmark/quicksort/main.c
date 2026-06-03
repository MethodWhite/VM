/* Bench: quicksort Lomuto sobre array i32 de 100K. */
#include <stdlib.h>
#include <stdint.h>

static int32_t partition_(int32_t *arr, int32_t lo, int32_t hi) {
    int32_t pivot = arr[hi];
    int32_t i = lo - 1;
    for (int32_t j = lo; j < hi; ++j) {
        if (arr[j] <= pivot) {
            ++i;
            int32_t t = arr[i]; arr[i] = arr[j]; arr[j] = t;
        }
    }
    int32_t t = arr[i + 1]; arr[i + 1] = arr[hi]; arr[hi] = t;
    return i + 1;
}
static void qsort_rec(int32_t *arr, int32_t lo, int32_t hi) {
    if (lo < hi) {
        int32_t p = partition_(arr, lo, hi);
        qsort_rec(arr, lo, p - 1);
        qsort_rec(arr, p + 1, hi);
    }
}
int main(void) {
    const int32_t N = 100000;
    int32_t *arr = (int32_t *) malloc(N * sizeof(int32_t));
    uint64_t seed = 12345;
    for (int32_t i = 0; i < N; ++i) {
        seed = seed * 6364136223846793005ULL;
        seed = seed + 1442695040888963407ULL;
        arr[i] = (int32_t)((seed >> 33) & 0xFFFFFFFFULL);
    }
    qsort_rec(arr, 0, N - 1);
    int32_t r = arr[N / 2] & 0xFF;
    free(arr);
    return r;
}
