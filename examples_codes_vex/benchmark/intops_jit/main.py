# intops_jit: 5M iter.
import sys
class IntOps:
    def run_hot(self, n):
        acc = 0; i = 1
        while i < n:
            a = i; b = i + 7
            acc += min(a, b)
            acc += max(a, b)
            acc += abs(a - 5000)
            i += 1
        return acc
def main():
    r = IntOps().run_hot(5000000)
    sys.exit(r & 0xFF)
main()
