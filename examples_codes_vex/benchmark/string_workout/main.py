#!/usr/bin/env python3
"""Bench: string workout AGRESIVO (Python version).

Equivalente a las otras impl: 100K iter, 6 ops por iter.
Python strings son inmutables, asi cada operacion crea un nuevo objeto.
"""
import sys

def main():
    sum_v = 0
    hits1 = 0
    hits2 = 0
    bound = 1000000
    pat1 = "AAAA00"

    for i in range(bound):
        # Build A (4 chars).
        a = chr(65 + (i & 7)) + chr(65 + ((i >> 3) & 7)) + \
            chr(65 + ((i >> 6) & 7)) + chr(65 + ((i >> 9) & 7))

        # Build B (2 chars).
        b = chr(48 + (i % 10)) + chr(48 + ((i // 10) % 10))

        # OP1: concat.
        c = a + b

        # OP2: length.
        sum_v += len(c)

        # OP3: equals fixed.
        if c == pat1:
            hits1 += 1

        # OP5: substring.
        sub = a[0:3]

        # OP6: equals varying.
        q = chr(65 + ((i // 100) & 7)) + chr(65 + ((i // 800) & 7)) + \
            chr(65 + ((i // 6400) & 7))
        if sub == q:
            hits2 += 1

        # Final: concat sub + B.
        c2 = sub + b
        sum_v += len(c2)

    r = sum_v + hits1 * 1000 + hits2 * 7
    code = r & 0x7FFFFFFF
    print(f"R={code}")
    sys.exit(code & 0xFF)  # exit code clamp a 8 bits

if __name__ == "__main__":
    main()
