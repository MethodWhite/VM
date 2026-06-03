"""Bench: quicksort Lomuto sobre array i32 de 100K.
CPython tiene recursion limit 1000.  qsort de 100K puede llegar a
~17 niveles en el caso balanceado (log2 100K ≈ 17).  Aumentamos por
si acaso.
"""
import sys
sys.setrecursionlimit(50000)

M64 = (1 << 64) - 1

def partition(arr, lo, hi):
    pivot = arr[hi]
    i = lo - 1
    for j in range(lo, hi):
        if arr[j] <= pivot:
            i += 1
            arr[i], arr[j] = arr[j], arr[i]
    arr[i + 1], arr[hi] = arr[hi], arr[i + 1]
    return i + 1

def qsort_rec(arr, lo, hi):
    if lo < hi:
        p = partition(arr, lo, hi)
        qsort_rec(arr, lo, p - 1)
        qsort_rec(arr, p + 1, hi)

def main():
    N = 100000
    arr = [0] * N
    seed = 12345
    for i in range(N):
        seed = (seed * 6364136223846793005) & M64
        seed = (seed + 1442695040888963407) & M64
        # Truncate to int32 signed range.
        v = (seed >> 33) & 0xFFFFFFFF
        if v >= 0x80000000:
            v -= 0x100000000
        arr[i] = v
    qsort_rec(arr, 0, N - 1)
    return arr[N // 2] & 0xFF

if __name__ == "__main__":
    sys.exit(main())
