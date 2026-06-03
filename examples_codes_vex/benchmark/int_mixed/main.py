"""Bench: aritmetica entera mixta intensiva (20M iter x 10 ops)."""
import sys

MASK64 = (1 << 64) - 1
SIGN_BIT = 1 << 63


def s64(x):
    """Trunca a int64 con signo (Python tiene ints ilimitados)."""
    x &= MASK64
    return x - (1 << 64) if x & SIGN_BIT else x


def main():
    a, b, c, d = 1, 2, 3, 5
    for i in range(20_000_000):
        a = s64(a + i)
        b = s64(b - 1)
        c = s64(c * 3)
        d = s64(d + (a ^ b))
        a &= 0xFFFFFFFF
        b |= 1
        c >>= 1
        d ^= a
        a = s64(a + b)
        c = s64(c + d)
    return (a + b + c + d) & 0xFF


if __name__ == "__main__":
    sys.exit(main())
