"""Bench: branches no predecibles (xorshift) + 4 branches."""
import sys
MASK64 = (1 << 64) - 1

def main():
    rng = 1
    a = b = c = d = 0
    for _ in range(10_000_000):
        rng = (rng ^ ((rng << 13) & MASK64)) & MASK64
        rng = rng ^ (rng >> 7)
        rng = (rng ^ ((rng << 17) & MASK64)) & MASK64
        if rng & 1: a += 1
        else:        a -= 1
        if rng & 2: b += 1
        else:        b -= 1
        if rng & 4: c += 1
        else:        c -= 1
        if rng & 8: d += 1
        else:        d -= 1
    return (a + b + c + d) & 0xFF

if __name__ == "__main__":
    sys.exit(main())
