import time, subprocess
def measure(cmd, n=10):
    ts = []
    for _ in range(n):
        t0 = time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        ts.append((time.perf_counter()-t0)*1000)
    ts.sort()
    return ts[len(ts)//2]

c_path     = r"C:\Users\DESMON~1\AppData\Local\Temp\init_c.exe"
velb_path  = r"C:\Users\DESMON~1\AppData\Local\Temp\init_overhead.velb"
vm_path    = r"F:\C\VM\cmake-build-windows\vm.exe"
py_path    = r"C:\Users\desmon0xff\AppData\Local\Programs\Python\Python311\python.exe"

print(f'C binary (empty main):     {measure([c_path]):6.2f}ms')
print(f'Vex --run (empty):         {measure([vm_path, "--run", velb_path]):6.2f}ms')
print(f'Vex --run --stats:         {measure([vm_path, "--run", velb_path, "--stats"]):6.2f}ms')
print(f'Python -c "pass":          {measure([py_path, "-c", "pass"]):6.2f}ms')
