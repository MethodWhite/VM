# El lenguaje Vex

**Vex** es el lenguaje de alto nivel de VestaVM. Esta pagina es una vision
general; para detalles de cada feature consulta los docs especificos en
[doc/VMdoc/Vex/](./VMdoc/Vex/).

---

## Indice

1. [Filosofia de diseño](#1-filosofia-de-diseno)
2. [Comparativa rapida con otros lenguajes](#2-comparativa-rapida-con-otros-lenguajes)
3. [Tour del lenguaje](#3-tour-del-lenguaje)
   - [Tipos y variables](#tipos-y-variables)
   - [Funciones](#funciones)
   - [Clases y OOP](#clases-y-oop)
   - [Genericos](#genericos)
   - [Pattern matching y enums](#pattern-matching-y-enums)
   - [Optional y Result](#optional-y-result)
   - [Smart pointers y borrow checker](#smart-pointers-y-borrow-checker)
   - [Async y concurrencia](#async-y-concurrencia)
   - [Strings con interpolacion](#strings-con-interpolacion)
   - [FFI a APIs nativas](#ffi-a-apis-nativas)
4. [El pipeline de compilacion](#4-el-pipeline-de-compilacion)
5. [Referencia detallada por tema](#5-referencia-detallada-por-tema)

---

## 1. Filosofia de diseño

Vex se diseño con cuatro principios no negociables:

1. **Multi-paradigma sin ceremonias**: combina imperativo + POO + funcional
   ligero. Un `.vex` puede ser solo funciones y variables, sin envoltura
   `class`. Llaves estilo C, no indentacion significativa.

2. **Tipado estatico con inferencia local**: declaraciones explicitas pero
   inferencia donde aporta (`fn(i32) -> i32 f = (x) => x*x;`). Nullability
   explicita (`Optional<T>`, `nonnull T`).

3. **Performante por diseño**: baja directamente al SSA IR, sin capas de
   intercambio. Comparte backend con el JIT y el futuro AOT nativo. Sin GC
   pauses largos (generacional + write barriers).

4. **Seguro sin sacrificar control**: borrow checker estilo Rust para
   referencias temporales, smart pointers (`unique<T>`/`shared<T>`) para
   ownership, pero tambien `T*` raw y `malloc/free` para los casos extremos
   donde lo necesitas.

---

## 2. Comparativa rapida con otros lenguajes

| Caracteristica           | Vex       | Java/C# | Rust      | C++       | Python |
| :----------------------- | :-------: | :-----: | :-------: | :-------: | :----: |
| Tipado estatico          | si        | si      | si        | si        | no     |
| GC                       | si        | si      | no        | manual    | si     |
| Smart pointers nativos   | si        | no      | si        | si (lib)  | no     |
| Borrow checker           | si        | no      | si        | no        | no     |
| Pattern matching         | si        | si (>17)| si        | si (>17)  | si     |
| Async/await              | si        | si      | si        | si (>20)  | si     |
| Reflexion runtime        | si        | si      | limit.    | RTTI      | si     |
| AOP nativo               | si        | no (lib)| no        | no        | no     |
| Distribuido nativo       | si (VDP)  | no (lib)| no        | no        | no     |
| Compilable a nativo      | roadmap   | AOT     | si        | si        | no     |
| Sintaxis tipo            | C/Java    | Java    | propia    | C         | propia |
| Memoria sin GC opcional  | malloc/free | no    | si (def)  | si        | no     |

Vex toma elementos buenos de cada uno: la sintaxis legible de Java, el borrow
checker de Rust, la concurrencia ligera de Erlang/Go, la flexibilidad de C/C++
y la dinamica de Python (reflexion).

---

## 3. Tour del lenguaje

### Tipos y variables

```vex
// Primitivos
i8 byte_value   = -128;       // i16, i32, i64
u8 unsigned_b   = 255;        // u16, u32, u64
f32 single_prec = 3.14;       // f64
bool flag       = true;
char letter     = 'A';
string greeting = "Hola";

// Aliases (typedef o using estilo C++)
typedef u32 UserId;
using NodeId = u64;

// Type inference: el tipo se deduce del init
auto count = 0;               // i32 (default int)
auto pi = 3.14159;            // f64

// Arrays nativos (stack)
i32[10] stack_array;          // 10 ints en stack
i32[] heap_array = ...;       // dinamico

// Punteros raw
i32 v = 42;
i32* p = &v;                  // host pointer (memoria HOST)
VirtualPtr<i32> vp = ...;     // VM pointer (memoria virtual VM)
*p = 100;
```

Detalles: [doc/VMdoc/Vex/TiposDatos.md](./VMdoc/Vex/TiposDatos.md).

### Funciones

```vex
i32 add(i32 a, i32 b) {
    return a + b;
}

// Expression-bodied
i32 square(i32 x) => x * x;

// Multiples returns con tuplas (proximamente)
// (i32, string) result();

// Default args (proximamente)
// void greet(string name = "world");
```

Vex tiene **closures de primera clase** con captura lexica:

```vex
i32 main() {
    i32 y = 25;
    fn(i32) -> i32 add_y = (i32 x) => x + y;  // captura y
    return add_y(5);  // 30
}
```

Funciones top-level se **promueven automaticamente** a function values cuando
se pasan como argumento (A.14):

```vex
i32 add2(i32 a, i32 b) { return a + b; }

i32 reduce(fn(i32, i32) -> i32 op, i32 init, i32[] arr) {
    i32 acc = init;
    for (i32 x : arr) { acc = op(acc, x); }
    return acc;
}

i32 main() {
    i32[] xs = {1, 2, 3, 4};
    return reduce(add2, 0, xs);  // 10 - add2 promovido automaticamente
}
```

Detalles: [doc/VMdoc/Vex/Closures.md](./VMdoc/Vex/Closures.md).

### Clases y OOP

```vex
class Animal {
    public string name;
    public i32 age;
    
    public Animal(string n, i32 a) {
        this.name = n;
        this.age = a;
    }
    
    public string toString() => "Animal(${this.name}, ${this.age})";
}

class Dog : Animal {
    public string breed;
    
    public Dog(string n, i32 a, string b) : super(n, a) {
        this.breed = b;
    }
    
    @Override
    public string toString() => "Dog(${this.name}, ${this.breed})";
}

interface Greeter {
    public string greet();
}

class Cat : Animal, Greeter {
    public Cat(string n, i32 a) : super(n, a) {}
    public string greet() => "Meow from ${this.name}!";
}

i32 main() {
    Dog d = new Dog("Rex", 5, "Labrador");
    println(d.toString());        // -> Dog(Rex, Labrador)
    
    Greeter g = new Cat("Whiskers", 3);
    println(g.greet());            // -> Meow from Whiskers! (dispatch dinamico)
    return 0;
}
```

Soporta: herencia simple + multiples interfaces, `@Override` obligatorio,
modificadores `public`/`private`/`protected`/`static`/`final`, propiedades
get/set, expression-bodied, destructores RAII (`~Class()`), AOP via
`@Aspect class`.

Detalles: [doc/VMdoc/Vex/OOP.md](./VMdoc/Vex/OOP.md) y
[doc/VMdoc/Vex/ReflexionAOP.md](./VMdoc/Vex/ReflexionAOP.md).

### Genericos

```vex
class Box<T> {
    public T value;
    public Box(T v) { this.value = v; }
    public T read() => this.value;
}

i32 main() {
    Box<i32> a = new Box<i32>(42);
    Box<string> b = new Box<string>("hello");
    return a.read();
}
```

Monomorphizacion compile-time: cada `Box<i32>` y `Box<string>` genera clases
separadas (`Box_i32`, `Box_string`) con vtables propias. Cero overhead vs
codigo no-generico.

Detalles: [doc/VMdoc/Vex/Generics.md](./VMdoc/Vex/Generics.md).

### Pattern matching y enums

```vex
enum Shape {
    Circle(f64),
    Rectangle(f64, f64),
    Triangle(f64, f64, f64)
}

f64 area(Shape s) {
    match s {
        case Circle(r)        => return 3.14159 * r * r;
        case Rectangle(w, h)  => return w * h;
        case Triangle(a, b, c) => {
            f64 s = (a + b + c) / 2.0;
            return sqrt(s * (s-a) * (s-b) * (s-c));
        }
    }
}

i32 main() {
    Shape c = Shape.Circle(5.0);
    Shape r = Shape.Rectangle(3.0, 4.0);
    println("Circle area: ${area(c)}");
    println("Rect area:   ${area(r)}");
    return 0;
}
```

Match con bindings + exhaustividad obligatoria. Layout en stack:
`[tag][payload[0]][payload[1]]...` sin heap allocations.

Detalles: [doc/VMdoc/Vex/ControlFlow.md](./VMdoc/Vex/ControlFlow.md) seccion 6.

### Optional y Result

```vex
Optional<i32> find(i32[] arr, i32 target) {
    for (i32 i = 0; i < arr.length; i = i + 1) {
        if (arr[i] == target) return Some(i);
    }
    return None();
}

Result<i32, string> safe_divide(i32 a, i32 b) {
    if (b == 0) return Err("division by zero");
    return Ok(a / b);
}

i32 main() {
    i32[] data = {10, 20, 30, 40};
    
    Optional<i32> idx = find(data, 20);
    if (isPresent(idx)) {
        println("Found at index ${unwrap(idx)}");
    }
    
    Result<i32, string> r = safe_divide(100, 7);
    match isOk(r) {
        case true  => println("Quotient: ${value(r)}");
        case false => println("Error: ${error(r)}");
    }
    
    // Operador !! (unwrap-or-fail)
    i32 v = !!find(data, 30);   // unwrap; FATAL si None
    return v;
}
```

Builtins del compilador (no templates). Layout fijo 16/24 bytes en stack, ABI
SRET, cero heap. `Result` tiene must-handle (rechaza ExprStmt que descarte el
valor).

Detalles: [doc/VMdoc/Vex/OptionalResult.md](./VMdoc/Vex/OptionalResult.md).

### Smart pointers y borrow checker

```vex
extern "kernel32.dll" {
    fn VirtualAlloc(u64 a, u64 s, u32 t, u32 p) -> u64;
    fn VirtualFree(u64 a, u64 s, u32 t) -> u32;
}

void release_vmem(i64 p) { VirtualFree(p, 0, 0x8000); }

i32 main() {
    // unique con deleter custom: adopta el recurso, libera al exit del scope
    unique<i64> mem = unique_with(
        VirtualAlloc(0, 4096, 0x3000, 0x04),
        release_vmem
    );
    
    // borrow checker valida que no haya aliasing peligroso
    borrow_mut<i64> view = lend_mut(mem);
    write_borrow(view, 42);
    
    i64 v = read_borrow(view);
    println("Value: ${v}");
    
    return 0;
    // al exit: release_vmem(mem) automatico (RAII)
}
```

Smart pointers son **tipos primitivos del lenguaje** (no templates), cero
overhead (8-16 bytes en stack, deleter inline). Borrow checker es compile-time:
verifica 4 reglas + NLL + reborrow + lifetime elision.

Detalles: [doc/VMdoc/Vex/SmartPointers.md](./VMdoc/Vex/SmartPointers.md) y
[doc/VMdoc/Vex/BorrowChecker.md](./VMdoc/Vex/BorrowChecker.md).

### Async y concurrencia

```vex
@Async
i64 fetch_data(i32 id) {
    // ... simula trabajo async ...
    return id * 100;
}

i32 main() {
    // @Async retorna Future<T> automaticamente
    Future<i64> f1 = fetch_data(1);
    Future<i64> f2 = fetch_data(2);
    
    // await bloquea hasta que el future resuelva
    i64 r1 = await f1;
    i64 r2 = await f2;
    
    println("Results: ${r1}, ${r2}");
    
    // spawn local (proceso virtual ligero)
    spawn {
        println("from spawn");
    };
    
    // spawn con placement: en el mismo scheduler que el padre
    spawn here { ... };
    
    // spawn en scheduler especifico (multi-thread real)
    spawn on(2) { ... };
    
    // synchronized para exclusion mutua
    Counter c = new Counter();
    synchronized (c) {
        c.value = c.value + 1;
    }
    
    return 0;
}
```

Modelo actor (mailboxes + procesos ligeros) tipo Erlang. Scheduler cooperativo
con cuotas de instrucciones; opcional multi-thread real con `--schedulers N`.

Detalles: [doc/VMdoc/Vex/Async.md](./VMdoc/Vex/Async.md) y
[doc/VMdoc/Vex/Sincronizacion.md](./VMdoc/Vex/Sincronizacion.md).

### Strings con interpolacion

```vex
i32 count = 42;
string name = "World";

// Interpolacion basica
println("Hello ${name}, count is ${count}");

// Format specifiers (estilo Python/Rust)
println("Hex: ${count:hex}");           // 0x000000000000002A
println("Bin: ${count:bin}");           // 0b101010
println("Right-align: ${count:>10}");   // "        42"
println("Combined: ${count:hex:>20}");  // "  0x000000000000002A"

// Triple-quoted (multilinea)
string sql = """
SELECT * FROM users
WHERE name = '${name}'
  AND count = ${count}
""";

// Raw strings (sin escapes, sin interpolacion)
string regex = r"\d{3}-\d{4}";
string path = r"C:\Users\name";

// Operadores nativos
string s = "Hola" + " " + name;
bool same = (s == "Hola World");
```

`string` es GcHandle a StringObject. Soporta UTF-8/16/32, FNV-1a hash cacheado,
intern pool, encoding conversion, ROPE/SLICE/FLAT internamente.

Detalles: [doc/VMdoc/Vex/Strings.md](./VMdoc/Vex/Strings.md).

### FFI a APIs nativas

```vex
// FFI declarativo (zero-overhead, mismo coste que plugins compilados)
extern "kernel32.dll" {
    fn GetCurrentProcessId() -> u32;
    fn GetTickCount() -> u32;
    fn Sleep(u32 ms) -> void;
}

i32 main() {
    u32 pid = GetCurrentProcessId();
    u32 t0 = GetTickCount();
    Sleep(100);
    u32 t1 = GetTickCount();
    
    println("PID: ${pid}, elapsed: ${t1 - t0} ms");
    return 0;
}

// FFI runtime dinamico (cuando la DLL/funcion se decide en ejecucion)
i32 dynamic() {
    i64 lib = ffi_open("user32.dll");
    i64 fn  = ffi_sym(lib, "MessageBoxA");
    ffi_call(fn, 0, str_cstr("Hello"), str_cstr("Title"), 0);
    return 0;
}
```

Detalles: [doc/VMdoc/Vex/FFI.md](./VMdoc/Vex/FFI.md).

---

## 4. El pipeline de compilacion

```text
   .vex source
        |
        v  VPP preprocesador
        |  (#define, #include, #if, #foreach, ...)
        v  Lexer Vex
        |  (tokens, string interpolation, multiline string)
        v  Parser Vex
        |  (AST: decls, stmts, exprs, tipos)
        v  Type checker
        |  (inferencia, aliases, generics, nullability, borrow checker)
        v  Lowering (AST -> SSA IR)
        |
        v  IR Optimizer (~15 pasadas O2)
        |  DCE, copy-prop, TCO, const-fold, CSE, strength reduction,
        |  LICM, dead-alloc-elim, inline-loop-header, DSE+SLF,
        |  devirt+inline, const-cse, load_narrow, list scheduling
        |
        v  ir_emitter (SSA IR -> .vel ensamblador VM)
        |  Emite super-instrucciones cuando aplica: alu3, loadz/loadzh,
        |  cmpjmp/cmpjmpu, gcallocp, spawnargs, fulfillhlt
        |
        v  Assembler + Linker
        |  (.vel -> .velb bytecode con seccion @ir embebida)
        v
   VM ejecucion
   - Interprete threaded computed-goto (~340 MIPS)
   - JIT C1 template-based (~13x speedup)
   - Distribuido VDP (rspawn cross-node)
```

Flags utiles para inspeccionar:

- `--vex-emit-ir`: dumpea SSA IR pre y post optimizacion.
- `--diagram-all`: genera diagramas Mermaid (`.ast.mmd`, `.ir.pre.mmd`,
  `.ir.post.mmd`, `.vel.mmd`).
- `--vex-debug`: incluye debug info (file:line) en el `.velb` para el debugger.
- `--jit-warn`: warnings sobre que IR ops el selector no soporta.

---

## 5. Referencia detallada por tema

### Sintaxis y semantica

- [TiposDatos](./VMdoc/Vex/TiposDatos.md) - primitivos, punteros, struct, enum, string
- [Operadores](./VMdoc/Vex/Operadores.md) - referencia completa con precedencia
- [ControlFlow](./VMdoc/Vex/ControlFlow.md) - if/while/for/foreach/break/continue/goto/match
- [Strings](./VMdoc/Vex/Strings.md) - tipo string, interpolacion, format specs, FFI
- [OptionalResult](./VMdoc/Vex/OptionalResult.md) - Optional, Result, !!, nonnull
- [Closures](./VMdoc/Vex/Closures.md) - lambdas, captura, HOF, top-level promotion

### Modelo de programacion

- [OOP](./VMdoc/Vex/OOP.md) - clases, herencia, interfaces, properties, modificadores
- [Generics](./VMdoc/Vex/Generics.md) - class T, monomorphizacion
- [ReflexionAOP](./VMdoc/Vex/ReflexionAOP.md) - forName, getClass, @Aspect, advice
- [Colecciones](./VMdoc/Vex/Colecciones.md) - ArrayList, HashMap, Queue, etc.
- [Excepciones](./VMdoc/Vex/Excepciones.md) - try/catch/finally, FatalError, panic

### Memoria y seguridad

- [SmartPointers](./VMdoc/Vex/SmartPointers.md) - unique/shared, RAII, deleters
- [BorrowChecker](./VMdoc/Vex/BorrowChecker.md) - borrow/borrow_mut, 4 reglas + F1-F4

### Concurrencia y FFI

- [Async](./VMdoc/Vex/Async.md) - @Async, spawn, await, msgsend/msgrecv
- [Sincronizacion](./VMdoc/Vex/Sincronizacion.md) - synchronized, monitor, wait/notify
- [FFI](./VMdoc/Vex/FFI.md) - extern, ffi_open/sym/call, plugins nativos

---

Si vienes de otro lenguaje, [doc/EXAMPLES.md](./EXAMPLES.md) tiene side-by-side
comparisons (Java -> Vex, Python -> Vex, etc.).
