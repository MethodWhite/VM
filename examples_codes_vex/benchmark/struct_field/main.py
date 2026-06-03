# struct_field: 30M field read/write.
import sys
class Vec3:
    __slots__ = ('x','y','z')
    def __init__(self): self.x = 1; self.y = 2; self.z = 3
def main():
    v = Vec3()
    s = 0; i = 0
    while i < 30000000:
        v.x += 1; v.y += 2; v.z += 3
        s += v.x + v.y + v.z
        i += 1
    sys.exit(s & 0xFF)
main()
