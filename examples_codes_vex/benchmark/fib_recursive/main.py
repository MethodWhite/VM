"""Bench: Fibonacci recursivo profundo. fib(32) = 2178309."""
import sys

# CPython tiene un recursion limit default de 1000, suficiente para fib(32).


def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


if __name__ == "__main__":
    r = fib(32)
    sys.exit(r & 0xFF)
