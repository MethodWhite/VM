// VestaShell - 01: Tipos basicos, variables y operadores
// Cubre: let, reasignacion, tipos primitivos, operadores aritmeticos,
//        operadores de comparacion, operadores logicos, operador **.

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

println("=== 01: Tipos basicos y operadores ===")

// ---- null ----
let n = null
check("null es null", is_null(n))
check("null no es truthy", not n)

// ---- bool ----
let b_true  = true
let b_false = false
check("bool true", b_true == true)
check("bool false", b_false == false)
check("not false", not b_false)
check("not not true", not not b_true)
check("and: true and true", true and true)
check("and: false and true es false", not (false and true))
check("or: false or true", false or true)
check("or: false or false es false", not (false or false))

// ---- int ----
let x = 42
let y = 10
check("int literal", x == 42)
check("suma int", x + y == 52)
check("resta int", x - y == 32)
check("multiplicacion int", x * y == 420)
check("division entera int", x / y == 4)   // division entera entre ints
check("modulo", x % y == 2)
check("potencia **", 2 ** 10 == 1024)
check("negativo", -x == -42)

// ---- reasignacion ----
let v = 1
v = 99
check("reasignacion", v == 99)

// ---- operadores compuestos ----
let c = 10
c += 5
check("+=", c == 15)
c -= 3
check("-=", c == 12)
c *= 2
check("*=", c == 24)
c /= 4
check("/=", c == 6)
c %= 4
check("%=", c == 2)

// ---- float ----
let f = 3.14
check("float literal", f > 3.0 and f < 3.2)
check("float suma", f + 1.0 > 4.0)
check("float igualdad", 1.0 == 1)   // int y float iguales si mismo valor
check("int a float aritm", 1 + 0.5 == 1.5)

// ---- comparaciones ----
check("menor que", 1 < 2)
check("mayor que", 2 > 1)
check("menor o igual", 2 <= 2)
check("mayor o igual", 3 >= 3)
check("distinto", 1 != 2)
check("igualdad string", "hola" == "hola")
check("desigualdad string", "a" != "b")

// ---- type() ----
check("type null",  type(null)  == "null")
check("type bool",  type(true)  == "bool")
check("type int",   type(42)    == "int")
check("type float", type(3.14)  == "float")
check("type str",   type("hi")  == "string")

// ---- predicados de tipo ----
check("is_null",  is_null(null))
check("is_bool",  is_bool(true))
check("is_int",   is_int(7))
check("is_float", is_float(1.5))
check("is_str",   is_str("abc"))

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
