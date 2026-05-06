// VestaShell - 02: Strings e interpolacion
// Cubre: string literals, interpolacion ${}, concatenacion,
//        builtins de string: len, upper, lower, trim, split, join,
//        starts_with, ends_with, replace, substr, contains.

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

println("=== 02: Strings e interpolacion ===")

// ---- concatenacion y len ----
let s = "Hola"
let t = " mundo"
check("concatenacion", s + t == "Hola mundo")
check("len string", len("abc") == 3)
check("len vacio", len("") == 0)

// ---- interpolacion basica ----
let nombre = "VestaShell"
let version = 1
let msg = "Motor: ${nombre} v${version}"
check("interpolacion simple", msg == "Motor: VestaShell v1")

// ---- interpolacion con expresion ----
let a = 3
let b = 4
let hip = "La hipotenusa es ${sqrt(a**2 + b**2)}"
// sqrt(9+16) = sqrt(25) = 5.0
check("interpolacion con expresion matematica", starts_with(hip, "La hipotenusa es 5"))

// ---- interpolacion anidada con funcion ----
fn saludo(n) { return "Hola " + n }
let arg_mundo = "mundo"
let gs = "${saludo(arg_mundo)}"
check("interpolacion con llamada a funcion", gs == "Hola mundo")

// ---- upper / lower ----
check("upper", upper("hola") == "HOLA")
check("lower", lower("MUNDO") == "mundo")
check("upper mixto", upper("AbC") == "ABC")

// ---- trim ----
check("trim espacios", trim("  hola  ") == "hola")
check("trim sin espacios", trim("ok") == "ok")
check("trim solo espacios", trim("   ") == "")

// ---- starts_with / ends_with ----
check("starts_with ok", starts_with("hola mundo", "hola"))
check("starts_with ko", not starts_with("hola mundo", "mundo"))
check("ends_with ok", ends_with("hola mundo", "mundo"))
check("ends_with ko", not ends_with("hola mundo", "hola"))

// ---- replace ----
check("replace basico", replace("hola mundo", "mundo", "vesta") == "hola vesta")
check("replace sin ocurrencia", replace("abc", "x", "y") == "abc")
check("replace multiple", replace("aaaa", "a", "b") == "bbbb")

// ---- substr ----
check("substr inicio y fin", substr("hola mundo", 5, 5) == "mundo")
check("substr desde el inicio", substr("abcdef", 0, 3) == "abc")
check("substr un caracter", substr("vesta", 2, 1) == "s")  // v=0,e=1,s=2,t=3,a=4

// ---- split / join ----
let partes = split("uno,dos,tres", ",")
check("split len", len(partes) == 3)
check("split primer elem", partes[0] == "uno")
check("split ultimo elem", partes[2] == "tres")

let unido = join(partes, " - ")
check("join", unido == "uno - dos - tres")

let lineas = split("a\nb\nc", "\n")
check("split newline", len(lineas) == 3)

// ---- contains en string ----
check("contains str ok", contains("hola mundo", "mundo"))
check("contains str ko", not contains("hola mundo", "vesta"))

// ---- conversion int/float -> str ----
check("str(int)", str(42) == "42")
check("str(float) tiene punto", contains(str(3.14), "."))
check("str(bool)", str(true) == "true")
check("str(null)", str(null) == "null")

// ---- conversion str -> int/float ----
check("int(str)", int("42") == 42)
check("float(str)", float("3.14") > 3.1 and float("3.14") < 3.2)
check("int(float)", int(3.9) == 3)   // trunca hacia cero
check("float(int)", float(5) == 5.0)

// ---- iteracion sobre caracteres ----
let chars = []
for c in "abc" {
    append(chars, c)
}
check("for in string len", len(chars) == 3)
check("for in string primer char", chars[0] == "a")
check("for in string ultimo char", chars[2] == "c")

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
