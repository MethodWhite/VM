// Bench: quicksort Lomuto sobre array i32 de 100K.
#include <cstdint>
#include <vector>
static int32_t partition_(std::vector<int32_t> &arr, int32_t lo, int32_t hi) {
    int32_t pivot = arr[hi];
    int32_t i = lo - 1;
    for (int32_t j = lo; j < hi; ++j) {
        if (arr[j] <= pivot) {
            ++i;
            std::swap(arr[i], arr[j]);
        }
    }
    std::swap(arr[i + 1], arr[hi]);
    return i + 1;
}
static void qsort_rec(std::vector<int32_t> &arr, int32_t lo, int32_t hi) {
    if (lo < hi) {
        int32_t p = partition_(arr, lo, hi);
        qsort_rec(arr, lo, p - 1);
        qsort_rec(arr, p + 1, hi);
    }
}
int main() {
    const int32_t N = 100000;
    std::vector<int32_t> arr(N);
    uint64_t seed = 12345;
    for (int32_t i = 0; i < N; ++i) {
        seed = seed * 6364136223846793005ULL;
        seed = seed + 1442695040888963407ULL;
        arr[i] = static_cast<int32_t>((seed >> 33) & 0xFFFFFFFFULL);
    }
    qsort_rec(arr, 0, N - 1);
    return arr[N / 2] & 0xFF;
}
