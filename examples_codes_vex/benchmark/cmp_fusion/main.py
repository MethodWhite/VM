# cmp_fusion: 50M loop.
import sys
class C:
    def run(self, n):
        acc = 0; i = 0
        while i < n:
            acc += 1; i += 1
        return acc
def main():
    c = C()
    r = c.run(50000000)
    sys.exit(r & 0xFF)
main()
