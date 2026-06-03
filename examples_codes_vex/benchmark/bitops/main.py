"""Bench: bitops (and/or/xor/shl/shr)."""
import sys
M64 = (1 << 64) - 1

def main():
    a = 0xDEADBEEFCAFEBABE
    b = 0x1234567890ABCDEF
    for i in range(30_000_000):
        a = (a ^ i) & M64
        a &= 0xFFFFFFFFFFFF
        a |= 0x1010101010101
        a = (a << 1) & M64
        a >>= 1
        b = (b + (a & 0xFFFF)) & M64
        b = (b ^ (a >> 16)) & M64
        b = (b << 1) & M64
    return (a + b) & 0xFF

if __name__ == "__main__":
    sys.exit(main())
