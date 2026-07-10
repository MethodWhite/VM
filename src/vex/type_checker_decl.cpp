#include "vex/lowering.h"
#include "vex/type_checker.h"
#include "vex/ast.h"
#include "vex/diagnostic.h"

#include <algorithm>
#include <set>
#include <sstream>
#include <utility>
#include <functional>
#include "vex/comptime_introspect.h"
#include "vex/collection_intrinsics.h"
#include "vex/lexer.h"
#include "vex/parser.h"
#include "loader/loader.h"

namespace vex {

    void TypeChecker::collect_globals() {
        // ----- Builtins predefinidos -----
        //
        // Declarados con firmas que el type checker valida por unificacion
        // exacta (PTR == PTR, I64 == I64, ...).  El lowering intercepta
        // las llamadas a estos nombres en try_lower_builtin_call() y emite
        // la convencion FFI correspondiente a vesta_io.
        //
        // Restriccion: los argumentos de tipo PTR deben ser literales
        // de string directos (StringLitExpr), porque para los FFI
        // necesitamos la longitud en compile-time.  El type checker NO
        // exige esto (le basta con el tipo PTR); el lowering lo verifica
        // por su lado y reporta error claro si el arg no es literal.
        auto reg_builtin = [&](const std::string &name,
                               Type ret,
                               std::initializer_list<PrimitiveKind> params) {
            FunctionSig sig;
            sig.return_type = ret;
            sig.param_types.reserve(params.size());
            for (auto p : params) sig.param_types.push_back(Type{p});
            Symbol s;
            s.kind      = SymbolKind::Function;
            s.sig_index = (uint32_t)function_sigs_.size();
            sig_by_name_[name] = s.sig_index;
            function_sigs_.push_back(std::move(sig));
            (void)declare(name, s);
        };
        // Salida de texto (aceptan ANY tipo via dispatch en lowering).
        // El check_call hace bypass especial para estos nombres y permite
        // string interpolado (StringLitExpr con interp_exprs) o escalares.
        reg_builtin("print",     Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        reg_builtin("println",   Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        reg_builtin("echo",      Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        // flush() sin argumentos (vacia el buffer de vesta_io).
        reg_builtin("flush",     Type{PrimitiveKind::VOID}, {});
        // Salida de valores numericos (sin acceso a memoria VM).
        reg_builtin("print_int",   Type{PrimitiveKind::VOID}, {PrimitiveKind::I64});
        reg_builtin("print_uint",  Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        reg_builtin("print_hex",   Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        reg_builtin("print_float", Type{PrimitiveKind::VOID}, {PrimitiveKind::F64});
        reg_builtin("print_bool",  Type{PrimitiveKind::VOID}, {PrimitiveKind::BOOL});
        reg_builtin("print_char",  Type{PrimitiveKind::VOID}, {PrimitiveKind::U32});
        reg_builtin("print_color", Type{PrimitiveKind::VOID}, {PrimitiveKind::U32});
        // print_cstr(host_ptr) imprime una cstring desde memoria host
        // (FatalError.message o .stack_trace).  Acepta cualquier PTR.
        reg_builtin("print_cstr",  Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        // Formatos numericos alternativos: binario y octal con prefijo
        // "0b" / "0o".  Compactos (sin ceros lider).
        reg_builtin("print_bin",   Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        reg_builtin("print_oct",   Type{PrimitiveKind::VOID}, {PrimitiveKind::U64});
        // print_ptr(addr) imprime "0x<hex>" compacto sin ceros lider.
        // Acepta cualquier PTR (host o virtual) o un i64 con la direccion.
        // print_gchandle(handle) imprime "<gc:N>" donde N es el handle
        // como entero decimal.  Para uso con CLASS objects el lowering
        // emite la instruccion @c gchandle antes de llamar.
        reg_builtin("print_ptr",      Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});
        reg_builtin("print_gchandle", Type{PrimitiveKind::VOID}, {PrimitiveKind::I64});
        // Padding/alineacion: emite @p width copias del codepoint
        // @p fill_cp.  El usuario calcula la diferencia entre el ancho
        // deseado y el ancho actual del texto y llama a este builtin.
        // Para alineacion manual de columnas en TUIs.
        reg_builtin("print_pad",   Type{PrimitiveKind::VOID},
                    {PrimitiveKind::U32, PrimitiveKind::U64});
        // I/O de fichero.  fopen recibe path y modo como literales de
        // string; devuelve un FILE* (uint64_t).  fwrite recibe el FILE*
        // y un buffer literal; devuelve el numero de bytes escritos.
        // fclose recibe el FILE* y devuelve un codigo i32.
        reg_builtin("fopen",  Type{PrimitiveKind::I64},
                    {PrimitiveKind::PTR, PrimitiveKind::PTR});
        reg_builtin("fwrite", Type{PrimitiveKind::I64},
                    {PrimitiveKind::I64, PrimitiveKind::PTR});
        reg_builtin("fclose", Type{PrimitiveKind::I32},
                    {PrimitiveKind::I64});

        // Allocator manual: malloc devuelve void* (puntero host obtenido del
        // RawAllocator del proceso); free libera un puntero anteriormente
        // devuelto por malloc.  El usuario calcula los bytes manualmente
        // (sizeof aun no implementado): malloc(4 * 10) reserva 10 i32s.
        // El lowering marca el resultado de malloc como is_host_ptr=true
        // para que LOAD/STORE emitan `movh` (acceso a memoria host).
        // free admite cualquier T* (sin chequeo dinamico de tipo).
        reg_builtin("malloc", Type::make_ptr(Type{PrimitiveKind::VOID}),
                    {PrimitiveKind::I64});
        reg_builtin("free",   Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});

        // Alias predefinido `cstring` = `char*`.  Permite
        // declarar `cstring p` para FFI con char* sin tener que escribir
        // `char* p`.  Se registra como type alias (typedef) global; el
        // type checker resuelve `cstring` a `Type{PTR, pointee=CHAR}`.
        {
            Type t_char = Type{PrimitiveKind::CHAR};
            Type t_cstring = Type::make_ptr(t_char);
            type_aliases_["cstring"] = t_cstring;
        }

        // Alias predefinidos para reflexion: Class / Method / Field / Object.
        // Se resuelven a i64 (handle opaco del ClassRegistry / MethodInfo* /
        // FieldInfo* / host_ptr del objeto).  Cuando se declara una variable
        // con uno de estos tipos (e.g. `Class cls = forName("X")`), el type
        // checker registra el alias en el Symbol; las llamadas
        // `cls.getMethod("foo")` se desazucaran a `getMethod(cls, "foo")`.
        // Esto provee sintaxis OO ergonomica sin cambios en el runtime.
        {
            const Type t_i64 = Type{PrimitiveKind::I64};
            type_aliases_["Class"]  = t_i64;
            type_aliases_["Method"] = t_i64;
            type_aliases_["Field"]  = t_i64;
            type_aliases_["Object"] = t_i64;
        }

        // Builtins de string operando sobre StringObject (tipo
        // STRING).  Todos se bajan a un solo opcode bytecode (~5 ns).
        reg_builtin("str_length", Type{PrimitiveKind::I64}, {PrimitiveKind::STRING});
        reg_builtin("str_bytes",  Type{PrimitiveKind::I64}, {PrimitiveKind::STRING});
        reg_builtin("str_cstr",   Type{PrimitiveKind::PTR}, {PrimitiveKind::STRING});
        reg_builtin("str_wstr",   Type{PrimitiveKind::PTR}, {PrimitiveKind::STRING});
        reg_builtin("str_hash",   Type{PrimitiveKind::U64}, {PrimitiveKind::STRING});
        reg_builtin("str_intern", Type{PrimitiveKind::STRING}, {PrimitiveKind::STRING});
        reg_builtin("str_concat", Type{PrimitiveKind::STRING}, {PrimitiveKind::STRING, PrimitiveKind::STRING});
        reg_builtin("str_equals", Type{PrimitiveKind::BOOL},   {PrimitiveKind::STRING, PrimitiveKind::STRING});
        reg_builtin("str_make",   Type{PrimitiveKind::STRING}, {PrimitiveKind::PTR, PrimitiveKind::I64});
        // Encoding explicito.  str_convert(s, enc) usa
        // STRCONV bytecode.  Acepta cualquier int de las constantes
        // ENC_* (ASCII=0, ANSI=1, UTF8=2, UTF16=3, UTF32=4) declaradas
        // como Symbol::Constant abajo.  Util para preparar wstr (UTF16)
        // antes de pasar a Win32 *W APIs.
        reg_builtin("str_convert", Type{PrimitiveKind::STRING},
                    {PrimitiveKind::STRING, PrimitiveKind::I32});

        // Constantes de encoding (idem ANSI codes: registradas como
        // Symbol::Constant con tipo i32).  El lowering las inlinea como
        // const literal sin lookup runtime.
        auto reg_const_int = [&](const std::string &name, int32_t value) {
            Symbol s;
            s.kind = SymbolKind::Constant;
            s.type = Type{PrimitiveKind::I32};
            // Reusamos campo type como contenedor + valor en sig_index
            // (truco: sig_index es uint32_t y vale como inmediato).
            s.sig_index = static_cast<uint32_t>(value);
            (void)declare(name, s);
        };
        reg_const_int("ENC_ASCII", 0);
        reg_const_int("ENC_ANSI",  1);
        reg_const_int("ENC_UTF8",  2);
        reg_const_int("ENC_UTF16", 3);
        reg_const_int("ENC_UTF32", 4);

        // panic("msg") lanza FatalError(USER_ABORT, msg).  Si hay
        // try/catch FatalError lo captura; si no, mata el proceso (no la VM).
        reg_builtin("panic",  Type{PrimitiveKind::VOID}, {PrimitiveKind::PTR});

        // dispose(xs) libera explicitamente una coleccion antes
        // del exit del scope.  Tras la llamada, el local queda con handle=0
        // y el cleanup automatico llama free fn que es no-op con handle=0.
        // Aceptamos cualquier tipo coleccion (bypass relax en check_call).
        reg_builtin("dispose", Type{PrimitiveKind::VOID}, {PrimitiveKind::I64});

        // Constructores de tipos primitivos de coleccion.
        // Cada uno acepta una capacidad inicial opcional (i64, default 16);
        // por simplicidad, registramos la signature con el arg requerido.
        // El usuario llama @c arraylist(N) (N >= 16 internamente capado) o
        // @c arraylist() (caso 0-args sin registrar firma extra: usamos el
        // bypass relajado en check_call analogo a print).  Para esta primera
        // iteracion exigimos siempre 1 arg explicito o wrapper Vex; suficiente
        // para validar la integracion.
        for (size_t i = 0; i < COL_TYPES_N; ++i) {
            const ColType &ct = COL_TYPES[i];
            // arraylist(i64) -> ArrayList     (constructor con capacidad)
            // tmap_new() / tset_new() son sin args; los tratamos igual:
            // el lowering ignora args extra cuando default_cap == 0.
            if (ct.default_cap == 0) {
                reg_builtin(ct.vex_ctor_name, Type{ct.kind}, {});
            } else {
                reg_builtin(ct.vex_ctor_name, Type{ct.kind}, {PrimitiveKind::I64});
            }
        }

        // FFI runtime dinamico estilo VSH.  Permite cargar DLLs y
        // resolver simbolos en runtime (vs el FFI declarativo extern, que
        // resuelve en compile-time).  Cero overhead de la ruta normal:
        // solo paga cuando el usuario decide usarlo.
        //   ffi_open(string path) -> i64 handle
        //   ffi_sym (i64 handle, string name) -> i64 fn_addr
        //   ffi_call(i64 fn_addr, ...args) -> i64 result   (variadic 0-12)
        // El check_call hace bypass para ffi_call (acepta argc variable).
        reg_builtin("ffi_open", Type{PrimitiveKind::I64}, {PrimitiveKind::PTR});
        reg_builtin("ffi_sym",  Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::PTR});
        reg_builtin("ffi_call", Type{PrimitiveKind::I64}, {PrimitiveKind::I64});

        // Math builtins delegando a stdlib/native/math/vesta_math.dll.  El
        // ABI nativo es uint64_t -> uint64_t con bits IEEE 754; el lowering
        // emite CALLN directo a vmath_<name> (no pasa por IR ops float).
        // Cubrir todo lo expuesto por vmath_*: scalar y trig.
        reg_builtin("sqrt",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("pow",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64, PrimitiveKind::F64});
        reg_builtin("fabs",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("floor", Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("ceil",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("round", Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("fmin",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64, PrimitiveKind::F64});
        reg_builtin("fmax",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64, PrimitiveKind::F64});
        reg_builtin("log",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("log2",  Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("log10", Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("sin",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("cos",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("tan",   Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("abs",   Type{PrimitiveKind::I64}, {PrimitiveKind::I64});
        reg_builtin("imin",  Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::I64});
        reg_builtin("imax",  Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::I64});
        reg_builtin("clamp", Type{PrimitiveKind::I64}, {PrimitiveKind::I64, PrimitiveKind::I64, PrimitiveKind::I64});
        // Math-IR-promote v2.2a: float + bit ops nuevos.
        reg_builtin("trunc",    Type{PrimitiveKind::F64}, {PrimitiveKind::F64});
        reg_builtin("iminu",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});
        reg_builtin("imaxu",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});
        reg_builtin("ilog2",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("popcount", Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("clz",      Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("ctz",      Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("bswap",    Type{PrimitiveKind::U64}, {PrimitiveKind::U64});
        reg_builtin("rotl",     Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});
        reg_builtin("rotr",     Type{PrimitiveKind::U64}, {PrimitiveKind::U64, PrimitiveKind::U64});

        /* Sprint B.1: callback Vex -> C nativo.  Toma una fn Vex como
         * argumento y devuelve un host_ptr (i64) a un thunk con cc C
         * estandar (Win64 o SysV).  El check_call hace bypass especial
         * para validar que el arg es una IdentExpr a una funcion
         * declarada (no a un lambda value).  Sintaxis:
         *
         *   i32 mi_cmp(u8* a, u8* b) { ... }
         *   u64 cb = as_native_callback(mi_cmp);
         *   qsort(arr, n, sz, cb);  // C llama a mi_cmp via cc nativa */
        reg_builtin("as_native_callback", Type{PrimitiveKind::I64}, {PrimitiveKind::PTR});

        // Identificadores magicos para colores y atributos ANSI.
        //
        // Registramos como Symbols globales tipo PTR (string).  El
        // lowering los detecta en lower_ident y emite la secuencia ANSI
        // correspondiente como string literal estatico (zero overhead).
        // Lista cubre los SGR mas comunes: 8 colores foreground + 8
        // brillantes + atributos de estilo + RESET + dos helpers de
        // pantalla (CLEAR_SCREEN, CURSOR_HOME).  Todos quedan en el
        // scope global y se ven desde cualquier funcion.
        auto reg_const_string = [&](const std::string &name) {
            Symbol s;
            s.kind = SymbolKind::Constant;
            s.type = Type{PrimitiveKind::PTR};
            (void)declare(name, s);
        };
        // Foreground (regulares 30..37 y brillantes 90..97).
        reg_const_string("BLACK");        reg_const_string("RED");
        reg_const_string("GREEN");        reg_const_string("YELLOW");
        reg_const_string("BLUE");         reg_const_string("MAGENTA");
        reg_const_string("CYAN");         reg_const_string("WHITE");
        reg_const_string("BR_BLACK");     reg_const_string("BR_RED");
        reg_const_string("BR_GREEN");     reg_const_string("BR_YELLOW");
        reg_const_string("BR_BLUE");      reg_const_string("BR_MAGENTA");
        reg_const_string("BR_CYAN");      reg_const_string("BR_WHITE");
        // Background (40..47).
        reg_const_string("BG_BLACK");     reg_const_string("BG_RED");
        reg_const_string("BG_GREEN");     reg_const_string("BG_YELLOW");
        reg_const_string("BG_BLUE");      reg_const_string("BG_MAGENTA");
        reg_const_string("BG_CYAN");      reg_const_string("BG_WHITE");
        // Atributos de estilo y reset.
        reg_const_string("BOLD");         reg_const_string("DIM");
        reg_const_string("ITALIC");       reg_const_string("UNDERLINE");
        reg_const_string("BLINK");        reg_const_string("REVERSE");
        reg_const_string("RESET");
        // Helpers de pantalla (utiles para TUIs).
        reg_const_string("CLEAR_SCREEN"); reg_const_string("CURSOR_HOME");

        // registrar @c FatalError como clase pre-definida en
        // runtime.  El @c init_exception_classes del runtime ya la creo
        // en el ClassRegistry; aqui solo damos al type checker / lowering
        // la metadata para validar @c catch (FatalError e) y @c e.kind /
        // @c e.message / @c e.stack_trace.  Marcada con
        // @c is_runtime_predefined=true para que el lowering NO emita
        // defclass / __new_FatalError / etc.
        {
            ClassLayout fe;
            fe.name = "FatalError";
            fe.is_runtime_predefined = true;
            // ABI fija (matching exception_runtime.h):
            //   +24 i32 kind  (size 8 con padding)
            //   +32 u64 pc
            //   +40 ptr message
            //   +48 ptr stack_trace
            // Total 56 bytes (ObjectHeader 24 + 4*8).
            auto add_field = [&](const char *name, PrimitiveKind k,
                                 uint32_t off) {
                StructFieldInfo fi;
                fi.name      = name;
                fi.type      = Type{k};
                fi.offset    = off;
                fi.size      = 8;
                fe.fields.push_back(fi);
            };
            add_field("kind",        PrimitiveKind::I32, 24);
            add_field("pc",          PrimitiveKind::U64, 32);
            add_field("message",     PrimitiveKind::PTR, 40);
            add_field("stack_trace", PrimitiveKind::PTR, 48);
            fe.size_bytes = 56;
            class_layouts_["FatalError"] = std::move(fe);
        }

        // BugFix R4: registrar clases excepcion estandar (sincronizado con
        // exception_runtime.cpp::init_exception_classes).  Cada una tiene
        // un solo field `message` (string ptr) en offset 24 (despues del
        // ObjectHeader).  El lowering del frontend detecta
        // `new <ExceptionClass>(msg)` y emite la secuencia inline
        // (newobj + store message at +24).  El catch puede leer
        // `e.message` via getfield estandar.
        {
            const char *std_exc_names[] = {
                "RuntimeException",
                "ArithmeticException",
                "IllegalArgumentException",
                "IndexOutOfBoundsException",
                "NullPointerException",
                "IOException",
                "ClassCastException",
                "UnsupportedOperationException",
            };
            for (const char *n : std_exc_names) {
                ClassLayout cl;
                cl.name = n;
                cl.is_runtime_predefined = true;
                StructFieldInfo fi;
                fi.name   = "message";
                fi.type   = Type{PrimitiveKind::PTR};
                fi.offset = 24;
                fi.size   = 8;
                cl.fields.push_back(fi);
                cl.size_bytes = 32;
                class_layouts_[n] = std::move(cl);
            }
        }

        // Pre-pasada (robustness): registrar nombres de TODOS
        // los structs / clases / enums con layouts mininos (vacio) ANTES
        // del procesamiento real de campos y metodos.  Esto permite:
        //   - Self-references: `class Node { Node next; }`
        //   - Forward refs: `class A { B b; }  class B { A a; }`
        //   - Mutual recursion entre clases en cualquier orden.
        // Sin esto, type_from_node falla al ver el primer uso del
        // nombre y devuelve VOID, propagando errores en cascada.
        for (auto &decl : mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::StructDecl) {
                auto *s = static_cast<ast::StructDecl *>(decl.get());
                if (!struct_layouts_.count(s->name)) {
                    StructLayout empty;
                    empty.name          = s->name;
                    empty.is_introspect = s->is_introspect;
                    struct_layouts_.emplace(s->name, std::move(empty));
                }
            } else if (decl->kind == ast::NodeKind::ClassDecl) {
                auto *c = static_cast<ast::ClassDecl *>(decl.get());
                // Templates con type_params no son clases concretas.
                if (!c->type_params.empty()) continue;
                if (!class_layouts_.count(c->name)) {
                    ClassLayout empty;
                    empty.name          = c->name;
                    empty.is_interface  = c->is_interface;
                    empty.is_aspect     = c->is_aspect;
                    empty.is_introspect = c->is_introspect;
                    class_layouts_.emplace(c->name, std::move(empty));
                }
            } else if (decl->kind == ast::NodeKind::EnumDecl) {
                auto *en = static_cast<ast::EnumDecl *>(decl.get());
                // L2.3: enums template (con type_params) NO se registran
                // como concretos; se monomorphizan on demand.
                if (!en->type_params.empty()) continue;
                if (!enum_layouts_.count(en->name)) {
                    EnumLayout empty;
                    empty.name          = en->name;
                    empty.is_introspect = en->is_introspect;
                    enum_layouts_.emplace(en->name, std::move(empty));
                }
            }
        }
        // Pase 0: registrar typedef/using y struct ANTES de funciones y
        // globales, para que cualquier referencia posterior a esos nombres
        // se resuelva correctamente.  Procesar typedef/using en orden
        // permite alias anidados (typedef A B; typedef B C;) si A esta
        // declarado antes que B; alias adelantados (B antes de A) no se
        // resuelven y emiten error.
        for (auto &decl : mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::TypeAliasDecl) {
                auto *a = static_cast<ast::TypeAliasDecl *>(decl.get());
                Type resolved = type_from_node(a->aliased.get());
                if (resolved.kind == PrimitiveKind::COUNT
                 || (resolved.kind == PrimitiveKind::VOID
                     && a->aliased
                     && a->aliased->kind == ast::NodeKind::NamedTypeNode)) {
                    // No se pudo resolver el tipo subyacente: nombre
                    // desconocido o forward-reference.
                    diags_.error(a->loc,
                        "tipo no resuelto en alias '" + a->name + "'");
                    continue;
                }
                // Newtype: asignar nominal_id unico para que sea
                // nominalmente distinto del underlying y de otros newtypes
                // con misma representacion.  Sin esto, `typedef u64 fd new`
                // y `typedef u64 port new` serian tipos identicos (ambos
                // u64) -- exactamente lo que queremos EVITAR.
                if (a->is_newtype) {
                    resolved.nominal_id    = ++newtype_counter_;
                    resolved.nominal_name  = a->name;
                    resolved.is_opaque     = a->is_opaque;
                    resolved.align_override = a->align_override;
                    // Underlying preservado (para introspeccion + cast
                    // explicito).  Conservamos COPIA del underlying en
                    // newtype_underlying_ para que `typedef is T` y
                    // `(T)x` puedan responder sin parsear el AST.
                    Type underlying = resolved;
                    underlying.nominal_id     = 0;
                    underlying.is_opaque      = false;
                    underlying.align_override = 0;
                    underlying.nominal_name.clear();
                    newtype_underlying_.emplace(a->name, std::move(underlying));
                    // Registrar conversiones permitidas + fichero de
                    // declaracion (para module-privacy de @opaque).
                    NewtypeInfo info;
                    info.source_file = a->loc.file;
                    for (auto &ec : a->explicit_from) {
                        if (!ec.type) continue;
                        Type t = type_from_node(ec.type.get());
                        if (t.kind == PrimitiveKind::COUNT
                         || t.kind == PrimitiveKind::VOID) {
                            diags_.error(a->loc,
                                "tipo no resuelto en 'explicit from' de '"
                                + a->name + "'");
                            continue;
                        }
                        info.from_conversions.push_back({std::move(t), ec.is_public});
                    }
                    for (auto &ec : a->explicit_to) {
                        if (!ec.type) continue;
                        Type t = type_from_node(ec.type.get());
                        if (t.kind == PrimitiveKind::COUNT
                         || t.kind == PrimitiveKind::VOID) {
                            diags_.error(a->loc,
                                "tipo no resuelto en 'explicit to' de '"
                                + a->name + "'");
                            continue;
                        }
                        info.to_conversions.push_back({std::move(t), ec.is_public});
                    }
                    newtype_info_.emplace(a->name, std::move(info));
                }
                if (!type_aliases_.emplace(a->name, resolved).second) {
                    diags_.error(a->loc, "alias de tipo redefinido: '" + a->name + "'");
                }
            } else if (decl->kind == ast::NodeKind::StructDecl) {
                auto *s = static_cast<ast::StructDecl *>(decl.get());
                // Pre-pasada (mas arriba) ya creo una entrada vacia.  Si
                // size_bytes > 0 (ya completada) -> redeclaracion real.
                auto it_pre = struct_layouts_.find(s->name);
                if (it_pre != struct_layouts_.end() && !it_pre->second.fields.empty()) {
                    diags_.error(s->loc, "struct redeclarado: '" + s->name + "'");
                    continue;
                }

                // Calcular layout con alineamiento natural por campo, igual
                // que C: cada campo arranca en el siguiente offset que sea
                // multiplo de su sizeof.  El struct entero tambien queda
                // alineado al campo mas grande, redondeando size_bytes
                // al final para que arrays de structs tengan offset
                // consistente entre elementos.
                StructLayout layout;
                layout.name = s->name;
                uint32_t offset = 0;
                uint32_t max_align = 1;
                std::unordered_map<std::string, bool> seen_names;

                // Estado para packing de bit fields.
                //   bf_active=true mientras hay un storage word abierto.
                //   bf_offset: offset (bytes) del storage word actual.
                //   bf_size: tamano (bytes) del storage word.
                //   bf_used: bits ya usados dentro del word.
                bool     bf_active = false;
                uint32_t bf_offset = 0, bf_size = 0;
                uint8_t  bf_used   = 0;
                auto close_bf = [&]() {
                    if (bf_active) {
                        offset = bf_offset + bf_size;
                        bf_active = false;
                        bf_used = 0;
                    }
                };

                for (const auto &f : s->fields) {
                    if (!seen_names.emplace(f.name, true).second) {
                        diags_.error(f.loc,
                            "campo duplicado en struct '" + s->name + "': '" + f.name + "'");
                        continue;
                    }
                    Type ft = type_from_node(f.type.get());
                    if (ft.kind == PrimitiveKind::COUNT
                     || ft.kind == PrimitiveKind::VOID) {
                        diags_.error(f.loc,
                            "tipo invalido en campo '" + f.name + "' del struct '" + s->name + "'");
                        continue;
                    }
                    // Tamano y alineamiento del campo.  Para STRUCT anidado
                    // consultamos su propio layout previamente registrado;
                    // si aun no se registro (forward ref dentro del mismo
                    // pase) emitimos error de orden de declaracion.
                    uint32_t fsize = (uint32_t)primitive_size_bytes(ft.kind);
                    uint32_t falign = fsize;
                    if (ft.kind == PrimitiveKind::STRUCT) {
                        auto it = struct_layouts_.find(ft.struct_name);
                        if (it == struct_layouts_.end()) {
                            diags_.error(f.loc,
                                "struct '" + ft.struct_name +
                                "' debe declararse antes de usarse como campo");
                            continue;
                        }
                        fsize  = it->second.size_bytes;
                        falign = it->second.align_bytes;
                    }
                    if (fsize == 0) {
                        // Defensa: deberia haber sido atrapado arriba.
                        fsize = 1;
                    }
                    if (falign == 0) falign = 1;
                    // Newtype con @align(N): forzar alineacion del campo.
                    // El tamano sigue siendo el del underlying (el align
                    // solo afecta el padding antes del campo + el align
                    // del struct contenedor).
                    if (ft.align_override > 0 && ft.align_override > falign) {
                        falign = ft.align_override;
                    }

                    // Bit field handling.
                    if (f.bit_width > 0) {
                        // Solo permitido sobre tipos integer.
                        if (!is_integral(ft.kind)) {
                            diags_.error(f.loc,
                                "bit field '" + f.name + "' requiere tipo integer");
                            continue;
                        }
                        const uint8_t bw = f.bit_width;
                        const uint8_t cap = (uint8_t)(fsize * 8u);
                        if (bw > cap) {
                            diags_.error(f.loc,
                                "bit_width (" + std::to_string((int)bw)
                                + ") excede el tamano del tipo (" + std::to_string((int)cap) + ")");
                            continue;
                        }
                        // Si hay storage abierto del MISMO tamano y queda hueco,
                        // empaquetar.  Si no, abrir nuevo storage en posicion
                        // alineada.
                        if (!bf_active || bf_size != fsize
                         || (bf_used + bw) > cap) {
                            close_bf();
                            // Padding hasta multiplo de falign.
                            if (offset % falign != 0) {
                                offset += falign - (offset % falign);
                            }
                            bf_active = true;
                            bf_offset = offset;
                            bf_size   = fsize;
                            bf_used   = 0;
                            if (falign > max_align) max_align = falign;
                        }
                        StructFieldInfo fi;
                        fi.name = f.name;
                        fi.type = ft;
                        fi.offset = bf_offset;
                        fi.size   = bf_size;
                        fi.bit_offset = bf_used;
                        fi.bit_width  = bw;
                        layout.fields.push_back(std::move(fi));
                        bf_used += bw;
                        continue;
                    }
                    // Campo normal: cerrar bit field activo si lo hay.
                    close_bf();

                    // Padding hasta multiplo de falign.
                    if (offset % falign != 0) {
                        offset += falign - (offset % falign);
                    }

                    StructFieldInfo fi;
                    fi.name   = f.name;
                    fi.type   = ft;
                    fi.offset = offset;
                    fi.size   = fsize;
                    layout.fields.push_back(std::move(fi));

                    offset += fsize;
                    if (falign > max_align) max_align = falign;
                }
                // Cerrar bit field activo al final del struct.
                close_bf();
                // Tamano total redondeado al max_align (compatible con
                // arrays de structs).
                if (max_align > 1 && offset % max_align != 0) {
                    offset += max_align - (offset % max_align);
                }
                layout.size_bytes   = offset;
                layout.align_bytes  = max_align;
                /* preservar la marca @Introspect que
                 * la pre-pasada copio del AST.  Como aqui sobrescribimos
                 * la entrada con un layout local fresco, hay que re-copiar
                 * el flag desde el StructDecl. */
                layout.is_introspect = s->is_introspect;
                // Phase M6.a L.3.
                layout.is_public = s->is_public;

                // Sobrescribir la entrada vacia pre-registrada con el layout
                // ya completo.  Usar operator[] = porque la entrada existe.
                struct_layouts_[s->name] = std::move(layout);
            } else if (decl->kind == ast::NodeKind::EnumDecl) {
                // Registrar el enum (ADT) con su layout: max_payload determina
                // el tamano del slot (8 + 8*N_payload_fields_max) y los tags.
                auto *en = static_cast<ast::EnumDecl *>(decl.get());
                // L2.3: enums template (con type_params) NO se procesan como
                // concretos; se monomorphizan on demand.
                if (!en->type_params.empty()) continue;
                // Pre-pasada creo entradas vacias en enum_layouts_; un
                // enum es "ya registrado" si tiene variantes.  Para
                // colisiones cross-tipo, struct/class no deberian
                // existir con el mismo nombre.
                auto it_pre_e = enum_layouts_.find(en->name);
                const bool already_done = (it_pre_e != enum_layouts_.end()
                                          && !it_pre_e->second.variants.empty());
                auto it_struct_done = struct_layouts_.find(en->name);
                auto it_class_done  = class_layouts_.find(en->name);
                const bool struct_collision = (it_struct_done != struct_layouts_.end()
                                              && !it_struct_done->second.fields.empty());
                const bool class_collision  = (it_class_done != class_layouts_.end()
                                              && !it_class_done->second.name.empty()
                                              && it_class_done->second.name == en->name
                                              && (!it_class_done->second.fields.empty()
                                                  || !it_class_done->second.methods.empty()));
                // L2.3: enums monomorphizados ya tienen su layout completo
                // (lo construye monomorphize_enum); skip silente sin error.
                if (monomorphized_.count(en->name)) {
                    continue;
                }
                if (already_done || struct_collision || class_collision) {
                    diags_.error(en->loc,
                        "tipo redeclarado: '" + en->name + "' (colision con struct/class/enum)");
                    continue;
                }
                EnumLayout elay;
                elay.name = en->name;
                std::unordered_map<std::string, bool> seen_v;
                uint32_t max_pl = 0;
                for (size_t vi = 0; vi < en->variants.size(); ++vi) {
                    const auto &vd = en->variants[vi];
                    if (!seen_v.emplace(vd.name, true).second) {
                        diags_.error(vd.loc,
                            "variante duplicada en enum '" + en->name + "': '" + vd.name + "'");
                        continue;
                    }
                    EnumVariantInfo vi_info;
                    vi_info.name = vd.name;
                    vi_info.tag  = static_cast<uint32_t>(vi);
                    vi_info.field_types.reserve(vd.field_types.size());
                    for (const auto &ft : vd.field_types) {
                        vi_info.field_types.push_back(type_from_node(ft.get()));
                    }
                    if (vi_info.field_types.size() > max_pl) {
                        max_pl = static_cast<uint32_t>(vi_info.field_types.size());
                    }
                    elay.variants.push_back(std::move(vi_info));
                }
                elay.max_payload_fields = max_pl;
                // Layout: 8 (tag) + 8 * max_payload_fields.  Cada payload
                // se padea a 8 bytes para uniformidad del offset acceso
                // (i*8 a partir de offset 8) sin tablas por variante.
                elay.size_bytes = 8 + 8 * max_pl;
                /* preservar marca @Introspect del AST. */
                elay.is_introspect = en->is_introspect;
                // Phase M6.a L.3.
                elay.is_public = en->is_public;
                // Sobrescribir entrada vacia pre-registrada.
                enum_layouts_[en->name] = std::move(elay);
            } else if (decl->kind == ast::NodeKind::ClassDecl) {
                auto *c = static_cast<ast::ClassDecl *>(decl.get());
                // generics: templates (con type_params) no son clases
                // concretas.  Solo se procesa la version monomorphizada
                // (que tiene type_params vacio).
                if (!c->type_params.empty()) continue;
                // Pre-pasada creo entradas vacias en class_layouts_; un
                // class es "ya completada" si tiene fields o methods.
                auto it_pre_c = class_layouts_.find(c->name);
                if (it_pre_c != class_layouts_.end()
                 && (!it_pre_c->second.fields.empty()
                     || !it_pre_c->second.methods.empty())) {
                    diags_.error(c->loc, "clase redeclarada: '" + c->name + "'");
                    continue;
                }

                // Layout de campos: cada slot ocupa 8 bytes (igual que el
                // ClassRegistry) por simplicidad y alineacion.  El offset
                // efectivo en el ObjectHeader lo recalcula el ClassRegistry
                // sumandole sizeof(ObjectHeader); aqui guardamos el offset
                // relativo al payload para que el lowering pueda emitir
                // GETFIELD <off>.
                ClassLayout layout;
                layout.name            = c->name;
                layout.super_name      = c->super_name;
                layout.interface_names = c->interface_names;
                layout.is_interface    = c->is_interface;
                /* preservar la marca @Introspect del AST. */
                layout.is_introspect   = c->is_introspect;
                layout.is_aspect       = c->is_aspect;
                // Phase M6.a L.3: visibilidad cross-module.
                layout.is_public       = c->is_public;

                // Herencia: si hay super_name resuelto, copiamos sus fields
                // y metodos al inicio del layout actual.  Los offsets
                // continuan tras los heredados; los slots del vtable se
                // mantienen para que un metodo no override lo mantenga
                // como heredado.  Override por nombre se resuelve mas
                // abajo cuando procesamos los metodos propios.
                const ClassLayout *super_layout = nullptr;
                if (!c->super_name.empty()) {
                    // Phase M.L30: detector de ciclos de herencia.
                    // Recorrer la cadena super_name -> super_name de la
                    // clase candidata.  Si llegamos al name de la clase
                    // que estamos procesando, hay un ciclo (e.g. A:B y
                    // B:A o A:B:C:A).  Limite defensivo de 256 niveles
                    // para evitar loops espurios en layouts mal formados.
                    {
                        const std::string &self_name = c->name;
                        std::string cur = c->super_name;
                        for (int depth = 0; depth < 256; ++depth) {
                            if (cur == self_name) {
                                diags_.error(c->loc,
                                    "ciclo de herencia detectado: la clase '" +
                                    self_name + "' aparece en su propia "
                                    "cadena de superclases");
                                break;
                            }
                            auto it_cycle = class_layouts_.find(cur);
                            if (it_cycle == class_layouts_.end()) break;
                            if (it_cycle->second.super_name.empty()) break;
                            cur = it_cycle->second.super_name;
                        }
                    }
                    auto it_super = class_layouts_.find(c->super_name);
                    if (it_super == class_layouts_.end()) {
                        diags_.error(c->loc,
                            "superclase '" + c->super_name +
                            "' no encontrada (debe declararse antes que '" +
                            c->name + "')");
                    } else if (it_super->second.is_interface
                               && !c->is_interface) {
                        // El identificador despues de `:` resulto ser una
                        // interfaz, no una clase.  Promocionamos a la lista
                        // de interfaces implementadas y vaciamos super_name
                        // (la clase queda sin super, equivalente a Object).
                        // Esto permite la sintaxis natural Vex
                        // `class X : IFoo, IBar` sin requerir un super
                        // dummy en primera posicion.
                        layout.interface_names.insert(
                            layout.interface_names.begin(),
                            c->super_name);
                        layout.super_name.clear();
                    } else {
                        super_layout = &it_super->second;
                        // Copiar fields heredados (offsets ya incluyen header).
                        for (const auto &sf : super_layout->fields) {
                            layout.fields.push_back(sf);
                        }
                        for (const auto &ssf : super_layout->static_fields) {
                            layout.static_fields.push_back(ssf);
                        }
                        // Copiar metodos heredados con sus vtable_index.
                        for (const auto &sm : super_layout->methods) {
                            layout.methods.push_back(sm);
                        }
                        // Marcar cuantos elementos son heredados para que
                        // el lowering de __module_init los omita (los
                        // anyade ya define_class en el loader).
                        layout.inherited_field_count =
                            static_cast<uint32_t>(super_layout->fields.size());
                        layout.inherited_static_field_count =
                            static_cast<uint32_t>(super_layout->static_fields.size());
                    }
                }

                std::unordered_map<std::string, bool> seen_field;
                for (const auto &fi : layout.fields) seen_field[fi.name] = true;
                // Los campos de instancia van DESPUES del ObjectHeader
                // (24 bytes) en el layout en memoria.  El frontend usa
                // estos offsets directamente con xchg cur0,r_obj +
                // addcur, asi que deben incluir el header para apuntar
                // al field real.  Los static_fields parten de 0
                // (offset relativo al bloque static_data).
                uint32_t off_inst = static_cast<uint32_t>(sizeof(loader::ObjectHeader));
                if (super_layout) {
                    // Continuar tras los fields heredados.
                    off_inst += static_cast<uint32_t>(super_layout->fields.size()) * 8;
                }
                uint32_t off_stat = 0;
                for (const auto &f : c->fields) {
                    if (!seen_field.emplace(f.name, true).second) {
                        diags_.error(f.loc,
                            "campo duplicado en clase '" + c->name + "': '" + f.name + "'");
                        continue;
                    }
                    Type ft = type_from_node(f.type.get());
                    if (ft.kind == PrimitiveKind::COUNT) {
                        diags_.error(f.loc,
                            "tipo invalido en campo '" + f.name + "' de la clase '" + c->name + "'");
                        continue;
                    }
                    StructFieldInfo fi;
                    fi.name = f.name;
                    fi.type = ft;
                    fi.size = 8; // todos los slots de instancia ocupan 8 bytes
                    if (f.is_static) {
                        fi.offset = off_stat;
                        off_stat += 8;
                        layout.static_fields.push_back(std::move(fi));
                    } else {
                        fi.offset = off_inst;
                        off_inst += 8;
                        layout.fields.push_back(std::move(fi));
                    }
                }
                layout.size_bytes = off_inst;

                // Resumen de metodos (incluyendo constructor).  Override:
                // si un metodo de la subclase tiene el mismo nombre que
                // uno heredado, REEMPLAZA el slot del super (mismo
                // vtable_index).  El AST lo marca con is_override pero
                // tambien aceptamos override implicito si el nombre coincide.
                std::unordered_map<std::string, bool> seen_method;
                for (const auto &m_inh : layout.methods) seen_method[m_inh.name] = true;
                for (size_t mi = 0; mi < c->methods.size(); ++mi) {
                    const auto *m = c->methods[mi].get();
                    const std::string &mname = m->name;

                    // Detectar override: ya existe un metodo con ese nombre
                    // (heredado del super).  Buscar su slot.
                    int override_idx = -1;
                    if (!m->is_constructor) {
                        for (size_t j = 0; j < layout.methods.size(); ++j) {
                            if (layout.methods[j].name == mname) {
                                override_idx = static_cast<int>(j);
                                break;
                            }
                        }
                    }

                    if (override_idx >= 0) {
                        // Override de metodo heredado.  Validar que el
                        // metodo del super NO es final.
                        if (layout.methods[override_idx].is_final) {
                            diags_.error(m->loc,
                                "no se puede sobrescribir el metodo final '" +
                                mname + "' de la superclase");
                            continue;
                        }
                        // Reemplazar el slot manteniendo vtable_index.
                        ClassMethodInfo mi_info;
                        mi_info.name           = mname;
                        mi_info.is_constructor = false;
                        mi_info.is_static      = m->is_static;
                        mi_info.is_final       = m->is_final;
                        mi_info.is_inline      = m->is_inline;
                        mi_info.defining_class = c->name;  // override en esta clase
                        mi_info.return_type    = m->return_type
                                                  ? type_from_node(m->return_type.get())
                                                  : Type{PrimitiveKind::VOID};
                        mi_info.param_types.reserve(m->params.size());
                        for (const auto &p : m->params) {
                            mi_info.param_types.push_back(type_from_node(p->type.get()));
                        }
                        mi_info.vtable_index = layout.methods[override_idx].vtable_index;
                        layout.methods[override_idx] = std::move(mi_info);
                        continue;
                    }

                    // Metodo nuevo (no override).  Si is_override estaba
                    // marcado pero no hay metodo con ese nombre en la
                    // jerarquia, error.
                    // Bug fix 2026-05-23: @Override tambien valido si
                    // implementa un metodo de cualquier interface declarada
                    // por la clase (interface_names).  Sin esto, el patron
                    // estandar Java/C# `class X : IFoo { @Override foo() }`
                    // se rechazaba con "metodo 'foo' no existe en la jerarquia"
                    // forzando a quitar @Override del codigo.
                    if (m->is_override) {
                        bool found_in_iface = false;
                        // Usar layout.interface_names (con promote `IFoo` desde
                        // super_name aplicada) en lugar de c->interface_names
                        // (que es el AST raw sin promote).
                        for (const auto &iname : layout.interface_names) {
                            auto it_if = class_layouts_.find(iname);
                            if (it_if == class_layouts_.end()) continue;
                            for (const auto &im : it_if->second.methods) {
                                if (im.name == mname) {
                                    found_in_iface = true;
                                    break;
                                }
                            }
                            if (found_in_iface) break;
                        }
                        if (!found_in_iface) {
                            diags_.error(m->loc,
                                "@Override: el metodo '" + mname +
                                "' no existe en la jerarquia de la clase");
                        }
                    }

                    if (!seen_method.emplace(mname, true).second
                     && !m->is_constructor) {
                        diags_.error(m->loc,
                            "metodo duplicado en clase '" + c->name + "': '" + mname + "'");
                        continue;
                    }
                    ClassMethodInfo mi_info;
                    mi_info.name           = mname;
                    mi_info.is_constructor = m->is_constructor;
                    mi_info.is_destructor  = m->is_destructor;
                    mi_info.is_static      = m->is_static;
                    mi_info.is_final       = m->is_final;
                    mi_info.is_inline      = m->is_inline;
                    mi_info.defining_class = c->name;
                    // capturar source file + line del decl.
                    mi_info.source_file    = m->loc.file;
                    mi_info.source_line    = m->loc.line;
                    mi_info.return_type    = m->return_type
                                              ? type_from_node(m->return_type.get())
                                              : Type{PrimitiveKind::VOID};
                    mi_info.param_types.reserve(m->params.size());
                    for (const auto &p : m->params) {
                        mi_info.param_types.push_back(type_from_node(p->type.get()));
                    }
                    mi_info.vtable_index = static_cast<uint32_t>(layout.methods.size());

                    // fix12 - detectar ctores trivial zero-init.  Si el
                    // body es solo `this.field = 0|0.0|null|false` para varios
                    // fields, podemos saltar la callvirt al ctor en runtime
                    // porque el GC ya hace memset a 0 del payload.  Solo aplica
                    // si la clase NO tiene super custom (super == "Object" o
                    // ausente): si hay super con ctor no-trivial, hay que llamarlo.
                    if (m->is_constructor && m->body
                        && (c->super_name.empty() || c->super_name == "Object")) {
                        bool zero_init_only = true;
                        for (const auto &stmt : m->body->body) {
                            if (!stmt) { zero_init_only = false; break; }
                            // Aceptamos solo ExprStmt con AssignExpr de la forma
                            // <FieldAccess>.field = literal_zero
                            if (stmt->kind != ast::NodeKind::ExprStmt) {
                                zero_init_only = false; break;
                            }
                            auto *es = static_cast<ast::ExprStmt *>(stmt.get());
                            if (!es->expr || es->expr->kind != ast::NodeKind::AssignExpr) {
                                zero_init_only = false; break;
                            }
                            auto *ae = static_cast<ast::AssignExpr *>(es->expr.get());
                            if (ae->op != ast::AssignOp::Assign) {
                                zero_init_only = false; break;
                            }
                            // target: FieldAccessExpr cuyo base es ThisExpr.
                            if (!ae->target
                             || ae->target->kind != ast::NodeKind::FieldAccessExpr) {
                                zero_init_only = false; break;
                            }
                            auto *fa = static_cast<ast::FieldAccessExpr *>(ae->target.get());
                            if (!fa->base || fa->base->kind != ast::NodeKind::ThisExpr) {
                                zero_init_only = false; break;
                            }
                            // value: literal cero/false/null.
                            if (!ae->value) { zero_init_only = false; break; }
                            const ast::Expr *v = ae->value.get();
                            bool is_zero_lit = false;
                            if (v->kind == ast::NodeKind::IntLitExpr) {
                                is_zero_lit = (static_cast<const ast::IntLitExpr *>(v)->value == 0);
                            } else if (v->kind == ast::NodeKind::FloatLitExpr) {
                                is_zero_lit = (static_cast<const ast::FloatLitExpr *>(v)->value == 0.0);
                            } else if (v->kind == ast::NodeKind::BoolLitExpr) {
                                is_zero_lit = (static_cast<const ast::BoolLitExpr *>(v)->value == false);
                            } else if (v->kind == ast::NodeKind::NullLitExpr) {
                                is_zero_lit = true;
                            }
                            if (!is_zero_lit) { zero_init_only = false; break; }
                        }
                        // Body vacio tambien cuenta como zero-init trivial.
                        mi_info.is_zero_init_ctor = zero_init_only;
                    }

                    layout.methods.push_back(std::move(mi_info));
                }

                // precomputar has_destructor para que las rules de
                // escape (check_assign) lo consulten en O(1) sin iterar
                // metodos.  Importante: heredamos del super, asi que si la
                // clase no declara su propio dtor pero el super si lo tiene,
                // se considera destructible (el dtor del super correra).
                for (const auto &mi : layout.methods) {
                    if (mi.is_destructor) { layout.has_destructor = true; break; }
                }

                // Sobrescribir entrada vacia pre-registrada con el layout
                // ya completo.
                class_layouts_[c->name] = std::move(layout);
            }
        }

        // punto-fijo de @c has_destructible_field.  Una clase tiene
        // @c has_destructible_field si alguno de sus fields (incluidos los
        // heredados del super) es de tipo CLASS y esa clase tiene
        // @c has_destructor o @c has_destructible_field a su vez.
        //
        // Iteramos hasta estabilizar para soportar referencias mutuamente
        // recursivas (LinkedList { Node head; } / Node { Node next; }).
        // En cada iteracion, si una clase X gana @c has_destructible_field,
        // las clases que la contienen como field tambien lo ganan.  Como
        // efecto del cierre transitivo, cualquier ciclo se resuelve en N
        // iteraciones donde N es la profundidad maxima de la cadena.
        for (bool changed = true; changed; ) {
            changed = false;
            for (auto &kv : class_layouts_) {
                ClassLayout &cl = kv.second;
                if (cl.is_interface || cl.is_runtime_predefined) continue;
                if (cl.has_destructible_field) continue;  // ya maximo
                for (const auto &f : cl.fields) {
                    // Solo fields de tipo CLASS aportan destructibilidad.
                    if (f.type.kind != PrimitiveKind::CLASS) continue;
                    auto it_inner = class_layouts_.find(f.type.struct_name);
                    if (it_inner == class_layouts_.end()) continue;
                    const ClassLayout &inner = it_inner->second;
                    if (inner.has_destructor || inner.has_destructible_field) {
                        cl.has_destructible_field = true;
                        changed = true;
                        break;
                    }
                }
            }
        }

        // sintesis del destructor implicito.  Para cada clase con
        // @c has_destructible_field == true y SIN destructor declarado por
        // el usuario, anadimos un ClassMethodDecl sintetico con body vacio
        // y @c is_destructor = true.  El lowering augmenta el body de TODOS
        // los destructores (sintetizados o no) con CALLVIRT a los dtors de
        // los fields destructibles, asi que un cuerpo vacio basta para
        // disparar la cadena RAII recursiva.
        //
        // Tras la sintesis actualizamos @c has_destructor a true (el flag
        // representa la presencia EFECTIVA de un destructor invocable, sea
        // user o sintetico).  Esto unifica todas las consultas downstream.
        for (auto &mod_node : mod_.decls) {
            if (!mod_node || mod_node->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(mod_node.get());
            auto it_lay = class_layouts_.find(cd->name);
            if (it_lay == class_layouts_.end()) continue;
            ClassLayout &lay = it_lay->second;
            if (lay.is_interface || lay.is_runtime_predefined) continue;
            if (!lay.has_destructible_field) continue;
            if (lay.has_destructor) continue;  // user ya declaro uno

            // Anadir ClassMethodDecl sintetica al AST con body vacio.
            auto dtor = std::make_unique<ast::ClassMethodDecl>();
            dtor->loc           = cd->loc;
            dtor->name          = "__dtor";
            dtor->is_destructor = true;
            dtor->access        = 0;
            dtor->body          = std::make_unique<ast::BlockStmt>();
            dtor->body->loc     = cd->loc;
            cd->methods.push_back(std::move(dtor));

            // Reflejarlo en el ClassLayout: anadir un ClassMethodInfo y
            // marcar @c has_destructor.  El @c vtable_index sigue al final
            // de los metodos existentes.
            ClassMethodInfo mi_info;
            mi_info.name           = "__dtor";
            mi_info.is_destructor  = true;
            mi_info.is_constructor = false;
            mi_info.is_static      = false;
            mi_info.is_final       = false;
            mi_info.is_inline      = false;
            mi_info.defining_class = cd->name;
            mi_info.return_type    = Type{PrimitiveKind::VOID};
            mi_info.vtable_index   = static_cast<uint32_t>(lay.methods.size());
            mi_info.source_file    = cd->loc.file;
            mi_info.source_line    = cd->loc.line;
            lay.methods.push_back(std::move(mi_info));
            lay.has_destructor = true;
        }

        // -----------------------------------------------------------------
        // Validacion de implementacion de interfaces.
        //
        // Para cada clase NO-interface con `class X : Base, IFoo, IBar`,
        // verificar que todos los metodos abstractos de IFoo y IBar
        // (incluyendo los heredados por sus super-interfaces) estan
        // presentes en X (o en algun ancestro) con firma compatible.
        // Las interfaces que no encuentre se reportan como error claro.
        // -----------------------------------------------------------------
        for (auto &kv : class_layouts_) {
            const ClassLayout &cl = kv.second;
            if (cl.is_interface) continue;        // las interfaces no implementan otras
            for (const std::string &iname : cl.interface_names) {
                auto it_iface = class_layouts_.find(iname);
                if (it_iface == class_layouts_.end()) {
                    diags_.error(SourceLoc{},
                        "interfaz '" + iname + "' usada por '" + cl.name +
                        "' no esta declarada");
                    continue;
                }
                if (!it_iface->second.is_interface) {
                    diags_.error(SourceLoc{},
                        "'" + iname + "' usada como interfaz por '" + cl.name +
                        "' es una clase, no una interfaz");
                    continue;
                }
                // Verificar que cada metodo abstracto de la interfaz esta
                // implementado en cl (mismo nombre + aridad + tipos).
                const ClassLayout &iface = it_iface->second;
                for (const ClassMethodInfo &im : iface.methods) {
                    bool found = false;
                    for (const ClassMethodInfo &cm : cl.methods) {
                        if (cm.name != im.name) continue;
                        if (cm.param_types.size() != im.param_types.size()) continue;
                        // Comparacion shallow de firmas: kinds primarios.
                        bool sigs_ok = (cm.return_type.kind == im.return_type.kind);
                        for (size_t i = 0;
                             sigs_ok && i < cm.param_types.size(); ++i) {
                            if (cm.param_types[i].kind != im.param_types[i].kind) {
                                sigs_ok = false;
                            }
                        }
                        if (sigs_ok) { found = true; break; }
                    }
                    if (!found) {
                        diags_.error(SourceLoc{},
                            "la clase '" + cl.name + "' no implementa el metodo '" +
                            im.name + "' requerido por la interfaz '" + iname + "'");
                    }
                }
            }
        }

        for (auto &decl : mod_.decls) {
            if (!decl) continue;
            if (decl->kind == ast::NodeKind::FunctionDecl) {
                auto *fn = static_cast<ast::FunctionDecl *>(decl.get());
                /* comptime fn -- registrar para que las llamadas
                 * desde contextos comptime puedan interpretar el body.
                 * NO se registra como Symbol::Function regular porque
                 * NO debe llamarse desde codigo runtime.  Pero sigue
                 * declarando el nombre en el scope global para que el
                 * type checker resuelva el ident en el callee de las
                 * llamadas (comptime_eval_expr discrimina luego). */
                if (fn->is_comptime) {
                    register_comptime_fn(fn->name, fn);
                    /* Tambien registramos un Symbol::Function dummy para
                     * que `lookup(fn->name)` lo resuelva.  El sig real
                     * importa poco porque la llamada nunca sale a IR. */
                    FunctionSig sig_ct;
                    sig_ct.return_type = type_from_node(fn->return_type.get());
                    sig_ct.param_types.reserve(fn->params.size());
                    for (auto &p : fn->params) {
                        sig_ct.param_types.push_back(type_from_node(p->type.get()));
                    }
                    Symbol s;
                    s.kind      = SymbolKind::Function;
                    s.sig_index = (uint32_t)function_sigs_.size();
                    sig_by_name_[fn->name] = s.sig_index;
                    function_sigs_.push_back(std::move(sig_ct));
                    if (!declare(fn->name, s)) {
                        diags_.error(fn->loc,
                            "comptime fn: redefinicion de '" + fn->name + "'");
                    }
                    continue;
                }

                FunctionSig sig;
                Type ret_t = type_from_node(fn->return_type.get());
                // Mejora II: si la funcion es @Async, el wrapper publico
                // visible al callsite devuelve Future<T> donde T es el tipo
                // declarado del @c return.  El bytecode del wrapper sigue
                // produciendo un i64 (handle), pero al frontend le
                // interesa preservar T para que `T r = await fn();` se
                // tipo-checkee correctamente.
                if (fn->is_async) {
                    sig.return_type = Type::make_future(std::move(ret_t));
                } else {
                    sig.return_type = std::move(ret_t);
                }
                sig.param_types.reserve(fn->params.size());
                for (auto &p : fn->params) {
                    sig.param_types.push_back(type_from_node(p->type.get()));
                }

                Symbol s;
                s.kind      = SymbolKind::Function;
                s.sig_index = (uint32_t)function_sigs_.size();
                sig_by_name_[fn->name] = s.sig_index;
                function_sigs_.push_back(std::move(sig));
                if (!declare(fn->name, s)) {
                    // Bug fix 2026-05-23: forward declaration -- si el simbolo
                    // ya existe Y este es un forward decl (sin body), OK.
                    // Si la PREVIA era forward y esta tiene body, tambien OK
                    // (es la definicion completando la forward).  Solo error
                    // si AMBAS tienen body (redefinicion real).
                    if (!fn->is_forward_decl) {
                        // Verificar si el simbolo existente era un forward decl.
                        const Symbol *prev = lookup(fn->name);
                        bool prev_is_forward = false;
                        if (prev && prev->kind == SymbolKind::Function) {
                            for (auto &d2 : mod_.decls) {
                                if (!d2 || d2->kind != ast::NodeKind::FunctionDecl) continue;
                                auto *prev_fn = static_cast<ast::FunctionDecl *>(d2.get());
                                if (prev_fn != fn && prev_fn->name == fn->name
                                 && prev_fn->is_forward_decl) {
                                    prev_is_forward = true; break;
                                }
                            }
                        }
                        if (!prev_is_forward) {
                            diags_.error(fn->loc,
                                "redefinicion de simbolo a nivel global: '" + fn->name + "'");
                        }
                    }
                }
            } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
                auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                Symbol s;
                s.kind     = SymbolKind::Variable;
                s.type     = type_from_node(gv->type.get());
                s.is_const = gv->is_const;
                if (!declare(gv->name, s)) {
                    diags_.error(gv->loc, "redefinicion de simbolo a nivel global: '" + gv->name + "'");
                }
            } else if (decl->kind == ast::NodeKind::ExternFnDecl) {
                // FFI declarativo: registrar como Symbol::Function con
                // FunctionSig::extern_lib != "" para que el lowering emita
                // CALLN @Method("<lib>:<name>") en vez de CALLVM al llamarla.
                auto *efd = static_cast<ast::ExternFnDecl *>(decl.get());
                FunctionSig sig;
                sig.return_type = type_from_node(efd->return_type.get());
                sig.param_types.reserve(efd->params.size());
                for (auto &p : efd->params) {
                    sig.param_types.push_back(type_from_node(p->type.get()));
                }
                sig.extern_lib = efd->lib;
                Symbol s;
                s.kind      = SymbolKind::Function;
                s.sig_index = (uint32_t)function_sigs_.size();
                sig_by_name_[efd->name] = s.sig_index;
                function_sigs_.push_back(std::move(sig));
                if (!declare(efd->name, s)) {
                    diags_.error(efd->loc,
                        "redefinicion de simbolo extern: '" + efd->name + "'");
                }
            }
        }
    }

    // ---------------------------------------------------------------------
    // Pase 2: cuerpos de funciones.
    // ---------------------------------------------------------------------

    void TypeChecker::check_functions() {
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::FunctionDecl) {
                // Globales: si tienen init, chequear el tipo.
                if (decl && decl->kind == ast::NodeKind::GlobalVarDecl) {
                    auto *gv = static_cast<ast::GlobalVarDecl *>(decl.get());
                    if (gv->init) {
                        Type t = check_expr(gv->init.get());
                        const Type want = type_from_node(gv->type.get());
                        /* A.39: para comptime const NO aplicamos el check
                         * estricto -- el "tipo" del literal es flexible
                         * (string literal tiene type PTR pero comptime
                         * lo evalua como STRING).  La validez se chequea
                         * via comptime_eval_expr abajo. */
                        if (gv->is_comptime) {
                            /* skip */
                        } else
                        if (t.kind != PrimitiveKind::COUNT
                         && want.kind != PrimitiveKind::COUNT
                         && !is_numeric(t.kind) == !is_numeric(want.kind)
                         && t != want)
                        {
                            // Si ambos son numericos pero no exactamente iguales,
                            // toleramos la promocion implicita (la decision final
                            // la toma el lowering).  Si no son ambos numericos,
                            // exigimos igualdad estricta.
                            // Bug fix 2026-05-23: tolerar `string g = "lit"` --
                            // el init es PTR (literal raw) y el destino STRING;
                            // el lowering hara la promotion via STRMAKE en
                            // __module_init (igual que vars locales).
                            const bool is_str_from_lit =
                                want.kind == PrimitiveKind::STRING
                                && t.kind == PrimitiveKind::PTR
                                && gv->init
                                && gv->init->kind == ast::NodeKind::StringLitExpr;
                            if (!(is_numeric(t.kind) && is_numeric(want.kind))
                                && !is_str_from_lit) {
                                diags_.error(gv->loc,
                                    std::string("tipo del inicializador (") + primitive_name(t.kind) +
                                    ") incompatible con el tipo declarado (" + primitive_name(want.kind) + ")");
                            }
                        }
                    }
                    /* comptime const NAME = expr; evaluar init en
                     * compile-time y cachear el valor.  Errors si no es
                     * comptime-evaluable.  LANG.fix-2: si el global YA
                     * fue inicializado (por init_comptime_globals previo
                     * que llama el runtime de top-level comptime block),
                     * NO re-evaluar el init -- preservamos las mutaciones
                     * aplicadas por el bloque. */
                    if (gv->is_comptime
                     && comptime_const_values_.find(gv->name)
                            == comptime_const_values_.end()) {
                        if (!gv->init) {
                            diags_.error(gv->loc,
                                "'comptime const " + gv->name +
                                "' requiere un inicializador");
                        } else {
                            const ComptimeEvalResult r =
                                comptime_eval_expr(*this, gv->init.get());
                            if (!r.ok) {
                                diags_.error(gv->init->loc,
                                    "el init de 'comptime const " + gv->name +
                                    "' no es comptime-evaluable");
                            } else {
                                ComptimeConst c;
                                /* sugar: si gv->type es nullptr
                                 * (sugar `comptime X = ...` sin tipo
                                 * explicito), inferimos el tipo desde el
                                 * ComptimeEvalResult: is_str->STRING,
                                 * is_type->TYPE_META, else->I64. */
                                if (gv->type) {
                                    c.type = type_from_node(gv->type.get());
                                } else if (r.is_str) {
                                    c.type = Type{PrimitiveKind::STRING};
                                } else if (r.is_type) {
                                    c.type = Type{PrimitiveKind::TYPE_META};
                                } else {
                                    c.type = Type{PrimitiveKind::I64};
                                }
                                c.is_str    = r.is_str;
                                c.is_array  = r.is_array;
                                c.is_struct = r.is_struct;
                                c.is_type   = r.is_type;
                                /* A.43.17: globales `comptime var` son mutables.
                                 * Los `comptime const` mantienen is_mutable=false.
                                 * Las @Macro y comptime fn pueden modificar
                                 * globals mutables via apply_comptime_assign. */
                                c.is_mutable = !gv->is_const;
                                if (r.is_str)         c.str_value     = r.str;
                                else if (r.is_array)  c.array_vals    = r.array_vals;
                                else if (r.is_struct) c.struct_fields = r.struct_fields;
                                else if (r.is_type)   c.type_val      = r.type_val;
                                else                  c.value         = r.value;
                                // v4: propagar atributos @align/@hot/@cold/@section.
                                c.attr_align   = gv->attr_align;
                                c.attr_hot     = gv->attr_hot;
                                c.attr_cold    = gv->attr_cold;
                                c.attr_section = gv->attr_section;
                                comptime_const_values_[gv->name] = c;
                            }
                        }
                    }
                }
                continue;
            }
            auto *fn = static_cast<ast::FunctionDecl *>(decl.get());
            if (!fn->body) continue;
            /* comptime fn -- el body solo se interpreta al
             * call site via comptime_eval_stmt.  No type-check estatico
             * aqui (los IdentExpr no necesitan annotation; el eval los
             * busca en tc.comptime_const_locals_).  Si hay errores de
             * sintaxis o expresiones invalidas, el eval fallara con
             * `ok=false` en el call y emitiremos error alli. */
            if (fn->is_comptime) continue;

            // Mejora II: validacion @Async extendida.  Antes solo permitia
            // funciones sin parametros y return type i64.  Ahora:
            //   - Cualquier numero de parametros, cada uno con tipo de
            //     tamano <= 8 bytes (primitivos numericos, bool, char,
            //     ptr, handle de string/objeto, futures).
            //   - Cualquier tipo de retorno T con tamano <= 8 bytes.
            //   - El wrapper publico visible al callsite devuelve
            //     Future<T> (envuelto automaticamente por el type checker).
            //     `i32 r = await compute(10, 20);` tipo-checkea correctamente.
            //
            // Tipos > 8 bytes (struct, array, optional, result) requeririan
            // serializacion en buffer auxiliar.  Deferido a Phase B.
            if (fn->is_async) {
                auto fits_in_qword = [](const Type &t) -> bool {
                    if (t.kind == PrimitiveKind::COUNT) return true; // tipo desconocido OK
                    return primitive_size_bytes(t.kind) > 0
                        && primitive_size_bytes(t.kind) <= 8;
                };
                for (size_t pi = 0; pi < fn->params.size(); ++pi) {
                    Type pt = type_from_node(fn->params[pi]->type.get());
                    if (!fits_in_qword(pt)) {
                        diags_.error(fn->params[pi]->loc,
                            "@Async: parametro '" + fn->params[pi]->name +
                            "' de tipo '" + type_to_string(pt) +
                            "' excede 8 bytes.  Tipos compuestos (struct, array, "
                            "Optional, Result) no soportados aun.");
                    }
                }
                const Type rt_chk = type_from_node(fn->return_type.get());
                if (rt_chk.kind != PrimitiveKind::COUNT
                 && rt_chk.kind != PrimitiveKind::VOID
                 && !fits_in_qword(rt_chk)) {
                    diags_.error(fn->loc,
                        "@Async: tipo de retorno '" + type_to_string(rt_chk) +
                        "' excede 8 bytes.  Tipos compuestos no soportados aun.");
                }
            }

            const Type fn_ret = type_from_node(fn->return_type.get());
            push_scope(); // scope de la funcion (parametros)
            for (auto &p : fn->params) {
                Symbol sp;
                sp.kind = SymbolKind::Param;
                sp.type = type_from_node(p->type.get());
                if (!declare(p->name, sp)) {
                    diags_.error(p->loc, "parametro repetido: '" + p->name + "'");
                }
            }
            // Guardar y settear current_fn_return_type_ para que
            // check_match (que recibe expected_return_type via miembro
            // y no via param para no contaminar la signature) pueda
            // validar returns dentro del match con el tipo correcto.
            const Type saved_ret = current_fn_return_type_;
            current_fn_return_type_ = fn_ret;
            // Resetear el borrow checker al entrar a cada funcion.
            // Cada funcion tiene su propio scope de borrows; los borrows
            // de una funcion no afectan a otra.
            borrow_checker_.reset();
            // F2: registrar parametros como owners con OwnerKind::Param.
            // Si el parametro ES un borrow (su tipo es BORROW/BORROW_MUT),
            // ademas lo registramos como borrower self-referencial: el
            // borrow checker lo reconoce como "borrow valido cuyo owner
            // es Param" -> escape via return permitido.
            for (auto &p : fn->params) {
                borrow_checker_.declare_owner(p->name, OwnerKind::Param);
                const Type pt = type_from_node(p->type.get());
                if (pt.kind == PrimitiveKind::BORROW
                 || pt.kind == PrimitiveKind::BORROW_MUT) {
                    borrow_checker_.register_borrow(
                        p->name, p->name,
                        /*is_mut=*/(pt.kind == PrimitiveKind::BORROW_MUT));
                }
            }
            // F1 NLL: pre-pase de last-uses + reset del contador.
            current_stmt_idx_ = 0;
            compute_borrow_last_uses(fn->body.get());
            // BugFix R8: cuando estamos dentro de un @Macro body, todas
            // las var-decls se tratan automaticamente como comptime
            // const (incluso si no llevan `comptime` explicito).  Esto
            // permite que `string n = typename<T>(); comptime_concat(n, "/")`
            // funcione naturalmente sin que el usuario tenga que escribir
            // `comptime const string n = ...`.
            const bool saved_is_macro = current_fn_is_macro_;
            current_fn_is_macro_ = fn->is_macro;
            // Tambien empujamos un scope comptime nuevo para los locals del
            // macro body (para que find_comptime_local_mut los encuentre).
            if (fn->is_macro) push_comptime_scope();
            check_block(fn->body.get(), fn_ret);
            if (fn->is_macro) pop_comptime_scope();
            current_fn_is_macro_ = saved_is_macro;
            current_fn_return_type_ = saved_ret;
            pop_scope();
        }

        // Chequeo de cuerpos de metodos de clase.  Para cada ClassDecl
        // recorremos sus metodos: el scope local incluye 'this' y los
        // parametros declarados.  El campo current_class_ guia
        // check_this para que sepa la clase contenedora.
        for (auto &decl : mod_.decls) {
            if (!decl || decl->kind != ast::NodeKind::ClassDecl) continue;
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            auto it = class_layouts_.find(cd->name);
            if (it == class_layouts_.end()) continue;
            const ClassLayout &cls = it->second;

            const std::string saved_class = current_class_;
            current_class_ = cd->name;

            for (auto &m : cd->methods) {
                if (!m || !m->body) continue;
                check_class_method(cls, m.get());
            }
            current_class_ = saved_class;
        }
    }

    /**
     * @brief Sprint edge-bugs (2026-06-02): rewrite implicit-this.
     *
     * En metodos de instancia, identifiers como @c v dentro del body que
     * matchean un field name de la clase contenedora se reescriben como
     * @c this.v (FieldAccessExpr).  Convencion Java/C#.  No reescribe si:
     *   - el ident matchea un parametro (params_set)
     *   - el ident matchea una local declarada en un scope previo (locals_stack)
     *   - es lhs de un VarDecl (la propia declaracion)
     *
     * El walker recibe @c unique_ptr<Expr>& para poder swap-ear el nodo
     * IdentExpr por FieldAccessExpr in-place.  Walkea TODAS las statements
     * y exprs hijas (recursivo).
     */
    static void rewrite_implicit_this(
        std::unique_ptr<ast::Stmt> &stmt,
        const std::unordered_set<std::string> &field_names,
        const std::unordered_set<std::string> &params_set);

    static void rewrite_implicit_this_expr(
        std::unique_ptr<ast::Expr> &node,
        const std::unordered_set<std::string> &field_names,
        const std::unordered_set<std::string> &params_set,
        std::vector<std::unordered_set<std::string>> &locals_stack) {
        if (!node) return;
        // IdentExpr: si matches field y NO en params/locals, rewrite.
        if (node->kind == ast::NodeKind::IdentExpr) {
            auto *id = static_cast<ast::IdentExpr *>(node.get());
            if (field_names.count(id->name)
             && !params_set.count(id->name)
             && id->name != "this" && id->name != "super") {
                bool shadowed = false;
                for (auto &s : locals_stack) if (s.count(id->name)) { shadowed = true; break; }
                if (!shadowed) {
                    auto fa = std::make_unique<ast::FieldAccessExpr>();
                    fa->loc = id->loc;
                    auto base = std::make_unique<ast::IdentExpr>();
                    base->loc  = id->loc;
                    base->name = "this";
                    fa->base       = std::move(base);
                    fa->field_name = id->name;
                    node = std::move(fa);
                }
            }
            return;
        }
        switch (node->kind) {
            case ast::NodeKind::BinaryExpr: {
                auto *b = static_cast<ast::BinaryExpr *>(node.get());
                rewrite_implicit_this_expr(b->lhs, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(b->rhs, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::UnaryExpr: {
                auto *u = static_cast<ast::UnaryExpr *>(node.get());
                rewrite_implicit_this_expr(u->operand, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::TernaryExpr: {
                auto *t = static_cast<ast::TernaryExpr *>(node.get());
                rewrite_implicit_this_expr(t->cond, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(t->then_expr, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(t->else_expr, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::CallExpr: {
                auto *c = static_cast<ast::CallExpr *>(node.get());
                // Para el callee: si es IdentExpr (llamada simple `foo(x)`),
                // NO reescribimos a this.foo -- la llamada se resuelve por
                // simbolo global (clase methods se invocan con this. explicito
                // o se inlinean).  Si es FieldAccessExpr o algo mas, recurse.
                if (c->callee && c->callee->kind != ast::NodeKind::IdentExpr) {
                    rewrite_implicit_this_expr(c->callee, field_names, params_set, locals_stack);
                }
                for (auto &a : c->args)
                    rewrite_implicit_this_expr(a, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::FieldAccessExpr: {
                auto *fa = static_cast<ast::FieldAccessExpr *>(node.get());
                rewrite_implicit_this_expr(fa->base, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::IndexExpr: {
                auto *ix = static_cast<ast::IndexExpr *>(node.get());
                rewrite_implicit_this_expr(ix->base, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(ix->index, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::AssignExpr: {
                auto *a = static_cast<ast::AssignExpr *>(node.get());
                rewrite_implicit_this_expr(a->target, field_names, params_set, locals_stack);
                rewrite_implicit_this_expr(a->value, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::CastExpr: {
                auto *c = static_cast<ast::CastExpr *>(node.get());
                rewrite_implicit_this_expr(c->operand, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::NewExpr: {
                auto *n = static_cast<ast::NewExpr *>(node.get());
                for (auto &a : n->args)
                    rewrite_implicit_this_expr(a, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::LambdaExpr: {
                auto *lam = static_cast<ast::LambdaExpr *>(node.get());
                std::unordered_set<std::string> lam_locals;
                for (auto &p : lam->params) lam_locals.insert(p->name);
                locals_stack.push_back(std::move(lam_locals));
                if (lam->body)
                    for (auto &child : lam->body->body)
                        rewrite_implicit_this(child, field_names, params_set);
                locals_stack.pop_back();
                return;
            }
            case ast::NodeKind::StringLitExpr: {
                auto *sl = static_cast<ast::StringLitExpr *>(node.get());
                for (auto &ie : sl->interp_exprs)
                    rewrite_implicit_this_expr(ie, field_names, params_set, locals_stack);
                return;
            }
            case ast::NodeKind::MatchExpr: {
                auto *me = static_cast<ast::MatchExpr *>(node.get());
                rewrite_implicit_this_expr(me->scrutinee, field_names, params_set, locals_stack);
                for (auto &arm : me->arms) {
                    std::unordered_set<std::string> arm_locals;
                    for (auto &bn : arm.bindings) arm_locals.insert(bn);
                    locals_stack.push_back(std::move(arm_locals));
                    if (arm.body)
                        rewrite_implicit_this(arm.body, field_names, params_set);
                    locals_stack.pop_back();
                }
                return;
            }
            case ast::NodeKind::InitListExpr: {
                auto *il = static_cast<ast::InitListExpr *>(node.get());
                for (auto &e : il->elements)
                    rewrite_implicit_this_expr(e, field_names, params_set, locals_stack);
                return;
            }
            default:
                return;
        }
    }

    static void rewrite_implicit_this(
        std::unique_ptr<ast::Stmt> &stmt,
        const std::unordered_set<std::string> &field_names,
        const std::unordered_set<std::string> &params_set) {
        if (!stmt) return;
        /* Stack de scopes locales (a parte de los params).  Cada BlockStmt
         * push/pop su scope para soportar shadowing.  El VarDecl agrega el
         * nombre al scope ACTUAL antes de procesar las stmts siguientes. */
        std::vector<std::unordered_set<std::string>> locals_stack;
        std::function<void(std::unique_ptr<ast::Stmt> &)> visit;
        visit = [&](std::unique_ptr<ast::Stmt> &s) {
            if (!s) return;
            switch (s->kind) {
                case ast::NodeKind::BlockStmt: {
                    auto *bs = static_cast<ast::BlockStmt *>(s.get());
                    locals_stack.push_back({});
                    for (auto &child : bs->body) visit(child);
                    locals_stack.pop_back();
                    return;
                }
                case ast::NodeKind::VarDeclStmt: {
                    auto *vd = static_cast<ast::VarDeclStmt *>(s.get());
                    /* Reescribir el init ANTES de declarar el local (el init
                     * no puede referirse a si mismo). */
                    if (vd->init)
                        rewrite_implicit_this_expr(vd->init, field_names,
                            params_set, locals_stack);
                    /* Anadir el nombre al scope local actual.  Si stack vacio,
                     * crear scope top-level. */
                    if (locals_stack.empty()) locals_stack.push_back({});
                    locals_stack.back().insert(vd->name);
                    return;
                }
                case ast::NodeKind::ExprStmt: {
                    auto *es = static_cast<ast::ExprStmt *>(s.get());
                    rewrite_implicit_this_expr(es->expr, field_names,
                        params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::ReturnStmt: {
                    auto *rs = static_cast<ast::ReturnStmt *>(s.get());
                    if (rs->value)
                        rewrite_implicit_this_expr(rs->value, field_names,
                            params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::IfStmt: {
                    auto *is = static_cast<ast::IfStmt *>(s.get());
                    rewrite_implicit_this_expr(is->cond, field_names,
                        params_set, locals_stack);
                    visit(is->then_branch);
                    visit(is->else_branch);
                    return;
                }
                case ast::NodeKind::WhileStmt: {
                    auto *ws = static_cast<ast::WhileStmt *>(s.get());
                    rewrite_implicit_this_expr(ws->cond, field_names,
                        params_set, locals_stack);
                    visit(ws->body);
                    return;
                }
                case ast::NodeKind::DoWhileStmt: {
                    auto *ds = static_cast<ast::DoWhileStmt *>(s.get());
                    visit(ds->body);
                    rewrite_implicit_this_expr(ds->cond, field_names,
                        params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::ForStmt: {
                    auto *fs = static_cast<ast::ForStmt *>(s.get());
                    locals_stack.push_back({});
                    visit(fs->init);
                    if (fs->cond)
                        rewrite_implicit_this_expr(fs->cond, field_names,
                            params_set, locals_stack);
                    if (fs->step)
                        rewrite_implicit_this_expr(fs->step, field_names,
                            params_set, locals_stack);
                    visit(fs->body);
                    locals_stack.pop_back();
                    return;
                }
                case ast::NodeKind::TryStmt: {
                    auto *ts = static_cast<ast::TryStmt *>(s.get());
                    /* body es BlockStmt; iterar children. */
                    if (ts->body)
                        for (auto &child : ts->body->body)
                            visit(child);
                    for (auto &c : ts->catches) {
                        locals_stack.push_back({});
                        if (!c.var_name.empty())
                            locals_stack.back().insert(c.var_name);
                        if (c.body)
                            for (auto &child : c.body->body) visit(child);
                        locals_stack.pop_back();
                    }
                    if (ts->finally_body)
                        for (auto &child : ts->finally_body->body) visit(child);
                    return;
                }
                case ast::NodeKind::ThrowStmt: {
                    auto *th = static_cast<ast::ThrowStmt *>(s.get());
                    if (th->value)
                        rewrite_implicit_this_expr(th->value, field_names,
                            params_set, locals_stack);
                    return;
                }
                case ast::NodeKind::SynchronizedStmt: {
                    auto *ss = static_cast<ast::SynchronizedStmt *>(s.get());
                    rewrite_implicit_this_expr(ss->target, field_names,
                        params_set, locals_stack);
                    if (ss->body)
                        for (auto &child : ss->body->body) visit(child);
                    return;
                }
                default:
                    return;
            }
        };
        visit(stmt);
    }

    void TypeChecker::check_class_method(const ClassLayout &cls, ast::ClassMethodDecl *m) {
        // Semantica de modificadores:
        //  - constructor static: error (no tiene sentido).
        //  - metodo static que use 'this': error claro via check_this
        //    consultando current_method_is_static_.
        //  - final: aceptado, sera enforced en cuando haya override.
        if (m->is_static && m->is_constructor) {
            diags_.error(m->loc,
                "el constructor de '" + cls.name + "' no puede ser 'static'");
        }

        const bool saved_static = current_method_is_static_;
        current_method_is_static_ = m->is_static;

        push_scope();
        // 'this' implicito como Symbol solo en metodos de instancia.
        if (!m->is_static) {
            Symbol s_this;
            s_this.kind = SymbolKind::Param;
            s_this.type = Type{PrimitiveKind::CLASS, cls.name};
            (void)declare("this", s_this);
        }
        // Parametros declarados.
        for (auto &p : m->params) {
            Symbol sp;
            sp.kind = SymbolKind::Param;
            sp.type = type_from_node(p->type.get());
            if (!declare(p->name, sp)) {
                diags_.error(p->loc, "parametro repetido: '" + p->name + "'");
            }
        }
        // Sprint edge-bugs (2026-06-02): pre-pase de implicit-this.
        // Recolecta field names de la clase + sus supers transitivos.
        // Despues walkea el body y reescribe identifiers de fields a
        // FieldAccessExpr(this, field).  Solo para metodos de instancia
        // (en static methods no hay this, debe ser explicito error).
        if (!m->is_static && m->body) {
            std::unordered_set<std::string> field_names;
            const ClassLayout *cl_cur = &cls;
            int depth_guard = 256;
            while (cl_cur && depth_guard-- > 0) {
                for (const auto &f : cl_cur->fields) field_names.insert(f.name);
                if (cl_cur->super_name.empty()) break;
                auto sup_it = class_layouts_.find(cl_cur->super_name);
                if (sup_it == class_layouts_.end()) break;
                cl_cur = &sup_it->second;
            }
            std::unordered_set<std::string> params_set;
            params_set.insert("this");
            params_set.insert("super");
            for (auto &p : m->params) params_set.insert(p->name);
            /* Rewrite inline las stmts del body.  m->body es
             * unique_ptr<BlockStmt> con vector body de unique_ptr<Stmt>.
             *
             * BugFix (2026-06-04): antes iterabamos los children uno a uno
             * llamando @c rewrite_implicit_this(child, ...) , pero esa
             * funcion crea su PROPIO @c locals_stack vacio internamente, lo
             * que provocaba que las var locals declaradas en un statement
             * (e.g. `i64 fp = ffi_call(...)`) NO se vieran como shadowed en
             * el siguiente statement (e.g. `if (fp == 0)`).  Resultado: el
             * `fp` se reescribia a `this.fp` cuando el ident colisionaba
             * con un field, leyendo basura.  Caso real cerrado: file_io
             * `read_all` con variable local `fp` colisionando con field
             * `this.fp`.
             *
             * Fix: tratar el body como UN SOLO BlockStmt, lo que comparte
             * el @c locals_stack interno entre todos los statements del body.
             */
            std::unique_ptr<ast::Stmt> body_as_stmt(m->body.release());
            rewrite_implicit_this(body_as_stmt, field_names, params_set);
            // Re-inyectar el BlockStmt al field body (el rewrite no lo movio).
            m->body.reset(static_cast<ast::BlockStmt *>(body_as_stmt.release()));
        }
        // Tipo de retorno: VOID para constructores; el declarado para los demas.
        const Type fn_ret = m->is_constructor
                              ? Type{PrimitiveKind::VOID}
                              : type_from_node(m->return_type.get());
        const Type saved_ret = current_fn_return_type_;
        current_fn_return_type_ = fn_ret;
        check_block(m->body.get(), fn_ret);
        current_fn_return_type_ = saved_ret;
        pop_scope();
        current_method_is_static_ = saved_static;
    }

} // namespace vex
