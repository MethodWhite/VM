"""Bench: 5M alloc + free de bloques pequenos.

En Python no hay 'free' explicito.  Equivalente idiomatico: crear y
descartar bytearrays cuya refcount cae a 0 (CPython los libera de
inmediato gracias al refcounting).
"""
import sys

def do_iter(i):
    buf = bytearray(96)
    buf[0]  = i & 0xFF
    buf[95] = (i + 95) & 0xFF
    # buf cae fuera de scope al return; refcount -> 0 -> free.

def main():
    for i in range(5_000_000):
        do_iter(i)
    return 42

if __name__ == "__main__":
    sys.exit(main())
