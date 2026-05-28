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
 * @file types.h
 * @brief Representacion canonica del sistema de tipos del lenguaje Vex.
 *
 * En Vex se aceptan dos estilos sintacticos para los tipos primitivos
 * (estilo C++/stdint con sufijo _t y estilo corto sin sufijo); ambos
 * son tokens distintos pero designan exactamente el mismo tipo a
 * efectos semanticos.  Este modulo concentra la conversion canonica:
 *
 *   uint8_t  <->  u8   <->  PrimitiveKind::U8
 *   int32_t  <->  i32  <->  PrimitiveKind::I32
 *   double   <->  f64  <->  PrimitiveKind::F64
 *   ...
 *
 * La estructura @c Type
 * ya esta disenyada como union etiquetada por extension futura: el
 * miembro discriminador @c kind permite anyadir variantes sin tocar
 * los call sites.
 *
 * Decision de hardware:
 *   - PrimitiveKind cabe en un byte; permite serializarlo en flat
 *     arrays y mantenerlo en cache cuando el type checker recorre
 *     muchos tipos (e.g. al validar parametros de una funcion).
 */

#ifndef VEX_TYPES_H
#define VEX_TYPES_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vex/token.h"

namespace vex {

    /**
     * @enum PrimitiveKind
     * @brief Tipos primitivos canonicos del lenguaje Vex.
     *
     * Los valores ordinales son estables.  Si se anyaden nuevos tipos,
     * hacerlo SIEMPRE al final del enum para que las tablas planas
     * indexadas por (uint8_t)kind permanezcan validas.
     */
    enum class PrimitiveKind : uint8_t {
        VOID = 0,
        BOOL,
        CHAR,
        I8,
        I16,
        I32,
        I64,
        U8,
        U16,
        U32,
        U64,
        F32,
        F64,
        /// Puntero raw (8 bytes en x86-64). solo se usa como tipo
        /// del literal de string para FFI; en hitos posteriores aparece como
        /// T* generico 
        PTR,
        /// Tipo agregado declarado por @c struct.  El nombre del struct se
        /// guarda en @c Type::struct_name; el TypeChecker mantiene el
        /// layout (offsets de campos + tamano total) en una tabla aparte.
        STRUCT,
        /// Tipo de instancia de clase Vex (reference type).  El nombre de
        /// la clase se guarda en @c Type::struct_name (reusamos el campo
        /// para no duplicar storage).  A diferencia de STRUCT, las
        /// instancias viven en heap (NEWOBJ) y se acceden via puntero;
        /// igualdad estructural compara por nombre de clase.
        CLASS,
        /// Array nativo @c T[N] o @c T[].  El tipo de elemento se guarda en
        /// @c Type::pointee y el numero de elementos en @c Type::array_size
        /// (0 indica @c T[] sin size, usado como parametro de funcion para
        /// permitir el decay-to-pointer).  El layout es contiguo, sin
        /// header de longitud (no es @c Array<T> gestionado).
        ARRAY,
        /// `Optional<T>` builtin del compilador.  NO se modela como
        /// clase template: el lowering emite directamente un buffer en
        /// stack de 16 bytes (8 flag + 8 valor).  Tipo del payload en
        /// @c pointee.  Sin allocacion de heap, sin clase concreta por
        /// instanciado: el bytecode es identico para todos los Optional
        /// y solo varia el ancho del LOAD/STORE del payload.
        OPTIONAL,
        /// `Result<V, E>` builtin del compilador.  Stack value de
        /// 24 bytes: 8 tag (0=err, 1=ok) + 8 V + 8 E.  V en @c pointee y
        /// E en @c pointee2.
        RESULT,
        /// Tipo @c fn(T1, T2, ...) -> R: closure / function pointer.
        /// Layout en memoria: 16 bytes = `[+0 i64 fn_addr][+8 i64 env_addr]`.
        /// Cuando la lambda no captura nada, env_addr = 0 (sentinela "sin
        /// env") y la calling convention degenera a un callvmr puro sin
        /// pase de env.  Cuando captura N variables, env_addr apunta a un
        /// bloque contiguo en la pila del CALLER del cual el helper lee
        /// los captures por offset fijo.  Sin GC: el env vive en el stack
        /// frame del scope que crea la lambda; cuando ese scope sale, la
        /// lambda deja de ser valida (escape detection no implementado en
        /// MVP, vease README para extension a heap).
        ///
        /// Type carries the full signature in `fn_params` (parameter types)
        /// and `pointee` (return type); el call site las usa para validar
        /// aridad y compatibilidad de argumentos en compile time.
        FUNCTION,
        /// Tipo @c string de Vex.  Reference-type al @c StringObject
        /// gestionado por GC.  Internamente es un GcHandle (i64) que el
        /// runtime dereferencia para acceder al header (encoding, length,
        /// byte_len, hash) y al buffer FLAT/ROPE/SLICE.  Cero overhead vs
        /// PTR; solo el type checker y el lowering distinguen entre
        /// @c string (managed) y @c cstring (alias de @c char* raw).
        ///
        /// Operaciones soportadas (todas via opcodes 0x46-0x54 del runtime):
        ///   s + t     -> STRCAT (rope concatenation O(1))
        ///   s == t    -> STRCMP (byte-level equality)
        ///   s.length()-> STRLEN (count code points)
        ///   s.bytes() -> STRGETBYTES (raw byte count)
        ///   s.cstr()  -> STRRAW (host_ptr al buffer NUL-terminated)
        ///   s.wstr()  -> STRCONV(UTF16) + STRRAW (host_ptr a wchar_t*)
        ///   s.slice(a,b) -> STRSLICE (vista O(1) sin copia)
        ///   s.intern()-> STRINTERN (canonical pool)
        ///   s.hash()  -> STRHASH (FNV-1a cacheado)
        ///   s.encoding() -> STRGETENC
        ///
        /// Literales `"hola"` siguen siendo PTR a static_data (cero coste);
        /// solo se promueven a StringObject via STRMAKE cuando se asignan
        /// a una variable @c string o se usan en operaciones que lo
        /// requieren (lazy promotion).
        STRING,
        /// Tipos primitivos de coleccion.  Cada uno modela un GcHandle
        /// opaco (i64, host pointer al descriptor del plugin nativo
        /// `vesta_collections.dll`).  El frontend trata estos tipos como
        /// primitivos, NO como clases: cero overhead vs llamar el plugin
        /// directo (sin vtable, sin CALLVIRT).  Las operaciones se despachan
        /// como builtins (CALLN directo) y la liberacion es AUTOMATICA al
        /// salir del scope (cleanup_stack_ del lowering).
        ///
        ///   ArrayList - array dinamico de uint64_t (vcol_alist_*)
        ///   HashMap   - tabla hash uint64->uint64 (vcol_map_*, swisstable+SSE2)
        ///   HashSet   - set hash uint64 (vcol_set_*)
        ///   Queue     - ring buffer FIFO uint64 (vcol_queue_*)
        ///   Deque     - ring buffer doble extremos (vcol_deque_*)
        ///   TreeMap   - mapa ordenado Red-Black uint64->uint64 (vcol_tmap_*)
        ///   TreeSet   - set ordenado Red-Black (vcol_tset_*)
        ///   Stack     - alias logico de ArrayList con LIFO (push/pop)
        ARRAYLIST,
        HASHMAP,
        HASHSET,
        QUEUE,
        DEQUE,
        TREEMAP,
        TREESET,
        STACK,
        /// `Future<T>` builtin del compilador (Mejora II).  Reference-type
        /// que envuelve el handle del FutureObject runtime (i64) preservando
        /// el tipo logico T del valor que el future resolvera.  El bytecode
        /// no cambia: FutureObject::result sigue siendo i64 opaco.  El
        /// frontend hace coercion T -> i64 al @c fulfill (zero/sign-extend
        /// para tipos < 8B) y i64 -> T al @c await (truncate/cast back).
        ///
        /// @c pointee = tipo T (payload del future).
        ///
        /// Limitaciones MVP:
        ///   - T debe tener tamano <= 8 bytes (i8..i64, u8..u64, f32, f64,
        ///     bool, char, ptr/handle).  Tipos compuestos (struct, array)
        ///     requeririan futures con buffer auxiliar (deferido).
        ///   - El handle queda en HandleTable hasta que el caller hace
        ///     await; tras await, el future muere salvo que algo lo
        ///     referencie (e.g., otra closure).
        FUTURE,
        /// `unique<T>` builtin del compilador (smart pointer move-only).
        /// Modelado como un slot de stack de 8 bytes (Tier 0: deleter
        /// conocido en compile-time, p.ej. @c free) o 16 bytes (Tier 1:
        /// deleter custom guardado al lado del ptr).
        ///
        /// Tier 0 (caso 95%): `[+0 u64 ptr]`.  El deleter se decide en
        /// compile-time por el builtin de construccion (@c unique_malloc
        /// usa @c free; @c unique_new usa @c ~T() virtual; @c unique_fopen
        /// usa @c fclose).  Cleanup al exit del scope emite un CALL
        /// directo al deleter, sin indireccion.
        ///
        /// Tier 1 (caso 4%): `[+0 u64 ptr][+8 u64 deleter_fn]`.  El
        /// deleter es variable runtime (capturado en construccion).
        /// Cleanup emite un CALL indirecto via reg.  Si el deleter es
        /// literal, el compilador colapsa a Tier 0.
        ///
        /// Move semantics: `unique<T> q = move(p)` emite UN solo opcode
        /// @c mvtake (0x72) que copia el ptr de p a q y zerifica p en
        /// 1 instr VM (3 instr host x86-64 tras JIT).
        ///
        /// @c pointee = tipo T del recurso apuntado.
        /// `borrow<T>` (shared) y `borrow_mut<T>` (exclusivo) builtins
        /// del compilador.  Modelan un puntero T* host pero con
        /// VERIFICACION DE ALIASING en compile-time (borrow checker).
        ///
        /// Runtime: ambos son 8 bytes (host_ptr, identico a `T*`).
        /// Toda la seguridad esta en el type checker:
        ///   R1: solo UN borrow_mut activo OR multiples borrow shared.
        ///   R2: prohibido leer/mutar/mover el owner mientras prestado.
        ///   R3: los borrows no pueden sobrevivir al owner (no escape).
        ///
        /// @c pointee = tipo T apuntado.
        BORROW,
        BORROW_MUT,
        UNIQUE_PTR,
        /// `shared<T>` builtin del compilador (smart pointer con refcount).
        /// Slot de stack de 8 bytes (host_ptr al control block).  El
        /// control block vive en el GcHeap y se aloca via @c gcallocp
        /// (alocacion unica con el payload contiguo, estilo
        /// @c std::make_shared):
        ///
        ///   `[+0 i64 refcount][+8 u64 deleter][+16 T payload]`
        ///
        /// La copia incrementa el refcount; la destruccion al exit lo
        /// decrementa y, si llega a 0, ejecuta el deleter.  No atomic
        /// por defecto (single-thread).  Con @c @ThreadSafe se usaria
        /// CAS, pero el MVP mantiene refcount simple.
        ///
        /// @c pointee = tipo T del payload.
        SHARED_PTR,
        /// Type-as-first-class-value.  Sentinela usado SOLO en
        /// declaraciones de @c comptime const Type T = comptime_type<X>().
        /// No tiene representacion runtime (cero bytes); el ComptimeConst
        /// asociado lleva el @c Type real en su campo @c type_val.  Al
        /// usar `T` en posicion de tipo, `type_from_node` lo resuelve via
        /// @c comptime_const_values_.
        TYPE_META,
        // Sentinela para construir tablas planas.
        COUNT
    };

    /**
     * @struct Type
     * @brief Representacion del tipo de una expresion o declaracion.
     *
     * El miembro @c kind es el discriminador principal; campos adicionales
     * (@c pointee para punteros, @c fn_params/fn_return para funciones,
     * @c element_type para arrays, @c struct_name para structs/clases/
     * enums, @c is_virtual para distinguir punteros VM vs host, etc.)
     * estan declarados pero solo son relevantes para el @c kind apropiado.
     *
     * Layout: 1 byte util + padding -> queda en 1 cache line incluso
     * cuando se almacenan miles de tipos juntos.
     */
    struct Type {
        PrimitiveKind kind = PrimitiveKind::VOID;
        /// Nombre del struct (vacio para todo lo demas).  Se incluye aqui
        /// porque distinguir @c Punto de @c Color requiere el nombre, y
        /// porque el numero de structs por modulo es pequenyo (tipicamente
        /// decenas), por lo que el coste del std::string en cache es
        /// despreciable comparado con la simplicidad del modelo.  Si en
        /// el futuro la presion de cache importa, se puede sustituir por
        /// un indice a una pool de StructLayout en TypeChecker.
        std::string   struct_name;
        /// Tipo apuntado cuando @c kind == PTR o tipo de elemento cuando
        /// @c kind == ARRAY; nulo para todo lo demas.  Se usa @c shared_ptr
        /// porque @c Type debe ser copiable (el AST Type vive como valor en
        /// cada Expr) y porque la cadena puede ser arbitraria (T**, T**[5]
        /// etc.).  El coste de heap por nivel es aceptable: pocos arrays /
        /// punteros por modulo y el campo solo se materializa cuando aplica.
        std::shared_ptr<Type> pointee;
        /// Segundo tipo para cuando @c kind == RESULT (V en @c pointee, E
        /// aqui).  No se usa para los demas kinds.
        std::shared_ptr<Type> pointee2;
        /// Tamano del array cuando @c kind == ARRAY; 0 si es @c T[] (decay).
        /// No se usa para los otros kinds.
        uint32_t array_size = 0;

        /// Naturaleza del puntero cuando @c kind == PTR o ARRAY.
        /// false = puntero HOST (default; los datos viven en memoria del
        ///         proceso real, accesibles via opcode @c movh).  Producido
        ///         por @c malloc, @c str_cstr, FFI, fields de objetos GC, etc.
        /// true  = puntero VIRTUAL (la direccion es offset dentro del
        ///         vm_mem del proceso VM, accesible via opcode @c mov).
        ///         Producido por @c &local, @c T[N] arrays nativos
        ///         locales, y por la sintaxis explicita @c VirtualPtr<T>.
        ///
        /// La naturaleza de la direccion (host/virtual) y el tipo del
        /// contenido (apuntado por @c pointee) son ortogonales: un
        /// @c VirtualPtr<u8*> es una direccion VM cuyo contenido es a su
        /// vez un puntero HOST (resultado de @c malloc por ejemplo).
        ///
        /// La aritmetica preserva el flag.  Mezclar host con virtual en
        /// asignacion / comparacion es error de tipo.
        bool is_virtual = false;

        /// Tipos de los parametros cuando @c kind == FUNCTION.  Vacio para
        /// los demas kinds.  La lista se materializa solo cuando se crea un
        /// tipo @c fn(...), por lo que el coste de cache para tipos no-fn
        /// es solo el de un std::vector vacio (3 punteros, 24 bytes en x64
        /// libstdc++) que se queda sin alocar nada en heap.  Para llamar a
        /// un function value, el call site recorre esta lista para validar
        /// aridad y tipos en compile time.
        std::vector<Type> fn_params;

        /// Newtype nominal ID (typedef T name new).  0 = no es newtype
        /// (alias transparente clasico).  Cualquier valor > 0 identifica
        /// univocamente al newtype: dos Type con kinds/representacion
        /// identicos pero distinto @c nominal_id son tipos DIFERENTES.
        /// El ID se asigna en el type checker (counter global) cuando
        /// se procesa la TypeAliasDecl con marca @c is_newtype.
        /// Conservacion: cualquier copia de un Type preserva el ID;
        /// la representacion subyacente (kind, pointee, etc.) se mantiene
        /// para que el lowering y el ABI nativo no cambien.
        uint32_t nominal_id = 0;

        /// @c true si el newtype es @c @opaque: el usuario NO puede leer
        /// los bits subyacentes (no hay cast implicito ni inicializacion
        /// por literal).  Solo se puede construir/consumir via funciones
        /// que lo manejen explicitamente.  Sin efecto si @c nominal_id == 0.
        bool is_opaque = false;

        /// Nombre humano-legible del newtype (para mensajes de error e
        /// introspeccion).  Vacio si no es newtype.
        std::string nominal_name;

        /// Alineacion forzada en bytes (sintaxis @c "@align(N)").  0 = sin
        /// override (usar alineacion natural del kind).  Debe ser potencia
        /// de 2 en [1, 4096].  Aplica cuando el tipo se usa como campo de
        /// struct o como ALLOCA: el frontend padea hasta cumplir el
        /// requirement.  Util para SIMD (16/32/64), DMA, GPU descriptors,
        /// protocolos de wire format con padding fijo.
        uint16_t align_override = 0;

        Type() = default;
        explicit Type(PrimitiveKind k) : kind(k) {}
        Type(PrimitiveKind k, std::string sn) : kind(k), struct_name(std::move(sn)) {}

        /**
         * @brief Construye un PTR a otro tipo (host por defecto).
         * @param virt true para VirtualPtr<T> (direccion VM), false para
         *             T* convencional (host).
         */
        static Type make_ptr(Type pointee_ty, bool virt = false) {
            Type t;
            t.kind = PrimitiveKind::PTR;
            t.pointee = std::make_shared<Type>(std::move(pointee_ty));
            t.is_virtual = virt;
            return t;
        }

        /**
         * @brief Construye un ARRAY de tamano fijo (size > 0) o variable (size == 0).
         *
         * Los arrays nativos (@c T[N] declarados como local o miembro) son
         * SIEMPRE virtuales: el ALLOCA reserva en stack VM.  Por eso
         * @c is_virtual queda a true por defecto.  Los arrays cargados de
         * memoria host (rara vez) deben construirse con virt=false.
         */
        static Type make_array(Type element_ty, uint32_t size, bool virt = true) {
            Type t;
            t.kind       = PrimitiveKind::ARRAY;
            t.pointee    = std::make_shared<Type>(std::move(element_ty));
            t.array_size = size;
            t.is_virtual = virt;
            return t;
        }

        /**
         * @brief Compara dos tipos por igualdad estructural (sigue la cadena de punteros).
         */
        bool operator==(const Type &o) const noexcept {
            // Newtypes con distinto nominal_id son tipos DIFERENTES aunque
            // la representacion subyacente coincida.  nominal_id == 0
            // significa "no es newtype" -> caen al check estructural.
            if (nominal_id != o.nominal_id) return false;
            // align_override forma parte de la identidad del tipo: dos
            // newtypes con misma representacion pero @align(N) distinto
            // son tipos distintos (al menos para evitar mezcla accidental).
            if (align_override != o.align_override) return false;
            if (kind != o.kind || struct_name != o.struct_name) return false;
            // Para PTR/ARRAY la naturaleza host vs virtual ES parte de la
            // identidad de tipo: `T*` y `VirtualPtr<T>` son tipos distintos.
            if ((kind == PrimitiveKind::PTR || kind == PrimitiveKind::ARRAY)
             && is_virtual != o.is_virtual) return false;
            // Para ARRAY el tamano es parte de la identidad (i32[3] != i32[5]).
            if (kind == PrimitiveKind::ARRAY && array_size != o.array_size) return false;
            // Para PTR/ARRAY/OPTIONAL/RESULT/FUNCTION comparamos pointee
            // (return type para FUNCTION) y, si aplica, pointee2 (E del
            // Result).
            const bool a_has = static_cast<bool>(pointee);
            const bool b_has = static_cast<bool>(o.pointee);
            if (a_has != b_has) return false;
            if (a_has && b_has && !(*pointee == *o.pointee)) return false;
            if (kind == PrimitiveKind::RESULT) {
                const bool a2 = static_cast<bool>(pointee2);
                const bool b2 = static_cast<bool>(o.pointee2);
                if (a2 != b2) return false;
                if (a2 && b2 && !(*pointee2 == *o.pointee2)) return false;
            }
            // Para FUNCTION la lista de parametros tambien forma parte de
            // la identidad estructural.  Dos `fn(i32) -> i32` con distinta
            // aridad o distinto tipo de parametro son tipos diferentes.
            if (kind == PrimitiveKind::FUNCTION) {
                if (fn_params.size() != o.fn_params.size()) return false;
                for (size_t i = 0; i < fn_params.size(); ++i) {
                    if (!(fn_params[i] == o.fn_params[i])) return false;
                }
            }
            return true;
        }

        /// @brief Construye un Optional<T> builtin.  Layout en memoria
        /// (compartido por todos los Optional, sin clase concreta):
        /// `[+0 i64 flag][+8 8-byte payload]`.  El pointee es el tipo
        /// del payload; el frontend usa este tipo para decidir el ancho
        /// del LOAD/STORE en Some/unwrap.
        static Type make_optional(Type inner) {
            Type t;
            t.kind    = PrimitiveKind::OPTIONAL;
            t.pointee = std::make_shared<Type>(std::move(inner));
            return t;
        }
        /// @brief Construye un Result<V, E> builtin.  Layout 24 bytes:
        /// `[+0 i64 tag (0=err, 1=ok)][+8 V][+16 E]`.
        static Type make_result(Type ok_t, Type err_t) {
            Type t;
            t.kind     = PrimitiveKind::RESULT;
            t.pointee  = std::make_shared<Type>(std::move(ok_t));
            t.pointee2 = std::make_shared<Type>(std::move(err_t));
            return t;
        }

        /**
         * @brief Construye un tipo @c fn(P1, P2, ...) -> R (closure).
         *
         * Layout en memoria: 16 bytes = `[+0 i64 fn_addr][+8 i64 env_addr]`.
         * Dos function types son iguales si tienen el mismo numero de
         * parametros con tipos identicos en el mismo orden y el mismo
         * return type.  La firma se usa en compile time para validar la
         * llamada y deducir el tipo del resultado.
         *
         * @param params Tipos de los parametros (en orden).  Se mueve.
         * @param ret    Tipo de retorno.  Se mueve.
         * @return Type con kind = FUNCTION, pointee = ret, fn_params = params.
         */
        static Type make_function(std::vector<Type> params, Type ret) {
            Type t;
            t.kind      = PrimitiveKind::FUNCTION;
            t.pointee   = std::make_shared<Type>(std::move(ret));
            t.fn_params = std::move(params);
            return t;
        }

        /// @brief Construye un Future<T> builtin (Mejora II).  Layout en
        /// memoria: 8 bytes (handle del FutureObject runtime).  El @c pointee
        /// es el tipo T del payload que el future resolvera.  Dos
        /// @c Future<T> son iguales si los T son iguales.
        static Type make_future(Type inner) {
            Type t;
            t.kind    = PrimitiveKind::FUTURE;
            t.pointee = std::make_shared<Type>(std::move(inner));
            return t;
        }

        /// @brief Construye un @c borrow<T> (shared) o @c borrow_mut<T>
        /// (exclusive).  Layout 8 bytes (host_ptr).  El borrow checker
        /// del type_checker valida las reglas de aliasing en compile-time.
        static Type make_borrow(Type inner, bool is_mut) {
            Type t;
            t.kind    = is_mut ? PrimitiveKind::BORROW_MUT : PrimitiveKind::BORROW;
            t.pointee = std::make_shared<Type>(std::move(inner));
            return t;
        }

        /// @brief Construye un @c unique<T> builtin (smart pointer move-only).
        /// El campo @c is_virtual del Type result distingue HOST vs VM,
        /// heredado del Type apuntado segun lo decida el builtin de
        /// construccion (e.g. @c unique_malloc devuelve host, una
        /// hipotetica @c unique_local devolveria virtual).  Por
        /// defecto: host (la mayoria de usos: malloc/new/fopen).
        static Type make_unique(Type inner) {
            Type t;
            t.kind    = PrimitiveKind::UNIQUE_PTR;
            t.pointee = std::make_shared<Type>(std::move(inner));
            return t;
        }

        /// @brief Construye un @c shared<T> builtin (smart pointer con refcount).
        /// El control block + payload viven en el GcHeap (heap GC); el
        /// slot stack contiene un host_ptr al control block.
        static Type make_shared(Type inner) {
            Type t;
            t.kind    = PrimitiveKind::SHARED_PTR;
            t.pointee = std::make_shared<Type>(std::move(inner));
            return t;
        }

        bool operator!=(const Type &o) const noexcept {
            return !(*this == o);
        }
    };

    /**
     * @brief Convierte un TokenKind de palabra reservada de tipo a PrimitiveKind.
     *
     * Acepta tanto el estilo corto (i8, u32, f64) como el estilo C/stdint
     * (int8_t, uint32_t, double).  Ambos producen el mismo PrimitiveKind.
     *
     * @param k Token producido por el lexer.
     * @return PrimitiveKind canonico, o @c PrimitiveKind::COUNT si @p k no
     *         designa un tipo primitivo.  El caller debe chequear contra
     *         COUNT antes de usar el valor.
     */
    constexpr PrimitiveKind primitive_kind_from_token(TokenKind k) noexcept {
        switch (k) {
            // Bloque "compartido" (sin alias).
            case TokenKind::KW_VOID: return PrimitiveKind::VOID;
            case TokenKind::KW_BOOL: return PrimitiveKind::BOOL;
            case TokenKind::KW_CHAR: return PrimitiveKind::CHAR;

            // Estilo corto.
            case TokenKind::KW_INT8:  return PrimitiveKind::I8;
            case TokenKind::KW_INT16: return PrimitiveKind::I16;
            case TokenKind::KW_INT32: return PrimitiveKind::I32;
            case TokenKind::KW_INT64: return PrimitiveKind::I64;

            case TokenKind::KW_UINT8:  return PrimitiveKind::U8;
            case TokenKind::KW_UINT16: return PrimitiveKind::U16;
            case TokenKind::KW_UINT32: return PrimitiveKind::U32;
            case TokenKind::KW_UINT64: return PrimitiveKind::U64;

            case TokenKind::KW_F32:    return PrimitiveKind::F32;
            case TokenKind::KW_F64:    return PrimitiveKind::F64;

            // Estilo C/stdint.
            case TokenKind::KW_INT8_T:  return PrimitiveKind::I8;
            case TokenKind::KW_INT16_T: return PrimitiveKind::I16;
            case TokenKind::KW_INT32_T: return PrimitiveKind::I32;
            case TokenKind::KW_INT64_T: return PrimitiveKind::I64;

            case TokenKind::KW_UINT8_T:  return PrimitiveKind::U8;
            case TokenKind::KW_UINT16_T: return PrimitiveKind::U16;
            case TokenKind::KW_UINT32_T: return PrimitiveKind::U32;
            case TokenKind::KW_UINT64_T: return PrimitiveKind::U64;

            case TokenKind::KW_FLOAT:  return PrimitiveKind::F32;
            case TokenKind::KW_DOUBLE: return PrimitiveKind::F64;

            // Tipo string dedicado (referencia a StringObject GC-managed).
            case TokenKind::KW_STRING: return PrimitiveKind::STRING;

            // tipos primitivos de coleccion (handles del plugin
            // vesta_collections.dll, gestionados con auto-free al exit).
            case TokenKind::KW_ARRAYLIST: return PrimitiveKind::ARRAYLIST;
            case TokenKind::KW_HASHMAP:   return PrimitiveKind::HASHMAP;
            case TokenKind::KW_HASHSET:   return PrimitiveKind::HASHSET;
            case TokenKind::KW_QUEUE:     return PrimitiveKind::QUEUE;
            case TokenKind::KW_DEQUE:     return PrimitiveKind::DEQUE;
            case TokenKind::KW_TREEMAP:   return PrimitiveKind::TREEMAP;
            case TokenKind::KW_TREESET:   return PrimitiveKind::TREESET;
            case TokenKind::KW_STACK:     return PrimitiveKind::STACK;

            // Smart pointers builtins (move-only + refcount).  El payload T
            // se completa en el parser via `unique<T>` / `shared<T>`.
            case TokenKind::KW_UNIQUE:    return PrimitiveKind::UNIQUE_PTR;
            case TokenKind::KW_SHARED:    return PrimitiveKind::SHARED_PTR;
            // Borrows (referencias compile-time-checkadas, runtime = host_ptr).
            case TokenKind::KW_BORROW:    return PrimitiveKind::BORROW;
            case TokenKind::KW_BORROW_MUT: return PrimitiveKind::BORROW_MUT;

            // No es un tipo primitivo.
            default: return PrimitiveKind::COUNT;
        }
    }

    /**
     * @brief @c true si k designa un tipo entero (con o sin signo).
     */
    constexpr bool is_integral(PrimitiveKind k) noexcept {
        // Rango contiguo en el enum: I8..U64.  Comparacion numerica
        // mas barata que un switch lineal.
        return (uint8_t)k >= (uint8_t)PrimitiveKind::I8
            && (uint8_t)k <= (uint8_t)PrimitiveKind::U64;
    }

    /**
     * @brief @c true si k designa un tipo entero con signo.
     */
    constexpr bool is_signed_integral(PrimitiveKind k) noexcept {
        return (uint8_t)k >= (uint8_t)PrimitiveKind::I8
            && (uint8_t)k <= (uint8_t)PrimitiveKind::I64;
    }

    /**
     * @brief @c true si k designa un tipo flotante (f32 o f64).
     */
    constexpr bool is_floating(PrimitiveKind k) noexcept {
        return k == PrimitiveKind::F32 || k == PrimitiveKind::F64;
    }

    /**
     * @brief @c true si k designa un tipo numerico (entero o flotante).
     */
    constexpr bool is_numeric(PrimitiveKind k) noexcept {
        return is_integral(k) || is_floating(k);
    }

    /**
     * @brief Tamano en bytes del tipo primitivo.
     *
     * @return Numero de bytes ocupados por una instancia, o 0 para VOID.
     */
    constexpr size_t primitive_size_bytes(PrimitiveKind k) noexcept {
        switch (k) {
            case PrimitiveKind::VOID: return 0;
            case PrimitiveKind::BOOL: return 1;
            case PrimitiveKind::CHAR: return 1;
            case PrimitiveKind::I8:   return 1;
            case PrimitiveKind::I16:  return 2;
            case PrimitiveKind::I32:  return 4;
            case PrimitiveKind::I64:  return 8;
            case PrimitiveKind::U8:   return 1;
            case PrimitiveKind::U16:  return 2;
            case PrimitiveKind::U32:  return 4;
            case PrimitiveKind::U64:  return 8;
            case PrimitiveKind::F32:  return 4;
            case PrimitiveKind::F64:  return 8;
            case PrimitiveKind::PTR:  return 8;
            // Para STRUCT y ARRAY el caller debe consultar metadata extra
            // (StructLayout, array_size + sizeof(element)).  Devolvemos 0
            // como sentinela y el callsite valida.
            case PrimitiveKind::STRUCT: return 0;
            // CLASS es un reference type: la "variable" guarda un puntero
            // al ObjectHeader, asi que ocupa 8 bytes.  El tamano de la
            // instancia (instance_size) lo gestiona el ClassRegistry.
            case PrimitiveKind::CLASS:  return 8;
            case PrimitiveKind::ARRAY:  return 0;
            // FUNCTION es siempre un par (fn_addr, env_addr) -> 16 bytes.
            // Cero overhead vs un puntero plano cuando no hay capturas
            // (env_addr = 0) y captura "first-class" cuando si las hay.
            case PrimitiveKind::OPTIONAL: return 16;
            case PrimitiveKind::RESULT:   return 24;
            case PrimitiveKind::FUNCTION: return 16;
            // string es un GcHandle (i64) opaco al frontend; el StringObject
            // real vive en el heap del GC y se accede via instrucciones
            // dedicadas (STRRAW, STRGETBYTES, etc.).
            case PrimitiveKind::STRING:   return 8;
            // Tipos primitivos de coleccion: cada uno es un handle (i64) que
            // el plugin nativo de colecciones traduce a la estructura real
            // en su propia heap.  El frontend solo conoce el handle opaco.
            case PrimitiveKind::ARRAYLIST: return 8;
            case PrimitiveKind::HASHMAP:   return 8;
            case PrimitiveKind::HASHSET:   return 8;
            case PrimitiveKind::QUEUE:     return 8;
            case PrimitiveKind::DEQUE:     return 8;
            case PrimitiveKind::TREEMAP:   return 8;
            case PrimitiveKind::TREESET:   return 8;
            case PrimitiveKind::STACK:     return 8;
            // Future<T>: 8 bytes (handle del FutureObject runtime).  El T
            // logico no afecta al storage: el bytecode siempre opera sobre
            // el handle como i64.
            case PrimitiveKind::FUTURE:    return 8;
            // Borrows: 8 bytes (host_ptr).  Verificacion compile-time.
            case PrimitiveKind::BORROW:     return 8;
            case PrimitiveKind::BORROW_MUT: return 8;
            // Smart pointers: 8 bytes (slot stack con host_ptr al recurso).
            // Para Tier 1 con deleter custom seran 16 bytes; el frontend
            // todavia no usa esa variante (cuando llegue se diferenciara
            // via campo extra del Type).
            case PrimitiveKind::UNIQUE_PTR: return 8;
            case PrimitiveKind::SHARED_PTR: return 8;
            // TYPE_META: marcador comptime sin storage runtime.
            case PrimitiveKind::TYPE_META: return 0;
            case PrimitiveKind::COUNT:  return 0;
        }
        return 0;
    }

    /**
     * @brief Nombre canonico del tipo primitivo (estilo corto).
     *
     * Usar el estilo corto en mensajes de error es preferible porque es
     * mas legible y unifica los dos alias del usuario en una sola
     * representacion.
     *
     * @param k Tipo primitivo.
     * @return Cadena estatica (no liberar).
     */
    /**
     * @brief Nombre legible de un Type completo (incluye chain de punteros).
     *
     * Crea una cadena nueva por cada llamada (tipico de mensajes de error),
     * por lo que no se debe usar en hot paths.  Para PTR escribe "T*", para
     * STRUCT usa el nombre concreto, para primitivos usa el estilo corto.
     *
     * @param t Tipo a formatear.
     * @return std::string nuevo con la representacion textual.
     */
    inline std::string type_to_string(const Type &t) {
        // Newtype: mostrar el nombre nominal (e.g. "fd" en vez de "u64").
        // Asi los mensajes de error son legibles ("incompatible con fd"
        // en lugar de "incompatible con u64").
        if (t.nominal_id != 0 && !t.nominal_name.empty()) {
            return t.nominal_name;
        }
        if (t.kind == PrimitiveKind::PTR) {
            // T* (host por defecto) vs VirtualPtr<T> (direccion VM).
            if (t.is_virtual) {
                return std::string("VirtualPtr<") +
                       (t.pointee ? type_to_string(*t.pointee) : "?") + ">";
            }
            if (t.pointee) return type_to_string(*t.pointee) + "*";
            return "ptr";
        }
        if (t.kind == PrimitiveKind::ARRAY) {
            // T[N] con tamano fijo o T[] (decay) cuando array_size == 0.
            std::string elem = t.pointee ? type_to_string(*t.pointee) : "?";
            std::string sfx  = (t.array_size == 0) ? "[]" :
                               ("[" + std::to_string(t.array_size) + "]");
            // Los arrays VM (locales) no anaden marcador; los raros host
            // se prefijan con `host` para no confundir.
            if (!t.is_virtual) return std::string("host ") + elem + sfx;
            return elem + sfx;
        }
        if (t.kind == PrimitiveKind::STRUCT) return t.struct_name.empty()
            ? std::string("struct") : t.struct_name;
        if (t.kind == PrimitiveKind::CLASS) return t.struct_name.empty()
            ? std::string("class") : t.struct_name;
        if (t.kind == PrimitiveKind::OPTIONAL) {
            return std::string("Optional<") +
                   (t.pointee ? type_to_string(*t.pointee) : "?") + ">";
        }
        if (t.kind == PrimitiveKind::RESULT) {
            return std::string("Result<") +
                   (t.pointee  ? type_to_string(*t.pointee)  : "?") + ", " +
                   (t.pointee2 ? type_to_string(*t.pointee2) : "?") + ">";
        }
        if (t.kind == PrimitiveKind::FUTURE) {
            return std::string("Future<") +
                   (t.pointee ? type_to_string(*t.pointee) : "?") + ">";
        }
        if (t.kind == PrimitiveKind::UNIQUE_PTR) {
            return std::string("unique<") +
                   (t.pointee ? type_to_string(*t.pointee) : "?") + ">";
        }
        if (t.kind == PrimitiveKind::SHARED_PTR) {
            return std::string("shared<") +
                   (t.pointee ? type_to_string(*t.pointee) : "?") + ">";
        }
        if (t.kind == PrimitiveKind::BORROW) {
            return std::string("borrow<") +
                   (t.pointee ? type_to_string(*t.pointee) : "?") + ">";
        }
        if (t.kind == PrimitiveKind::BORROW_MUT) {
            return std::string("borrow_mut<") +
                   (t.pointee ? type_to_string(*t.pointee) : "?") + ">";
        }
        if (t.kind == PrimitiveKind::FUNCTION) {
            // fn(P1, P2, ...) -> R con cada Pi formateado recursivamente.
            std::string s = "fn(";
            for (size_t i = 0; i < t.fn_params.size(); ++i) {
                if (i) s += ", ";
                s += type_to_string(t.fn_params[i]);
            }
            s += ") -> ";
            s += t.pointee ? type_to_string(*t.pointee) : "?";
            return s;
        }
        switch (t.kind) {
            case PrimitiveKind::VOID:  return "void";
            case PrimitiveKind::BOOL:  return "bool";
            case PrimitiveKind::CHAR:  return "char";
            case PrimitiveKind::I8:    return "i8";
            case PrimitiveKind::I16:   return "i16";
            case PrimitiveKind::I32:   return "i32";
            case PrimitiveKind::I64:   return "i64";
            case PrimitiveKind::U8:    return "u8";
            case PrimitiveKind::U16:   return "u16";
            case PrimitiveKind::U32:   return "u32";
            case PrimitiveKind::U64:   return "u64";
            case PrimitiveKind::F32:   return "f32";
            case PrimitiveKind::F64:   return "f64";
            case PrimitiveKind::STRING:return "string";
            case PrimitiveKind::ARRAYLIST: return "ArrayList";
            case PrimitiveKind::HASHMAP:   return "HashMap";
            case PrimitiveKind::HASHSET:   return "HashSet";
            case PrimitiveKind::QUEUE:     return "Queue";
            case PrimitiveKind::DEQUE:     return "Deque";
            case PrimitiveKind::TREEMAP:   return "TreeMap";
            case PrimitiveKind::TREESET:   return "TreeSet";
            case PrimitiveKind::STACK:     return "Stack";
            default:                   return "<unknown>";
        }
    }

    inline const char *primitive_name(PrimitiveKind k) noexcept {
        switch (k) {
            case PrimitiveKind::VOID:  return "void";
            case PrimitiveKind::BOOL:  return "bool";
            case PrimitiveKind::CHAR:  return "char";
            case PrimitiveKind::I8:    return "i8";
            case PrimitiveKind::I16:   return "i16";
            case PrimitiveKind::I32:   return "i32";
            case PrimitiveKind::I64:   return "i64";
            case PrimitiveKind::U8:    return "u8";
            case PrimitiveKind::U16:   return "u16";
            case PrimitiveKind::U32:   return "u32";
            case PrimitiveKind::U64:   return "u64";
            case PrimitiveKind::F32:    return "f32";
            case PrimitiveKind::F64:    return "f64";
            case PrimitiveKind::PTR:    return "ptr";
            case PrimitiveKind::STRUCT: return "struct";
            case PrimitiveKind::CLASS:  return "class";
            case PrimitiveKind::ARRAY:  return "array";
            case PrimitiveKind::OPTIONAL: return "Optional";
            case PrimitiveKind::RESULT:   return "Result";
            case PrimitiveKind::FUNCTION: return "fn";
            case PrimitiveKind::STRING:   return "string";
            case PrimitiveKind::ARRAYLIST: return "ArrayList";
            case PrimitiveKind::HASHMAP:   return "HashMap";
            case PrimitiveKind::HASHSET:   return "HashSet";
            case PrimitiveKind::QUEUE:     return "Queue";
            case PrimitiveKind::DEQUE:     return "Deque";
            case PrimitiveKind::TREEMAP:   return "TreeMap";
            case PrimitiveKind::TREESET:   return "TreeSet";
            case PrimitiveKind::STACK:     return "Stack";
            case PrimitiveKind::FUTURE:    return "Future";
            case PrimitiveKind::BORROW:     return "borrow";
            case PrimitiveKind::BORROW_MUT: return "borrow_mut";
            case PrimitiveKind::UNIQUE_PTR: return "unique";
            case PrimitiveKind::SHARED_PTR: return "shared";
            case PrimitiveKind::TYPE_META: return "Type";
            case PrimitiveKind::COUNT:  return "<count>";
        }
        return "<unknown>";
    }

    /**
     * @brief Calcula el tipo resultante de una promocion entre dos numericos.
     *
     * Reglas (estilo C, simplificadas):
     *   - Si alguno es flotante, el resultado es f64 si alguno es f64,
     *     en caso contrario f32.
     *   - Si ambos son enteros, se promociona al entero mas ancho;
     *     si uno es signed y otro unsigned y comparten anchura, gana
     *     el unsigned (cohesion con C).
     *
     * Si los tipos no son compatibles (uno no-numerico), devuelve
     * @c PrimitiveKind::COUNT como senyal de error; el caller debe
     * emitir el diagnostico oportuno.
     *
     * @param a Tipo del operando izquierdo.
     * @param b Tipo del operando derecho.
     * @return Tipo promovido, o COUNT si no son compatibles.
     */
    /**
     * @brief Indica si un valor de tipo @p value puede asignarse a una
     *        ubicacion de tipo @p target sin diagnostico.
     *
     * Reglas:
     *   - Tipos identicos -> ok.
     *   - Ambos numericos (entero/flotante) -> ok (con conversion implicita).
     *   - Ambos PTR y alguno apunta a void -> ok (null o void* generico).
     *   - Ambos PTR con pointee identico -> ok (cubierto por igualdad).
     *
     * Para todo lo demas devuelve false; el caller debe emitir un
     * diagnostico claro indicando ambos tipos.
     */
    inline bool types_assignable(const Type &target, const Type &value) noexcept {
        if (target == value) return true;
        // Newtype barrier: si target O value es un newtype (nominal_id > 0)
        // y NO son el mismo newtype (operator== ya lo cubrio), el tipo
        // es nominalmente distinto y requiere cast explicito `(T)x`.
        // Sin este check, `typedef u64 port new; port p = 100;` permitiria
        // la coercion numerica u64 -> port silenciosamente, perdiendo la
        // garantia del newtype.  Salvo: newtype -> u64 (extraer bits) y
        // u64 -> newtype (envolver) requieren cast explicito (manejado en
        // lower_cast_expr + check_call).
        if (target.nominal_id != 0 || value.nominal_id != 0) return false;
        if (is_numeric(target.kind) && is_numeric(value.kind)) return true;
        if (target.kind == PrimitiveKind::PTR && value.kind == PrimitiveKind::PTR) {
            // Tratamos un PTR sin pointee como @c void* (caso de builtins
            // historicamente declarados con @c Type{PTR} a secas).  Esto
            // permite que free(p) acepte cualquier T* sin diagnosticar
            // type mismatch contra el tipo concreto del puntero.
            const bool t_void = !target.pointee
                              || target.pointee->kind == PrimitiveKind::VOID;
            const bool v_void = !value.pointee
                              || value.pointee->kind  == PrimitiveKind::VOID;
            if (t_void || v_void) return true;
        }
        // Decay automatico de array: T[N] o T[] se convierten implicitamente
        // a T*.  Util para pasar arrays como argumentos `T*` o asignarlos a
        // variables puntero.  Tambien admitimos PTR -> ARRAY[] (sin tamano)
        // para simetria, util en el lowering al unificar parametros.
        if (target.kind == PrimitiveKind::PTR && value.kind == PrimitiveKind::ARRAY) {
            if (target.pointee && value.pointee && *target.pointee == *value.pointee) return true;
            // void* destino acepta cualquier array.
            if (!target.pointee || target.pointee->kind == PrimitiveKind::VOID) return true;
        }
        if (target.kind == PrimitiveKind::ARRAY && value.kind == PrimitiveKind::PTR) {
            if (target.pointee && value.pointee && *target.pointee == *value.pointee
             && target.array_size == 0) return true;
        }
        // Array a array: T[N] -> T[] (target array sin tamano).  Permite
        // pasar un array de tamano fijo como parametro `T[]` (decay).
        if (target.kind == PrimitiveKind::ARRAY && value.kind == PrimitiveKind::ARRAY) {
            if (target.array_size == 0
             && target.pointee && value.pointee
             && *target.pointee == *value.pointee) return true;
        }
        // Lazy promotion: literal de string (PTR) -> string.  El
        // lowering del var-decl detecta el caso y emite STRMAKE para
        // producir un StringObject GC-managed.  Sin esto el type checker
        // rechazaria `string s = "hola"`.
        if (target.kind == PrimitiveKind::STRING
         && value.kind == PrimitiveKind::PTR) return true;
        // null asignable a CLASS (Java-style nullability).  null se modela
        // como PTR void; CLASS es una referencia que admite null por
        // defecto.  Necesario para `return null` en metodos que devuelven
        // tipo CLASS (lookup/find que pueden no encontrar el item) y para
        // `class_field = null` en init de structures de datos opcionales.
        if (target.kind == PrimitiveKind::CLASS
         && value.kind == PrimitiveKind::PTR) {
            const bool v_void = !value.pointee
                              || value.pointee->kind == PrimitiveKind::VOID;
            if (v_void) return true;
        }
        // string -> i64: extraer el GcHandle como entero opaco para FFI o
        // comparaciones manuales.  La direccion es valida solo durante el
        // lifetime del proceso (no exportar a otros procesos).
        if (is_numeric(target.kind) && value.kind == PrimitiveKind::STRING) {
            return primitive_size_bytes(target.kind) >= 8;
        }
        // los handles de las colecciones primitivas son i64
        // opacos a runtime; la informacion del tipo de elemento solo vive
        // en el frontend para decidir el dispatch *_gc.  Una asignacion
        // entre `ArrayList<T>` y `ArrayList` (sin params) o entre dos
        // colecciones del mismo @c kind con elementos GC-equivalentes es
        // siempre legal: el handle es identico bit-a-bit.
        if (target.kind == value.kind && (
                target.kind == PrimitiveKind::ARRAYLIST ||
                target.kind == PrimitiveKind::HASHMAP   ||
                target.kind == PrimitiveKind::HASHSET   ||
                target.kind == PrimitiveKind::QUEUE     ||
                target.kind == PrimitiveKind::DEQUE     ||
                target.kind == PrimitiveKind::TREEMAP   ||
                target.kind == PrimitiveKind::TREESET   ||
                target.kind == PrimitiveKind::STACK))
        {
            return true;
        }
        // Mejora II: Future<T1> -> Future<T2> con coercion numerica del
        // payload (mismo principio que Optional/Result).  Tambien permite
        // Future<T> -> Future<T> trivial (cubierto por target==value
        // anterior; redundante pero explicito para clarity).
        if (target.kind == PrimitiveKind::FUTURE
         && value.kind == PrimitiveKind::FUTURE
         && target.pointee && value.pointee) {
            // Compatibilidad estructural exacta o coercion numerica.
            if (*target.pointee == *value.pointee) return true;
            if (is_numeric(target.pointee->kind) && is_numeric(value.pointee->kind)) return true;
        }
        // Mejora II compat: Future<T> -> i64/u64 (extraer el handle como
        // entero opaco).  Permite codigo legacy `i64 f = compute()` cuando
        // compute es @Async.  El handle es identico bit-a-bit en runtime.
        if (value.kind == PrimitiveKind::FUTURE
         && (target.kind == PrimitiveKind::I64 || target.kind == PrimitiveKind::U64)) {
            return true;
        }
        // Y la inversa: i64/u64 -> Future<T> (interpretar el handle).
        // Sin esto, codigo que aloca un future raw via `future_alloc()`
        // (devuelve i64) no podria asignarse a una variable Future<T>.
        if (target.kind == PrimitiveKind::FUTURE
         && (value.kind == PrimitiveKind::I64 || value.kind == PrimitiveKind::U64)) {
            return true;
        }
        // Smart pointers: unique<X> -> unique<Y> y shared<X> -> shared<Y>
        // son asignables si X == Y exacto (no admitimos coercion numerica
        // entre punteros porque cambiaria la semantica del sizeof + del
        // deleter; el usuario debe usar el tipo correcto).  La excepcion
        // es Y == VOID (placeholder devuelto por los builtins `unique_box`
        // / `shared_box` cuando no hay info de tipo - se unifica con el
        // tipo concreto del LHS).
        if (target.kind == PrimitiveKind::UNIQUE_PTR
         && value.kind == PrimitiveKind::UNIQUE_PTR
         && target.pointee && value.pointee) {
            if (*target.pointee == *value.pointee) return true;
            if (value.pointee->kind == PrimitiveKind::VOID) return true;
            // Coercion numerica para `unique_box(50)` que devuelve
            // unique<i64> y se asigna a unique<i32> - misma logica que
            // Some(50) con Optional.
            if (is_numeric(target.pointee->kind) && is_numeric(value.pointee->kind)) return true;
        }
        if (target.kind == PrimitiveKind::SHARED_PTR
         && value.kind == PrimitiveKind::SHARED_PTR
         && target.pointee && value.pointee) {
            if (*target.pointee == *value.pointee) return true;
            if (value.pointee->kind == PrimitiveKind::VOID) return true;
            if (is_numeric(target.pointee->kind) && is_numeric(value.pointee->kind)) return true;
        }
        return false;
    }

    inline PrimitiveKind promote_arith(PrimitiveKind a, PrimitiveKind b) noexcept {
        if (!is_numeric(a) || !is_numeric(b)) return PrimitiveKind::COUNT;

        // Si alguno es flotante, ganan los flotantes.
        if (is_floating(a) || is_floating(b)) {
            if (a == PrimitiveKind::F64 || b == PrimitiveKind::F64) return PrimitiveKind::F64;
            return PrimitiveKind::F32;
        }
        // Ambos enteros: el de mayor anchura; en empate, el unsigned.
        const size_t sa = primitive_size_bytes(a);
        const size_t sb = primitive_size_bytes(b);
        if (sa != sb) return sa > sb ? a : b;
        // Misma anchura: si alguno es unsigned, el resultado es unsigned.
        const bool a_signed = is_signed_integral(a);
        const bool b_signed = is_signed_integral(b);
        if (a_signed && !b_signed) return b;
        if (!a_signed && b_signed) return a;
        return a; // ambos signed o ambos unsigned: da igual cual devolvamos.
    }

} // namespace vex

#endif // VEX_TYPES_H
