/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */

/**
 * @file test_class_registry.cpp
 * @brief Test unitario del ClassRegistry: define clases, busca campos /
 *        metodos por nombre, valida AOP y caches de lookup.
 *
 * Cubre el flujo basico de meta-programacion de A.4 sin tocar el resto
 * de la VM.  Compila con el framework estandar de tests del proyecto
 * (cualquier .cpp dentro de tests/ se vuelve un ejecutable separado).
 *
 * Convencion de salida: imprime "OK <descripcion>" para cada chequeo
 * pasado y "FAIL <descripcion>" + razon ante un fallo, devolviendo
 * codigo != 0 para que CI lo detecte.
 */

#include "loader/class_registry.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace loader;

// Contadores globales de tests pasados / fallidos.
static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *desc) {
    if (cond) {
        std::printf("OK  %s\n", desc);
        ++g_pass;
    } else {
        std::printf("FAIL %s\n", desc);
        ++g_fail;
    }
}

/**
 * @brief Verifica que la API basica define_class + find_class funciona.
 */
static void test_define_and_find() {
    ClassRegistry reg;

    ClassInfo *empty = reg.define_class("Empty", nullptr, {}, {}, {},
                                         CLASS_VIS_PUBLIC);
    check(empty != nullptr, "define_class('Empty') devuelve no-nulo");
    check(reg.find_class("Empty") == empty,
          "find_class('Empty') devuelve el mismo puntero");
    check(reg.find_class("NoExiste") == nullptr,
          "find_class de clase inexistente devuelve nullptr");

    check(empty->instance_size == sizeof(ObjectHeader),
          "Empty.instance_size == sizeof(ObjectHeader)");
    check(reg.class_count() == 1, "class_count == 1 tras un define");
}

/**
 * @brief Verifica fields: offsets, lookup por nombre, separacion
 *        instance/static.
 */
static void test_fields() {
    ClassRegistry reg;

    std::vector<FieldDecl> fields = {
        {"x",      FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC,  false},
        {"y",      FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC,  false},
        {"name",   FIELD_PRIMITIVE, nullptr, 8, FIELD_PRIVATE, false},
        {"count",  FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC,  true},
    };
    ClassInfo *cls = reg.define_class("Point", nullptr, {}, fields, {});
    check(cls != nullptr, "define_class('Point') con fields");
    check(cls->field_count == 3,        "instance field_count == 3");
    check(cls->static_field_count == 1, "static_field_count == 1");

    // Offsets: cada slot redondeado a 8 bytes desde el final del header (24).
    check(cls->fields[0].offset == sizeof(ObjectHeader),
          "field 'x' offset == sizeof(ObjectHeader)");
    check(cls->fields[1].offset == sizeof(ObjectHeader) + 8,
          "field 'y' offset == header + 8");
    check(cls->fields[2].offset == sizeof(ObjectHeader) + 16,
          "field 'name' offset == header + 16");
    check(cls->instance_size == sizeof(ObjectHeader) + 24,
          "instance_size == header + 3 slots");

    FieldInfo *fx = ClassRegistry::find_field(cls, "x");
    FieldInfo *fy = ClassRegistry::find_field(cls, "y");
    FieldInfo *fz = ClassRegistry::find_field(cls, "z");
    FieldInfo *fc = ClassRegistry::find_field(cls, "count");
    check(fx == &cls->fields[0], "find_field('x') == &fields[0]");
    check(fy == &cls->fields[1], "find_field('y') == &fields[1]");
    check(fz == nullptr,         "find_field('z') == nullptr");
    check(fc == &cls->static_fields[0],
          "find_field('count') == &static_fields[0]");
}

/**
 * @brief Verifica methods: vtable, lookup por nombre.
 */
static void test_methods() {
    ClassRegistry reg;

    std::vector<MethodDecl> methods = {
        {"ctor",     "()V",   METHOD_FLAG_CONSTRUCTOR, 0x1000, 16},
        {"toString", "()Str", METHOD_FLAG_VIRTUAL,     0x2000, 32},
        {"equals",   "(O)Z",  METHOD_FLAG_VIRTUAL,     0x3000, 48},
    };
    ClassInfo *cls = reg.define_class("Obj", nullptr, {}, {}, methods);
    check(cls != nullptr,            "define_class('Obj') con methods");
    check(cls->method_count == 3,    "method_count == 3");
    check(cls->vtable_size == 3,     "vtable_size == 3");
    check(cls->vtable[0] == &cls->methods[0], "vtable[0] == &methods[0]");
    check(cls->vtable[1] == &cls->methods[1], "vtable[1] == &methods[1]");

    MethodInfo *mc = ClassRegistry::find_method(cls, "ctor");
    MethodInfo *mt = ClassRegistry::find_method(cls, "toString");
    MethodInfo *mn = ClassRegistry::find_method(cls, "noexiste");
    check(mc == &cls->methods[0],  "find_method('ctor')");
    check(mt == &cls->methods[1],  "find_method('toString')");
    check(mn == nullptr,           "find_method('noexiste') == nullptr");

    check(mc->code_vaddr == 0x1000, "code_vaddr de ctor preservado");
    check(mt->code_vaddr == 0x2000, "code_vaddr de toString preservado");
}

/**
 * @brief Verifica AOP: anadir advices y verificar la cadena.
 */
static void test_aop_advice_chain() {
    ClassRegistry reg;

    std::vector<MethodDecl> methods = {
        {"target",  "()V", METHOD_FLAG_VIRTUAL, 0x1000, 16},
        {"before1", "()V", METHOD_FLAG_VIRTUAL, 0x2000, 8},
        {"after1",  "()V", METHOD_FLAG_VIRTUAL, 0x3000, 8},
        {"around1", "()V", METHOD_FLAG_VIRTUAL, 0x4000, 8},
    };
    ClassInfo *cls = reg.define_class("Aop", nullptr, {}, {}, methods);

    MethodInfo *target = ClassRegistry::find_method(cls, "target");
    MethodInfo *b1     = ClassRegistry::find_method(cls, "before1");
    MethodInfo *a1     = ClassRegistry::find_method(cls, "after1");
    MethodInfo *r1     = ClassRegistry::find_method(cls, "around1");
    check(target && b1 && a1 && r1, "metodos AOP localizados");

    check(target->advice_chain == nullptr,
          "target.advice_chain == nullptr al inicio (fast path)");

    bool ok = reg.add_advice(target, ADVICE_BEFORE, b1);
    check(ok, "add_advice(BEFORE, b1) ok");
    check(target->advice_chain != nullptr, "advice_chain != nullptr tras anadir");
    check(target->advice_chain->kind == ADVICE_BEFORE, "primer advice es BEFORE");
    check(target->advice_chain->advice_method == b1,   "primer advice apunta a b1");
    check(target->advice_chain->next == nullptr,       "primer advice no tiene next");

    ok = reg.add_advice(target, ADVICE_AFTER, a1);
    check(ok, "add_advice(AFTER, a1) ok");
    check(target->advice_chain->next != nullptr,             "segundo nodo presente");
    check(target->advice_chain->next->kind == ADVICE_AFTER,  "segundo advice es AFTER");
    check(target->advice_chain->next->advice_method == a1,   "segundo advice apunta a a1");

    ok = reg.add_advice(target, ADVICE_AROUND, r1);
    check(ok, "add_advice(AROUND, r1) ok");
    AdviceEntry *third = target->advice_chain->next->next;
    check(third != nullptr && third->kind == ADVICE_AROUND
       && third->advice_method == r1, "tercer advice es AROUND -> r1");
}

/**
 * @brief Verifica que el lookup hash maneja muchos nombres sin colision
 *        falsa (factor de carga 0.5).
 */
static void test_lookup_scale() {
    ClassRegistry reg;

    std::vector<FieldDecl> fields;
    fields.reserve(64);
    for (int i = 0; i < 64; ++i) {
        fields.push_back({"f" + std::to_string(i), FIELD_PRIMITIVE,
                          nullptr, 8, FIELD_PUBLIC, false});
    }
    ClassInfo *cls = reg.define_class("Big", nullptr, {}, fields, {});
    check(cls != nullptr, "define_class con 64 fields");

    int hits = 0;
    for (int i = 0; i < 64; ++i) {
        const std::string n = "f" + std::to_string(i);
        FieldInfo *fi = ClassRegistry::find_field(cls, n);
        if (fi == &cls->fields[i]) ++hits;
    }
    check(hits == 64, "los 64 fields se localizan por hash sin colision");

    check(ClassRegistry::find_field(cls, "f64")  == nullptr,
          "f64 (out of range) -> nullptr");
    check(ClassRegistry::find_field(cls, "")     == nullptr,
          "lookup con nombre vacio -> nullptr");
    check(ClassRegistry::find_field(cls, "fxxx") == nullptr,
          "lookup con prefijo similar -> nullptr");
}

/**
 * @brief Verifica mutacion incremental: add_field / add_method tras un
 *        define_class vacio replican el resultado de define_class con
 *        todos los miembros de una vez.
 */
static void test_incremental_mutation() {
    ClassRegistry reg;

    ClassInfo *cls = reg.define_class("Vector", nullptr, {}, {}, {});
    check(cls != nullptr, "define_class vacio inicial");
    check(cls->field_count == 0,  "field_count == 0 tras define vacio");
    check(cls->method_count == 0, "method_count == 0 tras define vacio");

    // Anadir 3 fields incrementalmente.
    bool ok1 = reg.add_field(cls, {"x", FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC, false});
    bool ok2 = reg.add_field(cls, {"y", FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC, false});
    bool ok3 = reg.add_field(cls, {"z", FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC, false});
    check(ok1 && ok2 && ok3,         "3 add_field consecutivos ok");
    check(cls->field_count == 3,     "field_count == 3 tras 3 add");
    check(ClassRegistry::find_field(cls, "x") == &cls->fields[0],
          "lookup 'x' tras add incremental");
    check(ClassRegistry::find_field(cls, "z") == &cls->fields[2],
          "lookup 'z' tras add incremental");
    check(cls->fields[0].offset == sizeof(ObjectHeader),
          "offset de 'x' tras incremental");
    check(cls->fields[2].offset == sizeof(ObjectHeader) + 16,
          "offset de 'z' tras incremental");

    // Duplicado debe fallar.
    bool dup = reg.add_field(cls, {"x", FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC, false});
    check(!dup, "add_field duplicado retorna false");
    check(cls->field_count == 3, "field_count no cambia tras duplicado");

    // Anadir 2 metodos incrementalmente.
    bool om1 = reg.add_method(cls, {"add", "(V)V", METHOD_FLAG_VIRTUAL, 0x100, 16});
    bool om2 = reg.add_method(cls, {"sub", "(V)V", METHOD_FLAG_VIRTUAL, 0x200, 16});
    check(om1 && om2,             "2 add_method ok");
    check(cls->method_count == 2, "method_count == 2");
    check(ClassRegistry::find_method(cls, "add") == &cls->methods[0],
          "lookup 'add' tras incremental");
    check(ClassRegistry::find_method(cls, "sub") == &cls->methods[1],
          "lookup 'sub' tras incremental");
    check(cls->vtable[0] == &cls->methods[0], "vtable[0] tras incremental");
    check(cls->vtable[1] == &cls->methods[1], "vtable[1] tras incremental");
}

/**
 * @brief Replica el flujo de las instrucciones bytecode defclass /
 *        deffield / defmethod / findclass: desde C++, sin pasar por
 *        el .vel.  Valida el "shape" exacto de las llamadas que las
 *        exec functions de exec_instruction_meta.cpp realizan.
 */
static void test_bytecode_shape() {
    ClassRegistry reg;

    // Equivalente a defclass(name="Punto", super=null, flags=PUBLIC):
    ClassInfo *cls = reg.define_class("Punto", nullptr, {}, {}, {},
                                       CLASS_VIS_PUBLIC);
    check(cls != nullptr,            "defclass shape: clase creada");
    check(reg.class_count() == 1,    "defclass shape: 1 clase registrada");

    // Equivalente a deffield(cls, FieldDecl{...}) repetido:
    bool of1 = reg.add_field(cls, {"x", FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC, false});
    bool of2 = reg.add_field(cls, {"y", FIELD_PRIMITIVE, nullptr, 4, FIELD_PUBLIC, false});
    check(of1 && of2,                "deffield shape: 2 fields ok");
    check(cls->field_count == 2,     "deffield shape: count == 2");

    // Equivalente a defmethod(cls, MethodDecl{...}):
    bool om = reg.add_method(cls, {"distance", "()f64", METHOD_FLAG_VIRTUAL, 0x1234, 64});
    check(om,                        "defmethod shape: 1 method ok");
    check(cls->method_count == 1,    "defmethod shape: count == 1");

    // Equivalente a findclass("Punto"):
    ClassInfo *found = reg.find_class("Punto");
    check(found == cls,              "findclass shape: clase encontrada");

    // Equivalente al lookup runtime via name_hash que NEWOBJ + GETFIELD
    // harian implicitamente:
    check(ClassRegistry::find_field(cls, "x") == &cls->fields[0],
          "shape: getfield lookup ok");
    check(ClassRegistry::find_method(cls, "distance") == &cls->methods[0],
          "shape: callvirt lookup ok");

    // instance_size correctamente acumulado: header + 2 slots de 8 bytes.
    check(cls->instance_size == sizeof(ObjectHeader) + 16,
          "shape: instance_size correcto para newobj");

    // code_vaddr preservado tal como el .vel lo paso.
    check(cls->methods[0].code_vaddr == 0x1234,
          "shape: code_vaddr preservado para callvirt");
}

/**
 * @brief Stress + leak check: define muchas clases con muchos miembros
 *        en un registry transitorio y verifica que su destructor libera
 *        correctamente toda la memoria (sin double-free ni segfault).
 *        Ejecutar bajo valgrind/sanitizer detecta fugas reales.
 */
static void test_lifecycle_stress() {
    // Ciclos repetidos de define + lookup + destructor.
    for (int iter = 0; iter < 16; ++iter) {
        ClassRegistry reg;
        // 32 clases distintas con 8 fields y 4 methods cada una.
        for (int ci = 0; ci < 32; ++ci) {
            std::string cname = "K" + std::to_string(ci);
            std::vector<FieldDecl> fields;
            for (int f = 0; f < 8; ++f) {
                fields.push_back({"f" + std::to_string(f), FIELD_PRIMITIVE,
                                  nullptr, 8, FIELD_PUBLIC, false});
            }
            std::vector<MethodDecl> methods;
            for (int m = 0; m < 4; ++m) {
                methods.push_back({"m" + std::to_string(m), "()V",
                                   METHOD_FLAG_VIRTUAL,
                                   static_cast<uint64_t>(0x1000 + m * 16), 16});
            }
            ClassInfo *cls = reg.define_class(cname, nullptr, {}, fields, methods);
            if (!cls) continue;
            // Hacer lookups para forzar acceso a las hash tables.
            for (int f = 0; f < 8; ++f) {
                (void)ClassRegistry::find_field(cls, "f" + std::to_string(f));
            }
            for (int m = 0; m < 4; ++m) {
                (void)ClassRegistry::find_method(cls, "m" + std::to_string(m));
            }
            // Anyadir un advice por cada metodo.
            for (int m = 0; m < 4; ++m) {
                MethodInfo *target = ClassRegistry::find_method(cls,
                    "m" + std::to_string(m));
                MethodInfo *advice = ClassRegistry::find_method(cls,
                    "m" + std::to_string((m + 1) % 4));
                if (target && advice) {
                    (void)reg.add_advice(target, ADVICE_BEFORE, advice);
                }
            }
            // Mutacion incremental: anyadir 2 fields mas tras el define.
            (void)reg.add_field(cls, {"extra1", FIELD_PRIMITIVE, nullptr, 8,
                                       FIELD_PUBLIC, false});
            (void)reg.add_field(cls, {"extra2", FIELD_PRIMITIVE, nullptr, 8,
                                       FIELD_PRIVATE, false});
        }
        // Stats finales antes de destruir.
        check(reg.class_count() == 32, "stress: 32 clases creadas en este ciclo");
        // El destructor de ClassRegistry libera toda la memoria via
        // unique_ptr.  Si hubiera fugas, valgrind/asan lo detectaria al
        // salir.  Aqui solo verificamos que el destructor no abortea.
    }
    check(true, "lifecycle stress: 16 ciclos de define+destroy sin abortar");
}

/**
 * @brief A.5.1 - herencia: copia de fields/vtable + override + extension.
 *
 * Construye Base con un campo y un metodo, luego Sub : Base.  Verifica
 * que Sub al ser definido (sin add_field/add_method aun) ya tiene los
 * miembros heredados (porque define_class los copia del super), que
 * los offsets continuan tras el ultimo del super y que add_method con
 * mismo nombre del super hace override en el mismo vtable_index.
 */
static void test_inheritance_basic() {
    ClassRegistry reg;

    // Base con 1 field + 2 methods (ctor + metodo virtual).
    std::vector<FieldDecl> base_fields = {
        {"x", FIELD_PRIMITIVE, nullptr, 8, FIELD_PUBLIC, false},
    };
    std::vector<MethodDecl> base_methods = {
        {"ctor",   "()V", METHOD_FLAG_CONSTRUCTOR, 0x1000, 16},
        {"sonido", "()I", METHOD_FLAG_VIRTUAL,     0x2000, 32},
    };
    ClassInfo *base = reg.define_class("Base", nullptr, {},
                                        base_fields, base_methods);
    check(base != nullptr,         "define Base ok");
    check(base->field_count == 1,  "Base.field_count == 1");
    check(base->method_count == 2, "Base.method_count == 2");

    // Sub : Base, sin propios al pasarlos por define_class - simulamos
    // el patron real del frontend: defclass + deffield/defmethod luego.
    ClassInfo *sub = reg.define_class("Sub", base, {}, {}, {});
    check(sub != nullptr,                  "define Sub : Base ok");
    check(sub->super_count == 1,           "Sub.super_count == 1");
    check(sub->supers[0] == base,          "Sub.supers[0] == Base");
    // Los fields heredados se copiaron en define_class.
    check(sub->field_count == 1,           "Sub.field_count == 1 (heredado x)");
    check(sub->methods != nullptr,         "Sub tiene methods (heredados)");
    check(sub->method_count == 2,          "Sub.method_count == 2 (heredados ctor+sonido)");
    check(sub->vtable_size == 2,           "Sub.vtable_size == 2");

    // Lookup de heredados via tabla hash funciona en Sub.
    FieldInfo  *fx = ClassRegistry::find_field(sub, "x");
    MethodInfo *ms = ClassRegistry::find_method(sub, "sonido");
    check(fx != nullptr,                       "Sub.find_field('x') ok");
    check(ms != nullptr,                       "Sub.find_method('sonido') ok");
    check(fx->offset == base->fields[0].offset,
          "Sub.x offset igual al de Base.x");
    check(ms->code_vaddr == 0x2000,
          "Sub.sonido code_vaddr heredado del super");

    // Override: add_method con mismo nombre debe reemplazar en mismo slot.
    MethodDecl over{"sonido", "()I", METHOD_FLAG_VIRTUAL, 0x9000, 64};
    bool ok = reg.add_method(sub, over);
    check(ok,                              "add_method override ok");
    check(sub->method_count == 2,          "method_count no crece tras override");
    MethodInfo *ms2 = ClassRegistry::find_method(sub, "sonido");
    check(ms2->code_vaddr == 0x9000,       "override apunta al nuevo code_vaddr");
    // El metodo del super NO se toca; sigue accesible via Base.
    MethodInfo *base_s = ClassRegistry::find_method(base, "sonido");
    check(base_s->code_vaddr == 0x2000,    "Base.sonido sigue intacto");

    // add_method con nombre nuevo amplia la vtable.
    MethodDecl extra{"ladrar", "()V", METHOD_FLAG_VIRTUAL, 0xA000, 24};
    ok = reg.add_method(sub, extra);
    check(ok,                              "add_method nuevo metodo ok");
    check(sub->method_count == 3,          "method_count == 3 tras nuevo metodo");
    MethodInfo *ml = ClassRegistry::find_method(sub, "ladrar");
    check(ml != nullptr && ml->code_vaddr == 0xA000,
          "lookup del nuevo metodo ok");

    // add_field nuevo: el offset debe continuar tras el heredado.
    FieldDecl raza{"raza", FIELD_PRIMITIVE, nullptr, 8, FIELD_PUBLIC, false};
    ok = reg.add_field(sub, raza);
    check(ok,                              "add_field('raza') ok");
    check(sub->field_count == 2,           "Sub.field_count == 2");
    FieldInfo *fr = ClassRegistry::find_field(sub, "raza");
    check(fr != nullptr,                   "find_field('raza') ok");
    check(fr->offset == fx->offset + 8,
          "raza.offset == x.offset + 8 (continuacion correcta)");
}

int main() {
    std::printf("=== test_class_registry ===\n");

    test_define_and_find();
    test_fields();
    test_methods();
    test_aop_advice_chain();
    test_lookup_scale();
    test_incremental_mutation();
    test_bytecode_shape();
    test_lifecycle_stress();
    test_inheritance_basic();

    std::printf("\nResultado: %d OK, %d FAIL\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
