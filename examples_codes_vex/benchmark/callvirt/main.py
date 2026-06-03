# callvirt: 30M virtual-call trivial.
import sys
class Counter:
    def __init__(self): self.value = 0
    def inc(self): return 1
def main():
    c = Counter()
    s = 0; i = 0
    while i < 30000000:
        s += c.inc(); i += 1
    sys.exit(s & 0xFF)
main()
