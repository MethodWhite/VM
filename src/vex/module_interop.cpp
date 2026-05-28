/**
 * @file module_interop.cpp
 * @brief Interop entre TypeChecker y formato @c .vexi (Phase M.2.d).
 *
 * Funciones libres en namespace @c vex que conectan el estado del
 * @c TypeChecker con un @c VexiModule:
 *
 *   - @c export_typechecker_to_vexi: extrae los simbolos publicos del
 *     TypeChecker a un @c VexiModule listo para serializar.  En el MVP
 *     todos los simbolos top-level son publicos; @c public/private
 *     explicito llega en M6.
 *
 *   - @c import_vexi_into_typechecker: inyecta los simbolos listados en
 *     @c only_symbols en las tablas del TypeChecker (type_aliases_,
 *     struct_layouts_, class_layouts_, enum_layouts_, function_sigs_).
 *
 * Las firmas viven en este TU (no en type_checker.h) para no forzar el
 * include de vexi_format.h en todos los consumidores del TypeChecker.
 * El compiler.cpp (en M2.e) las invoca explicitamente.
 */

#include "vex/module_interop.h"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "vex/type_checker.h"
#include "vex/types.h"
#include "vex/vexi_format.h"

namespace vex {

// ---------------------------------------------------------------------------
// Convertir un @c Type del checker a un typename canonico.  Usado para
// serializar tipos de fields, returns, params, etc.
// ---------------------------------------------------------------------------
static std::string canonical_typename_of(const Type &t) {
    // type_to_string ya produce un nombre canonico legible.  Por ejemplo:
    //   i32, u64*, Optional<i32>, Result<i32, string>, fn(i32) -> i64,
    //   VirtualPtr<T>, struct_name (para STRUCT), nominal_name (newtype).
    return type_to_string(t);
}

} // namespace vex

// ---------------------------------------------------------------------------
// resolve_type_string: re-parsea un typename canonico al Type del checker.
// Coverage v1 (M2 MVP):
//   - Primitivos: i8/i16/i32/i64, u8/u16/u32/u64, f32/f64, bool, char, void.
//   - string.
//   - Tipos nombrados ya conocidos en el checker (struct/class/enum/alias).
//   - Punteros `T*` (parsea recursivamente).
//   - Arrays `T[]` (decay, size=0).
//
// No cubre (deferred): fn(...), Optional<T>, Result<V,E>, VirtualPtr<T>,
// arrays con tamano explicito, generics anidados.  Estos casos requieren
// un mini-parser de tipos que llega en M5.
// ---------------------------------------------------------------------------
namespace vex {

// Helper: encuentra el ultimo `>` que coincide con el primer `<` en
// `prefix<contents>`.  Devuelve std::string::npos si no es la forma
// generica esperada.  Util para Optional<T>, Result<V,E>, fn(...), etc.
static size_t find_matching_close_(const std::string &s,
                                    size_t open_idx,
                                    char open_ch,
                                    char close_ch) noexcept {
    int depth = 1;
    for (size_t i = open_idx + 1; i < s.size(); ++i) {
        if (s[i] == open_ch) ++depth;
        else if (s[i] == close_ch) {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

// Helper: separa una lista de tipos por comas a top-level (respetando
// anidamiento de < > / ( ) ).  Trimea espacios.
static std::vector<std::string> split_type_list_(const std::string &s) {
    std::vector<std::string> out;
    int depth_angle = 0, depth_paren = 0;
    std::string cur;
    cur.reserve(s.size());
    for (char c : s) {
        if (c == '<') ++depth_angle;
        else if (c == '>') --depth_angle;
        else if (c == '(') ++depth_paren;
        else if (c == ')') --depth_paren;
        if (c == ',' && depth_angle == 0 && depth_paren == 0) {
            // Trim
            size_t a = 0, b = cur.size();
            while (a < b && (cur[a] == ' ' || cur[a] == '\t')) ++a;
            while (b > a && (cur[b-1] == ' ' || cur[b-1] == '\t')) --b;
            out.push_back(cur.substr(a, b - a));
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) {
        size_t a = 0, b = cur.size();
        while (a < b && (cur[a] == ' ' || cur[a] == '\t')) ++a;
        while (b > a && (cur[b-1] == ' ' || cur[b-1] == '\t')) --b;
        out.push_back(cur.substr(a, b - a));
    }
    return out;
}

Type TypeChecker::resolve_type_string(const std::string &type_str) const {
    if (type_str.empty()) return Type{};

    // Punteros: stripear sufijo '*' antes del resto.
    if (type_str.back() == '*') {
        std::string inner = type_str.substr(0, type_str.size() - 1);
        Type pt = resolve_type_string(inner);
        if (pt.kind == PrimitiveKind::VOID && inner != "void") {
            // No se pudo resolver inner: devolver void* generico.
            return Type::make_ptr(Type{PrimitiveKind::VOID});
        }
        return Type::make_ptr(std::move(pt));
    }

    // Phase M5.b L.10: ARRAY con tamano explicito `T[N]` o decay `T[]`.
    if (type_str.size() >= 2 && type_str.back() == ']') {
        size_t lb = type_str.find_last_of('[');
        if (lb != std::string::npos) {
            std::string inner    = type_str.substr(0, lb);
            std::string size_str = type_str.substr(lb + 1, type_str.size() - lb - 2);
            Type elt = resolve_type_string(inner);
            uint32_t sz = 0;
            if (!size_str.empty()) {
                try { sz = static_cast<uint32_t>(std::stoul(size_str)); }
                catch (...) { sz = 0; }
            }
            return Type::make_array(std::move(elt), sz, /*virt=*/true);
        }
    }

    // Phase M5.b L.10: VirtualPtr<T> (PTR is_virtual=true).
    {
        const std::string prefix = "VirtualPtr<";
        if (type_str.size() > prefix.size()
         && type_str.compare(0, prefix.size(), prefix) == 0
         && type_str.back() == '>') {
            std::string inner = type_str.substr(prefix.size(),
                                                  type_str.size() - prefix.size() - 1);
            Type elt = resolve_type_string(inner);
            return Type::make_ptr(std::move(elt), /*virt=*/true);
        }
    }

    // Phase M5.b L.10: Optional<T>.
    {
        const std::string prefix = "Optional<";
        if (type_str.size() > prefix.size()
         && type_str.compare(0, prefix.size(), prefix) == 0
         && type_str.back() == '>') {
            std::string inner = type_str.substr(prefix.size(),
                                                  type_str.size() - prefix.size() - 1);
            Type elt = resolve_type_string(inner);
            return Type::make_optional(std::move(elt));
        }
    }

    // Phase M5.b L.10: Result<V, E>.
    {
        const std::string prefix = "Result<";
        if (type_str.size() > prefix.size()
         && type_str.compare(0, prefix.size(), prefix) == 0
         && type_str.back() == '>') {
            std::string inner = type_str.substr(prefix.size(),
                                                  type_str.size() - prefix.size() - 1);
            auto parts = split_type_list_(inner);
            if (parts.size() == 2) {
                Type ok_t  = resolve_type_string(parts[0]);
                Type err_t = resolve_type_string(parts[1]);
                return Type::make_result(std::move(ok_t), std::move(err_t));
            }
        }
    }

    // Phase M5.b L.10: Future<T>.
    {
        const std::string prefix = "Future<";
        if (type_str.size() > prefix.size()
         && type_str.compare(0, prefix.size(), prefix) == 0
         && type_str.back() == '>') {
            std::string inner = type_str.substr(prefix.size(),
                                                  type_str.size() - prefix.size() - 1);
            Type elt = resolve_type_string(inner);
            return Type::make_future(std::move(elt));
        }
    }

    // Phase M5.b L.10: smart pointers `unique<T>` y `shared<T>`.
    {
        const std::string p_uniq = "unique<";
        if (type_str.size() > p_uniq.size()
         && type_str.compare(0, p_uniq.size(), p_uniq) == 0
         && type_str.back() == '>') {
            std::string inner = type_str.substr(p_uniq.size(),
                                                  type_str.size() - p_uniq.size() - 1);
            return Type::make_unique(resolve_type_string(inner));
        }
        const std::string p_shr = "shared<";
        if (type_str.size() > p_shr.size()
         && type_str.compare(0, p_shr.size(), p_shr) == 0
         && type_str.back() == '>') {
            std::string inner = type_str.substr(p_shr.size(),
                                                  type_str.size() - p_shr.size() - 1);
            return Type::make_shared(resolve_type_string(inner));
        }
    }

    // Phase M5.b L.10: function types `fn(T1, T2) -> R`.
    // El typename canonico de type_to_string para FUNCTION es
    // `fn(p1, p2) -> ret`.  Detectamos el patron y parseamos.
    {
        const std::string prefix = "fn(";
        if (type_str.size() > prefix.size()
         && type_str.compare(0, prefix.size(), prefix) == 0) {
            // Buscar el matching ')'.
            size_t close = find_matching_close_(type_str, prefix.size() - 1, '(', ')');
            if (close != std::string::npos) {
                std::string params_str = type_str.substr(prefix.size(),
                                                            close - prefix.size());
                // Tras el `)`, esperamos ` -> RET`.
                std::string after = type_str.substr(close + 1);
                // Trim leading spaces.
                size_t i = 0;
                while (i < after.size() && (after[i] == ' ' || after[i] == '\t')) ++i;
                after = after.substr(i);
                if (after.size() >= 3
                 && after[0] == '-' && after[1] == '>' && after[2] == ' ') {
                    std::string ret_str = after.substr(3);
                    auto pp = split_type_list_(params_str);
                    std::vector<Type> params;
                    params.reserve(pp.size());
                    for (auto &p : pp) {
                        if (!p.empty()) params.push_back(resolve_type_string(p));
                    }
                    Type ret = resolve_type_string(ret_str);
                    return Type::make_function(std::move(params), std::move(ret));
                }
            }
        }
    }

    // Primitivos.
    static const std::unordered_map<std::string, PrimitiveKind> primitives = {
        {"void",   PrimitiveKind::VOID},
        {"bool",   PrimitiveKind::BOOL},
        {"char",   PrimitiveKind::CHAR},
        {"i8",     PrimitiveKind::I8},
        {"i16",    PrimitiveKind::I16},
        {"i32",    PrimitiveKind::I32},
        {"i64",    PrimitiveKind::I64},
        {"u8",     PrimitiveKind::U8},
        {"u16",    PrimitiveKind::U16},
        {"u32",    PrimitiveKind::U32},
        {"u64",    PrimitiveKind::U64},
        {"f32",    PrimitiveKind::F32},
        {"f64",    PrimitiveKind::F64},
        {"string", PrimitiveKind::STRING},
    };
    auto itp = primitives.find(type_str);
    if (itp != primitives.end()) {
        return Type{itp->second};
    }

    // Alias / newtype ya registrado en el checker.
    {
        auto it = type_aliases_.find(type_str);
        if (it != type_aliases_.end()) return it->second;
    }
    // Struct conocido.
    {
        auto it = struct_layouts_.find(type_str);
        if (it != struct_layouts_.end()) {
            return Type{PrimitiveKind::STRUCT, type_str};
        }
    }
    // Class conocido.
    {
        auto it = class_layouts_.find(type_str);
        if (it != class_layouts_.end()) {
            return Type{PrimitiveKind::CLASS, type_str};
        }
    }
    // Enum conocido (se modela como STRUCT con struct_name = enum_name).
    {
        auto it = enum_layouts_.find(type_str);
        if (it != enum_layouts_.end()) {
            return Type{PrimitiveKind::STRUCT, type_str};
        }
    }
    // No se pudo resolver: devolver VOID (sentinel).  El caller decide
    // si emitir error o intentar resolver mas adelante (round-trip
    // cross-modulo cuando se inyectan tipos en orden equivocado).
    return Type{};
}

// Phase M.7: registrar un namespace importado.  Devuelve el indice
// asignado.  El namespace se declara como Symbol::Namespace al inicio
// de run() (via la cola pending_imported_ns_names_).
uint32_t TypeChecker::register_imported_namespace(
        const std::string &local_name,
        const std::string &module_name) {
    // LANG.fix-3: si ya hay un namespace registrado con este local_name,
    // devolver su idx para evitar duplicados (el caller puede llamar dos
    // veces al pre-importar transit + import explicito del mismo dep).
    auto existing = ns_idx_by_local_name_.find(local_name);
    if (existing != ns_idx_by_local_name_.end()) {
        return existing->second;
    }
    const uint32_t idx = static_cast<uint32_t>(imported_namespaces_.size());
    ImportedNamespace ns;
    ns.module_name = module_name;
    imported_namespaces_.push_back(std::move(ns));
    pending_imported_ns_names_.push_back({local_name, idx});
    ns_idx_by_local_name_[local_name] = idx;
    return idx;
}

void TypeChecker::register_namespace_symbol(uint32_t ns_index,
                                              const std::string &public_name,
                                              ImportedNamespace::Sym sym) {
    if (ns_index >= imported_namespaces_.size()) return;
    auto &ns = imported_namespaces_[ns_index];
    const uint32_t sym_idx = static_cast<uint32_t>(ns.symbols.size());
    ns.symbols.push_back(std::move(sym));
    ns.by_name.emplace(public_name, sym_idx);
}

} // namespace vex

namespace vex {

// ---------------------------------------------------------------------------
// export: TypeChecker -> VexiModule.
// ---------------------------------------------------------------------------
void export_typechecker_to_vexi(const TypeChecker &tc,
                                  uint64_t source_hash,
                                  VexiModule &out,
                                  const std::string &strip_prefix) {
    out.source_hash = source_hash;
    out.symbols.clear();
    // Reserva pesimista: cubrir todos los simbolos conocidos en una sola
    // alocacion sin reallocs durante el push_back.
    const size_t cap = tc.type_aliases().size()
                     + tc.struct_layouts().size()
                     + tc.class_layouts().size()
                     + tc.enum_layouts().size()
                     + 32;
    out.symbols.reserve(cap);

    // --- Type aliases + newtypes ---
    for (const auto &kv : tc.type_aliases()) {
        // Phase M6.a L.3: solo exportar si es publico.
        if (!tc.is_typedef_public(kv.first)) continue;
        // Phase M.L23: filtrar imports NO re-exportados.  Solo lo que
        // venga de otro .vexi vive en imported_names_; los locales no.
        if (tc.is_imported(kv.first) && !tc.is_reexported(kv.first)) continue;
        VexiSymbol s;
        const Type &t = kv.second;
        s.name = kv.first;
        if (t.nominal_id != 0) {
            s.kind             = VexiSymbolKind::TYPEDEF_NEW;
            s.is_opaque        = t.is_opaque;
            s.align_override   = t.align_override;
            s.nominal_abi      = vexi_fnv1a(s.name);
            // El underlying canonico se obtiene del registro de newtypes.
            const Type *u = tc.newtype_underlying(s.name);
            if (u != nullptr) {
                s.underlying_type = canonical_typename_of(*u);
            } else {
                // Fallback defensivo: representar el propio Type sin la
                // marca nominal.
                Type tmp = t;
                tmp.nominal_id     = 0;
                tmp.is_opaque      = false;
                tmp.align_override = 0;
                tmp.nominal_name.clear();
                s.underlying_type  = canonical_typename_of(tmp);
            }
            // Phase M.L8: serializar bloque {explicit from/to T;}.
            // Solo se exportan las conversiones marcadas @c is_public ;
            // las privadas (module-scope del fichero origen) NO viajan
            // cross-module porque su semantica solo aplica intra-modulo.
            const TypeChecker::NewtypeInfo *ni = tc.newtype_info(s.name);
            if (ni != nullptr) {
                for (const auto &conv : ni->from_conversions) {
                    if (!conv.is_public) continue;
                    VexiSymbol::ExplicitConvEntry e;
                    e.type_str  = canonical_typename_of(conv.type);
                    e.is_public = true;
                    s.from_conversions.push_back(std::move(e));
                }
                for (const auto &conv : ni->to_conversions) {
                    if (!conv.is_public) continue;
                    VexiSymbol::ExplicitConvEntry e;
                    e.type_str  = canonical_typename_of(conv.type);
                    e.is_public = true;
                    s.to_conversions.push_back(std::move(e));
                }
            }
        } else {
            s.kind             = VexiSymbolKind::TYPEDEF_ALIAS;
            s.underlying_type  = canonical_typename_of(t);
        }
        out.symbols.push_back(std::move(s));
    }

    // --- Structs ---
    for (const auto &kv : tc.struct_layouts()) {
        const auto &name   = kv.first;
        const auto &layout = kv.second;
        // Phase M6.a L.3: solo exportar publicos.
        if (!layout.is_public) continue;
        // Phase M.L23: filtrar imports NO re-exportados.
        if (tc.is_imported(name) && !tc.is_reexported(name)) continue;
        VexiSymbol s;
        s.kind        = VexiSymbolKind::STRUCT;
        s.name        = name;
        s.size_bytes  = layout.size_bytes;
        s.align_bytes = layout.align_bytes;
        s.fields.reserve(layout.fields.size());
        for (const auto &f : layout.fields) {
            VexiSymbol::FieldInfo fi;
            fi.name       = f.name;
            fi.type_str   = canonical_typename_of(f.type);
            fi.offset     = f.offset;
            fi.size       = f.size;
            fi.bit_offset = f.bit_offset;
            fi.bit_width  = f.bit_width;
            s.fields.push_back(std::move(fi));
        }
        out.symbols.push_back(std::move(s));
    }

    // --- Classes ---
    for (const auto &kv : tc.class_layouts()) {
        const auto &name   = kv.first;
        const auto &layout = kv.second;
        // Phase M6.a L.3: solo exportar publicos.  Tambien skipear las
        // clases runtime-predefined (FatalError etc.) que vienen con la
        // VM y no son parte del modulo del usuario.
        if (!layout.is_public) continue;
        // Phase M.L23: filtrar imports NO re-exportados.
        if (tc.is_imported(name) && !tc.is_reexported(name)) continue;
        if (layout.is_runtime_predefined) continue;
        VexiSymbol s;
        s.kind        = VexiSymbolKind::CLASS;
        s.name        = name;
        s.super_class = layout.super_name;
        s.size_bytes  = layout.size_bytes;
        s.align_bytes = 8;   // las instancias se alinean a 8 (ObjectHeader)
        s.interfaces  = layout.interface_names;
        s.fields.reserve(layout.fields.size());
        for (const auto &f : layout.fields) {
            VexiSymbol::FieldInfo fi;
            fi.name       = f.name;
            fi.type_str   = canonical_typename_of(f.type);
            fi.offset     = f.offset;
            fi.size       = f.size;
            fi.bit_offset = f.bit_offset;
            fi.bit_width  = f.bit_width;
            s.fields.push_back(std::move(fi));
        }
        // Phase M6.b L.6: emitir methods con firmas + vtable_index +
        // mangled_label para que el consumer pueda emitir CALLVIRT
        // correcto cross-module.
        s.methods.reserve(layout.methods.size());
        for (const auto &m : layout.methods) {
            VexiSymbol::MethodInfo mi;
            mi.name          = m.name;
            mi.return_type   = canonical_typename_of(m.return_type);
            mi.vtable_index  = m.vtable_index;
            mi.flags         = 0;
            if (m.is_static)      mi.flags |= 0x01;
            if (m.is_constructor) mi.flags |= 0x02;
            // Todos los metodos de instancia son virtuales por defecto
            // en Vex (mismo despacho que Java).
            if (!m.is_static)     mi.flags |= 0x04;
            // El label real en el .vel: si la clase fue mangled (deps en
            // compile_vex_project), el method label tiene el mismo
            // prefix.  Aqui usamos `<ClassName>__<MethodName>` que es
            // como el lowering lo emite.
            mi.mangled_label = name + "__" + m.name;
            mi.param_types.reserve(m.param_types.size());
            for (const auto &pt : m.param_types) {
                mi.param_types.push_back(canonical_typename_of(pt));
            }
            s.methods.push_back(std::move(mi));
        }
        out.symbols.push_back(std::move(s));
    }

    // --- Enums ---
    for (const auto &kv : tc.enum_layouts()) {
        const auto &name   = kv.first;
        const auto &layout = kv.second;
        // Phase M6.a L.3: solo exportar publicos.
        if (!layout.is_public) continue;
        // Phase M.L23: filtrar imports NO re-exportados.
        if (tc.is_imported(name) && !tc.is_reexported(name)) continue;
        VexiSymbol s;
        s.kind = VexiSymbolKind::ENUM;
        s.name = name;
        // Preservar size_bytes para que el consumidor pueda allocar
        // slots del enum (8 + 8*max_payload_fields).  Sin esto, el
        // consumer ve size_bytes=0 -> ALLOCA cero -> corrupcion de
        // memoria adjacente.
        s.size_bytes  = layout.size_bytes;
        s.align_bytes = 8;
        s.variants.reserve(layout.variants.size());
        for (const auto &v : layout.variants) {
            VexiSymbol::EnumVariant ev;
            ev.name = v.name;
            ev.tag  = v.tag;
            ev.payload_types.reserve(v.field_types.size());
            for (const auto &ft : v.field_types) {
                ev.payload_types.push_back(canonical_typename_of(ft));
            }
            s.variants.push_back(std::move(ev));
        }
        out.symbols.push_back(std::move(s));
    }

    // --- Funciones ---
    // Si @c strip_prefix esta seteado (e.g. "lib__"), SOLO exportamos las
    // funciones cuyo nombre empieza con ese prefijo (las que pertenecen
    // a este modulo).  El nombre publico se obtiene quitando el prefijo;
    // el mangled_label conserva el nombre interno completo.  Asi el
    // consumidor importa "sumar" pero el lowering emite CALLVM al label
    // "lib__sumar" -- cierra L.4 (colisiones cross-module).
    //
    // Si @c strip_prefix esta vacio, exportamos todas las funciones
    // sin filtrar (caso del root o tests directos).
    for (const auto &kv : tc.function_names()) {
        const std::string &fname = kv.first;
        const FunctionSig *sig = tc.function_sig_by_name(fname);
        if (sig == nullptr) continue;
        // Filtro: si hay prefix, ignorar simbolos que no lo lleven (son
        // builtins como print, malloc, sqrt -- no parte de este modulo).
        // Excepcion (L.23): imports re-exportados (`public import "X" only
        // A, B;`) llegan SIN el prefix del modulo actual pero deben
        // exportarse igual con su nombre publico original.
        std::string public_name;
        std::string mangled_label;
        if (!strip_prefix.empty()) {
            const bool has_prefix = fname.size() > strip_prefix.size()
                && fname.compare(0, strip_prefix.size(), strip_prefix) == 0;
            if (has_prefix) {
                public_name   = fname.substr(strip_prefix.size());
                mangled_label = fname;
            } else if (tc.is_reexported(fname)) {
                // L.23: re-export.  Conservamos el mangled_label original
                // (e.g. `base__base_value`) para que el linker del consumer
                // resuelva al codigo real en el .velb mergeado.
                public_name   = fname;
                const FunctionSig *si = tc.function_sig_by_name(fname);
                if (si && !si->mangled_label.empty()) {
                    mangled_label = si->mangled_label;
                }
            } else {
                continue;
            }
        } else {
            public_name = fname;
            // mangled_label = "" (mismo que name).
        }
        // Phase M6.a L.3: filtrar privadas.  La fn esta registrada con su
        // nombre mangled (post pre-pase de mangling cross-module).  Si el
        // mapa la marca explicitamente como privada, omitir; en cualquier
        // otro caso (entrada con true o no registrada) se exporta.  Esto
        // arregla la fallback al public_name que daba false-positive porque
        // el unmangled name nunca esta en el mapa (default true).
        if (!tc.is_function_public(fname)) {
            continue;
        }
        // Phase M.L23: filtrar imports NO re-exportados.  El check
        // contra public_name (no mangled) porque los imports se
        // registran con su nombre publico al consumer.
        if (tc.is_imported(public_name) && !tc.is_reexported(public_name)) {
            continue;
        }
        VexiSymbol s;
        s.kind          = VexiSymbolKind::FUNCTION;
        s.name          = public_name;
        s.mangled_label = mangled_label;
        s.return_type   = canonical_typename_of(sig->return_type);
        s.is_extern     = !sig->extern_lib.empty();
        s.extern_lib    = sig->extern_lib;
        s.param_types.reserve(sig->param_types.size());
        for (const auto &pt : sig->param_types) {
            s.param_types.push_back(canonical_typename_of(pt));
        }
        s.param_names.assign(sig->param_types.size(), std::string());
        out.symbols.push_back(std::move(s));
    }

    // --- Globals (Phase M.L7) ---
    // Iteramos el AST del modulo en busca de GlobalVarDecl publicas (las
    // privadas + `static_assert` sinteticas se filtran).  El TypeChecker
    // ya valido el tipo y la visibilidad; aqui solo lo serializamos.
    // Comptime const tambien se exportan: el valor se materializa via
    // @c comptime_const_values_ y se inlinea como CONST en el consumer
    // (mismo path que `const` runtime, cero overhead).
    for (const auto &decl : tc.ast_module().decls) {
        if (!decl || decl->kind != ast::NodeKind::GlobalVarDecl) continue;
        const auto *gv = static_cast<const ast::GlobalVarDecl *>(decl.get());
        if (!gv->is_public) continue;
        // El name post-pre-pase puede estar mangled (`lib__MAX_USERS`).
        // Aplicar strip_prefix para obtener el nombre publico tal como
        // el consumidor lo importa.  Igual que funciones.
        std::string public_name = gv->name;
        std::string mangled_label;
        if (!strip_prefix.empty()) {
            if (gv->name.size() > strip_prefix.size()
             && gv->name.compare(0, strip_prefix.size(), strip_prefix) == 0) {
                public_name   = gv->name.substr(strip_prefix.size());
                mangled_label = gv->name;
            } else {
                // No empieza con el prefix -> probablemente builtin o
                // sintetico de otro modulo; saltar.
                continue;
            }
        }
        // Filtrar `__static_assert_N` sinteticas + cualquier identificador
        // que comience con `__` (reservados).
        if (public_name.size() >= 2 && public_name[0] == '_' && public_name[1] == '_') {
            continue;
        }
        VexiSymbol s;
        s.kind            = VexiSymbolKind::GLOBAL_VAR;
        s.name            = public_name;
        s.mangled_label   = mangled_label;
        Type gv_type      = const_cast<TypeChecker &>(tc).resolve_type_node(gv->type.get());
        s.underlying_type = canonical_typename_of(gv_type);
        // Para los comptime const, marcamos @c is_const=true en el .vexi
        // porque el consumer los va a inlinear como CONST (mismo flujo que
        // los const runtime).  El bit @c is_comptime no se persiste; es
        // detalle interno del modulo emisor.
        s.is_const        = gv->is_const || gv->is_comptime;
        // v4: propagar atributos al .vexi para que el consumer/AOT/JIT
        // los respeten al materializar el simbolo.
        if (gv->attr_hot)  s.attr_flags |= 0x01;
        if (gv->attr_cold) s.attr_flags |= 0x02;
        s.attr_align   = gv->attr_align;
        s.attr_section = gv->attr_section;
        // L.7: extraer valor inicial.  Tres caminos:
        //   1) `comptime const X = <expr>` -> consultar el valor ya
        //      evaluado en @c comptime_const_values_ (cubre expresiones
        //      arbitrarias resueltas en compile time).
        //   2) `const X = <int literal>` -> leer literal directo.
        //   3) `const X = -<int literal>` -> leer literal del UnaryExpr Neg.
        if (gv->is_comptime) {
            const auto &cv_map = tc.comptime_const_values();
            auto it = cv_map.find(gv->name);
            if (it != cv_map.end() && !it->second.is_str
             && !it->second.is_array && !it->second.is_struct
             && !it->second.is_type) {
                s.has_init_value = true;
                s.init_value     = static_cast<uint64_t>(it->second.value);
            } else if (it != cv_map.end() && it->second.is_str) {
                // v4: materializar como blob STRING en el pool.
                const std::string &sv = it->second.str_value;
                const uint32_t blob_off = vexi_blob_append(
                    out.blob_pool,
                    VexiBlobKind::STRING,
                    reinterpret_cast<const uint8_t*>(sv.data()), sv.size(),
                    /*element_size=*/1u,
                    /*count=*/static_cast<uint32_t>(sv.size()),
                    /*alignment=*/8u);
                s.has_blob_ref   = true;
                s.blob_offset    = blob_off;
                s.blob_kind_hint = static_cast<uint8_t>(VexiBlobKind::STRING);
            } else if (it != cv_map.end() && it->second.is_array) {
                // v4: materializar como blob ARRAY_PRIM.  Solo i64 elements
                // soportados en v4 (el evaluador comptime guarda los array
                // values como int64_t aunque el tipo declarado sea i32/u32).
                // El consumer puede truncar al tipo declarado al materializar.
                const auto &av = it->second.array_vals;
                std::vector<uint8_t> bytes;
                bytes.resize(av.size() * sizeof(int64_t));
                for (size_t i = 0; i < av.size(); ++i) {
                    int64_t v = av[i] ? av[i]->value : 0;
                    std::memcpy(bytes.data() + i * sizeof(int64_t),
                                 &v, sizeof(int64_t));
                }
                const uint32_t blob_off = vexi_blob_append(
                    out.blob_pool,
                    VexiBlobKind::ARRAY_PRIM,
                    bytes.data(), bytes.size(),
                    /*element_size=*/static_cast<uint32_t>(sizeof(int64_t)),
                    /*count=*/static_cast<uint32_t>(av.size()),
                    /*alignment=*/8u);
                s.has_blob_ref   = true;
                s.blob_offset    = blob_off;
                s.blob_kind_hint = static_cast<uint8_t>(VexiBlobKind::ARRAY_PRIM);
            } else if (it != cv_map.end() && it->second.is_struct) {
                // v4: materializar como blob STRUCT_PRIM (campos primitivos
                // i64).  El consumer reconstruye el struct accediendo a los
                // campos por nombre via lookup.
                const auto &fields = it->second.struct_fields;
                std::vector<uint8_t> bytes;
                // Layout: (u32 name_off + u32 name_len + u64 value) por field.
                // El name_off apunta al string pool del .vexi (que sigue al
                // blob_pool).  Lo resolvemos via el StringPoolBuilder NO
                // accesible aqui directamente.  Para v4 inicial, almacenamos
                // el nombre EN EL BLOB mismo via inline u32 len + bytes.
                // Layout per field: u32 name_len + u32 _pad + u64 value
                //                 + name_bytes (padded a 8).
                for (const auto &kv : fields) {
                    const std::string &name = kv.first;
                    int64_t v = kv.second ? kv.second->value : 0;
                    uint32_t nl = static_cast<uint32_t>(name.size());
                    bytes.insert(bytes.end(),
                                  reinterpret_cast<uint8_t*>(&nl),
                                  reinterpret_cast<uint8_t*>(&nl) + 4);
                    uint32_t pad = 0;
                    bytes.insert(bytes.end(),
                                  reinterpret_cast<uint8_t*>(&pad),
                                  reinterpret_cast<uint8_t*>(&pad) + 4);
                    bytes.insert(bytes.end(),
                                  reinterpret_cast<uint8_t*>(&v),
                                  reinterpret_cast<uint8_t*>(&v) + 8);
                    bytes.insert(bytes.end(),
                                  name.begin(), name.end());
                    while (bytes.size() % 8 != 0) bytes.push_back(0);
                }
                const uint32_t blob_off = vexi_blob_append(
                    out.blob_pool,
                    VexiBlobKind::STRUCT_PRIM,
                    bytes.data(), bytes.size(),
                    /*element_size=*/0u,
                    /*count=*/static_cast<uint32_t>(fields.size()),
                    /*alignment=*/8u);
                s.has_blob_ref   = true;
                s.blob_offset    = blob_off;
                s.blob_kind_hint = static_cast<uint8_t>(VexiBlobKind::STRUCT_PRIM);
            } else {
                // is_type u otros casos no soportados en v4: saltar.
                continue;
            }
        } else if (gv->is_const && gv->init) {
            if (gv->init->kind == ast::NodeKind::IntLitExpr) {
                auto *lit = static_cast<ast::IntLitExpr *>(gv->init.get());
                s.has_init_value = true;
                s.init_value     = static_cast<uint64_t>(lit->value);
            } else if (gv->init->kind == ast::NodeKind::UnaryExpr) {
                auto *u = static_cast<ast::UnaryExpr *>(gv->init.get());
                if (u->op == ast::UnOp::Neg
                 && u->operand
                 && u->operand->kind == ast::NodeKind::IntLitExpr) {
                    auto *lit = static_cast<ast::IntLitExpr *>(u->operand.get());
                    s.has_init_value = true;
                    s.init_value     = static_cast<uint64_t>(
                        -static_cast<int64_t>(lit->value));
                }
            }
        }
        out.symbols.push_back(std::move(s));
    }

    // Phase M.L23: re-export de globals const importadas.  Los globals
    // importados via `public import "lib" only X;` NO viven en el AST
    // del modulo actual (estan en @c imported_global_consts_ del
    // TypeChecker).  Iteramos ese mapa y exportamos los que esten
    // marcados como re-exportados.
    for (const auto &kv : tc.imported_global_consts()) {
        const std::string &name = kv.first;
        if (!tc.is_reexported(name)) continue;
        VexiSymbol s;
        s.kind             = VexiSymbolKind::GLOBAL_VAR;
        s.name             = name;
        s.underlying_type  = canonical_typename_of(kv.second.type);
        s.is_const         = true;     // imported_global_consts solo guarda const
        s.has_init_value   = true;
        s.init_value       = kv.second.value;
        out.symbols.push_back(std::move(s));
    }
}

// ---------------------------------------------------------------------------
// import: VexiModule -> TypeChecker (inyeccion selectiva via only_symbols).
//
// Phase M.2 MVP: solo procesamos los simbolos LISTADOS en only_symbols.
// Para cada uno, buscamos el VexiSymbol con ese nombre en el modulo y
// lo inyectamos en la tabla del TypeChecker que corresponda a su kind.
//
// Si only_symbols esta vacio, no se inyecta nada (los imports plain
// `import "x";` o `import "x" as alias;` requieren namespace support,
// pendiente en M2.x).
// ---------------------------------------------------------------------------
void import_vexi_into_typechecker(
        TypeChecker &tc,
        const VexiModule &mod,
        const std::vector<TypeChecker::VexiOnlyEntry> &only_symbols) {
    if (only_symbols.empty()) return;

    // Mapa name -> indice en mod.symbols para lookup O(1).
    std::unordered_map<std::string, size_t> by_name;
    by_name.reserve(mod.symbols.size() * 2);
    for (size_t i = 0; i < mod.symbols.size(); ++i) {
        by_name.emplace(mod.symbols[i].name, i);
    }

    for (const auto &os : only_symbols) {
        auto it = by_name.find(os.name);
        if (it == by_name.end()) {
            // El simbolo solicitado no existe en el modulo importado.
            // El caller (compiler.cpp) emitira un diagnostico claro.
            // Aqui solo skipeamos para no abortar las demas inyecciones.
            continue;
        }
        const VexiSymbol &s = mod.symbols[it->second];
        const std::string local_name = os.rename.empty() ? os.name : os.rename;

        switch (s.kind) {
            case VexiSymbolKind::TYPEDEF_ALIAS:
            case VexiSymbolKind::TYPEDEF_NEW: {
                Type underlying = tc.resolve_type_string(s.underlying_type);
                if (underlying.kind == PrimitiveKind::VOID
                 && s.underlying_type != "void") {
                    // No se pudo resolver el underlying (e.g. apunta a un
                    // tipo no importado todavia).  Skip silente; M5
                    // anyadira un round adicional de resolucion.
                    continue;
                }
                if (s.kind == VexiSymbolKind::TYPEDEF_NEW) {
                    underlying.nominal_id     = tc.allocate_nominal_id();
                    underlying.nominal_name   = local_name;
                    underlying.is_opaque      = s.is_opaque;
                    underlying.align_override = s.align_override;
                    Type clean = underlying;
                    clean.nominal_id     = 0;
                    clean.nominal_name.clear();
                    clean.is_opaque      = false;
                    clean.align_override = 0;
                    tc.register_imported_newtype(local_name, clean);
                    // Phase M.L8: registrar el bloque {explicit from/to T;}
                    // si el .vexi lo trae.  Las entries no-public ya fueron
                    // filtradas por el lado emit.  Las que llegan aqui son
                    // siempre is_public=true.
                    if (!s.from_conversions.empty()
                     || !s.to_conversions.empty()) {
                        TypeChecker::NewtypeInfo ni;
                        ni.from_conversions.reserve(s.from_conversions.size());
                        for (const auto &c : s.from_conversions) {
                            TypeChecker::ExplicitConv ec;
                            ec.type      = tc.resolve_type_string(c.type_str);
                            ec.is_public = c.is_public;
                            ni.from_conversions.push_back(std::move(ec));
                        }
                        ni.to_conversions.reserve(s.to_conversions.size());
                        for (const auto &c : s.to_conversions) {
                            TypeChecker::ExplicitConv ec;
                            ec.type      = tc.resolve_type_string(c.type_str);
                            ec.is_public = c.is_public;
                            ni.to_conversions.push_back(std::move(ec));
                        }
                        tc.register_imported_newtype_info(local_name,
                                                           std::move(ni));
                    }
                }
                tc.register_imported_type_alias(local_name, std::move(underlying));
                break;
            }
            case VexiSymbolKind::STRUCT: {
                StructLayout L;
                L.name        = local_name;
                L.size_bytes  = s.size_bytes;
                L.align_bytes = s.align_bytes;
                L.fields.reserve(s.fields.size());
                for (const auto &fi : s.fields) {
                    StructFieldInfo sfi;
                    sfi.name       = fi.name;
                    sfi.type       = tc.resolve_type_string(fi.type_str);
                    sfi.offset     = fi.offset;
                    sfi.size       = fi.size;
                    sfi.bit_offset = fi.bit_offset;
                    sfi.bit_width  = fi.bit_width;
                    L.fields.push_back(std::move(sfi));
                }
                tc.register_imported_struct(local_name, std::move(L));
                break;
            }
            case VexiSymbolKind::CLASS: {
                // M2 MVP: las clases inyectadas son layouts solo de
                // estructura (sin vtable funcional, sin metodos
                // dispatcheables).  Util para que el TypeChecker conozca
                // el TIPO de un identificador importado.  La llamada real
                // a sus metodos requiere M5 (link de los .velb).
                // ClassLayout reusa StructFieldInfo para sus fields.
                ClassLayout L;
                L.name         = local_name;
                L.super_name   = s.super_class;
                L.size_bytes   = s.size_bytes;
                L.fields.reserve(s.fields.size());
                for (const auto &fi : s.fields) {
                    StructFieldInfo cfi;
                    cfi.name       = fi.name;
                    cfi.type       = tc.resolve_type_string(fi.type_str);
                    cfi.offset     = fi.offset;
                    cfi.size       = fi.size;
                    cfi.bit_offset = fi.bit_offset;
                    cfi.bit_width  = fi.bit_width;
                    L.fields.push_back(std::move(cfi));
                }
                L.interface_names = s.interfaces;
                // Phase M6.b L.6: inyectar methods con sus firmas.  El
                // lowering de `obj.method(args)` en el consumidor podra
                // usar `vtable_index` para emitir CALLVIRT correcto.
                L.methods.reserve(s.methods.size());
                for (const auto &mi : s.methods) {
                    ClassMethodInfo cmi;
                    cmi.name           = mi.name;
                    cmi.return_type    = tc.resolve_type_string(mi.return_type);
                    cmi.vtable_index   = mi.vtable_index;
                    cmi.is_static      = (mi.flags & 0x01) != 0;
                    cmi.is_constructor = (mi.flags & 0x02) != 0;
                    cmi.defining_class = local_name;
                    cmi.param_types.reserve(mi.param_types.size());
                    for (const auto &pt : mi.param_types) {
                        cmi.param_types.push_back(tc.resolve_type_string(pt));
                    }
                    L.methods.push_back(std::move(cmi));
                }
                tc.register_imported_class(local_name, std::move(L));
                break;
            }
            case VexiSymbolKind::ENUM: {
                EnumLayout L;
                L.name = local_name;
                L.variants.reserve(s.variants.size());
                // Preservar size_bytes del enum (= 8 + 8*max_payload).
                // Sin esto el consumidor allocaria slots cero-byte y
                // sobreescribiria memoria adjacente al construir variantes
                // (`lib.Op.Nop` con size=0 -> el siguiente alloca pisa
                // el tag de Nop).
                L.size_bytes = static_cast<uint32_t>(s.size_bytes);
                for (const auto &v : s.variants) {
                    EnumVariantInfo ev;
                    ev.name = v.name;
                    ev.tag  = v.tag;
                    ev.field_types.reserve(v.payload_types.size());
                    for (const auto &pt : v.payload_types) {
                        ev.field_types.push_back(tc.resolve_type_string(pt));
                    }
                    L.variants.push_back(std::move(ev));
                }
                tc.register_imported_enum(local_name, std::move(L));
                break;
            }
            case VexiSymbolKind::FUNCTION: {
                FunctionSig sig;
                sig.return_type = tc.resolve_type_string(s.return_type);
                sig.param_types.reserve(s.param_types.size());
                for (const auto &pt : s.param_types) {
                    sig.param_types.push_back(tc.resolve_type_string(pt));
                }
                sig.extern_lib    = s.is_extern ? s.extern_lib : std::string();
                // Phase M.5: si el .vexi declara un mangled_label, el
                // lowering del consumidor emitira @c CALLVM a ese label
                // en lugar del nombre publico.  Cierra L.4.
                sig.mangled_label = s.mangled_label;
                tc.register_imported_function(local_name, std::move(sig));
                break;
            }
            case VexiSymbolKind::GLOBAL_VAR: {
                // Phase M.L7: registrar global importada.  El TypeChecker
                // la declarara como Symbol::Variable en el scope global
                // tras el push_scope inicial de run().  Si trae
                // has_init_value, ademas se inline-a en el lowering como
                // CONST literal -- cierra el caso `public const i32 X = N;`.
                Type t = tc.resolve_type_string(s.underlying_type);
                if (t.kind == PrimitiveKind::VOID && s.underlying_type != "void") {
                    continue;  // tipo no resoluble (skip silente)
                }
                tc.register_imported_global(local_name, std::move(t),
                                              s.is_const,
                                              s.has_init_value,
                                              s.init_value);
                break;
            }
        }
    }
}

// =========================================================================
// Variante que devuelve la lista de simbolos solicitados pero NO encontrados
// (o no publicos) en el .vexi.  El caller (compile_vex_project) la usa para
// emitir diagnosticos cross-module precisos (Phase M6.a L.3).
//
// Tras llamar a esta funcion, el TypeChecker queda con las inyecciones de
// los simbolos que SI existian (igual que la variante simple); los faltantes
// solo se devuelven para que el caller decida si emitir error o warning.
// =========================================================================
std::vector<std::string> import_vexi_into_typechecker_with_missing(
        TypeChecker &tc,
        const VexiModule &mod,
        const std::vector<TypeChecker::VexiOnlyEntry> &only_symbols) {
    std::vector<std::string> missing;
    if (only_symbols.empty()) return missing;

    // Mapa name -> indice en mod.symbols para lookup O(1).
    std::unordered_map<std::string, size_t> by_name;
    by_name.reserve(mod.symbols.size() * 2);
    for (size_t i = 0; i < mod.symbols.size(); ++i) {
        by_name.emplace(mod.symbols[i].name, i);
    }

    // Primera pasada: identificar simbolos faltantes.  Solo se serializan al
    // .vexi los simbolos PUBLICOS, asi que un `not found` significa "no
    // existe" O "existe pero es privado".  Para el usuario el mensaje es el
    // mismo en ambos casos (privado == no exportado).
    for (const auto &os : only_symbols) {
        if (by_name.find(os.name) == by_name.end()) {
            missing.push_back(os.name);
        }
    }

    // Delegar la inyeccion real a la variante simple (ya skipea los missing
    // silenciosamente).  De esta manera no duplicamos la logica de switch
    // sobre VexiSymbolKind.
    import_vexi_into_typechecker(tc, mod, only_symbols);

    return missing;
}

// =========================================================================
// Phase M.7: namespace qualified imports.
//
// Cuando `import "lib_a";` (sin `only`) o `import "lib_a" as foo;` se
// procesa, el compiler_project llama @c register_namespace_for_import
// para registrar el modulo como @c Symbol::Namespace en el scope global
// del consumidor.  El consumidor accede a @c lib_a.valor_a / @c foo.valor_a
// como FieldAccessExpr donde la base es un IdentExpr que resuelve a
// Namespace.  El check_field_access del type checker detecta el caso
// y resuelve el simbolo del namespace.
// =========================================================================

void register_namespace_for_import(
        TypeChecker &tc,
        const std::string &local_name,
        const std::string &module_name,
        const VexiModule &mod) {
    const uint32_t ns_idx = tc.register_imported_namespace(local_name, module_name);
    // Phase M7.b: para que `lib.MyClass` funcione como tipo qualified
    // (clase, struct, enum o typedef), necesitamos:
    //   1. Inyectar el layout en class_layouts_ / struct_layouts_ /
    //      enum_layouts_ / type_aliases_ del consumer, pero usando
    //      NOMBRE MANGLED (`<module>__<TypeName>`) para evitar colision
    //      con tipos locales del consumer del mismo nombre.
    //   2. Registrar Sym en el namespace con mangled_label apuntando a
    //      ese mismo mangled name, kind=2 (TypeAlias).
    // El type_checker ya tiene la resolucion (M7.c lineas 1471+):
    // detecta `lib.MyClass`, busca Sym en namespace, usa mangled_label
    // para lookup en layouts.  Solo faltaban estos dos pasos.
    // Helper local para resolver typenames con fallback a la version
    // mangled del modulo.  Si el .vexi exporta `return_type = "Point"` pero
    // el consumer registro el tipo como `lib__Point` (M7.b), debemos probar
    // ambos.  Cubre el bug 2 (struct value return cross-module).
    auto resolve_with_mangled_fallback =
        [&](const std::string &name) -> Type {
            Type t = tc.resolve_type_string(name);
            if (t.kind != PrimitiveKind::VOID || name == "void") return t;
            // Si la resolucion plana fallo, intentar con mangling.
            const std::string mangled = module_name + "__" + name;
            return tc.resolve_type_string(mangled);
        };

    // PASE 1a: pre-registrar el SKELETON de cada tipo (solo nombre + size,
    // sin fields/methods/variants) para que los tipos del mismo modulo
    // dep se vean entre si al resolver fields en el sub-pase 1b.  Ej:
    // `Workspace.active : EditorTab` resuelve correctamente aunque
    // Workspace aparezca antes que EditorTab en mod.symbols.
    //
    // LANG.fix-3: ademas de registrar el layout, marcar el NOMBRE MANGLED
    // como imported (sin re-export) para que cuando el modulo consumer
    // se exporte a su propia .vexi, NO incluya estos tipos importados
    // como simbolos exportados.  Sin esto, una cadena main -> outer ->
    // inner re-exportaba `inner__Bar` desde outer.vexi (double-mangling
    // y type mismatches).
    for (const auto &s : mod.symbols) {
        if (s.kind == VexiSymbolKind::FUNCTION) continue;
        if (s.kind == VexiSymbolKind::GLOBAL_VAR) continue;
        const std::string mangled_pre = module_name + "__" + s.name;
        switch (s.kind) {
            case VexiSymbolKind::STRUCT: {
                StructLayout L;
                L.name        = mangled_pre;
                L.size_bytes  = s.size_bytes;
                L.align_bytes = s.align_bytes;
                tc.register_imported_struct(mangled_pre, std::move(L));
                tc.mark_imported(mangled_pre, /*is_reexport=*/false);
                break;
            }
            case VexiSymbolKind::CLASS: {
                ClassLayout L;
                L.name                   = mangled_pre;
                L.imported_helper_suffix = s.name;
                L.size_bytes             = s.size_bytes;
                tc.register_imported_class(mangled_pre, std::move(L));
                tc.mark_imported(mangled_pre, /*is_reexport=*/false);
                break;
            }
            case VexiSymbolKind::ENUM: {
                EnumLayout L;
                L.name       = mangled_pre;
                L.size_bytes = static_cast<uint32_t>(s.size_bytes);
                tc.register_imported_enum(mangled_pre, std::move(L));
                tc.mark_imported(mangled_pre, /*is_reexport=*/false);
                break;
            }
            default: break;
        }
    }

    // PASE 1b: rellenar fields/methods/variants ahora que TODOS los
    // tipos del modulo dep tienen skeleton registrado.  Las referencias
    // cross-type dentro del mismo dep ya resuelven via mangled fallback.
    for (const auto &s : mod.symbols) {
        if (s.kind == VexiSymbolKind::FUNCTION) continue;
        if (s.kind == VexiSymbolKind::GLOBAL_VAR) continue;
        // M7.b: tipos cross-module via namespace qualified.
        // Compute mangled name = module_name + "__" + s.name.  Asi el
        // tipo no colisiona con un tipo del mismo nombre en el consumer.
        const std::string mangled = module_name + "__" + s.name;
        switch (s.kind) {
            case VexiSymbolKind::TYPEDEF_ALIAS:
            case VexiSymbolKind::TYPEDEF_NEW: {
                Type underlying = tc.resolve_type_string(s.underlying_type);
                if (underlying.kind == PrimitiveKind::VOID
                 && s.underlying_type != "void") {
                    continue;  // forward-ref no resolvible -> skip
                }
                if (s.kind == VexiSymbolKind::TYPEDEF_NEW) {
                    underlying.nominal_id     = tc.allocate_nominal_id();
                    underlying.nominal_name   = mangled;
                    underlying.is_opaque      = s.is_opaque;
                    underlying.align_override = s.align_override;
                    Type clean = underlying;
                    clean.nominal_id     = 0;
                    clean.nominal_name.clear();
                    clean.is_opaque      = false;
                    clean.align_override = 0;
                    tc.register_imported_newtype(mangled, clean);
                }
                tc.register_imported_type_alias(mangled, std::move(underlying));
                // LANG.fix-3: marcar typedef importado para que NO se
                // re-exporte cuando el consumer mismo emita su .vexi.
                tc.mark_imported(mangled, /*is_reexport=*/false);
                break;
            }
            case VexiSymbolKind::STRUCT: {
                StructLayout L;
                L.name        = mangled;
                L.size_bytes  = s.size_bytes;
                L.align_bytes = s.align_bytes;
                L.fields.reserve(s.fields.size());
                for (const auto &fi : s.fields) {
                    StructFieldInfo sfi;
                    sfi.name       = fi.name;
                    // Phase M.fix-classfield: fallback al mangled del dep
                    // para campos con tipo CLASS/STRUCT/ENUM del mismo
                    // modulo dep.  El .vexi guarda type_str unmangled
                    // ("EditorTab") pero el consumer registra mangled
                    // ("tabs__EditorTab"); el fallback lo encuentra.
                    sfi.type       = resolve_with_mangled_fallback(fi.type_str);
                    sfi.offset     = fi.offset;
                    sfi.size       = fi.size;
                    sfi.bit_offset = fi.bit_offset;
                    sfi.bit_width  = fi.bit_width;
                    L.fields.push_back(std::move(sfi));
                }
                tc.register_imported_struct(mangled, std::move(L));
                break;
            }
            case VexiSymbolKind::CLASS: {
                ClassLayout L;
                L.name                   = mangled;
                L.imported_helper_suffix = s.name;   // dep emitio __new_<s.name>
                L.super_name   = s.super_class;
                L.size_bytes   = s.size_bytes;
                L.fields.reserve(s.fields.size());
                for (const auto &fi : s.fields) {
                    StructFieldInfo cfi;
                    cfi.name       = fi.name;
                    cfi.type       = resolve_with_mangled_fallback(fi.type_str);
                    cfi.offset     = fi.offset;
                    cfi.size       = fi.size;
                    cfi.bit_offset = fi.bit_offset;
                    cfi.bit_width  = fi.bit_width;
                    L.fields.push_back(std::move(cfi));
                }
                L.interface_names = s.interfaces;
                L.methods.reserve(s.methods.size());
                for (const auto &mi : s.methods) {
                    ClassMethodInfo cmi;
                    cmi.name           = mi.name;
                    cmi.return_type    = resolve_with_mangled_fallback(mi.return_type);
                    cmi.vtable_index   = mi.vtable_index;
                    cmi.is_static      = (mi.flags & 0x01) != 0;
                    cmi.is_constructor = (mi.flags & 0x02) != 0;
                    cmi.defining_class = mangled;
                    cmi.param_types.reserve(mi.param_types.size());
                    for (const auto &pt : mi.param_types) {
                        cmi.param_types.push_back(resolve_with_mangled_fallback(pt));
                    }
                    L.methods.push_back(std::move(cmi));
                }
                tc.register_imported_class(mangled, std::move(L));
                break;
            }
            case VexiSymbolKind::ENUM: {
                EnumLayout L;
                L.name = mangled;
                L.variants.reserve(s.variants.size());
                L.size_bytes = static_cast<uint32_t>(s.size_bytes);
                for (const auto &v : s.variants) {
                    EnumVariantInfo ev;
                    ev.name = v.name;
                    ev.tag  = v.tag;
                    ev.field_types.reserve(v.payload_types.size());
                    for (const auto &pt : v.payload_types) {
                        ev.field_types.push_back(resolve_with_mangled_fallback(pt));
                    }
                    L.variants.push_back(std::move(ev));
                }
                tc.register_imported_enum(mangled, std::move(L));
                break;
            }
            case VexiSymbolKind::GLOBAL_VAR: {
                // M.L7 ext: globals const cross-module via namespace plain.
                // v4: si el simbolo tiene blob ref (string/array/struct),
                // leemos el blob del pool del .vexi y lo registramos como
                // comptime const string/array/struct importado.
                Type t = tc.resolve_type_string(s.underlying_type);
                if (t.kind == PrimitiveKind::VOID
                 && s.underlying_type != "void") {
                    continue;  // tipo no resolvible -> skip silente
                }
                if (s.is_const && s.has_init_value) {
                    tc.register_imported_global(s.name, t,
                                                  /*is_const=*/true,
                                                  /*has_init_value=*/true,
                                                  s.init_value);
                }
                if (s.is_const && s.has_blob_ref) {
                    // Leer el blob desde el pool del .vexi.
                    const VexiBlobHeader *bh =
                        vexi_blob_read(mod.blob_pool, s.blob_offset);
                    const uint8_t *payload =
                        vexi_blob_payload(mod.blob_pool, s.blob_offset);
                    if (bh && payload
                     && bh->kind == static_cast<uint32_t>(VexiBlobKind::STRING)) {
                        std::string str_val(
                            reinterpret_cast<const char *>(payload),
                            bh->count);
                        tc.register_imported_global_str(s.name, t,
                                                          std::move(str_val));
                    }
                    // Otros kinds (array/struct) requeriran extender
                    // imported_global_consts_ con esos tipos -- pendiente
                    // en proxima sub-fase.  Por ahora, registrar solo el
                    // Sym sin valor para que la resolucion de nombre no
                    // falle silenciosamente.
                }
                TypeChecker::ImportedNamespace::Sym sym;
                sym.kind          = 1;     // Variable / Constant
                sym.mangled_label = s.mangled_label.empty()
                                  ? (module_name + "__" + s.name)
                                  : s.mangled_label;
                sym.var_type      = t;
                sym.has_const_value = s.is_const && s.has_init_value;
                sym.const_value     = static_cast<int64_t>(s.init_value);
                tc.register_namespace_symbol(ns_idx, s.name, std::move(sym));
                continue;  // ya registrado, saltar el push generico
            }
            default:
                continue;
        }
        // Registrar Sym en el namespace con mangled_label apuntando
        // al layout recien inyectado.  El type_checker resuelve
        // `lib.MyClass` via `sym.mangled_label` lookup en layouts.
        TypeChecker::ImportedNamespace::Sym sym;
        sym.kind          = 2;  // TypeAlias
        sym.mangled_label = mangled;
        tc.register_namespace_symbol(ns_idx, s.name, std::move(sym));
    }

    // PASE 2: registrar FUNCTIONS y GLOBAL_VAR (que tambien podrian usar
    // structs como tipo).  Ahora los layouts ya estan en el TypeChecker;
    // el resolver con fallback encontrara "lib__Point" si "Point" no esta.
    for (const auto &s : mod.symbols) {
        if (s.kind == VexiSymbolKind::FUNCTION) {
            TypeChecker::ImportedNamespace::Sym sym;
            sym.kind          = 0;
            sym.mangled_label = s.mangled_label.empty() ? s.name : s.mangled_label;
            sym.sig.return_type    = resolve_with_mangled_fallback(s.return_type);
            sym.sig.param_types.reserve(s.param_types.size());
            for (const auto &pt : s.param_types) {
                sym.sig.param_types.push_back(resolve_with_mangled_fallback(pt));
            }
            sym.sig.extern_lib    = s.is_extern ? s.extern_lib : std::string();
            sym.sig.mangled_label = sym.mangled_label;
            tc.register_namespace_symbol(ns_idx, s.name, std::move(sym));
        } else if (s.kind == VexiSymbolKind::GLOBAL_VAR) {
            // M.L7 ext: globals const cross-module via namespace plain.
            // v4: si tiene blob_ref, leemos el blob string del pool.
            Type t = resolve_with_mangled_fallback(s.underlying_type);
            if (t.kind == PrimitiveKind::VOID && s.underlying_type != "void") {
                continue;
            }
            if (s.is_const && s.has_init_value) {
                tc.register_imported_global(s.name, t,
                                              /*is_const=*/true,
                                              /*has_init_value=*/true,
                                              s.init_value);
            }
            if (s.is_const && s.has_blob_ref) {
                const VexiBlobHeader *bh =
                    vexi_blob_read(mod.blob_pool, s.blob_offset);
                const uint8_t *payload =
                    vexi_blob_payload(mod.blob_pool, s.blob_offset);
                if (bh && payload
                 && bh->kind == static_cast<uint32_t>(VexiBlobKind::STRING)) {
                    std::string str_val(
                        reinterpret_cast<const char *>(payload),
                        bh->count);
                    tc.register_imported_global_str(s.name, t,
                                                      std::move(str_val));
                }
            }
            TypeChecker::ImportedNamespace::Sym sym;
            sym.kind          = 1;
            sym.mangled_label = s.mangled_label.empty()
                              ? (module_name + "__" + s.name)
                              : s.mangled_label;
            sym.var_type      = t;
            sym.has_const_value = s.is_const && s.has_init_value;
            sym.const_value     = static_cast<int64_t>(s.init_value);
            tc.register_namespace_symbol(ns_idx, s.name, std::move(sym));
        }
    }
}

} // namespace vex
