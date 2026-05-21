# Ejemplos de Vex - Catalogo curado

Esta pagina selecciona los ejemplos mas instructivos de
[`examples_codes_vex/`](../examples_codes_vex/) organizados por tema. Cada uno
es ejecutable directamente con:

```bash
./build/vm --vex examples_codes_vex/<archivo>.vex -o /tmp/ej
./build/vm --run /tmp/ej.velb
```

Para todos los 140+ ejemplos completos, ver el directorio. Los benches estan
separados en `examples_codes_vex/benchmark/`.

---

## Indice

- [Ejemplos de Vex - Catalogo curado](#ejemplos-de-vex---catalogo-curado)
  - [Indice](#indice)
  - [1. Hola Mundo y basico](#1-hola-mundo-y-basico)
  - [2. Tipos y operaciones](#2-tipos-y-operaciones)
  - [3. Estructuras de datos](#3-estructuras-de-datos)
  - [4. Punteros y memoria](#4-punteros-y-memoria)
  - [5. POO: clases, herencia, interfaces](#5-poo-clases-herencia-interfaces)
  - [6. Genericos](#6-genericos)
  - [7. Pattern matching y enums](#7-pattern-matching-y-enums)
  - [8. Optional y Result](#8-optional-y-result)
  - [9. Smart pointers](#9-smart-pointers)
  - [10. Borrow checker](#10-borrow-checker)
  - [11. Closures y HOF](#11-closures-y-hof)
  - [12. Async, spawn, futures](#12-async-spawn-futures)
  - [13. Sincronizacion](#13-sincronizacion)
  - [14. Reflexion y AOP](#14-reflexion-y-aop)
  - [15. FFI: extern y dinamico](#15-ffi-extern-y-dinamico)
  - [16. Excepciones](#16-excepciones)
  - [17. Colecciones](#17-colecciones)
  - [18. Strings](#18-strings)
  - [19. Distribuido](#19-distribuido)
  - [20. Metaprogramacion](#20-metaprogramacion)
  - [21. Comparativas side-by-side con otros lenguajes](#21-comparativas-side-by-side-con-otros-lenguajes)
    - [Hello World](#hello-world)
    - [Fibonacci recursivo](#fibonacci-recursivo)
    - [Patron Optional (vs nullable)](#patron-optional-vs-nullable)
    - [Pattern matching](#pattern-matching)
  - [Donde mirar a continuacion](#donde-mirar-a-continuacion)

---

## 1. Hola Mundo y basico

**[`02_hola_mundo.vex`](../examples_codes_vex/02_hola_mundo.vex)**

```vex
i32 main() {
    println("Hola Mundo desde Vex!");
    return 0;
}
```

**[`03_contador.vex`](../examples_codes_vex/03_contador.vex)** - loop while + print
con interpolacion:

```vex
i32 main() {
    i32 i = 0;
    while (i < 5) {
        println("Iteracion ${i}");
        i = i + 1;
    }
    return 0;
}
```

---

## 2. Tipos y operaciones

**[`00_aritmetica.vex`](../examples_codes_vex/00_aritmetica.vex)** - operadores
basicos:

```vex
i32 main() {
    i32 a = 1 + 2 * 3;     // 7 (precedencia)
    i32 b = (10 - 4) * 2;  // 12
    return a;
}
```

**[`01_factorial.vex`](../examples_codes_vex/01_factorial.vex)** - recursion:

```vex
i64 factorial(i64 n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

i32 main() {
    return factorial(10);  // 3628800
}
```

---

## 3. Estructuras de datos

**[`05_struct_punto.vex`](../examples_codes_vex/05_struct_punto.vex)** - struct
basico:

```vex
struct Punto {
    i32 x;
    i32 y;
}

i32 main() {
    Punto p = {10, 32};      // init list posicional
    return p.x + p.y;        // 42
}
```

**[`06_struct_multi.vex`](../examples_codes_vex/06_struct_multi.vex)** -
struct compuesto:

```vex
struct Vec3 {
    i32 x;
    i32 y;
    i32 z;
}

i32 main() {
    Vec3 a = {1, 2, 3};
    Vec3 b = {4, 5, 6};
    return a.x + a.y + a.z + b.x + b.y + b.z;  // 21
}
```

---

## 4. Punteros y memoria

**[`07_punteros.vex`](../examples_codes_vex/07_punteros.vex)** - punteros raw:

```vex
i32 read_ptr(i32* p) {
    return *p;
}

i32 main() {
    i32 a = 100;
    i32 b = 42;
    return read_ptr(&a) + read_ptr(&b);  // 142
}
```

**[`10_heap_malloc.vex`](../examples_codes_vex/10_heap_malloc.vex)** - malloc/free:

```vex
i32 main() {
    i32* arr = malloc<i32>(100);
    for (i32 i = 0; i < 100; i = i + 1) {
        arr[i] = i * 2;
    }
    i32 sum = 0;
    for (i32 i = 0; i < 100; i = i + 1) {
        sum = sum + arr[i];
    }
    free(arr);
    return sum;  // 9900
}
```

---

## 5. POO: clases, herencia, interfaces

**[`12_clases_basico.vex`](../examples_codes_vex/12_clases_basico.vex)** - clase
simple con ctor:

```vex
class Punto {
    public i32 x;
    public i32 y;
    
    public Punto(i32 x, i32 y) {
        this.x = x;
        this.y = y;
    }
    
    public i32 sum() => this.x + this.y;
}

i32 main() {
    Punto p = new Punto(10, 32);
    return p.sum();  // 42
}
```

**[`15_herencia_basica.vex`](../examples_codes_vex/15_herencia_basica.vex)** -
herencia + override:

```vex
class Animal {
    public string name;
    
    public Animal(string n) { this.name = n; }
    public string greet() => "Soy un animal llamado ${this.name}";
}

class Dog : Animal {
    public Dog(string n) : super(n) {}
    
    @Override
    public string greet() => "Soy un perro llamado ${this.name}";
}
```

**[`21_interfaces_basico.vex`](../examples_codes_vex/21_interfaces_basico.vex)** -
interfaces + polimorfismo:

```vex
interface Speaker {
    public string speak();
}

class Cat : Speaker {
    public string speak() => "Meow";
}

class Dog : Speaker {
    public string speak() => "Woof";
}

i32 main() {
    Speaker[2] animals;
    animals[0] = new Cat();
    animals[1] = new Dog();
    for (Speaker a : animals) {
        println(a.speak());
    }
    return 0;
}
```

---

## 6. Genericos

**[`27_generics_box.vex`](../examples_codes_vex/27_generics_box.vex)** -
generic monomorfizado:

```vex
class Box<T> {
    public T value;
    public Box(T v) { this.value = v; }
    public T read() => this.value;
}

i32 main() {
    Box<i32> a = new Box<i32>(42);
    Box<i64> b = new Box<i64>(99);
    return a.read() + b.read();  // 141
}
```

---

## 7. Pattern matching y enums

**[`54_enum_payload.vex`](../examples_codes_vex/54_enum_payload.vex)** - enum con
variantes:

```vex
enum Op {
    Nop,
    Square(i32),
    Sum(i32, i32)
}

i32 evaluate(Op o) {
    match o {
        case Nop          => return 0;
        case Square(n)    => return n * n;
        case Sum(a, b)    => return a + b;
    }
}

i32 main() {
    return evaluate(Op.Square(7)) + evaluate(Op.Sum(40, 2));  // 49 + 42 = 91
}
```

---

## 8. Optional y Result

**[`33_optional_basic.vex`](../examples_codes_vex/33_optional_basic.vex)** -
Optional con implicit Some:

```vex
Optional<i32> find_pos(i32[] arr, i32 t) {
    for (i32 i = 0; i < arr.length; i = i + 1) {
        if (arr[i] == t) return Some(i);
    }
    return None();
}

i32 main() {
    i32[] data = {10, 20, 30, 40};
    Optional<i32> idx = find_pos(data, 30);
    if (isPresent(idx)) {
        return unwrap(idx);  // 2
    }
    return -1;
}
```

**Result con must-handle**: el compilador rechaza descartar el Result sin
inspeccionarlo:

```vex
Result<i32, string> divide(i32 a, i32 b) {
    if (b == 0) return Err("division by zero");
    return Ok(a / b);
}

i32 main() {
    divide(10, 2);  // ERROR de compilacion: Result no consumido
    
    Result<i32, string> r = divide(10, 2);
    if (isOk(r)) return value(r);  // OK
    return 0;
}
```

---

## 9. Smart pointers

**[`95_unique_ptr_basico.vex`](../examples_codes_vex/95_unique_ptr_basico.vex)** -
unique_box:

```vex
i32 main() {
    unique<i32> p = unique_box(42);
    i32 v = *ptr_of(p);
    return v;
    // al exit: free automatico (RAII)
}
```

**[`106_virtualalloc_unique.vex`](../examples_codes_vex/106_virtualalloc_unique.vex)** -
unique_with + deleter custom (Windows API):

```vex
extern "kernel32.dll" {
    fn VirtualAlloc(u64 a, u64 s, u32 t, u32 p) -> u64;
    fn VirtualFree(u64 a, u64 s, u32 t) -> u32;
}

void release_vmem(i64 p) { VirtualFree(p, 0, 0x8000); }

i32 main() {
    // Adopta la memoria con deleter custom
    unique<i64> mem = unique_with(
        VirtualAlloc(0, 4096, 0x3000, 0x04),
        release_vmem
    );
    // al exit: VirtualFree(mem, 0, 0x8000) automatico
    return 42;
}
```

---

## 10. Borrow checker

**[`112_borrow_mut_ok.vex`](../examples_codes_vex/112_borrow_mut_ok.vex)** -
borrow_mut basico:

```vex
i32 main() {
    unique<i32> data = unique_box(0);
    borrow_mut<i32> m = lend_mut(data);
    write_borrow(m, 42);
    i32 v = read_borrow(m);
    return v;  // 42
}
```

**[`118_borrow_nll.vex`](../examples_codes_vex/118_borrow_nll.vex)** -
Non-Lexical Lifetimes (NLL):

```vex
i32 main() {
    unique<i32> data = unique_box(42);
    borrow<i32> view = lend(data);
    i32 v = read_borrow(view);
    // NLL: view se libera aqui (tras su ultimo uso), no al exit del scope.
    
    unique<i32> moved = move(data);  // OK porque view ya no esta activo
    return v;
}
```

**[`131_borrow_combined_real.vex`](../examples_codes_vex/131_borrow_combined_real.vex)** -
patron real: pipeline mut con reborrow encadenado + multiples shared + F4
elision. Modelando un cache simple.

---

## 11. Closures y HOF

**[`50_lambda_simple.vex`](../examples_codes_vex/50_lambda_simple.vex)** -
lambda inline:

```vex
i32 main() {
    fn(i32) -> i32 sq = (x) => x * x;
    return sq(6);  // 36
}
```

**[`51_lambda_captura.vex`](../examples_codes_vex/51_lambda_captura.vex)** -
captura lexica:

```vex
i32 main() {
    i32 y = 25;
    fn(i32) -> i32 add_y = (x) => x + y;
    return add_y(5);  // 30
}
```

**[`63_topfn_as_fnvalue.vex`](../examples_codes_vex/63_topfn_as_fnvalue.vex)** -
top-level fn promotion:

```vex
i32 add2(i32 a, i32 b) { return a + b; }
i32 mul2(i32 a, i32 b) { return a * b; }

i32 reduce(fn(i32, i32) -> i32 op, i32 init, i32[] arr) {
    i32 acc = init;
    for (i32 x : arr) { acc = op(acc, x); }
    return acc;
}

i32 main() {
    i32[] xs = {1, 2, 3, 4};
    return reduce(add2, 0, xs) + reduce(mul2, 1, xs);  // 10 + 24 = 34
}
```

---

## 12. Async, spawn, futures

**[`43_async_basico.vex`](../examples_codes_vex/43_async_basico.vex)** - @Async:

```vex
@Async
i64 compute() {
    return 42;
}

i32 main() {
    Future<i64> f = compute();
    i64 r = await f;
    return r;  // 42
}
```

**[`44_spawn_basico.vex`](../examples_codes_vex/44_spawn_basico.vex)** - spawn
+ msgsend:

```vex
i32 main() {
    i64 parent_pid = pid();
    
    i64 child = spawn {
        msgsend(parent_pid, 555);
    };
    
    i64 received = msgrecv();
    return received;  // 555
}
```

**[`45_spawn_placement.vex`](../examples_codes_vex/45_spawn_placement.vex)** -
spawn placement (multi-thread real con `--schedulers N`):

```vex
i32 main() {
    spawn here { /* mismo scheduler */ }
    spawn on(2) { /* scheduler 2 */ }
    return 0;
}
```

---

## 13. Sincronizacion

**[`35_synchronized_basico.vex`](../examples_codes_vex/35_synchronized_basico.vex)** -
monitor:

```vex
class Counter {
    public i32 value = 0;
}

i32 main() {
    Counter c = new Counter();
    
    synchronized (c) {
        c.value = c.value + 10;
    }
    
    return c.value;  // 10
}
```

---

## 14. Reflexion y AOP

**[`100_reflection_full.vex`](../examples_codes_vex/100_reflection_full.vex)** -
reflexion completa:

```vex
class Calculadora {
    public i32 doblar(i32 x) => x * 2;
}

i32 main() {
    i64 cls = forName("Calculadora");
    i64 obj = newInstance(cls);
    i64 method = getMethod(cls, "doblar");
    i64 result = invoke(method, obj, 21);
    return result;  // 42
}
```

**[AOP con @Aspect]** - BEFORE/AFTER/AROUND:

```vex
class Service {
    public i32 expensive() => 42;
}

@Aspect class Logging {
    @Before("Service.expensive")
    public void log_entry(Object self, Method m) {
        println("entering ${m.name}");
    }
    
    @Around("Service.expensive")
    public i32 cache(ProceedingJoinPoint pjp) {
        return pjp.proceed();  // o return cached value
    }
}
```

---

## 15. FFI: extern y dinamico

**[`85_extern_winapi.vex`](../examples_codes_vex/85_extern_winapi.vex)** - extern
declarativo:

```vex
extern "kernel32.dll" {
    fn GetCurrentProcessId() -> u32;
    fn GetTickCount() -> u32;
}

i32 main() {
    u32 pid = GetCurrentProcessId();
    u32 ticks = GetTickCount();
    return (pid > 0) ? 42 : 0;
}
```

**[`86_ffi_runtime.vex`](../examples_codes_vex/86_ffi_runtime.vex)** - FFI runtime:

```vex
i32 main() {
    i64 lib = ffi_open("kernel32.dll");
    i64 fn = ffi_sym(lib, "GetTickCount");
    i64 result = ffi_call(fn);
    return (result > 0) ? 42 : 0;
}
```

---

## 16. Excepciones

**[`36_synchronized_exception.vex`](../examples_codes_vex/36_synchronized_exception.vex)** -
try/catch + monitor cleanup:

```vex
class MyExc {
    public i32 code;
    public MyExc(i32 c) { this.code = c; }
}

i32 main() {
    try {
        throw new MyExc(7);
    } catch (MyExc e) {
        return e.code + 1000;  // 1007
    }
    return 0;
}
```

**Capturar FatalError de OS** (AV / segfault):

```vex
i32 main() {
    try {
        i64* bad = (i64*) 0;
        i64 v = *bad;  // segfault del OS
    } catch (FatalError e) {
        return e.kind;  // FATAL_SEGMENTATION_FAULT (7)
    }
    return 0;
}
```

---

## 17. Colecciones

**[`92_collections_primitive.vex`](../examples_codes_vex/92_collections_primitive.vex)** -
ArrayList + HashMap + free automatico:

```vex
i32 main() {
    ArrayList xs = arraylist(16);
    xs.push(10); xs.push(20); xs.push(30);
    i64 sum = xs.get(0) + xs.get(1) + xs.get(2);  // 60
    
    HashMap m = hashmap(16);
    m.put("answer", 42);
    i64 v = m.get("answer");
    
    return sum + v;  // 102
    // al exit: free automatico de ambas colecciones (cleanup_stack)
}
```

---

## 18. Strings

**[`76_string_exhaustive.vex`](../examples_codes_vex/76_string_exhaustive.vex)** -
strings completos (UTF-8, interpolacion, multi-alfabeto):

```vex
i32 main() {
    string s = "hola";
    string t = " mundo";
    string u = s + t;            // ROPE concat
    
    i32 len = u.length();        // 10
    i32 bytes = u.bytes();       // 10 (ASCII)
    
    string greek = "\xce\xb1";   // alpha griega: bytes ce b1
    i32 greek_bytes = greek.bytes();  // 2
    i32 greek_cp = greek.length();    // 1
    
    println("Total: ${u} (${bytes} bytes, ${len} code points)");
    return 42;
}
```

**Format specifiers** (interpolacion estilo Python/Rust):

```vex
i32 main() {
    i32 n = 255;
    println("${n:hex}");           // 0x00000000000000FF
    println("${n:bin}");           // 0b11111111
    println("${n:>10}");           // "       255"  (right-align)
    println("${n:hex:>20}");       // "  0x00000000000000FF"
    return 0;
}
```

---

## 19. Distribuido

**[`47_rspawn_basico.vex`](../examples_codes_vex/47_rspawn_basico.vex)** - rspawn
cross-node:

```vex
i32 main() {
    // spawn un proceso en el nodo remoto 0 (registrado con --dist-add-node)
    i64 fut = rspawn(0) {
        return 42;
    };
    i64 r = await fut;
    return r;  // 42
}
```

Compilar y ejecutar entre 2 procesos:

```bash
# Terminal 1: nodo servidor
./build/vm --dist-server --dist-port 7789

# Terminal 2: cliente que envia rspawn
./build/vm --vex examples_codes_vex/47_rspawn_basico.vex -o /tmp/rsp
./build/vm --run /tmp/rsp.velb --dist-port 7790 --dist-add-node 127.0.0.1:7789
```

---

## 20. Metaprogramacion

Macros compile-time que generan codigo, capturan DSLs y consultan tipos
sin overhead runtime. Detalles completos: [Metaprogramacion.md](./VMdoc/Vex/Metaprogramacion.md).

**[`159_macro_expr_capture.vex`](../examples_codes_vex/159_macro_expr_capture.vex)** -
captura raw de codigo arbitrario con `expr`:

```vex
@Macro
comptime string M_identity(expr code) {
    // `code` recibe el texto verbatim del call site.
    return "\"" + code + "\"";
}

i32 main() {
    // El parser captura "ptr -> 0x90 -> 0x10 -> 0x20" sin parsearlo
    // como expresion -- el macro decide como interpretarlo.
    string captured = M_identity(ptr -> 0x90 -> 0x10 -> 0x20);
    return 42;
}
```

**[`160_macro_walk_pchase.vex`](../examples_codes_vex/160_macro_walk_pchase.vex)** -
DSL real: pointer chase anidado generado en compile-time:

```vex
@Macro
comptime string walk(expr code) {
    // Parser interno: split por "->", emitir derefs anidados.
    // ...usa strlen, substr, operadores + y == sobre strings...
}

i32 main() {
    u64* root = (u64*)malloc(256);
    // ... poblar punteros ...

    // El compilador transforma esto:
    u64 v = walk(root -> 0x100 -> 0 -> 0);
    // en esto:
    // u64 v = *(u64*)((u64)( *(u64*)((u64)( *(u64*)((u64)( root ) + 0x100) ) + 0) ) + 0);
    return 42;
}
```

El `.vel` emitido contiene exactamente 3 instrucciones `movh` consecutivas,
una por hop. Sin overhead vs escribir el chase manualmente.

**[`161_macro_ffi_compile_time.vex`](../examples_codes_vex/161_macro_ffi_compile_time.vex)** -
FFI a DLLs del sistema en tiempo de compilacion:

```vex
extern "kernel32.dll" {
    fn GetCurrentProcessId() -> u32;
    fn GetTickCount() -> u32;
}

@Macro
comptime string build_id() {
    u64 pid  = GetCurrentProcessId();   // FFI en compile-time
    u64 tick = GetTickCount();          // FFI en compile-time
    u64 mix  = (pid * 31) ^ (tick * 17);
    return to_str(mix);  // embebido como literal en el .velb
}

@Macro
comptime string size_of_u64() {
    // Virtual lib `vesta_comptime` registrada in-process; sin extern explicito.
    return to_str(comptime_type_sizeof("u64"));  // siempre 8
}

i32 main() {
    u64 fingerprint = build_id();  // valor unico por compilacion
    u64 sz = size_of_u64();         // = 8
    return 42;
}
```

Cada compilacion produce un binario distinto (depende del PID + tick del
compilador). El `.velb` final NO referencia kernel32 -- solo contiene
`mov rN, <literal>` con el valor calculado al compilar.

**[`162_macro_comptime_data.vex`](../examples_codes_vex/162_macro_comptime_data.vex)** -
arrays y "diccionario" via arrays paralelos en compile-time:

```vex
@Macro
comptime string fib_at(i64 idx) {
    i64 fibs[16] = {0, 1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233, 377, 610};
    if (idx < 0 || idx >= 16) return "0";
    return to_str(fibs[idx]);
}

// Diccionario via arrays paralelos (no hay HashMap comptime).
@Macro
comptime string lookup(i64 key) {
    i64 keys[5] = {1, 2, 5, 10, 42};
    i64 vals[5] = {100, 200, 555, 1000, 9999};
    i64 i = 0;
    while (i < 5) {
        if (keys[i] == key) return to_str(vals[i]);
        i = i + 1;
    }
    return "-1";
}

// Switch-via-ternario generado en compile-time.
@Macro
comptime string switch_gen() {
    i64 cases[3] = {1, 2, 3};
    i64 results[3] = {10, 20, 30};
    string code = "0";
    i64 i = 2;
    while (i >= 0) {
        code = "((x == " + to_str(cases[i]) + ") ? " + to_str(results[i]) + " : " + code + ")";
        i = i - 1;
    }
    return code;
}

i32 main() {
    i64 f10 = fib_at(10);     // se compila como `i64 f10 = 55;`
    i64 v   = lookup(5);       // se compila como `i64 v = 555;`
    i64 x   = 2;
    i64 sw  = switch_gen();    // ternario anidado generado
    return 42;
}
```

**Patron clave: azucar sintactico**. Dentro del cuerpo de un macro, los
operadores nativos sobre strings (`+` concat, `==`/`!=` equals) y los
builtins cortos (`strlen`, `substr`, `to_str`, `chr`, `ord`, `repeat`,
`replace`, `contains`, `gensym`) son siempre preferidos a las versiones
`comptime_*` verbose. Mismo bytecode emitido, codigo ~3x mas corto.

```vex
// Verbose (legacy):
return comptime_concat("(", comptime_concat(comptime_to_str(n), ")"));

// Preferido:
return "(" + to_str(n) + ")";
```

---

## 21. Comparativas side-by-side con otros lenguajes

### Hello World

| Vex                                  | Python                          | Rust                                    |
| :----------------------------------- | :------------------------------ | :-------------------------------------- |
| `i32 main() { println("hi"); return 0; }` | `print("hi")`                | `fn main() { println!("hi"); }`         |

### Fibonacci recursivo

**Vex**:

```vex
i64 fib(i64 n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

i32 main() { return fib(30); }
```

**Java**:

```java
class Fib {
    static long fib(long n) {
        if (n < 2) return n;
        return fib(n - 1) + fib(n - 2);
    }
    public static void main(String[] args) {
        System.out.println(fib(30));
    }
}
```

**Python**:

```python
def fib(n):
    if n < 2: return n
    return fib(n - 1) + fib(n - 2)

print(fib(30))
```

### Patron Optional (vs nullable)

**Vex** (Optional builtin SRET, cero heap):

```vex
Optional<i32> find(i32[] arr, i32 t) {
    for (i32 i = 0; i < arr.length; i = i + 1) {
        if (arr[i] == t) return Some(i);
    }
    return None();
}
```

**Rust** (similar):

```rust
fn find(arr: &[i32], t: i32) -> Option<usize> {
    arr.iter().position(|&x| x == t)
}
```

**Java** (Optional, pero con heap alloc):

```java
Optional<Integer> find(int[] arr, int t) {
    for (int i = 0; i < arr.length; i++) {
        if (arr[i] == t) return Optional.of(i);
    }
    return Optional.empty();
}
```

### Pattern matching

**Vex**:

```vex
enum Shape { Circle(f64), Square(f64) }

f64 area(Shape s) {
    match s {
        case Circle(r) => return 3.14 * r * r;
        case Square(l) => return l * l;
    }
}
```

**Rust** (idéntico en concepto):

```rust
enum Shape { Circle(f64), Square(f64) }

fn area(s: Shape) -> f64 {
    match s {
        Shape::Circle(r) => 3.14 * r * r,
        Shape::Square(l) => l * l,
    }
}
```

**Java** (con sealed + records, syntax mas verbose):

```java
sealed interface Shape permits Circle, Square {}
record Circle(double r) implements Shape {}
record Square(double l) implements Shape {}

double area(Shape s) {
    return switch (s) {
        case Circle c -> 3.14 * c.r() * c.r();
        case Square sq -> sq.l() * sq.l();
    };
}
```

---

## Donde mirar a continuacion

Si tienes una caracteristica especifica en mente, busca el doc dedicado en
[doc/VMdoc/Vex/](./VMdoc/Vex/). Cada uno tiene ejemplos completos y
referencia tecnica.

Si vas a contribuir codigo, ver  [doc/CONTRIBUTING.md](./CONTRIBUTING.md).
