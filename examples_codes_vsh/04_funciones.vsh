// VestaShell - 04: Funciones, closures y funciones de orden superior
// Cubre: fn nombrada, return, recursion, funciones anonimas,
//        closures, funciones como valores, funciones de orden superior,
//        docstrings con help() y doc().

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

println("=== 04: Funciones y closures ===")

// ---- funcion basica ----
fn suma(a, b) {
    "Devuelve la suma de a y b."
    return a + b
}
check("fn basica", suma(3, 4) == 7)
check("fn con floats", suma(1.5, 2.5) == 4.0)

// ---- funcion sin return explicito devuelve null ----
fn sin_return() {
    let x = 1
}
check("fn sin return -> null", is_null(sin_return()))

// ---- return anticipado ----
fn valor_absoluto(x) {
    "Devuelve el valor absoluto de x."
    if x < 0 {
        return -x
    }
    return x
}
check("return anticipado negativo", valor_absoluto(-5) == 5)
check("return anticipado positivo", valor_absoluto(3)  == 3)

// ---- recursion ----
fn factorial(n) {
    "Calcula n! de forma recursiva. factorial(0) = 1."
    if n <= 1 { return 1 }
    return n * factorial(n - 1)
}
check("recursion factorial 0", factorial(0) == 1)
check("recursion factorial 5", factorial(5) == 120)
check("recursion factorial 10", factorial(10) == 3628800)

// ---- recursion mutua via variables ----
fn es_par(n) {
    if n == 0 { return true }
    return es_impar(n - 1)
}
fn es_impar(n) {
    if n == 0 { return false }
    return es_par(n - 1)
}
check("recursion mutua par 4", es_par(4))
check("recursion mutua impar 7", es_impar(7))

// ---- fibonacci iterativo ----
fn fib(n) {
    "Devuelve el n-esimo numero de Fibonacci (0-indexado)."
    if n <= 1 { return n }
    let a = 0
    let b = 1
    let i = 2
    while i <= n {
        let tmp = a + b
        a = b
        b = tmp
        i = i + 1
    }
    return b
}
check("fib(0)", fib(0) == 0)
check("fib(1)", fib(1) == 1)
check("fib(10)", fib(10) == 55)

// ---- funciones anonimas ----
let doble = fn(x) { return x * 2 }
check("fn anonima", doble(21) == 42)

let cuadrado = fn(x) { return x ** 2 }
check("fn anonima potencia", cuadrado(7) == 49)

// ---- funciones como valores: pasar como argumento ----
fn aplicar(f, x) {
    "Aplica la funcion f al valor x y devuelve el resultado."
    return f(x)
}
check("funcion como argumento doble", aplicar(doble, 5) == 10)
check("funcion como argumento cuadrado", aplicar(cuadrado, 4) == 16)

// ---- funciones como valores: devolver funcion ----
fn multiplicador(factor) {
    "Devuelve una closure que multiplica su argumento por factor."
    return fn(x) { return x * factor }
}
let por3 = multiplicador(3)
let por7 = multiplicador(7)
check("closure factor 3", por3(10) == 30)
check("closure factor 7", por7(10) == 70)
check("closures independientes", por3(2) != por7(2))

// ---- closure captura entorno ----
fn contador() {
    "Devuelve una closure que incrementa y devuelve un contador privado."
    let n = 0
    return fn() {
        n = n + 1
        return n
    }
}
let c1 = contador()
let c2 = contador()
check("closure contador c1 1er", c1() == 1)
check("closure contador c1 2do", c1() == 2)
check("closure contador c2 independiente", c2() == 1)
check("closure contador c1 3er", c1() == 3)

// ---- map/filter manual via funcion de orden superior ----
fn mi_map(lista, f) {
    "Aplica f a cada elemento de lista y devuelve una nueva lista con los resultados."
    let resultado = []
    for elem in lista {
        append(resultado, f(elem))
    }
    return resultado
}
fn mi_filter(lista, pred) {
    "Devuelve una nueva lista con los elementos de lista para los que pred devuelve true."
    let resultado = []
    for elem in lista {
        if pred(elem) {
            append(resultado, elem)
        }
    }
    return resultado
}
fn mi_reduce(lista, f, inicial) {
    "Reduce lista a un unico valor aplicando f(acumulador, elemento) de izquierda a derecha."
    let acc = inicial
    for elem in lista {
        acc = f(acc, elem)
    }
    return acc
}

let nums = [1, 2, 3, 4, 5]
let dobles = mi_map(nums, fn(x) { return x * 2 })
check("map dobles len", len(dobles) == 5)
check("map dobles[0]", dobles[0] == 2)
check("map dobles[4]", dobles[4] == 10)

let pares_filtro = mi_filter(nums, fn(x) { return x % 2 == 0 })
check("filter pares len", len(pares_filtro) == 2)
check("filter pares[0]", pares_filtro[0] == 2)

let suma_total = mi_reduce(nums, fn(acc, x) { return acc + x }, 0)
check("reduce suma", suma_total == 15)

// ---- funciones almacenadas en mapa ----
let ops = {
    "add": fn(a, b) { return a + b },
    "sub": fn(a, b) { return a - b },
    "mul": fn(a, b) { return a * b },
}
check("fn en mapa add", ops["add"](10, 3) == 13)
check("fn en mapa sub", ops["sub"](10, 3) == 7)
check("fn en mapa mul", ops["mul"](10, 3) == 30)

// ---- funciones variadicas simuladas con lista ----
fn suma_lista(nums) {
    "Suma todos los elementos de la lista nums."
    let total = 0
    for n in nums {
        total = total + n
    }
    return total
}
check("fn recibe lista", suma_lista([1,2,3,4,5]) == 15)

/* ---- docstrings: help() y doc() ----
   La primera sentencia de una funcion puede ser un string literal:
   ese string se convierte en la documentacion de la funcion,
   accesible mediante doc(fn) o help(fn).                         */
check("doc() de factorial", len(doc(factorial)) > 0)
check("doc() de suma",      doc(suma) == "Devuelve la suma de a y b.")
check("doc() de mi_map",    starts_with(doc(mi_map), "Aplica f a cada"))

// help() imprime la firma y el docstring; tambien devuelve el texto
let h = help(factorial)
check("help() devuelve string no vacio", len(h) > 0)

// funciones sin docstring devuelven string vacio en doc()
check("doc() sin docstring -> vacio", doc(sin_return) == "")

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
