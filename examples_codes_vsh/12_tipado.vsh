// VestaShell - 12: Anotaciones de tipo opcionales
// Cubre: let x: tipo = valor, fn f(a: int, b: str), error de tipo en asignacion,
//        error de tipo en llamada, mezcla de params tipados y no tipados.

let pass = 0
let fail = 0

fn check(nombre, cond) {
    if cond {
        pass = pass + 1
        println("  [PASS] " + nombre)
    } else {
        fail = fail + 1
        println("  [FAIL] " + nombre)
    }
}

println("=== 12: Tipado opcional ===")

// ---- let con tipo correcto ----
let n: int   = 42
let s: str   = "hola"
let f: float = 3.14
let b: bool  = true
let lst: list = [1, 2, 3]
let mp: map   = {"a": 1}

check("let int",   n == 42)
check("let str",   s == "hola")
check("let float", f == 3.14)
check("let bool",  b == true)
check("let list",  len(lst) == 3)
check("let map",   len(mp) == 1)

// ---- let con tipo incorrecto lanza error ----
let tipo_fallo = false
try {
    let x: int = "no soy un int"
} catch e {
    tipo_fallo = true
}
check("let tipo incorrecto lanza error", tipo_fallo)

let float_fallo = false
try {
    let y: float = true
} catch e {
    float_fallo = true
}
check("let float <- bool lanza error", float_fallo)

// ---- let sin tipo: no hay restriccion ----
let libre = "cualquier cosa"
libre = 42
libre = true
check("let sin tipo acepta cualquier valor", libre == true)

// ---- parametros tipados en funciones ----
fn doble(x: int) {
    "Devuelve el doble de un entero."
    return x * 2
}

fn concatenar(a: str, b: str) {
    "Concatena dos strings."
    return a + b
}

fn mezcla(nombre: str, edad) {
    // 'edad' sin tipo: acepta cualquier valor
    return nombre + " tiene " + str(edad)
}

check("doble(5)",        doble(5) == 10)
check("concatenar",      concatenar("ho", "la") == "hola")
check("mezcla str",      mezcla("Ana", 30) == "Ana tiene 30")
check("mezcla sin tipo", mezcla("Bob", "treinta") == "Bob tiene treinta")

// ---- error de tipo en parametro ----
let param_fallo = false
try {
    doble("no soy int")
} catch e {
    param_fallo = true
}
check("param tipado incorrecto lanza error", param_fallo)

let str_fallo = false
try {
    concatenar("ok", 99)
} catch e {
    str_fallo = true
}
check("param str <- int lanza error", str_fallo)

// ---- tipos de clase en parametros ----
class Punto {
    fn __init__(self, x, y) {
        self.x = x
        self.y = y
    }
}

fn distancia_orig(p: Punto) {
    "Distancia al origen (0,0) aproximada."
    return p.x * p.x + p.y * p.y
}

let pt = Punto(3, 4)
check("param clase Punto",  distancia_orig(pt) == 25)

let clase_fallo = false
try {
    distancia_orig("no es Punto")
} catch e {
    clase_fallo = true
}
check("param clase incorrecto lanza error", clase_fallo)

// ---- tipo list, map, fn en parametros ----
fn suma_lista_tipada(lst: list) {
    let t = 0
    for x in lst { t = t + x }
    return t
}

fn aplicar(f: fn, x) {
    return f(x)
}

check("param list",     suma_lista_tipada([1,2,3,4]) == 10)
check("param fn",       aplicar(fn(x){ return x * 3 }, 5) == 15)

let list_fallo = false
try {
    suma_lista_tipada("no soy lista")
} catch e {
    list_fallo = true
}
check("param list <- str lanza error", list_fallo)

// ---- tipo null ----
fn acepta_null(v: null) {
    return v == null
}
check("param null correcto",   acepta_null(null))

let null_fallo = false
try {
    acepta_null(0)
} catch e {
    null_fallo = true
}
check("param null <- int lanza error", null_fallo)

// ---- herencia en tipado: Punto3D pasa como Punto ----
class Punto3D : Punto {
    fn __init__(self, x, y, z) {
        self.x = x
        self.y = y
        self.z = z
    }
}

let p3 = Punto3D(1, 2, 3)
check("subclase pasa como clase padre en param", distancia_orig(p3) == 5)

// ---- tipo 'instance' acepta cualquier instancia ----
fn describe_instancia(obj: instance) {
    return classname(obj)
}
check("param instance cualquier clase", describe_instancia(pt) == "Punto")
check("param instance subclase",        describe_instancia(p3) == "Punto3D")

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
