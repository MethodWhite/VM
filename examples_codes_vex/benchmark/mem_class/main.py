# mem_class: 1M class instances.
import sys
class Foo:
    def __init__(self, v): self.x = v
def helper(i):
    f = Foo(i)
    return f.x
def main():
    s = 0; i = 0
    while i < 1000000:
        s += helper(1); i += 1
    sys.exit(s & 0xFF)
main()
