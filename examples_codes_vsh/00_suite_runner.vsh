// VestaShell - Runner de suite de tests
// Ejecuta todos los ejemplos y reporta resultados globales.
// Uso: script examples_codes_vsh/00_suite_runner.vsh

let archivos = [
    "examples_codes_vsh/01_tipos_basicos.vsh",
    "examples_codes_vsh/02_strings.vsh",
    "examples_codes_vsh/03_control_flujo.vsh",
    "examples_codes_vsh/04_funciones.vsh",
    "examples_codes_vsh/05_listas_mapas.vsh",
    "examples_codes_vsh/06_errores.vsh",
    "examples_codes_vsh/07_matematicas.vsh",
    "examples_codes_vsh/08_archivos.vsh",
    "examples_codes_vsh/09_import.vsh",
]

let total_ok = 0
let total_fail = 0

println("========================================")
println("  VestaShell - Suite completa de tests")
println("========================================")

for archivo in archivos {
    println("")
    println("--- " + archivo + " ---")
    try {
        import archivo
    } catch e {
        println("[ERROR inesperado] " + e)
        total_fail += 1
    }
}

println("")
println("========================================")
println("  TOTAL: " + str(total_ok) + " modulos OK, " + str(total_fail) + " con error")
println("========================================")
