// VestaShell - 09: Sistema de importacion
// Cubre: import de libreria .vsh, uso de funciones importadas,
//        uso de variables exportadas, deteccion de importacion circular.

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

println("=== 09: Importacion de modulos ===")

import "examples_codes_vsh/09_lib_utils.vsh"

// ---- funciones de la libreria importada ----
check("suma_lista",      suma_lista([1,2,3,4,5]) == 15)
check("producto_lista",  producto_lista([2,3,4]) == 24)

let inv = invertir_lista([1,2,3,4,5])
check("invertir_lista len", len(inv) == 5)
check("invertir_lista [0]", inv[0] == 5)
check("invertir_lista [4]", inv[4] == 1)

// ---- aplana ----
let anidada = [[1,2], [3,4], [5,6]]
let plana = aplana(anidada)
check("aplana len", len(plana) == 6)
check("aplana primer elem", plana[0] == 1)
check("aplana ultimo elem", plana[5] == 6)

// ---- unico ----
let con_dupes = [1, 2, 2, 3, 1, 4, 3]
let sin_dupes = unico(con_dupes)
check("unico len", len(sin_dupes) == 4)
check("unico primer", sin_dupes[0] == 1)

// ---- zip ----
let nombres = ["Ana", "Bob", "Carlos"]
let edades  = [25, 30, 22]
let pares = zip(nombres, edades)
check("zip len", len(pares) == 3)
check("zip [0][0]", pares[0][0] == "Ana")
check("zip [0][1]", pares[0][1] == 25)
check("zip [2][0]", pares[2][0] == "Carlos")

// ---- palindromo ----
check("palindromo 'radar'",   cadena_paloma("radar"))
check("palindromo 'level'",   cadena_paloma("level"))
check("palindromo 'a'",       cadena_paloma("a"))
check("palindromo '' vacio",  cadena_paloma(""))
check("no palindromo 'hola'", not cadena_paloma("hola"))
check("no palindromo 'abc'",  not cadena_paloma("abc"))

// ---- formatear_tabla ----
let tabla = formatear_tabla([["Ana", 25], ["Bob", 30]], " | ")
check("tabla contiene Ana", contains(tabla, "Ana"))
check("tabla contiene 30", contains(tabla, "30"))
check("tabla separador |", contains(tabla, " | "))

// ---- constantes exportadas ----
check("PI_APROX existe", PI_APROX > 3.14 and PI_APROX < 3.15)
check("E_APROX existe",  E_APROX  > 2.71 and E_APROX  < 2.72)

/* La misma importacion dos veces no duplica el estado de forma inconsistente:
   el interprete permite re-importar (redefine las funciones); comprobamos
   que el comportamiento se mantiene igual antes y despues.               */
let suma_antes = suma_lista([10, 20])
import "examples_codes_vsh/09_lib_utils.vsh"
let suma_despues = suma_lista([10, 20])
check("reimport no rompe funciones", suma_antes == suma_despues)

// ---- error en import de archivo inexistente ----
let fallo_import = false
try {
    import "ejemplos_que_no_existen_99999.vsh"
} catch e {
    fallo_import = true
}
check("import no existente lanza error", fallo_import)

// ---- los docstrings de funciones importadas son accesibles ----
check("doc() de suma_lista importada", len(doc(suma_lista)) > 0)
check("doc() de cadena_paloma importada", starts_with(doc(cadena_paloma), "Devuelve true"))

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
