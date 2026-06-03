"""Bench: state machine 8 estados (lexer-like)."""
import sys

MASK64 = (1 << 64) - 1


def main():
    state = 0
    counts = 0
    rng = 7
    for _ in range(10_000_000):
        rng = (rng * 6364136223846793005) & MASK64
        rng = (rng + 1442695040888963407) & MASK64
        b = (rng >> 33) & 0xFF
        if state == 0:
            state = 1 if b < 32 else 2
        elif state == 1:
            state = 3 if b < 64 else 4
        elif state == 2:
            state = 5 if b < 96 else 6
        elif state == 3:
            state = 7
        elif state == 4:
            state = 7
        elif state == 5:
            state = 0
            counts += 1
        elif state == 6:
            state = 0
            counts += 1
        else:
            state = 0
    return counts & 0xFFFF


if __name__ == "__main__":
    sys.exit(main() & 0xFF)
