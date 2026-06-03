# rotops_jit: 5M iter, rotl/rotr.
import sys
MASK64 = (1 << 64) - 1
def rotl(v, n): return ((v << n) | (v >> (64 - n))) & MASK64
def rotr(v, n): return ((v >> n) | (v << (64 - n))) & MASK64
class RotOps:
    def run_hot(self, n):
        acc = 0; i = 1
        while i < n:
            v = i
            acc = (acc + rotl(v, 7)) & MASK64
            acc = (acc + rotr(v, 3)) & MASK64
            i += 1
        return acc
def main():
    r = RotOps().run_hot(5000000)
    sys.exit(r & 0xFF)
main()
