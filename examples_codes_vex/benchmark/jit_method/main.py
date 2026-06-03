# jit_method: 30M loop en metodo.
import sys
class Worker:
    def __init__(self): self.dummy = 0
    def run_hot_loop(self, n):
        s = 0; i = 0
        while i < n:
            s += i; i += 1
        return s
def main():
    w = Worker()
    a = w.run_hot_loop(10000000)
    b = w.run_hot_loop(10000000)
    c = w.run_hot_loop(10000000)
    sys.exit((a + b + c) & 0xFF)
main()
