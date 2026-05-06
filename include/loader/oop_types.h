/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (c) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file oop_types.h
 * @brief Estructuras de metadatos del sistema de objetos de VestaVM.
 *
 * Este header es intencionalmente ligero (solo <cstdint> y <cstddef>) para
 * poder incluirse desde proceso_runtime.h sin crear dependencias circulares
 * con loader.h -> runtime.h -> proceso_runtime.h.
 */

#ifndef OOP_TYPES_H
#define OOP_TYPES_H

#include <cstdint>
#include <cstddef>

namespace loader {

    // -------------------------------------------------------------------------
    //  Flags de objeto en memoria (ObjectHeader::flags)
    // -------------------------------------------------------------------------
    static constexpr uint32_t OBJ_FLAG_GC_OWNED  = (1u << 0); ///< gestionado por GcHeap
    static constexpr uint32_t OBJ_FLAG_RAW_OWNED = (1u << 1); ///< gestionado por RawAllocator
    static constexpr uint32_t OBJ_FLAG_LOCKED    = (1u << 2); ///< monitor bloqueado

    // -------------------------------------------------------------------------
    //  Flags de clase (ClassInfo::flags)
    //
    //  bits 0-1 : visibilidad  (0=default, 1=public, 2=private, 3=protected)
    //  bit  2   : is_abstract
    //  bit  3   : is_interface
    //  bit  4   : is_final
    //  bit  5   : is_enum
    //  bit  6   : is_static
    //  bit  7   : is_exception  (hereda de Throwable -> habilita throw/catch)
    //  bit  8   : is_native     (implementada en C/C++ del runtime)
    // -------------------------------------------------------------------------
    static constexpr uint64_t CLASS_VIS_DEFAULT   = 0x0ULL;
    static constexpr uint64_t CLASS_VIS_PUBLIC    = 0x1ULL;
    static constexpr uint64_t CLASS_VIS_PRIVATE   = 0x2ULL;
    static constexpr uint64_t CLASS_VIS_PROTECTED = 0x3ULL;
    static constexpr uint64_t CLASS_FLAG_ABSTRACT  = (1ULL << 2);
    static constexpr uint64_t CLASS_FLAG_INTERFACE = (1ULL << 3);
    static constexpr uint64_t CLASS_FLAG_FINAL     = (1ULL << 4);
    static constexpr uint64_t CLASS_FLAG_ENUM      = (1ULL << 5);
    static constexpr uint64_t CLASS_FLAG_STATIC    = (1ULL << 6);
    static constexpr uint64_t CLASS_FLAG_EXCEPTION = (1ULL << 7);
    static constexpr uint64_t CLASS_FLAG_NATIVE    = (1ULL << 8);
    static constexpr uint64_t CLASS_FLAG_CLOSURE   = (1ULL << 9);  ///< clase es un closure GC
    static constexpr uint64_t CLASS_FLAG_GENERIC   = (1ULL << 10); ///< clase tiene parametros de tipo

    // -------------------------------------------------------------------------
    //  Flags de visibilidad de modulo (ClassInfo::visibility)
    // -------------------------------------------------------------------------
    static constexpr uint64_t MODULE_VIS_INTERNAL  = 0x0ULL; ///< solo accesible dentro del modulo
    static constexpr uint64_t MODULE_VIS_EXPORT    = 0x1ULL; ///< accesible desde otros modulos

    // -------------------------------------------------------------------------
    //  Flags de metodo (MethodInfo::flags)
    //
    //  bits 0-1 : visibilidad  (mismos valores que CLASS_VIS_*)
    //  bit  2   : is_static
    //  bit  3   : is_abstract
    //  bit  4   : is_final
    //  bit  8   : is_native
    //  bit  9   : is_constructor
    //  bit  10  : is_virtual
    //  bit  11  : is_override
    //  bit  12  : is_synchronized
    // -------------------------------------------------------------------------
    static constexpr uint64_t METHOD_FLAG_STATIC       = (1ULL << 2);
    static constexpr uint64_t METHOD_FLAG_ABSTRACT     = (1ULL << 3);
    static constexpr uint64_t METHOD_FLAG_FINAL        = (1ULL << 4);
    static constexpr uint64_t METHOD_FLAG_NATIVE       = (1ULL << 8);
    static constexpr uint64_t METHOD_FLAG_CONSTRUCTOR  = (1ULL << 9);
    static constexpr uint64_t METHOD_FLAG_VIRTUAL      = (1ULL << 10);
    static constexpr uint64_t METHOD_FLAG_OVERRIDE     = (1ULL << 11);
    static constexpr uint64_t METHOD_FLAG_SYNCHRONIZED = (1ULL << 12);

    // -------------------------------------------------------------------------
    //  Modificadores de acceso de campo
    // -------------------------------------------------------------------------
    typedef enum FieldAccess {
        FIELD_PUBLIC,
        FIELD_PRIVATE,
        FIELD_PROTECTED,
        FIELD_DEFAULT
    } FieldAccess;

    // -------------------------------------------------------------------------
    //  Tipos de campo
    // -------------------------------------------------------------------------
    typedef enum FieldKind {
        FIELD_PRIMITIVE,
        FIELD_CLASS,
        FIELD_STRUCT,
        FIELD_TYPEDEF,
        FIELD_ENUM,
        FIELD_ASPECT
    } FieldKind;

    // -------------------------------------------------------------------------
    //  String ligero (sin heap de C++)
    // -------------------------------------------------------------------------
    typedef struct stringx {
        uint8_t *data;
        uint32_t size;
    } stringx;

    // Declaraciones adelantadas
    struct ClassInfo;
    struct MethodInfo;
    struct FieldInfo;
    struct GenericParam;
    struct AdviceEntry;
    struct LookupSlot;

    // -------------------------------------------------------------------------
    //  Programacion de aspectos (AOP)
    //
    //  Cada MethodInfo lleva opcionalmente un puntero a una cadena de
    //  AdviceEntry.  Si el puntero es NULL, CALLVIRT toma el fast path
    //  (1 cmp + jmp) y llama directamente al code_vaddr del metodo.  Si
    //  no es NULL, recorre la lista en orden y llama a cada advice antes
    //  o despues del target segun su tipo.
    //
    //  Tipos de advice:
    //   BEFORE  - se invoca antes del target.  Si retorna distinto de 0
    //             (codigo de short-circuit), el target NO se invoca.
    //   AFTER   - se invoca despues del target con el resultado del target
    //             como argumento (puede transformarlo).
    //   AROUND  - reemplaza al target.  El advice recibe un puntero al
    //             target y decide si invocarlo o no via "proceed()".
    //
    //  Los advices comparten el mismo descriptor que el target para
    //  garantizar que la firma encaja sin reflexion adicional.
    // -------------------------------------------------------------------------
    static constexpr uint8_t ADVICE_BEFORE = 0;
    static constexpr uint8_t ADVICE_AFTER  = 1;
    static constexpr uint8_t ADVICE_AROUND = 2;

    typedef struct AdviceEntry {
        uint8_t      kind;          ///< ADVICE_BEFORE / ADVICE_AFTER / ADVICE_AROUND
        uint8_t      _pad[7];       ///< alineacion
        MethodInfo  *advice_method; ///< metodo que implementa el advice
        AdviceEntry *next;          ///< siguiente advice en la cadena (NULL = fin)
    } AdviceEntry;

    // -------------------------------------------------------------------------
    //  Lookup acelerado de fields/methods por nombre
    //
    //  Cada ClassInfo lleva una tabla hash open-addressing (tamano potencia
    //  de 2, factor de carga 0.5) que mapea hash(name) -> indice en fields[]
    //  o vtable[].  Permite getfield/getmethod por nombre en O(1) amortizado
    //  sin recorrer el array linealmente.  La tabla se construye al definir
    //  los miembros (una sola vez) y se consulta en cada lookup dinamico.
    //
    //  El campo `name_hash` es FNV-1a de los bytes de stringx::data; se
    //  cachea en el slot para evitar recalcular durante la sonda lineal.
    // -------------------------------------------------------------------------
    typedef struct LookupSlot {
        uint64_t name_hash;   ///< FNV-1a del nombre (0 = slot vacio)
        uint32_t index;       ///< indice en fields[] o vtable[]
        uint32_t name_len;    ///< longitud del nombre (para confirmar match)
        const char *name_ptr; ///< puntero al nombre (no propietario)
    } LookupSlot;

    // -------------------------------------------------------------------------
    //  AttrEntry - par clave/valor para anotaciones arbitrarias
    //
    //  Permite adjuntar metadatos extensibles a ClassInfo, MethodInfo y FieldInfo:
    //    @Deprecated, @Since, @Warning, @Author, @See, etc.
    //
    //  sizeof(AttrEntry) == 32 bytes (dos stringx de 12B c/u + padding de alineacion)
    // -------------------------------------------------------------------------
    typedef struct AttrEntry {
        stringx key;    ///< nombre de la anotacion  ("Deprecated", "Since", ...)
        stringx value;  ///< valor de la anotacion   ("true", "1.4", ...)
    } AttrEntry;

    // -------------------------------------------------------------------------
    //  FieldInfo - descripcion de un campo de instancia o estatico
    //
    //  Para campos genericos (tipo = parametro T, K, V...):
    //    is_type_param  = true
    //    type_param_idx = indice en ClassInfo::type_params
    //    type_class     = nullptr (se resuelve al especializar con SPECIALIZE)
    //
    //  Tras especializacion (specialize_class en loader.cpp):
    //    is_type_param  = false
    //    type_class     = ClassInfo* del tipo concreto instanciado
    //
    //  Todos los campos del VM tienen size = 8 (slot de 64 bits) independientemente
    //  del tipo: los objetos se representan por GcHandle (32 bits en los 4 bajos)
    //  y los primitivos se almacenan como uint64 con extension de signo / zero.
    // -------------------------------------------------------------------------
    typedef struct FieldInfo {
        stringx     name;
        FieldAccess access;
        FieldKind   kind;
        ClassInfo  *type_class;    ///< tipo concreto (nullptr si is_type_param == true)
        uint32_t    size;          ///< bytes del campo (8 para slots VM estandar)
        uint32_t    offset;        ///< offset dentro del payload del objeto
        bool        is_static;
        bool        is_type_param; ///< true si el tipo es un parametro de tipo (T, K, V...)
        uint16_t    type_param_idx;///< indice en ClassInfo::type_params (valido si is_type_param)
        uint8_t     _field_pad[4]; ///< relleno de alineacion (reservado, debe ser 0)
        // --- reflexion/documentacion ---
        stringx     doc;           ///< docstring del campo
        AttrEntry  *attrs;         ///< tabla de anotaciones clave/valor
        size_t      attr_count;
    } FieldInfo;

    // -------------------------------------------------------------------------
    //  HandlerException - entrada de la tabla de handlers de un metodo
    // -------------------------------------------------------------------------
    typedef struct HandlerException {
        ClassInfo *type;       ///< clase de la excepcion (nullptr = catch-all)
        uint32_t   start_pc;   ///< offset inicio del rango try (relativo a code_vaddr)
        uint32_t   end_pc;     ///< offset fin del rango try (relativo a code_vaddr)
        uint32_t   handler_pc; ///< offset del bloque catch (relativo a code_vaddr)
    } HandlerException;

    // -------------------------------------------------------------------------
    //  MethodInfo - metadatos de un metodo
    // -------------------------------------------------------------------------
    typedef struct MethodInfo {
        stringx           name;
        stringx           descriptor;   ///< firma legible "(int,String)int"
        uint64_t          flags;        ///< METHOD_FLAG_*
        ClassInfo        *owner_class;  ///< clase a la que pertenece

        FieldInfo        *args;         ///< descripcion de parametros (reflexion)
        size_t            arg_count;
        FieldInfo        *return_type;  ///< tipo de retorno (nullptr = void)

        uint64_t          code_vaddr;   ///< direccion virtual del inicio del bytecode
        size_t            code_size;    ///< bytes de bytecode

        HandlerException *handlers;
        size_t            handler_count;

        void             *jit_code;     ///< nullptr si no esta compilado JIT
        // --- reflexion/documentacion (offsets 112-143) ---
        stringx           doc;          ///< docstring del metodo
        AttrEntry        *attrs;        ///< tabla de anotaciones clave/valor
        size_t            attr_count;
        // --- programacion de aspectos (AOP) ---
        /// Cadena de advices o NULL si el metodo no tiene aspectos asociados.
        /// CALLVIRT comprueba este campo antes de invocar; NULL es el fast
        /// path (zero overhead).  Los advices se anaden con add_advice() del
        /// ClassRegistry.  El orden de la cadena define el orden de
        /// ejecucion: BEFORE en orden de insercion, AFTER en orden inverso,
        /// AROUND envolviendo el target.
        AdviceEntry      *advice_chain;
    } MethodInfo;

    // -------------------------------------------------------------------------
    //  ClassInfo - descriptor completo de una clase
    // -------------------------------------------------------------------------
    typedef struct ClassInfo {
        // --- identidad ---
        stringx    name;
        uint64_t   flags;          ///< CLASS_FLAG_*
        uint32_t   instance_size;  ///< bytes totales del objeto (incluye ObjectHeader)

        // --- jerarquia ---
        ClassInfo **supers;
        size_t      super_count;
        ClassInfo **interfaces;
        size_t      interface_count;

        // --- campos de instancia ---
        FieldInfo  *fields;
        size_t      field_count;

        // --- vtable ---
        MethodInfo **vtable;
        size_t       vtable_size;

        // --- campos estaticos ---
        uint8_t    *static_data;
        FieldInfo  *static_fields;
        size_t      static_field_count;

        // --- todos los metodos (para reflexion) ---
        MethodInfo *methods;
        size_t      method_count;
        // --- reflexion/documentacion ---
        stringx     doc;        ///< docstring de la clase
        AttrEntry  *attrs;      ///< tabla de anotaciones clave/valor
        size_t      attr_count;

        // --- genericos (monomorphization) ---
        GenericParam *type_params;      ///< parametros de tipo (nombre + restriccion)
        size_t        type_param_count; ///< numero de parametros de tipo
        ClassInfo    *generic_parent;   ///< clase generica original (para especializaciones)

        // --- modulos ---
        stringx      module_name;  ///< nombre calificado del modulo propietario ("com.vesta.col")
        uint64_t     visibility;   ///< MODULE_VIS_EXPORT o MODULE_VIS_INTERNAL

        // --- caches de lookup acelerado (open addressing, potencia de 2) ---
        /// Tabla hash de fields por nombre (incluye instance + static).
        /// Tamano = field_lookup_mask + 1 (siempre potencia de 2).  Slot
        /// vacio: name_hash == 0.  Si el cache no esta inicializado
        /// field_lookup_table es NULL y el caller debe recorrer fields[]
        /// linealmente.  Construido por ClassRegistry::finalize().
        LookupSlot  *field_lookup_table;
        uint32_t     field_lookup_mask;
        uint32_t     _flpad;       ///< alineacion (reservado, debe ser 0)

        /// Tabla hash de metodos por nombre (mapea a indice del vtable).
        LookupSlot  *method_lookup_table;
        uint32_t     method_lookup_mask;
        uint32_t     _mlpad;       ///< alineacion (reservado, debe ser 0)
    } ClassInfo;

    // -------------------------------------------------------------------------
    //  GenericParam - descriptor de un parametro de tipo en una clase generica
    //
    //  Cada parametro tiene un nombre simbolico ("T", "K", "V"), una restriccion
    //  opcional de tipo (bound) y, en especializaciones concretas, el tipo real
    //  instanciado.
    //
    //  En la plantilla generica original:  constraint = bound o nullptr; concrete = nullptr.
    //  En una especializacion (p.ej. List<int>): concrete = ClassInfo de int.
    //  constraint se preserva para poder validar que concrete cumple el bound.
    // -------------------------------------------------------------------------
    struct GenericParam {
        const char *name;       ///< nombre del parametro ("T", "K", "V", ...)
        ClassInfo  *constraint; ///< restriccion de tipo / bound (nullptr = sin restriccion)
        ClassInfo  *concrete;   ///< tipo concreto instanciado (nullptr en la plantilla generica)
    };

    // -------------------------------------------------------------------------
    //  ObjectHeader - cabecera que precede al payload de todo objeto
    //
    //  Layout en memoria:
    //    [GcHeader (8B)]      <- solo en objetos GC (antes del payload)
    //    [ObjectHeader (24B)] <- inicio del payload; aqui apunta GcHeap::deref()
    //    [campos del objeto]
    //
    //  Cambio de ABI respecto a v1:
    //    Se anaden owner_pid (4B), lock_depth (2B) y _mon_pad (2B) para
    //    soportar monitores (instrucciones monenter/monexit/monwait/monnoti/monnota).
    //    owner_pid almacena el local_pid del proceso propietario del monitor
    //    (0 = monitor libre).  lock_depth permite locks reentrantes.
    // -------------------------------------------------------------------------
    struct alignas(8) ObjectHeader {
        ClassInfo *class_ptr;  ///< 8 bytes - puntero a los metadatos de la clase
        uint32_t   flags;      ///< 4 bytes - OBJ_FLAG_*
        uint32_t   hash_code;  ///< 4 bytes - identidad del objeto (lazy)
        uint32_t   owner_pid;  ///< 4 bytes - local_pid del propietario del monitor (0=libre)
        uint16_t   lock_depth; ///< 2 bytes - contador de locks reentrantes
        uint16_t   _mon_pad;   ///< 2 bytes - relleno de alineacion
    };
    static_assert(sizeof(ObjectHeader) == 24,
                  "ObjectHeader debe medir exactamente 24 bytes");

    // -------------------------------------------------------------------------
    //  FrameHeader - frame en la cadena de llamadas (para throw/catch)
    // -------------------------------------------------------------------------
    typedef struct FrameHeader {
        FrameHeader *prev;            ///< frame del llamante (linked list)
        MethodInfo  *method;          ///< metodo en ejecucion
        uint64_t     return_pc;       ///< PC virtual al que volver al hacer ret
        uint64_t     frame_base;      ///< SP en el momento de la llamada
        /// Para AOP @Around: si este frame es un advice AROUND, apunta al
        /// MethodInfo* del target original (lo que el bytecode `proceed`
        /// invoca).  Es nullptr para frames normales (no-around).  Permite
        /// que `proceed()` funcione como una llamada implicita al wrapped
        /// method, con la misma calling convention que el receptor original.
        MethodInfo  *proceed_target;
    } FrameHeader;

    // -------------------------------------------------------------------------
    //  ClosureObject - closure gestionado por el GC
    //
    //  Layout en memoria (tras GcHeader de 8B):
    //    [ObjectHeader (16B)] <- header.class_ptr apunta a la ClassInfo de closure
    //    [method    (8B)]     <- puntero al MethodInfo del lambda/funcion capturada
    //    [captures  (8B)]     <- puntero a array de GcHandle de las capturas
    //    [cap_count (8B)]     <- numero de variables capturadas
    //
    //  Se crea con la instruccion mkclosure y se invoca con callclosure.
    //  El GC escanea el array captures como raices durante la fase de marcado.
    // -------------------------------------------------------------------------
    struct alignas(8) ClosureObject {
        ObjectHeader header;     ///< cabecera OOP; class_ptr -> ClassInfo con CLASS_FLAG_CLOSURE
        MethodInfo  *method;     ///< metodo o lambda capturado
        uint32_t    *captures;   ///< array de GcHandle (uint32_t) de las variables capturadas
        size_t       cap_count;  ///< numero de entradas en captures
    };

    // -------------------------------------------------------------------------
    //  RawClosureObject - closure no gestionado por GC (para FFI nativo)
    //
    //  Almacenado via RawAllocator. La funcion apuntada por fn_addr usa la
    //  convencion de llamada calln: r1-r12 argumentos, r0 retorno, r15 argc.
    //  El bloque de entorno en env_addr es opaco para la VM.
    // -------------------------------------------------------------------------
    struct alignas(8) RawClosureObject {
        uint64_t fn_addr;   ///< direccion de la funcion nativa o bytecode
        uint64_t env_addr;  ///< direccion del bloque de entorno en memoria VM
        size_t   env_size;  ///< tamano del bloque de entorno en bytes
    };

    // -------------------------------------------------------------------------
    //  FutureState - estado del ciclo de vida de un FutureObject
    // -------------------------------------------------------------------------
    enum class FutureState : uint8_t {
        PENDING  = 0, ///< la promesa aun no se ha cumplido ni rechazado
        RESOLVED = 1, ///< cumplida con un valor por FULFILL
        REJECTED = 2, ///< rechazada con un codigo de error por REJECT
    };

    // -------------------------------------------------------------------------
    //  FutureObject - promesa asincrona gestionada por el GC
    //
    //  Creado por FUTURE (0x29). El proceso que ejecuta AWAIT (0x2A) queda
    //  suspendido con blocking=true hasta que FULFILL (0x2B) o REJECT (0x2C)
    //  resuelvan la promesa y llamen a make_ready() del proceso esperador.
    //
    //  Layout en memoria (tras GcHeader de 8B):
    //    [ObjectHeader (16B)] <- header OOP
    //    [state      (1B)]    <- FutureState
    //    [_pad       (7B)]    <- alineacion
    //    [result     (8B)]    <- valor (RESOLVED) o codigo de error (REJECTED)
    //    [waiter_pid (8B)]    <- PID codificado del proceso esperador (0 si nadie)
    // -------------------------------------------------------------------------
    struct alignas(8) FutureObject {
        ObjectHeader header;      ///< cabecera OOP; flags = OBJ_FLAG_GC_OWNED
        FutureState  state;       ///< estado actual del future
        uint8_t      _pad[7];     ///< relleno de alineacion
        uint64_t     result;      ///< valor resuelto o codigo de error
        uint64_t     waiter_pid;  ///< PID codificado del proceso esperador (0 si ninguno)
    };

} // namespace loader

#endif // OOP_TYPES_H
