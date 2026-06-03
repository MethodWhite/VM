"""Bench: nested loops (matrix-like).  500 x 500 x 200 = 50M ops."""
import sys

def main():
    s = 0
    for i in range(500):
        for j in range(500):
            for k in range(200):
                s += (i + j + k)
    return s & 0xFF

if __name__ == "__main__":
    sys.exit(main())
