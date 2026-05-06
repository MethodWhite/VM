// VestaShell - 03: Control de flujo
// Cubre: if/elif/else, while con break/continue,
//        for/in sobre lista y sobre string, range().

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

println("=== 03: Control de flujo ===")

// ---- if simple ----
let r = 0
if true {
    r = 1
}
check("if simple true", r == 1)

if false {
    r = 99
}
check("if simple false no ejecuta", r == 1)

// ---- if/else ----
let x = 10
let etiqueta = ""
if x > 5 {
    etiqueta = "grande"
} else {
    etiqueta = "pequeno"
}
check("if/else rama true", etiqueta == "grande")

x = 3
if x > 5 {
    etiqueta = "grande"
} else {
    etiqueta = "pequeno"
}
check("if/else rama false", etiqueta == "pequeno")

// ---- if/elif/else ----
fn clasifica(n) {
    if n < 0 {
        return "negativo"
    } elif n == 0 {
        return "cero"
    } elif n < 10 {
        return "pequeno"
    } elif n < 100 {
        return "mediano"
    } else {
        return "grande"
    }
}
check("elif negativo", clasifica(-5)   == "negativo")
check("elif cero",     clasifica(0)    == "cero")
check("elif pequeno",  clasifica(7)    == "pequeno")
check("elif mediano",  clasifica(42)   == "mediano")
check("elif grande",   clasifica(1000) == "grande")

// ---- while basico ----
let suma = 0
let i = 1
while i <= 5 {
    suma = suma + i
    i = i + 1
}
check("while suma 1..5", suma == 15)

// ---- while con break ----
let cnt = 0
let j = 0
while true {
    j = j + 1
    if j > 5 {
        break
    }
    cnt = cnt + 1
}
check("while break", cnt == 5)

// ---- while con continue ----
let solo_pares = 0
let k = 0
while k < 10 {
    k = k + 1
    if k % 2 != 0 {
        continue
    }
    solo_pares = solo_pares + 1
}
check("while continue solo pares", solo_pares == 5)

// ---- for/in lista ----
let nums = [10, 20, 30, 40, 50]
let total = 0
for n in nums {
    total = total + n
}
check("for/in lista suma", total == 150)

// ---- for/in con break ----
let primero_mayor_25 = 0
for n in nums {
    if n > 25 {
        primero_mayor_25 = n
        break
    }
}
check("for/in break", primero_mayor_25 == 30)

// ---- for/in con continue ----
let sin_30 = 0
for n in nums {
    if n == 30 {
        continue
    }
    sin_30 = sin_30 + n
}
check("for/in continue salta 30", sin_30 == 120)

// ---- range(n) ----
let suma_range = 0
for i in range(5) {
    suma_range = suma_range + i
}
check("range(5) sum = 0+1+2+3+4", suma_range == 10)

// ---- range(start, end) ----
let lista_range = []
for i in range(3, 7) {
    append(lista_range, i)
}
check("range(3,7) len", len(lista_range) == 4)
check("range(3,7) inicio", lista_range[0] == 3)
check("range(3,7) fin", lista_range[3] == 6)

// ---- range(start, end, step) ----
let pares = []
for i in range(0, 10, 2) {
    append(pares, i)
}
check("range step 2 len", len(pares) == 5)
check("range step 2 ultimo", pares[4] == 8)

// ---- range con step negativo ----
let desc = []
for i in range(5, 0, -1) {
    append(desc, i)
}
check("range step -1 len", len(desc) == 5)
check("range step -1 primer elem", desc[0] == 5)
check("range step -1 ultimo elem", desc[4] == 1)

// ---- for/in string ----
let letras = []
for c in "vesta" {
    append(letras, upper(c))
}
check("for/in string upper", join(letras, "") == "VESTA")

// ---- anidado: for dentro de while ----
let matriz_suma = 0
let fila = 0
while fila < 3 {
    for col in range(3) {
        matriz_suma = matriz_suma + fila * 3 + col
    }
    fila = fila + 1
}
check("bucles anidados", matriz_suma == 36)   // 0+1+2+3+4+5+6+7+8

// ---- condicion compuesta ----
let edad = 25
let activo = true
check("and en condicion", edad >= 18 and activo)
check("or en condicion", edad < 18 or activo)
check("not en condicion", not (edad < 18))

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
