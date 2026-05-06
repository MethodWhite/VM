// VestaShell - 05: Listas y mapas
// Cubre: literales, indexado positivo y negativo, append, pop,
//        len, contains, keys, values, iteracion, anidado.

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

println("=== 05: Listas y mapas ===")

// ---- lista basica ----
let lista = [1, 2, 3, 4, 5]
check("lista len", len(lista) == 5)
check("lista[0]", lista[0] == 1)
check("lista[4]", lista[4] == 5)
check("lista[-1] ultimo", lista[-1] == 5)
check("lista[-2] penultimo", lista[-2] == 4)

// ---- lista vacia ----
let vacia = []
check("lista vacia len", len(vacia) == 0)
check("lista vacia es falsy", not vacia)

// ---- append ----
append(vacia, 10)
append(vacia, 20)
append(vacia, 30)
check("append len", len(vacia) == 3)
check("append primer elem", vacia[0] == 10)
check("append ultimo elem", vacia[2] == 30)

// ---- pop ----
let ultimo = pop(vacia)
check("pop devuelve ultimo", ultimo == 30)
check("pop reduce len", len(vacia) == 2)

// ---- modificar elemento ----
let mutable = [10, 20, 30]
mutable[1] = 99
check("asignacion por indice", mutable[1] == 99)

// ---- contains en lista ----
check("contains en lista ok", contains(lista, 3))
check("contains en lista ko", not contains(lista, 99))

// ---- lista de tipos mixtos ----
let mixta = [1, "dos", true, null, 3.14]
check("lista mixta len", len(mixta) == 5)
check("lista mixta string", mixta[1] == "dos")
check("lista mixta bool", mixta[2] == true)
check("lista mixta null", is_null(mixta[3]))

// ---- lista de listas (anidado) ----
let matriz = [[1,2,3], [4,5,6], [7,8,9]]
check("matriz[0][0]", matriz[0][0] == 1)
check("matriz[1][2]", matriz[1][2] == 6)
check("matriz[2][1]", matriz[2][1] == 8)

// ---- iteracion y construccion ----
let cuadrados = []
for i in range(1, 6) {
    append(cuadrados, i * i)
}
check("lista construida len", len(cuadrados) == 5)
check("lista construida [4]", cuadrados[4] == 25)

// ---- is_list ----
check("is_list ok", is_list([]))
check("is_list ko", not is_list("no soy lista"))

/*
 * ============================================================
 * Mapas
 * ============================================================
 */

// ---- mapa basico ----
let persona = {
    "nombre": "Ada",
    "edad": 36,
    "activa": true,
}
check("mapa acceso nombre", persona["nombre"] == "Ada")
check("mapa acceso edad", persona["edad"] == 36)
check("mapa acceso bool", persona["activa"] == true)

// ---- mapa vacio ----
let m = {}
check("mapa vacio len keys", len(keys(m)) == 0)
check("mapa vacio es falsy", not m)

// ---- asignacion a clave nueva ----
m["clave"] = "valor"
check("mapa clave nueva", m["clave"] == "valor")

// ---- sobreescribir clave existente ----
persona["edad"] = 37
check("mapa sobreescribir", persona["edad"] == 37)

// ---- clave inexistente lanza VshRuntimeError ----
let capto_clave_missing = false
try {
    let _x = persona["inexistente"]
} catch e {
    capto_clave_missing = true
}
check("mapa clave inexistente lanza error", capto_clave_missing)

// ---- keys / values ----
let kk = keys(persona)
check("keys len", len(kk) >= 3)
check("keys contiene nombre", contains(kk, "nombre"))
check("keys contiene edad", contains(kk, "edad"))

let vv = values(persona)
check("values len", len(vv) >= 3)
check("values contiene 'Ada'", contains(vv, "Ada"))

// ---- contains en mapa (busca clave) ----
check("contains mapa clave ok", contains(persona, "nombre"))
check("contains mapa clave ko", not contains(persona, "telefono"))

// ---- for/in mapa itera claves ----
let claves_visitadas = []
for k in persona {
    append(claves_visitadas, k)
}
check("for/in mapa recorre claves", len(claves_visitadas) >= 3)
check("for/in mapa incluye nombre", contains(claves_visitadas, "nombre"))

// ---- mapa anidado ----
let config = {
    "db": { "host": "localhost", "port": 5432 },
    "app": { "debug": true, "workers": 4 },
}
check("mapa anidado db host", config["db"]["host"] == "localhost")
check("mapa anidado db port", config["db"]["port"] == 5432)
check("mapa anidado app debug", config["app"]["debug"] == true)

// ---- lista dentro de mapa ----
let agenda = { "amigos": ["Ana", "Bob", "Carlos"] }
check("mapa con lista len", len(agenda["amigos"]) == 3)
check("mapa con lista elem", agenda["amigos"][1] == "Bob")

// ---- mapa dentro de lista ----
let registros = [
    { "id": 1, "nombre": "Eva" },
    { "id": 2, "nombre": "Luis" },
]
check("lista de mapas [0]", registros[0]["nombre"] == "Eva")
check("lista de mapas [1]", registros[1]["id"] == 2)

// ---- is_map ----
check("is_map ok", is_map({}))
check("is_map ko", not is_map([]))

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
