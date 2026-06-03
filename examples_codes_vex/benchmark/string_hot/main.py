"""Bench: strings en hot loop (concat + length + compare)."""
import sys


def main():
    base = "abc"
    suffix = "xyz"
    hits = 0
    for _ in range(200_000):
        combo = base + suffix
        if len(combo) == 6:
            hits += 1
        if combo == "abcxyz":
            hits += 1
    return hits


if __name__ == "__main__":
    sys.exit(main() & 0xFF)
