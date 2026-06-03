# mem_struct: 1M iter * 3 paths (todos heap en Python).
import sys
class Punto:
    __slots__ = ('x','y')
    def __init__(self, x, y): self.x = x; self.y = y
def path_a(base):
    p = Punto(base, base+1); return p.x + p.y
def path_b(base):
    p = Punto(base, base+1); return p.x + p.y
def path_c(base):
    p = Punto(base, base+1); return p.x + p.y
def main():
    s = 0; i = 0
    while i < 1000000:
        s += path_a(1); s += path_b(1); s += path_c(1); i += 1
    sys.exit(s & 0xFF)
main()
