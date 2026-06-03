# polymorphic: 10M, 3 subtypes.
import sys
class Shape:
    def area(self): return 0
class Circle(Shape):
    def __init__(self, r): self.r = r
    def area(self): return self.r * self.r * 3
class Rect(Shape):
    def __init__(self, w, h): self.w = w; self.h = h
    def area(self): return self.w * self.h
class Triangle(Shape):
    def __init__(self, b, h): self.b = b; self.h = h
    def area(self): return (self.b * self.h) // 2
def main():
    c = Circle(5); r = Rect(4, 6); t = Triangle(3, 8)
    s = 0; i = 0
    while i < 10000000:
        m = i % 3
        if m == 0: s += c.area()
        elif m == 1: s += r.area()
        else: s += t.area()
        i += 1
    sys.exit(s & 0xFF)
main()
