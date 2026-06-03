# alloc: 5M heap-alloc trivial.
import sys
class Foo:
    def __init__(self): self.x = 0
def helper():
    f = Foo()
    return f.x
def main():
    i = 0
    while i < 5000000:
        helper(); i += 1
    sys.exit(i & 0xFF)
main()
