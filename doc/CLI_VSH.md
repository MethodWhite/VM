# VestaShell (.vsh) — Referencia del lenguaje de scripting

VestaShell es el lenguaje de scripting embebido en VestaVM. Permite automatizar
tareas en el REPL, glue code entre modulos y scripting de sistema con acceso
completo al filesystem, shell y matematicas.

Los scripts tienen extension `.vsh` y se ejecutan con:

```
// Dentro del REPL (comando "script")
vesta> script mi_script.vsh

// Abrir el REPL interactivo VestaShell desde dentro del REPL
vesta> vsh

// Linea de comandos (sin abrir el REPL)
./vm --script mi_script.vsh

// REPL interactivo VestaShell al estilo Python (desde linea de comandos)
./vm --interprete
```

Los ficheros de estado (`vm_env.txt`, `vm_history.txt`, `vm_aliases.txt`) se
crean siempre junto al ejecutable, independientemente del directorio de trabajo
actual.

Los builtins son visibles como valores de primera clase: escribir `help` sin
parentesis en el REPL lo invoca automaticamente (comportamiento estilo Python).

---

## Indice

1. [Tipos de datos](#tipos-de-datos)
2. [Variables y anotaciones de tipo](#variables-y-anotaciones-de-tipo)
3. [Operadores](#operadores)
4. [Strings e interpolacion](#strings-e-interpolacion)
5. [Control de flujo](#control-de-flujo)
6. [Funciones](#funciones)
7. [Listas](#listas)
8. [Mapas](#mapas)
9. [Clases y POO](#clases-y-poo)
10. [Manejo de errores](#manejo-de-errores)
11. [Importacion de modulos](#importacion-de-modulos)
12. [Funciones integradas](#funciones-integradas)
13. [Sockets y red](#sockets-y-red)
14. [FFI: llamadas a librerias nativas](#ffi-llamadas-a-librerias-nativas)
15. [ANSI: colores y secuencias de escape](#ansi-colores-y-secuencias-de-escape)
16. [REPL interactivo VestaShell](#repl-interactivo-vestaShell)
17. [Integracion con el REPL](#integracion-con-el-repl)
18. [Comentarios y continuacion de linea](#comentarios-y-continuacion-de-linea)

---

## Tipos de datos

| Tipo       | Descripcion                                | Ejemplo                |
|------------|--------------------------------------------|------------------------|
| `null`     | Ausencia de valor                          | `null`                 |
| `bool`     | Booleano                                   | `true`, `false`        |
| `int`      | Entero de 64 bits con signo                | `42`, `-7`, `1_000`    |
| `float`    | Flotante IEEE 754 de 64 bits               | `3.14`, `-0.5`         |
| `string`   | Cadena de texto UTF-8                      | `"Hola"`, `'mundo'`    |
| `list`     | Lista dinamica de valores                  | `[1, "dos", true]`     |
| `map`      | Diccionario string -> valor                | `{"k": 1, "v": 2}`    |
| `function` | Funcion de primera clase (con closure)     | `fn(x) { return x*2 }` |
| `class`    | Descriptor de clase                        | `class Punto { ... }`  |
| `instance` | Instancia de clase                         | `Punto(1, 2)`          |

---

## Variables y anotaciones de tipo

```vsh
let nombre = "VestaShell"   // declaracion
nombre = "vsh"              // reasignacion (la variable ya debe existir)

let x = 10
let y = x + 5               // expresion en la inicializacion
let z = null                // null explicito
```

Las variables son dinamicamente tipadas por defecto. Opcionalmente se puede
anotar el tipo con `: tipo` para que el interprete lo verifique en tiempo de
ejecucion:

```vsh
let n: int    = 42          // solo acepta int
let s: str    = "hola"      // solo acepta str / string
let f: float  = 3.14        // solo acepta float
let b: bool   = true        // solo acepta bool
let lst: list = [1, 2, 3]   // solo acepta list
let mp: map   = {"k": 1}    // solo acepta map

// Error si el tipo no coincide:
let x: int = "texto"        // RuntimeError: variable 'x': se esperaba tipo 'int'
```

Nombres de tipo aceptados en anotaciones: `null`, `bool`, `int`, `float`,
`str`/`string`, `list`, `map`, `fn`/`function`, `class`, `instance`,
o el nombre de cualquier clase definida en el script.

Los parametros de funcion tambien admiten anotaciones de tipo:

```vsh
fn doble(x: int) { return x * 2 }
fn greet(nombre: str, edad) {       // 'edad' sin tipo: acepta cualquier valor
    return nombre + " tiene " + str(edad)
}

doble(5)        // ok
doble("text")   // RuntimeError: parametro 'x': se esperaba tipo 'int'
```

Las subclases pasan el check de tipo de la clase padre:

```vsh
class Animal { fn __init__(self, n) { self.nombre = n } }
class Perro : Animal { }

fn cuidar(a: Animal) { println(a.nombre) }
cuidar(Perro("Rex"))    // ok: Perro es subclase de Animal
```

---

## Operadores

### Aritmeticos

| Operador | Descripcion         | Ejemplo          |
|----------|---------------------|------------------|
| `+`      | Suma / concatenacion| `3 + 4`, `"a"+"b"` |
| `-`      | Resta / negacion    | `10 - 3`, `-x`   |
| `*`      | Multiplicacion      | `3 * 4`          |
| `/`      | Division            | `10 / 3` -> `3`   |
| `%`      | Modulo              | `10 % 3` -> `1`   |
| `**`     | Potencia (der)      | `2 ** 10` -> `1024`|

### Comparacion

```vsh
a == b    a != b    a < b    a <= b    a > b    a >= b
```

Los tipos `int` y `float` son iguales si el valor numerico coincide: `1 == 1.0`.

### Logicos

```vsh
not expr          // negacion booleana
a and b           // conjuncion (cortocircuito)
a or  b           // disyuncion (cortocircuito)
```

### Asignacion compuesta

```vsh
x += 5     x -= 3     x *= 2     x /= 4     x %= 3
```

---

## Strings e interpolacion

```vsh
let s = "Hola mundo"
let t = 'tambien funciona con comilla simple'

// Interpolacion: ${expresion} dentro de la cadena
let nombre = "Ada"
let n = 36
let msg = "Hola ${nombre}, tienes ${n} anos"

// Las expresiones pueden ser arbitrarias (sin comillas anidadas)
let pi = 3.14159
let area = "El area es ${pi * 4.0}"
```

**Nota**: evitar usar comillas del mismo tipo dentro de `${}`.
Usar variables intermedias cuando sea necesario:

```vsh
let arg = "mundo"
let r = "${saludo(arg)}"    // correcto
// let r = "${saludo("mundo")}"  // produce error de sintaxis
```

### Escapes en strings

| Secuencia | Caracter       |
|-----------|----------------|
| `\n`      | Nueva linea    |
| `\t`      | Tabulacion     |
| `\r`      | Retorno carro  |
| `\\`      | Barra invertida|
| `\"`      | Comilla doble  |
| `\$`      | Literal `$`    |

---

## Control de flujo

### if / elif / else

```vsh
if condicion {
    // ...
} elif otra_condicion {
    // ...
} else {
    // ...
}
```

### while

```vsh
let i = 0
while i < 10 {
    i += 1
    if i == 5 { continue }
    if i == 8 { break }
    println(str(i))
}
```

### for / in

Itera sobre listas, mapas (claves) y strings (caracteres):

```vsh
// Lista
for elem in [1, 2, 3, 4, 5] {
    println(str(elem))
}

// Mapa (da claves)
let config = {"host": "localhost", "port": 8080}
for clave in config {
    println(clave + " = " + str(config[clave]))
}

// String (da caracteres)
for c in "hola" {
    println(c)
}

// Rango numerico
for i in range(10) {          // 0..9
    println(str(i))
}
for i in range(1, 6) {        // 1..5
    println(str(i))
}
for i in range(0, 10, 2) {    // 0,2,4,6,8
    println(str(i))
}
for i in range(5, 0, -1) {    // 5,4,3,2,1
    println(str(i))
}
```

### break / continue

`break` sale del bucle mas cercano. `continue` salta a la siguiente iteracion.
Funcionan dentro de `while`, `for/in` y dentro de `try/catch` en un bucle.

---

## Funciones

```vsh
// Declaracion nombrada
fn suma(a, b) {
    return a + b
}

// Funcion anonima (lambda)
let doble = fn(x) { return x * 2 }

// Sin return explicito devuelve null
fn efecto() {
    println("hola")
}

// Las funciones son valores de primera clase
fn aplicar(f, x) { return f(x) }
let resultado = aplicar(doble, 21)   // 42
```

### Docstrings

La primera sentencia del cuerpo de una funcion puede ser un string literal. Ese
string se convierte en la documentacion de la funcion y es accesible mediante
`doc()` y `help()`:

```vsh
fn factorial(n) {
    "Calcula n! de forma recursiva. factorial(0) = 1."
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}

doc(factorial)    // "Calcula n! de forma recursiva. factorial(0) = 1."
help(factorial)   // imprime la firma y el docstring; tambien devuelve el texto
```

Reglas:
- Solo string literal simple (no interpolado) como primera sentencia.
- Las funciones sin docstring devuelven `""` en `doc()`.
- Los docstrings se propagan a traves de `import`.

### Closures

```vsh
fn contador() {
    let n = 0
    return fn() {
        n = n + 1
        return n
    }
}

let c = contador()
c()   // 1
c()   // 2
c()   // 3
```

### Funciones almacenadas en mapas

```vsh
let ops = {
    "add": fn(a, b) { return a + b },
    "mul": fn(a, b) { return a * b },
}
ops["add"](10, 5)   // 15
```

---

## Listas

```vsh
let lista = [1, "dos", true, null, [3, 4]]

// Indexado (0-based; negativo desde el final)
lista[0]    // 1
lista[-1]   // [3, 4]

// Modificar
lista[1] = "TWO"

// Funciones
len(lista)              // longitud
append(lista, valor)    // anadir al final (modifica in-place)
pop(lista)              // eliminar y devolver el ultimo elemento
contains(lista, valor)  // true si valor esta en lista
```

---

## Mapas

```vsh
let m = {
    "nombre": "Eva",
    "edad": 30,
}

// Acceso (lanza error si la clave no existe)
m["nombre"]         // "Eva"

// Asignacion (crea la clave si no existe)
m["activo"] = true

// Funciones
keys(m)             // lista de claves
values(m)           // lista de valores
contains(m, "edad") // true si la clave existe
len(keys(m))        // numero de claves

// Acceso seguro a clave posiblemente inexistente
let val = null
try {
    val = m["clave_opcional"]
} catch e {
    val = "por_defecto"
}
```

### Mapas y listas anidados

```vsh
let datos = {
    "config": { "debug": true, "workers": 4 },
    "tags":   ["prod", "backend"],
}
datos["config"]["workers"]    # 4
datos["tags"][0]              # "prod"
```

---

## Clases y POO

### Definicion de clase

```vsh
class Punto {
    "Representa un punto 2D."          // docstring de clase

    fn __init__(self, x, y) {
        self.x = x
        self.y = y
    }

    fn distancia_al_origen(self) {
        "Devuelve la distancia al origen."
        return sqrt(self.x ** 2 + self.y ** 2)
    }
}

let p = Punto(3.0, 4.0)
println(p.distancia_al_origen())    // 5.0
println(p.x)                        // 3.0
p.x = 0.0                           // asignacion de atributo
```

- El primer parametro de cada metodo es `self` por convencion.
- `__init__` es el constructor; se llama automaticamente al crear la instancia.
- Los atributos se crean dinamicamente asignando `self.campo = valor`.

### Herencia

```vsh
class Punto3D : Punto {
    fn __init__(self, x, y, z) {
        self.x = x
        self.y = y
        self.z = z
    }

    fn suma(self) {
        return self.x + self.y + self.z
    }
}

let q = Punto3D(1, 2, 3)
println(isinstance(q, Punto3D))  // true
println(isinstance(q, Punto))    // true (herencia)
println(classname(q))            // "Punto3D"
```

`super` esta disponible dentro de los metodos como referencia a la clase padre:

```vsh
class Animal {
    fn __init__(self, nombre) { self.nombre = nombre }
    fn hablar(self) { return "..." }
}

class Perro : Animal {
    fn __init__(self, nombre) { self.nombre = nombre }
    fn hablar(self) { return "Guau!" }
}
```

### Predicados OOP

```vsh
is_class(Punto)          // true
is_instance(p)           // true
isinstance(p, Punto)     // true
classname(p)             // "Punto"
doc(Punto)               // docstring de la clase
help(Punto)              // imprime metodos y docstring de la clase
```

---

## Manejo de errores

```vsh
try {
    // codigo que puede fallar
    let contenido = read_file("archivo.txt")
} catch e {
    // e es el mensaje de error como string
    println("Error: " + e)
}
```

### Lanzar errores

```vsh
error("descripcion del error")        // lanza VshRuntimeError
assert(condicion, "mensaje")          // lanza si condicion es falsa
assert(x > 0)                         // mensaje por defecto "asercion fallida"
```

Los errores de tipo `VshParseError` (sintaxis) NO son capturables con `try/catch`.

### Errores tipados con clases

VestaShell incluye la clase `Error` como base. Se pueden crear jerarquias de
errores personalizadas:

```vsh
class ValorError : Error {
    fn __init__(self, msg, valor) {
        self.message = msg
        self.valor   = valor
    }
}

class RangoError : ValorError {
    fn __init__(self, msg, valor, min, max) {
        self.message = msg
        self.valor   = valor
        self.minimo  = min
        self.maximo  = max
    }
}

// Lanzar con throw
fn validar(x) {
    if x < 0 or x > 100 {
        throw RangoError("fuera de rango", x, 0, 100)
    }
    return x
}

// Capturar con catch tipado
try {
    validar(150)
} catch e : RangoError {
    println("Rango: " + e.message)    // variable e es la instancia
    println("Valor: " + str(e.valor))
} catch e : ValorError {
    println("Valor: " + e.message)
} catch e {
    println("Error: " + e)            // catch generico (recibe string)
}
```

Reglas del catch tipado:
- Se evaluan en orden; se ejecuta el primer catch cuyo tipo coincide.
- Un catch con tipo de clase padre atrapa instancias de subclases.
- Un catch sin tipo atrapa cualquier error (incluidos strings lanzados con `throw`).
- `throw expr` con una instancia de clase envia la instancia al catch.
- `throw expr` con un valor no-instancia lanza un error de string.

### Relanzar

```vsh
try {
    operacion_riesgosa()
} catch e {
    if not starts_with(e, "error esperado") {
        error("error inesperado: " + e)
    }
}
```

---

## Importacion de modulos

```vsh
import "ruta/al/modulo.vsh"
```

- La ruta es relativa al directorio de trabajo actual (CWD).
- El modulo se ejecuta en el scope global: sus funciones y variables quedan disponibles.
- La importacion circular se detecta y lanza un error.
- Importar el mismo fichero dos veces re-ejecuta sus definiciones (idempotente en la practica para funciones).

**Convencion recomendada**: poner la libreria en el mismo directorio que el script principal y usar rutas relativas a donde se lanza el binario `vm`.

---

## Funciones integradas

### Entrada / salida

| Funcion                | Descripcion                                   |
|------------------------|-----------------------------------------------|
| `echo(v1, v2, ...)`    | Imprime valores separados por espacio + newline |
| `print(v1, v2, ...)`   | Imprime sin newline final                     |
| `println(v1, v2, ...)`  | Imprime con newline final                    |
| `input([prompt])`      | Lee una linea de stdin                        |

### Conversion de tipos

| Funcion       | Descripcion                                   |
|---------------|-----------------------------------------------|
| `str(v)`      | Convierte a string                            |
| `int(v)`      | Convierte a int64 (trunca float)              |
| `float(v)`    | Convierte a float64                           |
| `bool(v)`     | Convierte a bool (truthy)                     |
| `type(v)`     | Devuelve el tipo como string                  |

### Strings

| Funcion                        | Descripcion                                               |
|--------------------------------|-----------------------------------------------------------|
| `len(s)`                       | Longitud en bytes                                         |
| `upper(s)`                     | Todo en mayusculas                                        |
| `lower(s)`                     | Todo en minusculas                                        |
| `trim(s)`                      | Eliminar espacios al inicio y al final                    |
| `lstrip(s)`                    | Eliminar espacios solo al inicio                          |
| `rstrip(s)`                    | Eliminar espacios solo al final                           |
| `split(s, sep)`                | Dividir por separador -> lista                            |
| `join(lista, sep)`             | Unir lista de strings con separador                       |
| `starts_with(s, pref)`         | True si s empieza con pref                                |
| `ends_with(s, suf)`            | True si s termina con suf                                 |
| `replace(s, old, new)`         | Sustituir todas las ocurrencias de old                    |
| `substr(s, inicio [, len])`    | Subcadena (indices negativos cuentan desde el final)      |
| `find_str(s, sub [, inicio])`  | Indice de sub en s (-1 si no existe)                      |
| `count_str(s, sub)`            | Numero de ocurrencias de sub en s                         |
| `repeat(s, n)`                 | Repetir s n veces                                         |
| `pad_left(s, n [, char])`      | Rellenar por la izquierda hasta longitud n                |
| `pad_right(s, n [, char])`     | Rellenar por la derecha hasta longitud n                  |
| `contains(s, sub)`             | True si sub esta en s                                     |
| `char_code(s)`                 | Codigo ASCII/byte del primer caracter                     |
| `from_char(n)`                 | Caracter con codigo n                                     |
| `is_numeric(s)`                | True si s se puede parsear como numero                    |
| `hex(n)`                       | Representacion hex del entero (`"0x1f"`)                  |
| `bin_str(n)`                   | Representacion binaria del entero (`"0b1010"`)            |

### Listas

| Funcion                   | Descripcion                                              |
|---------------------------|----------------------------------------------------------|
| `len(lista)`              | Numero de elementos                                      |
| `append(lista, v)`        | Anadir v al final (in-place via shared ref)              |
| `pop(lista)`              | Eliminar y devolver el ultimo elemento                   |
| `contains(lista, v)`      | True si v esta en la lista (por valor)                   |
| `index_of(lista, v)`      | Indice de v en la lista (-1 si no existe)                |
| `range(n)`                | Lista `[0, 1, ..., n-1]`                                 |
| `range(s, e [, step])`    | Lista desde s hasta e-1 con paso step                    |
| `sort(lista)`             | Copia de la lista ordenada                               |
| `reverse(lista)`          | Copia de la lista al reves                               |
| `slice(lista, s [, e])`   | Sublista [s, e) con indices negativos soportados         |
| `flat(lista)`             | Aplanar un nivel (lista de listas)                       |
| `zip(a, b)`               | Lista de pares `[[a0,b0], [a1,b1], ...]`                 |
| `enumerate(lista)`        | Lista de pares `[[0,v0], [1,v1], ...]`                   |
| `sum(lista)`              | Suma de todos los elementos numericos                    |
| `any_of(lista)`           | True si algun elemento es truthy                         |
| `all_of(lista)`           | True si todos los elementos son truthy                   |
| `unique(lista)`           | Copia sin duplicados (preserva orden)                    |

```vsh
// Ordenar y transformar
let nums = [3, 1, 4, 1, 5, 9, 2, 6]
println(sort(nums))                          // [1, 1, 2, 3, 4, 5, 6, 9]
println(unique(sort(nums)))                  // [1, 2, 3, 4, 5, 6, 9]
println(reverse(slice(nums, 0, 4)))          // [4, 1, 3]
println(sum(nums))                           // 31

// Herramientas utiles
for pair in enumerate(["a", "b", "c"]) {
    println(pair[0] + ": " + pair[1])        // 0: a, 1: b, 2: c
}
let coords = zip([1,2,3], [4,5,6])           // [[1,4],[2,5],[3,6]]
println(flat([[1,2],[3],[4,5]]))              // [1, 2, 3, 4, 5]
println(any_of([false, false, true]))         // true
println(all_of([1, 2, 3]))                   // true
println(index_of([10,20,30], 20))            // 1
```

### Mapas

| Funcion             | Descripcion                               |
|---------------------|-------------------------------------------|
| `keys(m)`           | Lista de claves del mapa                  |
| `values(m)`         | Lista de valores del mapa                 |
| `contains(m, clave)`| True si la clave existe en el mapa        |

### Predicados de tipo

```vsh
is_null(v)      is_bool(v)      is_int(v)       is_float(v)
is_str(v)       is_list(v)      is_map(v)        is_fn(v)
is_class(v)     is_instance(v)
```

`type(v)` devuelve el tipo como string: `"null"`, `"bool"`, `"int"`, `"float"`,
`"string"`, `"list"`, `"map"`, `"function"`, `"class"`, `"instance"`.

### OOP

| Funcion                       | Descripcion                                          |
|-------------------------------|------------------------------------------------------|
| `isinstance(obj, clase)`      | True si obj es instancia de clase o subclase         |
| `classname(obj)`              | Nombre de la clase de obj (o de la clase misma)      |
| `is_class(v)`                 | True si v es un descriptor de clase                  |
| `is_instance(v)`              | True si v es una instancia                           |

### Matematicas

| Funcion                | Descripcion                                       |
|------------------------|---------------------------------------------------|
| `abs(x)`               | Valor absoluto                                    |
| `min(a, b)`            | Minimo de dos valores                             |
| `max(a, b)`            | Maximo de dos valores                             |
| `clamp(x, lo, hi)`     | Limitar x al rango [lo, hi]                       |
| `sign(x)`              | -1, 0 o 1 segun el signo de x                    |
| `floor(x)`             | Redondear hacia abajo                             |
| `ceil(x)`              | Redondear hacia arriba                            |
| `round(x)`             | Redondear al entero mas cercano                   |
| `sqrt(x)`              | Raiz cuadrada                                     |
| `pow(base, exp)`       | Potencia                                          |
| `log(x)`               | Logaritmo natural (base e)                        |
| `log2(x)`              | Logaritmo en base 2                               |
| `log10(x)`             | Logaritmo en base 10                              |
| `sin(x)` / `cos(x)` / `tan(x)` | Trigonometria (radianes)                 |
| `gcd(a, b)`            | Maximo comun divisor                              |
| `lcm(a, b)`            | Minimo comun multiplo                             |
| `pi()`                 | Constante pi (3.14159...)                         |
| `inf()`                | Infinito positivo (float)                         |
| `is_nan(x)`            | True si x es NaN                                  |
| `is_inf(x)`            | True si x es infinito                             |

### Numeros aleatorios

| Funcion                      | Descripcion                                              |
|------------------------------|----------------------------------------------------------|
| `rand()`                     | Float aleatorio en [0.0, 1.0)                            |
| `rand_int(min, max)`         | Entero aleatorio en [min, max] (extremos incluidos)      |
| `rand_float(min, max)`       | Float aleatorio en [min, max)                            |
| `rand_choice(lista)`         | Elemento aleatorio de la lista                           |
| `rand_shuffle(lista)`        | Copia de la lista barajada aleatoriamente                |
| `rand_seed(n)`               | Inicializar el generador con semilla n (reproducible)    |

```vsh
println(rand())                      // 0.73812...
println(rand_int(1, 6))              // dado: 1..6
println(rand_float(0.0, 100.0))      // 42.71...
println(rand_choice([10, 20, 30]))   // 20
let deck = rand_shuffle(range(52))
rand_seed(42)                        // resultados reproducibles
```

### Sistema de ficheros — consulta

| Funcion                    | Descripcion                                        |
|----------------------------|----------------------------------------------------|
| `exists(ruta)`             | True si la ruta existe                             |
| `is_file(ruta)`            | True si es un fichero regular                      |
| `is_dir(ruta)`             | True si es un directorio                           |
| `basename(ruta)`           | Nombre del fichero con extension                   |
| `dirname(ruta)`            | Directorio padre                                   |
| `stem(ruta)`               | Nombre sin extension                               |
| `extension(ruta)`          | Extension (incluye el punto)                       |
| `abspath(ruta)`            | Ruta absoluta normalizada                          |
| `normpath(ruta)`           | Normaliza `.` y `..` sin resolver symlinks         |
| `join_path(a, b, ...)`     | Une componentes de ruta con el separador del SO    |
| `file_size(ruta)`          | Tamano del fichero en bytes (int)                  |
| `glob(patron)`             | Lista de rutas que coinciden con el patron         |
| `read_file(ruta)`          | Leer fichero a string; lanza si no existe          |
| `write_file(ruta, texto)`  | Escribir string en fichero (sobreescribe)          |

### Sistema de ficheros — navegacion y mutacion

| Funcion                        | Descripcion                                              |
|--------------------------------|----------------------------------------------------------|
| `getcwd()`                     | Devuelve el directorio de trabajo actual como string     |
| `chdir(ruta)`                  | Cambia el directorio de trabajo actual                   |
| `listdir([ruta])`              | Lista los nombres de entradas en `ruta` (defecto `.`)   |
| `listdir_full([ruta])`         | Como `listdir` pero devuelve rutas completas             |
| `mkdir(ruta)`                  | Crea un directorio (un solo nivel)                       |
| `makedirs(ruta)`               | Crea el arbol de directorios completo (como `mkdir -p`) |
| `rmdir(ruta)`                  | Elimina un directorio vacio o un fichero                 |
| `remove_file(ruta)`            | Elimina un fichero                                       |
| `remove_all(ruta)`             | Elimina un arbol de directorios completo (recursivo)     |
| `rename_path(origen, destino)` | Renombra o mueve un fichero o directorio                 |
| `copy_file(origen, destino)`   | Copia un fichero (sobreescribe destino si existe)        |

**Ejemplos:**

```vsh
// Directorio actual y navegacion
let cwd = getcwd()
println("Estoy en: " + cwd)
chdir("..")
println("Ahora en: " + getcwd())

// Listar directorio
let entradas = listdir(".")          // ["build", "main.cpp", ...]
let rutas    = listdir_full("src")   // ["src/foo.cpp", "src/bar.h", ...]

// Filtrar solo ficheros
let ficheros: list = []
for nombre in listdir(".") {
    if (is_file(nombre)) {
        append(ficheros, nombre)
    }
}
println(ficheros)

// Rutas
let ruta = join_path("build", "out", "programa.velb")  // build/out/programa.velb
println(abspath("../config"))                           // ruta absoluta
println(normpath("a/b/../c"))                           // a/c

// Crear y eliminar directorios
makedirs("build/out/ir")
copy_file("src/main.vel", "build/main.vel")
rename_path("build/viejo.vel", "build/nuevo.vel")
remove_all("build/tmp")

// Tamano de fichero
println(file_size("programa.velb") + " bytes")
```

> **Nota sobre `append` y variables de bucle:**
> El `for var in lista` define `var` en un scope hijo. Si tienes una lista
> acumuladora con el mismo nombre, sera ocultada dentro del bucle.
> Usa nombres distintos para el acumulador y la variable de iteracion:
>
> ```vsh
> // INCORRECTO: 'dir' es string dentro del for, no la lista
> let dir: list = []
> for dir in entradas { append(dir, ...) }  // error: dir es string
>
> // CORRECTO: nombres distintos
> let resultado: list = []
> for entrada in entradas {
>     if (is_file(entrada)) { append(resultado, entrada) }
> }
> ```

### Shell y proceso

| Funcion                    | Descripcion                                                       |
|----------------------------|-------------------------------------------------------------------|
| `shell(cmd)`               | Ejecutar comando; devuelve stdout+stderr como string              |
| `shell_ex(cmd)`            | Ejecutar comando; devuelve `{"output": str, "code": int}`         |
| `sleep(ms)`                | Pausar N milisegundos                                             |
| `exit([codigo])`           | Salir del proceso con codigo de salida                            |
| `pid()`                    | PID del proceso actual                                            |
| `cpu_count()`              | Numero de CPUs logicas disponibles                                |
| `platform()`               | `"windows"`, `"linux"` o `"macos"`                               |
| `getenv(nombre)`           | Valor de la variable de entorno (null si no existe)               |
| `setenv(nombre, valor)`    | Asignar variable de entorno                                       |

```vsh
// Obtener codigo de salida de un comando
let res = shell_ex("vestad -h")
println(res["output"])
println("Codigo: " + res["code"])   // 0 = exito

// Plataforma y entorno
println(platform())                  // "windows"
println(getenv("PATH"))
setenv("MY_VAR", "hola")

// Informacion del proceso
println("PID: " + pid())
println("CPUs: " + cpu_count())
```

### Tiempo

| Funcion              | Descripcion                                                     |
|----------------------|-----------------------------------------------------------------|
| `time_ms()`          | Timestamp Unix en milisegundos (int)                            |
| `time_s()`           | Timestamp Unix en segundos (float)                              |
| `time_now([fmt])`    | Fecha/hora actual formateada (defecto: `"%Y-%m-%d %H:%M:%S"`)  |

```vsh
let t0 = time_ms()
// ... operacion ...
println("Tardo: " + (time_ms() - t0) + " ms")

println(time_now())                        // "2026-04-26 14:30:00"
println(time_now("%d/%m/%Y"))              // "26/04/2026"
println(time_now("%H:%M"))                 // "14:30"
```

### Formato y serializacion

| Funcion              | Descripcion                                                   |
|----------------------|---------------------------------------------------------------|
| `format(tmpl, ...)` | Plantilla con `{}` o `{0}`, `{1}`... sustituidos por args    |
| `json_str(v)`        | Serializar valor a JSON (null/bool/int/float/str/list/map)    |

```vsh
println(format("Hola {} en {}", "mundo", 2026))   // "Hola mundo en 2026"
println(format("{0} + {0} = {1}", 3, 6))           // "3 + 3 = 6"

let datos = {"nombre": "Vesta", "version": 2, "activo": true}
println(json_str(datos))
// {"nombre":"Vesta","version":2,"activo":true}

let lista = [1, 2, [3, 4], null, false]
println(json_str(lista))
// [1,2,[3,4],null,false]
```

### Utilidades

| Funcion                   | Descripcion                                              |
|---------------------------|----------------------------------------------------------|
| `assert(cond [, msg])`    | Lanzar error si cond es falso                            |
| `error(msg)`              | Lanzar VshRuntimeError con msg                           |
| `doc(fn_o_clase)`         | Docstring de funcion o clase (o `""`)                    |
| `help(fn_o_clase)`        | Imprime firma + docstring; devuelve el texto como string |

---

## Sockets y red

VestaShell incluye soporte multiplataforma para TCP, UDP y TLS, ademas de
funciones de alto nivel para HTTP/HTTPS. Todas las funciones de socket devuelven
un **handle** (entero) que identifica la conexion. Siempre cierra el socket con
`socket_close(h)` cuando ya no lo necesites.

### TCP

```vsh
// Conectar a un servidor TCP
let h = tcp_connect("httpbin.org", 80)
let sent = socket_send(h, "GET / HTTP/1.0\r\nHost: httpbin.org\r\n\r\n")
let resp = socket_recv_all(h)    // leer hasta EOF
socket_close(h)

// Recibir en chunks (loop manual)
let chunk = socket_recv(h, 4096)
while len(chunk) > 0 {
    procesar(chunk)
    chunk = socket_recv(h, 4096)
}
```

```vsh
// Crear servidor TCP (acepta una conexion sincrona)
let srv = tcp_listen(8080)          // escuchar en puerto 8080
let srv2 = tcp_listen(0)            // puerto dinamico asignado por el SO
let client = tcp_accept(srv)        // bloquea hasta conexion entrante
let data = socket_recv_all(client)
socket_send(client, "HTTP/1.0 200 OK\r\n\r\nHola!")
socket_close(client)
socket_close(srv)
```

| Funcion | Descripcion |
|---------|-------------|
| `tcp_connect(host, port)` | Conectar TCP; devuelve handle |
| `tcp_listen(port [, backlog])` | Crear socket servidor; `port=0` -> SO asigna puerto libre |
| `tcp_accept(handle)` | Aceptar conexion entrante (bloqueante); devuelve handle cliente |
| `socket_send(h, datos)` | Enviar string; devuelve bytes enviados |
| `socket_recv(h [, maxlen])` | Recibir hasta `maxlen` bytes (defecto 4096); `""` = EOF |
| `socket_recv_all(h)` | Leer hasta EOF y devolver todo como string |
| `socket_close(h)` | Cerrar socket y liberar recursos |

### UDP

```vsh
// Enviar datagram UDP
let u = udp_socket()
let n = udp_sendto(u, "payload", "127.0.0.1", 9)   // port 9 = discard
socket_close(u)

// Receptor UDP (bind + recvfrom)
let u = udp_socket()
udp_bind(u, 5000)                           // escuchar en puerto 5000
let r = udp_recvfrom(u, 1024)              // [datos, ip_remota, puerto_remoto]
println("de " + r[1] + ":" + str(r[2]))
println("datos: " + r[0])
socket_close(u)
```

| Funcion | Descripcion |
|---------|-------------|
| `udp_socket()` | Crear socket UDP; devuelve handle |
| `udp_bind(h, port)` | Enlazar a puerto local |
| `udp_sendto(h, datos, host, port)` | Enviar datagram; devuelve bytes enviados |
| `udp_recvfrom(h [, maxlen])` | Recibir datagram; devuelve `[datos, ip, puerto]` |

### TLS

```vsh
// Conexion TLS manual (equivale a HTTPS raw)
let h = tls_connect("example.com", 443)
socket_send(h, "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n")
let resp = socket_recv_all(h)
socket_close(h)
```

| Funcion | Descripcion |
|---------|-------------|
| `tls_connect(host, port)` | Conexion TCP + TLS handshake; devuelve handle cifrado |

Los handles TLS son compatibles con `socket_send`, `socket_recv`,
`socket_recv_all` y `socket_close`.

### HTTP / HTTPS de alto nivel

Estas funciones gestionan la conexion, peticion y cierre automaticamente y
devuelven directamente el **cuerpo** de la respuesta como string.

```vsh
// GET
let html  = http_get("http://example.com/api/users")
let json  = https_get("https://httpbin.org/get")

// POST
let resp  = http_post("http://api.example.com/items", "nombre=VestaShell&v=1")
let resp2 = https_post("https://httpbin.org/post", '{"x":1}', "application/json")

// PUT
let resp3 = http_put("http://api.example.com/items/1", "nombre=Nuevo")
let resp4 = https_put("https://httpbin.org/put", '{"y":2}', "application/json")

// DELETE
let resp5 = http_delete("http://api.example.com/items/1")
let resp6 = https_delete("https://httpbin.org/delete")
```

#### `http_request` — API generica

```vsh
let r = http_request("PATCH", "https://httpbin.org/patch",
                     '{"campo":"valor"}', "application/json")
// r es un mapa con tres claves:
println(str(r["status"]))   // ej: 200
println(r["headers"])       // cabeceras HTTP de la respuesta
println(r["body"])          // cuerpo de la respuesta
```

| Funcion | Firma | Descripcion |
|---------|-------|-------------|
| `http_get(url)` | `(url) -> str` | HTTP GET; devuelve cuerpo |
| `https_get(url)` | `(url) -> str` | HTTPS GET; devuelve cuerpo |
| `http_post(url, body [, ct])` | `-> str` | HTTP POST |
| `https_post(url, body [, ct])` | `-> str` | HTTPS POST |
| `http_put(url, body [, ct])` | `-> str` | HTTP PUT |
| `https_put(url, body [, ct])` | `-> str` | HTTPS PUT |
| `http_delete(url)` | `-> str` | HTTP DELETE |
| `https_delete(url)` | `-> str` | HTTPS DELETE |
| `http_request(method, url [, body [, ct]])` | `-> map` | Peticion generica; devuelve `{status, headers, body}` |

El parametro `ct` es el `Content-Type` (defecto: `application/x-www-form-urlencoded`
para las funciones especializadas; `application/json` para `http_request`).

### Ejemplo completo: scraping HTTPS

```vsh
// Obtener IP publica via HTTPS
let r = https_get("https://httpbin.org/ip")
if contains(r, "origin") {
    println("IP publica detectada en la respuesta")
}

// Peticion con control total de estado
let resp = http_request("POST", "https://httpbin.org/post",
                        "usuario=admin&pass=1234",
                        "application/x-www-form-urlencoded")
if resp["status"] == 200 {
    println("POST exitoso")
}
```

### Ejemplo completo: servidor TCP echo

```vsh
// Servidor echo de una sola conexion en puerto 7777
let srv = tcp_listen(7777)
println("Escuchando en :7777 ...")
let cli = tcp_accept(srv)
let msg = socket_recv(cli, 1024)
socket_send(cli, msg)            // eco
socket_close(cli)
socket_close(srv)
```

---

## FFI: llamadas a librerias nativas

VestaShell puede cargar y llamar funciones de cualquier libreria dinamica del
sistema (`.dll` en Windows, `.so` en Linux) sin necesidad de compilar un plugin.
Es la forma de acceder a la Win32 API, librerias C de sistema o cualquier ABI
nativo desde un script `.vsh`.

### Funciones FFI

| Funcion                            | Descripcion                                                       |
|------------------------------------|-------------------------------------------------------------------|
| `ffi_open(ruta)`                   | Carga la libreria y devuelve un handle (int)                      |
| `ffi_sym(handle, nombre)`          | Busca el simbolo en la libreria; devuelve su direccion como int   |
| `ffi_call(sym [, args...])`        | Llama a la funcion; retorna int64. Args: int/str/float/bool/null  |
| `ffi_call_f(sym [, args...])`      | Igual que ffi_call pero retorna double                            |
| `ffi_close(handle)`                | Descarga la libreria                                              |
| `ffi_call_sym(ruta, nom [, args])` | Atajo: open+sym+call+close en una sola expresion                  |
| `ffi_str(ptr_int)`                 | Lee un C-string desde un puntero retornado por ffi_call           |

### Tipos de argumentos

| Tipo VSH  | Como se pasa a la funcion nativa                                  |
|-----------|-------------------------------------------------------------------|
| `int`     | Directamente como `int64_t`                                       |
| `bool`    | `1` o `0` como `int64_t`                                         |
| `float`   | Bits IEEE-754 empaquetados en `int64_t` (convenio de registros)   |
| `string`  | Puntero `char*` al buffer interno (vivo durante la llamada)       |
| `null`    | `0` (puntero nulo)                                                |

> **Limite**: maximo 8 argumentos por llamada. Para funciones con mas parametros
> escribe un wrapper C fino y cargalo como FFI.

### Ejemplo: Win32 MessageBoxA

```vsh
// Mostrar un dialogo en Windows
let user32 = ffi_open("user32.dll")
let msgbox  = ffi_sym(user32, "MessageBoxA")
// HWND=0, texto, titulo, MB_OK=0
ffi_call(msgbox, 0, "Hola desde VestaShell!", "FFI Demo", 0)
ffi_close(user32)
```

### Ejemplo: GetTickCount (tiempo en ms desde el arranque)

```vsh
let r = ffi_call_sym("kernel32.dll", "GetTickCount")
echo("Uptime ms: " + str(r))
```

### Ejemplo: libc en Linux

```vsh
let libc = ffi_open("libc.so.6")
let strlen = ffi_sym(libc, "strlen")
let n = ffi_call(strlen, "hola mundo")  // str -> char*, retorna int
echo("longitud: " + str(n))
ffi_close(libc)
```

### Ejemplo: leer una string de retorno

```vsh
// GetCommandLineA devuelve un LPSTR (char*)
let ptr = ffi_call_sym("kernel32.dll", "GetCommandLineA")
echo("Linea de comandos: " + ffi_str(ptr))
```

---

## ANSI: colores y secuencias de escape

VestaShell activa automaticamente el soporte VT100 en Windows
(`ENABLE_VIRTUAL_TERMINAL_PROCESSING`) al iniciarse, por lo que los codigos
ANSI funcionan en `cmd.exe` y `PowerShell` sin configuracion adicional.

### Mapa de constantes ANSI

Disponible como variable global `ANSI`. Uso: `ANSI["RED"]`, `ANSI["RESET"]`, etc.

| Clave          | Secuencia         | Efecto                        |
|----------------|-------------------|-------------------------------|
| `RESET`        | `\033[0m`         | Restaura todo a defecto       |
| `BOLD`         | `\033[1m`         | Negrita                       |
| `DIM`          | `\033[2m`         | Tenue                         |
| `ITALIC`       | `\033[3m`         | Cursiva                       |
| `UNDERLINE`    | `\033[4m`         | Subrayado                     |
| `BLINK`        | `\033[5m`         | Parpadeo                      |
| `REVERSE`      | `\033[7m`         | Video inverso                 |
| `STRIKE`       | `\033[9m`         | Tachado                       |
| `BLACK`..`WHITE` | `\033[30m`..`37m` | Colores fg normales         |
| `BR_BLACK`..`BR_WHITE` | `\033[90m`..`97m` | Colores fg brillantes |
| `BG_BLACK`..`BG_WHITE` | `\033[40m`..`47m` | Colores de fondo      |
| `BG_BR_*`      | `\033[100m`..`107m` | Fondos brillantes           |
| `CLEAR`        | `\033[2J\033[H`   | Limpiar pantalla y mover cursor al inicio |
| `CLEAR_LINE`   | `\033[2K\r`       | Borrar la linea actual        |

### Funciones rapidas de color y estilo

Estas funciones aceptan un argumento opcional. Con argumento envuelven el texto
y añaden reset al final; sin argumento devuelven solo el codigo de escape.

```vsh
echo(red("Error critico"))          // texto en rojo con reset automatico
echo(green("OK"))                   // texto en verde
echo(bold("Titulo"))                // negrita
echo(yellow() + "Atencion" + ANSI["RESET"])  // solo el codigo de apertura
```

| Funcion     | Color/estilo     |
|-------------|-----------------|
| `red(s?)`   | Rojo fg         |
| `green(s?)` | Verde fg        |
| `yellow(s?)`| Amarillo fg     |
| `blue(s?)`  | Azul fg         |
| `magenta(s?)`| Magenta fg     |
| `cyan(s?)`  | Cian fg         |
| `white(s?)` | Blanco fg       |
| `bold(s?)`  | Negrita         |
| `dim(s?)`   | Tenue           |
| `italic(s?)`| Cursiva         |
| `underline(s?)`| Subrayado    |
| `strike(s?)`| Tachado         |

### Funciones de color avanzadas

| Funcion                              | Descripcion                                                  |
|--------------------------------------|--------------------------------------------------------------|
| `colorize(texto, fg [, bg [, bold]])` | Envuelve el texto con fg, bg opcional y negrita opcional    |
| `ansi_rgb(r, g, b)`                  | Codigo fg en True Color (24 bits)                           |
| `ansi_rgb_bg(r, g, b)`               | Codigo bg en True Color (24 bits)                           |
| `ansi_code(n)`                       | Codigo arbitrario `\033[{n}m`                               |
| `ansi_enable()`                      | Activa VT100 en Windows; no-op en Linux                     |
| `strip_ansi(texto)`                  | Elimina todas las secuencias ANSI de un string              |

### Control de cursor

| Funcion                    | Descripcion                              |
|----------------------------|------------------------------------------|
| `ansi_cursor_up(n?)`       | Mueve el cursor N lineas arriba          |
| `ansi_cursor_down(n?)`     | Mueve el cursor N lineas abajo           |
| `ansi_cursor_left(n?)`     | Mueve el cursor N columnas a la izquierda |
| `ansi_cursor_right(n?)`    | Mueve el cursor N columnas a la derecha  |
| `ansi_cursor_pos(fila,col)`| Posiciona el cursor en (fila, col)       |
| `ansi_clear()`             | Limpia la pantalla                       |
| `ansi_clear_line()`        | Borra la linea actual                    |

### Ejemplos

```vsh
// Barra de progreso simple
fn progreso(pct: int) {
    let lleno  = pct / 5
    let vacio  = 20 - lleno
    let barra  = green(repeat("#", lleno)) + dim(repeat("-", vacio))
    echo("\r[" + barra + "] " + str(pct) + "%")
}

// Tabla coloreada
echo(bold("Nombre")     + "  " + bold("Estado"))
echo(cyan("servidor-01") + "  " + green("OK"))
echo(cyan("servidor-02") + "  " + red("FALLO"))

// True Color
echo(ansi_rgb(255, 128, 0) + "Naranja" + ANSI["RESET"])

// colorize con fondo
echo(colorize("AVISO", "black", "yellow", true))

// Limpiar consola y escribir en posicion fija
echo(ansi_clear())
echo(ansi_cursor_pos(5, 10) + "Texto en fila 5 col 10")

// Eliminar escapes para guardar en fichero de log limpio
let log_line = red("[ERROR]") + " mensaje de error"
write_file("app.log", strip_ansi(log_line) + "\n")
```

---

## REPL interactivo VestaShell

```bash
./vm --interprete
```

Abre un shell interactivo de VestaShell al estilo Python:

```
VestaShell REPL interactivo - escribe 'exit' o 'quit' para salir
>>> let x = 5
>>> x * 3
15
>>> fn suma(a, b) { return a + b }
>>> suma(2, 3)
5
>>> class Punto {
...     fn __init__(self, x, y) { self.x = x; self.y = y }
... }
>>> let p = Punto(1, 2)
>>> p.x
1
>>> exit
```

Comportamiento:
- Las expresiones simples imprimen su valor (si no es `null`) como `repr`.
- Los bloques multilinea se detectan contando `{` / `}`.
- El prompt cambia a `... ` cuando se espera continuacion.
- Los errores de sintaxis y ejecucion se muestran sin terminar el REPL.
- Las definiciones (`fn`, `class`, `let`) persisten durante toda la sesion.
- `doc(fn)` y `help(fn)` son especialmente utiles en el REPL para explorar el API.

---

## Integracion con el REPL

Cuando el script se ejecuta desde dentro del REPL con `script archivo.vsh`,
todos los comandos del REPL son accesibles como si fueran funciones:

```vsh
// Compilar y ejecutar un programa Vesta desde un script .vsh
build("src/main.vel", "-o", "programa.velb")
run("mi-app", "programa.velb", "--schedulers", "4")

// Navegar el sistema de ficheros
cd("src/")
ls("*.vel")
pwd()

// Ver el estado de las VMs
vms()
```

Los argumentos se pasan como strings y se reensamblan en la linea de comando
antes de enviarse al despachador del REPL.

---

## Comentarios y continuacion de linea

```vsh
// Comentario de linea: desde // hasta el fin de linea

/* Comentario de bloque:
   puede abarcar multiples lineas. */

let valor = 1 + 2 + \
            3 + 4    // barra invertida continua la sentencia
```

Los comentarios `//` y `/* */` son compatibles con el preprocesador de Vesta,
lo que permite usar directivas como `#include` en ficheros `.vsh` procesados
por `vpp` antes de interpretarlos.

---

## Convenios de estilo

- Indentacion: 4 espacios (recomendado).
- Nombres de variables y funciones: `snake_case`.
- Constantes globales: `UPPER_SNAKE_CASE`.
- Terminadores de sentencia: NEWLINE o `;` (ambos equivalentes).
- Los bloques `{}` siempre en la misma linea que la palabra clave que los abre.

---

## Ejemplos

Ver la carpeta `examples_codes_vsh/` del repositorio para scripts de ejemplo
completos que cubren toda la sintaxis y los casos de uso tipicos:

| Fichero                              | Contenido                                              |
|--------------------------------------|--------------------------------------------------------|
| `01_tipos_basicos.vsh`               | Tipos, variables, operadores                           |
| `02_strings.vsh`                     | Strings, interpolacion, builtins                       |
| `03_control_flujo.vsh`               | if/elif/else, while, for/in, range                     |
| `04_funciones.vsh`                   | Funciones, closures, orden superior, docstrings        |
| `05_listas_mapas.vsh`                | Listas, mapas, indexado, iteracion                     |
| `06_errores.vsh`                     | try/catch, error(), assert()                           |
| `07_matematicas.vsh`                 | Builtins matematicos                                   |
| `08_archivos.vsh`                    | Sistema de ficheros                                    |
| `09_lib_utils.vsh` + `09_import.vsh` | Libreria y sistema de importacion                      |
| `10_clases.vsh`                      | Clases, herencia, POO, isinstance, classname, help     |
| `11_errores_tipados.vsh`             | Errores personalizados, throw, catch tipado, jerarquia |
| `12_tipado.vsh`                      | Anotaciones de tipo en let y parametros de funcion     |
| `13_sockets.vsh`                     | TCP, UDP, TLS, HTTP/HTTPS GET/POST/PUT/DELETE          |
| `14_ffi.vsh`                         | Llamadas a librerias nativas, Win32 API, ffi_call      |
| `15_ansi.vsh`                        | Colores, estilos, cursor, True Color, progreso         |

Ejecutar la suite completa (desde el directorio raiz del proyecto):

```bash
for f in examples_codes_vsh/0[1-9]_*.vsh examples_codes_vsh/1[0-9]_*.vsh; do
    ./build/vm.exe --script "$f"
done
```
