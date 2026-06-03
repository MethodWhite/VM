"""Bench: aritmetica f64 intensiva. 5M iter x 6 ops FP."""
import math
import sys


def main():
    acc = 0.0
    for i in range(5_000_000):
        x = float(i)
        s = math.sqrt(x)
        a = abs(s - 1000.0)
        m = min(a, 999.0)
        M = max(s, 1.0)
        f = math.floor(M)
        c = math.ceil(f)
        acc += m + c
    return int(acc) & 0xFF


if __name__ == "__main__":
    sys.exit(main())
