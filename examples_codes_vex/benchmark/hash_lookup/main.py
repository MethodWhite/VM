"""Bench: hash lookup simulado (FNV-style int ops)."""
import sys

MASK64 = (1 << 64) - 1


def main():
    seed = 0xCAFEBABE_DEADBEEF
    acc = 0
    for i in range(50_000_000):
        seed = (seed ^ i) & MASK64
        seed = (seed * 1099511628211) & MASK64
        seed >>= 7
        seed |= 1
        if (seed & 7) == 0:
            acc += 1
    return acc & 0xFF


if __name__ == "__main__":
    sys.exit(main())
