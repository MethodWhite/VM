// VestaShell - 10: Sistema de POO (clases, herencia, self, super)
// Cubre: class, herencia, self, super.__init__, isinstance(), classname(),
//        doc() y help() sobre clases, metodos, atributos de instancia.

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

println("=== 10: Clases y POO ===")

// ---- clase basica ----
class Punto {
    "Representa un punto 2D con coordenadas x e y."

    fn __init__(self, x, y) {
        self.x = x
        self.y = y
    }

    fn suma(self) {
        "Devuelve la suma de las coordenadas."
        return self.x + self.y
    }

    fn escalar(self, factor) {
        self.x = self.x * factor
        self.y = self.y * factor
    }
}

let p = Punto(3, 4)
check("instancia creada",       is_instance(p))
check("classname",              classname(p) == "Punto")
check("atributo x",             p.x == 3)
check("atributo y",             p.y == 4)
check("metodo suma",            p.suma() == 7)

p.escalar(2)
check("mutacion escalar x",     p.x == 6)
check("mutacion escalar y",     p.y == 8)

// ---- class doc ----
check("doc de clase",           len(doc(Punto)) > 0)
check("doc metodo suma",        len(doc(p.suma)) > 0)

// ---- herencia simple ----
class Punto3D : Punto {
    "Extiende Punto con coordenada z."

    fn __init__(self, x, y, z) {
        self.x = x
        self.y = y
        self.z = z
    }

    fn suma(self) {
        return self.x + self.y + self.z
    }
}

let q = Punto3D(1, 2, 3)
check("herencia instancia",     is_instance(q))
check("herencia classname",     classname(q) == "Punto3D")
check("herencia atributo z",    q.z == 3)
check("herencia metodo suma",   q.suma() == 6)
check("isinstance Punto3D",     isinstance(q, Punto3D))
check("isinstance Punto",       isinstance(q, Punto))

// ---- isinstance con clase base ----
check("isinstance p Punto",     isinstance(p, Punto))
check("isinstance p no Punto3D", not isinstance(p, Punto3D))

// ---- herencia multinivel ----
class Animal {
    "Clase base para animales."
    fn __init__(self, nombre) {
        self.nombre = nombre
    }
    fn hablar(self) {
        return "..."
    }
    fn descripcion(self) {
        return self.nombre + " dice: " + self.hablar()
    }
}

class Perro : Animal {
    fn __init__(self, nombre) {
        self.nombre = nombre
    }
    fn hablar(self) {
        return "Guau!"
    }
}

class Gato : Animal {
    fn __init__(self, nombre) {
        self.nombre = nombre
    }
    fn hablar(self) {
        return "Miau!"
    }
}

let perro = Perro("Rex")
let gato  = Gato("Misi")

check("polimorfismo perro hablar", perro.hablar() == "Guau!")
check("polimorfismo gato hablar",  gato.hablar()  == "Miau!")
check("metodo heredado perro",     perro.descripcion() == "Rex dice: Guau!")
check("metodo heredado gato",      gato.descripcion()  == "Misi dice: Miau!")
check("isinstance Perro Animal",   isinstance(perro, Animal))
check("isinstance Gato Animal",    isinstance(gato,  Animal))
check("isinstance Perro no Gato",  not isinstance(perro, Gato))

// ---- atributos dinamicos ----
let p2 = Punto(0, 0)
p2.etiqueta = "origen"
check("atributo dinamico",      p2.etiqueta == "origen")

// ---- clase sin herencia, solo metodos de datos ----
class Pila {
    "Estructura LIFO implementada con lista."
    fn __init__(self) {
        self.datos = []
    }
    fn push(self, v) {
        append(self.datos, v)
    }
    fn pop(self) {
        if len(self.datos) == 0 {
            return null
        }
        let n = len(self.datos) - 1
        let v = self.datos[n]
        let nueva = []
        let i = 0
        while i < n {
            append(nueva, self.datos[i])
            i = i + 1
        }
        self.datos = nueva
        return v
    }
    fn peek(self) {
        if len(self.datos) == 0 { return null }
        return self.datos[len(self.datos) - 1]
    }
    fn tamano(self) {
        return len(self.datos)
    }
}

let s = Pila()
s.push(10)
s.push(20)
s.push(30)
check("pila tamano 3",   s.tamano() == 3)
check("pila peek 30",    s.peek() == 30)
check("pila pop 30",     s.pop() == 30)
check("pila pop 20",     s.pop() == 20)
check("pila tamano 1",   s.tamano() == 1)
check("pila pop 10",     s.pop() == 10)
check("pila vacia null", s.pop() == null)

// ---- is_class / is_instance ----
check("is_class Punto",    is_class(Punto))
check("is_class Pila",     is_class(Pila))
check("is_instance p",     is_instance(p))
check("no is_class 42",    not is_class(42))
check("no is_instance 42", not is_instance(42))

// ---- help() sobre clase ----
let h = help(Punto)
check("help clase devuelve string", is_str(h))
check("help clase contiene nombre", contains(h, "Punto"))

println("--- Resultado: " + str(pass) + " PASS, " + str(fail) + " FAIL ---")
