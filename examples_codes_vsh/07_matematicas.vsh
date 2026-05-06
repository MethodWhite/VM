// VestaShell - 07: Funciones matematicas
// Cubre: abs, min, max, floor, ceil, round, sqrt, pow, log.

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

/* Compara dos flotantes con tolerancia eps.
   Devuelve true si |a - b| < eps. */
fn aprox(a, b, eps) {
    "Devuelve true si |a - b| < eps (comparacion aproximada de flotantes)."
    let d = a - b
    if d < 0 { d = -d }
    return d < eps
}

println("=== 07: Matematicas ===")

// ---- abs ----
check("abs positivo", abs(5)    == 5)
check("abs negativo", abs(-7)   == 7)
check("abs cero",     abs(0)    == 0)
check("abs float neg", abs(-3.5) == 3.5)

// ---- min / max ----
check("min dos", min(3, 7) == 3)
check("max dos", max(3, 7) == 7)
check("min igual", min(5, 5) == 5)
check("min negativo", min(-2, 3) == -2)
check("max negativo", max(-2, -5) == -2)

// ---- floor / ceil / round ----
check("floor positivo", floor(3.7)  == 3.0)
check("floor negativo", floor(-3.2) == -4.0)
check("ceil positivo",  ceil(3.2)   == 4.0)
check("ceil negativo",  ceil(-3.7)  == -3.0)
check("round 3.4",      round(3.4)  == 3.0)
check("round 3.5",      round(3.5)  == 4.0)
check("round -2.5",     round(-2.5) == -2.0 or round(-2.5) == -3.0)  // banco o aritm.

// ---- sqrt ----
check("sqrt 4", aprox(sqrt(4.0), 2.0, 1e-9))
check("sqrt 9", aprox(sqrt(9.0), 3.0, 1e-9))
check("sqrt 2", aprox(sqrt(2.0), 1.41421356, 1e-6))
check("sqrt 0", aprox(sqrt(0.0), 0.0, 1e-9))

// ---- pow ----
check("pow 2^10",  pow(2.0, 10.0) == 1024.0)
check("pow 3^3",   pow(3.0,  3.0) == 27.0)
check("pow 0^0",   pow(0.0,  0.0) == 1.0)
check("pow 4^0.5", aprox(pow(4.0, 0.5), 2.0, 1e-9))

// ---- log ----
check("log(e) = 1",        aprox(log(2.718281828), 1.0, 1e-6))
check("log(1) = 0",        aprox(log(1.0), 0.0, 1e-9))
check("log(e^2) = 2",      aprox(log(pow(2.718281828, 2.0)), 2.0, 1e-6))

// ---- operador ** vs pow ----
check("** vs pow 2^8",  (2 ** 8) == 256)
check("** enteros", (3 ** 3) == 27)

// ---- combinaciones ----
let hipotenusa = sqrt(3.0**2 + 4.0**2)
check("hipotenusa 3-4-5", aprox(hipotenusa, 5.0, 1e-9))

// distancia euclidea entre dos puntos
fn distancia(x1, y1, x2, y2) {
    "Calcula la distancia euclidea entre los puntos (x1,y1) y (x2,y2)."
    return sqrt((x2-x1)**2 + (y2-y1)**2)
}
check("distancia euclidea", aprox(distancia(0,0,3,4), 5.0, 1e-9))

// ---- cuadrado y cubo ----
fn cuadrado(x) { return x ** 2 }
fn cubo(x)     { return x ** 3 }
check("cuadrado 6", cuadrado(6) == 36)
check("cubo 4", cubo(4) == 64)

// ---- serie de fibonacci con redondeo ----
fn fib_aprox(n) {
    "Aproxima el n-esimo Fibonacci usando la formula de Binet con phi."
    let phi = (1.0 + sqrt(5.0)) / 2.0
    return round(pow(phi, float(n)) / sqrt(5.0))
}
check("fibonacci aprox fib(10)", fib_aprox(10) == 55.0)

// ---- estadistica basica ----
fn media(lista) {
    "Calcula la media aritmetica de una lista de numeros."
    let s = 0.0
    for x in lista { s = s + float(x) }
    return s / float(len(lista))
}
let datos = [2, 4, 4, 4, 5, 5, 7, 9]
check("media", aprox(media(datos), 5.0, 1e-9))

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
