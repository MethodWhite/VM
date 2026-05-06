import time

class A:
    def __init__(self, v):
        self.x = v

def main():
    start = time.perf_counter()

    for i in range(300_000_000):
        a = A(i)

    end = time.perf_counter()
    print("Tiempo:", end - start, "segundos")

main()
