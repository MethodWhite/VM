// VestaShell - 08: Sistema de ficheros
// Cubre: write_file, read_file, exists, is_file, is_dir,
//        basename, dirname, stem, extension, glob.
// Nota: los ficheros temporales se crean en el directorio actual
//       y se borran al final via shell("del"/"rm").

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

println("=== 08: Sistema de ficheros ===")

let tmp1 = "vsh_test_08_a.tmp"
let tmp2 = "vsh_test_08_b.tmp"

// ---- write_file / read_file ----
write_file(tmp1, "linea uno\nlinea dos\nlinea tres")
check("write_file crea fichero", exists(tmp1))
check("write_file es fichero regular", is_file(tmp1))

let contenido = read_file(tmp1)
check("read_file devuelve contenido", contains(contenido, "linea uno"))
check("read_file contiene newlines", contains(contenido, "\n"))
let lineas = split(contenido, "\n")
check("read_file 3 lineas", len(lineas) == 3)
check("read_file primera linea", lineas[0] == "linea uno")
check("read_file ultima linea", lineas[2] == "linea tres")

// ---- sobreescribir fichero ----
write_file(tmp1, "nuevo contenido")
let nuevo = read_file(tmp1)
check("write_file sobreescribe", nuevo == "nuevo contenido")

// ---- exists para fichero no existente ----
check("exists no existente", not exists("archivo_que_no_existe_jamas_9999.tmp"))

// ---- is_dir en directorio conocido ----
// El directorio actual siempre existe
check("is_dir en directorio actual", is_dir("."))
check("is_file en directorio no es file", not is_file("."))

// ---- basename / dirname / stem / extension ----
let ruta = "src/modulos/parser.vel"
check("basename", basename(ruta) == "parser.vel")
check("dirname", dirname(ruta) == "src/modulos")
check("stem", stem(ruta) == "parser")
check("extension", extension(ruta) == ".vel")

let solo_nombre = "archivo.txt"
check("basename sin dir", basename(solo_nombre) == "archivo.txt")
check("dirname sin dir -> .", dirname(solo_nombre) == ".")
check("stem sin dir", stem(solo_nombre) == "archivo")
check("extension txt", extension(solo_nombre) == ".txt")

let sin_ext = "README"
check("stem sin extension", stem(sin_ext) == "README")
check("extension vacia", extension(sin_ext) == "")

// ---- glob: buscar ficheros .tmp creados ----
write_file(tmp2, "segundo temporal")
let tmps = glob("*.tmp")
check("glob *.tmp encuentra al menos 2", len(tmps) >= 2)

let nombres_tmp = []
for t in tmps {
    append(nombres_tmp, basename(t))
}
check("glob contiene tmp1", contains(nombres_tmp, tmp1))
check("glob contiene tmp2", contains(nombres_tmp, tmp2))

// ---- error al leer fichero no existente ----
let error_lectura = false
try {
    read_file("no_existe_9999.tmp")
} catch e {
    error_lectura = true
}
check("read_file no existente lanza error", error_lectura)

/* Limpieza: borrar temporales.
   Intentamos primero con 'del' (Windows) y si falla con 'rm -f' (Unix). */
try {
    shell("del " + tmp1 + " " + tmp2 + " 2>nul")
} catch e {
    try {
        shell("rm -f " + tmp1 + " " + tmp2)
    } catch e2 {
        println("  [AVISO] No se pudo borrar temporales: " + tmp1 + ", " + tmp2)
    }
}

check("fichero borrado", not exists(tmp1))

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
