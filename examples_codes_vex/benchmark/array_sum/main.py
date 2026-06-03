"""Bench: suma de array i32 (100K x 200 pasadas).
CPython es lento accediendo a list por indice; usamos array.array para
algo mas justo, pero sigue siendo Python.
"""
import sys
import array

def main():
    arr = array.array('i', range(100000))
    s = 0
    for _ in range(200):
        for v in arr:
            s += v
    return s & 0xFF

if __name__ == "__main__":
    sys.exit(main())
