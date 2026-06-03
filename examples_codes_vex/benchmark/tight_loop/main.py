"""Bench: tight loop aritmetico simple.
Workload: 50M iteraciones de suma acumulada.
"""
import sys


def main():
    acc = 0
    for i in range(50_000_000):
        acc += i
    return acc & 0xFFFFFFFF


if __name__ == "__main__":
    sys.exit(main() & 0xFF)
