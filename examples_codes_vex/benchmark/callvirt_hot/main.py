# callvirt_hot: 10M virtual-call hot with state.
import sys
class Counter:
    def __init__(self): self.n = 0
    def inc(self, d):
        self.n += d
        return self.n
def main():
    c = Counter(); s = 0; i = 0
    while i < 10000000:
        s = c.inc(1); i += 1
    sys.exit(s & 0xFF)
main()
