# obj_accum: objeto mutable de 4 campos, RMW + 2 condicionales en loop de 20M.
import sys
class Stats:
    def __init__(self):
        self.sum = 0; self.cnt = 0; self.mn = 2000000000; self.mx = -2000000000
def main():
    s = Stats()
    seed = 12345; i = 0
    while i < 20000000:
        seed = (seed * 1103515245 + 12345) & 2147483647
        v = seed % 1000
        s.sum += v
        s.cnt += 1
        if v < s.mn: s.mn = v
        if v > s.mx: s.mx = v
        i += 1
    sys.exit(int((s.sum + s.cnt + s.mn + s.mx) % 256))
main()
