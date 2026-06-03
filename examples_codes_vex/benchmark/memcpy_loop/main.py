"""Bench: copia secuencial byte-a-byte (1 MB x 100 iter = 100 MB).

CPython es brutalmente lento en loops sobre bytearray, por eso este
bench es donde Vex JIT puede competir con Python pero NO con C/C++.
Para no tardar horas, reducimos a 10 iter en la version Python.
"""
import sys

N = 1048576


def main():
    src = bytearray(N)
    dst = bytearray(N)
    for i in range(N):
        src[i] = i & 0xFF
    # Reducimos 100 -> 10 iter para que termine en tiempo razonable.
    # El runner debe normalizar: Python = wall * 10 para comparable.
    for _ in range(10):
        for j in range(N):
            dst[j] = src[j]
    return dst[1234]


if __name__ == "__main__":
    sys.exit(main())
