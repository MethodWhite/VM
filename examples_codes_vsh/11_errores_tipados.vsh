// VestaShell - 11: Errores tipados y jerarquia de excepciones
// Cubre: clase Error incorporada, clases de error personalizadas,
//        throw, try/catch tipado (catch e : Tipo), relanzar,
//        jerarquia de herencia en catch.

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

println("=== 11: Errores tipados ===")

// ---- clase Error incorporada ----
check("Error es clase",   is_class(Error))

let e0 = Error("fallo base")
check("Error instancia",  is_instance(e0))
check("Error message",    e0.message == "fallo base")
check("classname Error",  classname(e0) == "Error")

// ---- errores personalizados ----
class ValorError : Error {
    "Error para valores invalidos."
    fn __init__(self, msg, valor) {
        self.message = msg
        self.valor   = valor
    }
}

class RangoError : ValorError {
    "Error para valores fuera de rango."
    fn __init__(self, msg, valor, minimo, maximo) {
        self.message = msg
        self.valor   = valor
        self.minimo  = minimo
        self.maximo  = maximo
    }
}

// ---- throw e instancias ----
let capturado = false
try {
    throw ValorError("valor negativo", -1)
} catch e {
    capturado = true
    check("catch sin tipo atrapa ValorError", is_instance(e))
    check("catch e.message",  e.message == "valor negativo")
    check("catch e.valor",    e.valor == -1)
}
check("throw ValorError capturado", capturado)

// ---- catch tipado exacto ----
let atrapado_ve = false
try {
    throw ValorError("tipo malo", 0)
} catch e : ValorError {
    atrapado_ve = true
    check("catch tipado ValorError message", e.message == "tipo malo")
}
check("catch tipado ValorError ejecutado", atrapado_ve)

// ---- catch tipado con clase padre atrapa hijos ----
let atrapado_base = false
try {
    throw RangoError("fuera de rango", 200, 0, 100)
} catch e : ValorError {
    atrapado_base = true
    check("catch ValorError atrapa RangoError message", e.message == "fuera de rango")
    check("catch ValorError atrapa RangoError valor",   e.valor == 200)
    check("catch tipado accede minimo",  e.minimo == 0)
    check("catch tipado accede maximo",  e.maximo == 100)
}
check("catch padre atrapa hijo", atrapado_base)

// ---- catch tipado NO atrapa tipo equivocado ----
class OtroError : Error {
    fn __init__(self, msg) { self.message = msg }
}

let paso_por_otro = false
let atrapado_otro = false
try {
    try {
        throw OtroError("otro tipo")
    } catch e : ValorError {
        paso_por_otro = true // no debe ejecutarse
    }
} catch e {
    atrapado_otro = true // debe llegar aqui
    check("catch generico atrapa no coincidente", classname(e) == "OtroError")
}
check("catch tipado no atrapa tipo incorrecto", not paso_por_otro)
check("error rethrown atrapado en catch generico", atrapado_otro)

// ---- jerarquia completa: Error <- ValorError <- RangoError ----
check("isinstance RangoError es ValorError", isinstance(RangoError("x",1,0,10), ValorError))
check("isinstance RangoError es Error",      isinstance(RangoError("x",1,0,10), Error))
check("isinstance ValorError es Error",      isinstance(ValorError("x",1), Error))

// ---- throw con objeto Error base ----
let capturado_error_base = false
try {
    throw Error("error de base")
} catch e : Error {
    capturado_error_base = true
    check("catch Error base message", e.message == "error de base")
}
check("catch tipado Error atrapa Error base", capturado_error_base)

// ---- throw un string simple (no instancia) ----
let capturado_string = false
try {
    throw "esto es un error de string"
} catch e {
    capturado_string = true
    check("catch string lanzado", e == "esto es un error de string")
}
check("throw string capturado", capturado_string)

// ---- funciones que lanzan errores tipados ----
fn dividir(a, b) {
    if b == 0 {
        throw ValorError("division por cero", b)
    }
    return a / b
}

let resultado_ok = false
let resultado_err = false

try {
    let r = dividir(10, 2)
    resultado_ok = (r == 5)
} catch e { }
check("dividir ok no lanza", resultado_ok)

try {
    dividir(5, 0)
} catch e : ValorError {
    resultado_err = true
    check("dividir lanza ValorError",   e.message == "division por cero")
    check("dividir lanza valor 0",      e.valor == 0)
}
check("dividir por cero capturado", resultado_err)

// ---- isinstance en catch para discriminar ----
fn lanzar_rango(v) {
    if v < 0 or v > 100 {
        throw RangoError("fuera de rango [0,100]", v, 0, 100)
    }
    return v
}

let era_rango = false
try {
    lanzar_rango(150)
} catch e : ValorError {
    era_rango = isinstance(e, RangoError)
    check("era RangoError dentro de catch ValorError", era_rango)
    check("rango minimo en error", e.minimo == 0)
    check("rango maximo en error", e.maximo == 100)
}
check("lanzar_rango capturado como ValorError", era_rango)

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
