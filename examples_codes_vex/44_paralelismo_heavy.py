import time
from multiprocessing import Process, Queue
import statistics

N = 10_000_000
RUNS = 10

def worker(q):
    acc = 0
    i = 1
    while i <= N:
        acc += i
        i += 1
    q.put(acc)

def run_once():
    q = Queue()
    procs = []

    start = time.perf_counter()

    # Lanzar 4 procesos
    for _ in range(4):
        p = Process(target=worker, args=(q,))
        p.start()
        procs.append(p)

    # Recibir resultados
    total = 0
    for _ in range(4):
        total += q.get()

    # Esperar a que terminen
    for p in procs:
        p.join()

    end = time.perf_counter()
    return end - start

def main():
    times = []

    print(f"Ejecutando benchmark {RUNS} veces...\n")

    for i in range(RUNS):
        t = run_once()
        times.append(t)
        print(f"Run {i+1}: {t:.6f} s")

    print("\n--- RESULTADOS ---")
    print(f"Min:   {min(times):.6f} s")
    print(f"Max:   {max(times):.6f} s")
    print(f"Media: {statistics.mean(times):.6f} s")
    print(f"Desv:  {statistics.stdev(times):.6f} s")

if __name__ == "__main__":
    main()
