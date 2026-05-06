// VestaShell - Libreria de utilidades (importada por 09_import.vsh)
// Define funciones reutilizables para demostrar el sistema de modulos.

fn suma_lista(lista) {
    "Devuelve la suma de todos los elementos numericos de lista."
    let total = 0
    for x in lista { total = total + x }
    return total
}

fn producto_lista(lista) {
    "Devuelve el producto de todos los elementos de lista."
    let p = 1
    for x in lista { p = p * x }
    return p
}

fn invertir_lista(lista) {
    "Devuelve una nueva lista con los elementos de lista en orden inverso."
    let resultado = []
    let i = len(lista) - 1
    while i >= 0 {
        append(resultado, lista[i])
        i = i - 1
    }
    return resultado
}

fn aplana(lista) {
    "Aplana un nivel de listas anidadas en una sola lista plana."
    let resultado = []
    for elem in lista {
        if is_list(elem) {
            for x in elem { append(resultado, x) }
        } else {
            append(resultado, elem)
        }
    }
    return resultado
}

fn unico(lista) {
    "Elimina duplicados de lista conservando el orden de primera aparicion."
    let visto = {}
    let resultado = []
    for x in lista {
        let k = str(x)
        if not contains(visto, k) {
            visto[k] = true
            append(resultado, x)
        }
    }
    return resultado
}

fn zip(a, b) {
    "Crea una lista de pares [a[i], b[i]] hasta el minimo de len(a) y len(b)."
    let resultado = []
    let n = min(len(a), len(b))
    for i in range(n) {
        append(resultado, [a[i], b[i]])
    }
    return resultado
}

fn cadena_paloma(s) {
    "Devuelve true si la cadena s es un palindromo (se lee igual al reves)."
    let n = len(s)
    let i = 0
    while i < n / 2 {
        if substr(s, i, 1) != substr(s, n - 1 - i, 1) {
            return false
        }
        i = i + 1
    }
    return true
}

fn formatear_tabla(filas, separador) {
    "Convierte una lista de listas en una cadena de texto con columnas separadas por separador."
    let lineas = []
    for fila in filas {
        let cols = []
        for col in fila {
            append(cols, str(col))
        }
        append(lineas, join(cols, separador))
    }
    return join(lineas, "\n")
}

// Constantes exportadas
let PI_APROX = 3.14159265358979
let E_APROX  = 2.71828182845905
