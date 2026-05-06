// VestaShell - 06: Manejo de errores
// Cubre: try/catch, error(), assert(),
//        errores en operaciones (division por cero, indice fuera de rango,
//        variable no definida, tipo incorrecto).

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

println("=== 06: Manejo de errores ===")

// ---- try/catch basico ----
let capturado = false
try {
    error("error de prueba")
} catch e {
    capturado = true
}
check("try/catch captura error()", capturado)

// ---- mensaje del error llega al catch ----
let msg_error = ""
try {
    error("mensaje especifico")
} catch e {
    msg_error = e
}
check("catch recibe mensaje", msg_error == "mensaje especifico")

// ---- error no ocurre: catch no se ejecuta ----
let ejecuto_catch = false
try {
    let x = 1 + 1
} catch e {
    ejecuto_catch = true
}
check("catch no se ejecuta sin error", not ejecuto_catch)

// ---- error en acceso a variable no definida ----
let capto_undef = false
try {
    let y = variable_que_no_existe
} catch e {
    capto_undef = true
}
check("catch variable no definida", capto_undef)

// ---- error en indice fuera de rango ----
let capto_idx = false
try {
    let lista = [1, 2, 3]
    let x = lista[10]
} catch e {
    capto_idx = true
}
check("catch indice fuera de rango", capto_idx)

// ---- error en tipo incorrecto ----
let capto_tipo = false
try {
    let r = "texto" < 42    // tipos no comparables
} catch e {
    capto_tipo = true
}
check("catch comparacion tipos incompatibles", capto_tipo)

// ---- try/catch con recuperacion ----
fn division_segura(a, b) {
    "Divide a entre b; devuelve null si b es cero en lugar de lanzar error."
    try {
        if b == 0 {
            error("division por cero")
        }
        return a / b
    } catch e {
        return null
    }
}
check("division segura ok", division_segura(10, 2) == 5)
check("division segura por cero -> null", is_null(division_segura(10, 0)))

// ---- assert() con condicion verdadera no lanza ----
let assert_ok = false
try {
    assert(1 + 1 == 2, "la aritmetica esta rota")
    assert_ok = true
} catch e {
    assert_ok = false
}
check("assert(true) no lanza", assert_ok)

// ---- assert() con condicion falsa lanza ----
let assert_fallo = false
try {
    assert(1 + 1 == 3, "suma incorrecta")
} catch e {
    assert_fallo = (e == "suma incorrecta")
}
check("assert(false) lanza con mensaje", assert_fallo)

// ---- assert() sin mensaje usa mensaje por defecto ----
let assert_default = false
try {
    assert(false)
} catch e {
    assert_default = (len(e) > 0)
}
check("assert sin mensaje tiene texto por defecto", assert_default)

// ---- try/catch anidados ----
let nivel_externo = false
let nivel_interno = false
try {
    try {
        error("error interno")
    } catch e {
        nivel_interno = true
        // volver a lanzar si el mensaje no es el esperado
        if e != "error interno" {
            error("error inesperado: " + e)
        }
    }
    nivel_externo = true
} catch e {
    nivel_externo = false
}
check("try/catch anidado interno captura", nivel_interno)
check("try/catch anidado externo continua", nivel_externo)

// ---- try/catch relanza ----
let relanzado = false
try {
    try {
        error("original")
    } catch e {
        error("relanzado: " + e)
    }
} catch e {
    relanzado = starts_with(e, "relanzado:")
}
check("relanzar error desde catch", relanzado)

// ---- try en funcion: break/continue/return se propagan ----
fn busca_primero_positivo(lista) {
    "Devuelve el primer elemento >= 0 de la lista; ignora negativos."
    for n in lista {
        try {
            if n < 0 { error("negativo") }
            return n
        } catch e {
            // ignorar negativos, continuar
        }
    }
    return null
}
let resultado = busca_primero_positivo([-3, -1, 0, 5, 8])
check("try en loop: return se propaga", resultado == 0)

// ---- error() acepta cualquier valor convertido a string ----
let num_error = false
try {
    error("codigo " + str(404))
} catch e {
    num_error = (e == "codigo 404")
}
check("error con string compuesta", num_error)

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
