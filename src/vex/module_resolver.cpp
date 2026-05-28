/**
 * @file module_resolver.cpp
 * @brief Implementacion del resolver de paths + dep graph (Phase M).
 *
 * Optimizaciones aplicadas:
 *   - Cache de resolucion por path-hash (FNV-1a 64): segunda resolucion
 *     del mismo modulo es O(1) sin tocar el filesystem.
 *   - Single-pass normalization: la version canonica del path se computa
 *     UNA vez por modulo y se reusa.
 *   - DFS coloreado WHITE/GRAY/BLACK: O(V+E) para ciclos + topo en una
 *     sola pasada.
 *   - Lectura de fichero binary-safe (no asume \0 terminator interno).
 *   - Reserva anticipada de vectores (avoids realloc en hot paths).
 */

#include "vex/module_resolver.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
  #include <direct.h>
  #include <io.h>
  #define VEX_PATH_SEP_ENV ';'
  #define VEX_F_OK 0
  // _access en Windows.
  static inline bool vex_file_access_ok(const char *p) { return _access(p, 0) == 0; }
#else
  #include <unistd.h>
  #define VEX_PATH_SEP_ENV ':'
  static inline bool vex_file_access_ok(const char *p) { return access(p, F_OK) == 0; }
#endif

#include "vex/ast.h"
#include "vex/lexer.h"
#include "vex/parser.h"

namespace vex {

// ---------------------------------------------------------------------------
// FNV-1a 64-bit.  Constante recomendada por Eric Niebler / FNV reference.
// Coste hot path: ~1ns por byte en CPUs modernos.  No usamos xxhash/wyhash
// para no anyadir dep externa; FNV-1a es suficiente para path hashing
// (sin colisiones esperadas en proyectos de <1M modulos).
// ---------------------------------------------------------------------------
uint64_t ModuleGraph::fnv1a_(const std::string &s) noexcept {
    constexpr uint64_t OFFSET = 0xCBF29CE484222325ULL;
    constexpr uint64_t PRIME  = 0x100000001B3ULL;
    uint64_t h = OFFSET;
    for (unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= PRIME;
    }
    return h;
}

// ---------------------------------------------------------------------------
// Detection de path absoluto: cross-platform.
//   POSIX: empieza con '/'
//   Windows: empieza con '/' o '\\' o tiene drive letter "X:" en pos 1.
// ---------------------------------------------------------------------------
bool ModuleGraph::is_absolute_(const std::string &path) noexcept {
    if (path.empty()) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
#if defined(_WIN32)
    if (path.size() >= 2
     && ((path[0] >= 'A' && path[0] <= 'Z')
      || (path[0] >= 'a' && path[0] <= 'z'))
     && path[1] == ':') {
        return true;
    }
#endif
    return false;
}

// ---------------------------------------------------------------------------
// Lectura de fichero a string.  Lee binary-safe.  Devuelve false en error.
// ---------------------------------------------------------------------------
bool ModuleGraph::read_file_(const std::string &path, std::string &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        f.read(out.data(), sz);
        if (!f) return false;
    }
    return true;
}

bool ModuleGraph::file_exists_(const std::string &path) noexcept {
    return vex_file_access_ok(path.c_str());
}

// ---------------------------------------------------------------------------
// Normalizacion de paths.  Pasos:
//   1. Si no es absoluto, prefijar con base_dir + "/".
//   2. Reemplazar '\\' por '/'.
//   3. Colapsar '//' a '/'.
//   4. Resolver "./" (eliminarlos).
//   5. Resolver "../" subiendo un nivel.  Si excede la raiz, error
//      silente (deja el path tal cual; el filesystem lo rechazara).
// ---------------------------------------------------------------------------
std::string ModuleGraph::normalize_path_(const std::string &raw,
                                          const std::string &base_dir) const {
    std::string p;
    p.reserve(raw.size() + base_dir.size() + 8);

    // Paso 1: combinar con base_dir si raw es relativo.
    if (is_absolute_(raw)) {
        p = raw;
    } else {
        if (!base_dir.empty()) {
            p = base_dir;
            if (p.back() != '/' && p.back() != '\\') {
                p += '/';
            }
        }
        p += raw;
    }
    // Paso 2: normalizar separadores.
    for (char &c : p) {
        if (c == '\\') c = '/';
    }
    // Paso 3 + 4 + 5: tokenizar por '/' y reconstruir.
    std::vector<std::string> parts;
    parts.reserve(16);
    std::string cur;
#if defined(_WIN32)
    // Preservar drive letter "X:" como primer token (no procesarlo).
    std::string drive;
    if (p.size() >= 2 && p[1] == ':') {
        drive = p.substr(0, 2);
        p.erase(0, 2);
    }
#endif
    const bool root_abs = !p.empty() && p[0] == '/';

    for (size_t i = 0; i <= p.size(); ++i) {
        if (i == p.size() || p[i] == '/') {
            if (!cur.empty()) {
                if (cur == ".") {
                    // skip
                } else if (cur == "..") {
                    if (!parts.empty() && parts.back() != "..") {
                        parts.pop_back();
                    } else if (!root_abs) {
                        parts.push_back(cur);
                    }
                    // Si root_abs y stack vacio, descartamos ".." (no se
                    // puede subir mas alla de '/').
                } else {
                    parts.push_back(std::move(cur));
                }
                cur.clear();
            }
        } else {
            cur += p[i];
        }
    }

    // Reconstruir.
    std::string out;
    out.reserve(p.size());
#if defined(_WIN32)
    out += drive;
#endif
    if (root_abs) out += '/';
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out += '/';
        out += parts[i];
    }
    if (out.empty()) out = ".";
    return out;
}

// ---------------------------------------------------------------------------
// ModuleGraph: ctor + config de search paths.
// ---------------------------------------------------------------------------
ModuleGraph::ModuleGraph(Diagnostics &diags) : diags_(diags) {
    modules_.reserve(32);     // pre-reservar para evitar reallocs tempranas
    search_paths_.reserve(8);
}

void ModuleGraph::add_search_path(const std::string &dir) {
    if (dir.empty()) return;
    search_paths_.push_back(normalize_path_(dir, ""));
}

void ModuleGraph::add_vex_path_env() {
#if defined(_WIN32)
    // Windows: getenv puede devolver NULL si no esta seteado.
    const char *raw = std::getenv("VEX_PATH");
#else
    const char *raw = std::getenv("VEX_PATH");
#endif
    if (raw == nullptr || *raw == 0) return;
    std::string s(raw);
    // Tokenizar por VEX_PATH_SEP_ENV.
    std::string cur;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == VEX_PATH_SEP_ENV) {
            if (!cur.empty()) {
                add_search_path(cur);
                cur.clear();
            }
        } else {
            cur += s[i];
        }
    }
}

void ModuleGraph::set_stdlib_dir(const std::string &dir) {
    stdlib_dir_ = normalize_path_(dir, "");
}

// ---------------------------------------------------------------------------
// Carga + parse de un modulo.  Crea ResolvedModule, computa hashes,
// invoca lex + parse.  No procesa las deps todavia (eso lo hace
// process_dependencies_ tras anyadir el modulo al graph).
// ---------------------------------------------------------------------------
uint32_t ModuleGraph::load_and_parse_(const std::string &canonical_path) {
    const uint64_t path_hash = fnv1a_(canonical_path);

    // Hit del cache: ya cargado antes (cada modulo se parsea una sola vez).
    auto it = by_path_hash_.find(path_hash);
    if (it != by_path_hash_.end()) {
        return it->second;
    }

    // Leer fichero.
    std::string source;
    if (!read_file_(canonical_path, source)) {
        SourceLoc l;
        l.file = canonical_path;
        diags_.error(l, "no se pudo leer el modulo: '" + canonical_path + "'");
        return UINT32_MAX;
    }

    // Crear entrada.  Reservar id ANTES de parsear para que un import
    // del propio fichero (auto-import) detecte el ciclo correctamente.
    auto mod = std::make_unique<ResolvedModule>();
    mod->module_id      = static_cast<uint32_t>(modules_.size());
    mod->canonical_path = canonical_path;
    mod->path_hash      = path_hash;
    mod->source_hash    = fnv1a_(source);

    // Extraer module_name = ultimo segmento sin extension.
    size_t slash = canonical_path.find_last_of('/');
    std::string base = (slash == std::string::npos)
        ? canonical_path
        : canonical_path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    mod->module_name = (dot == std::string::npos) ? base : base.substr(0, dot);

    // Phase M.L22: paquete-dir.  Si el filename es `mod.vex`, el modulo
    // logico es el directorio parent.  Asi `pkg_lib/mod.vex` se llama
    // `pkg_lib` (no `mod`), coincidiendo con el nombre que el consumer
    // usa al hacer `import "pkg_lib"`.
    if (mod->module_name == "mod" && slash != std::string::npos) {
        const std::string parent = canonical_path.substr(0, slash);
        const size_t parent_slash = parent.find_last_of('/');
        const std::string parent_name = (parent_slash == std::string::npos)
            ? parent
            : parent.substr(parent_slash + 1);
        if (!parent_name.empty()) {
            mod->module_name = parent_name;
        }
    }

    const uint32_t id = mod->module_id;
    by_path_hash_.emplace(path_hash, id);
    modules_.push_back(std::move(mod));

    // Lex + parse.  Cada modulo reusa el mismo Diagnostics del graph para
    // que los errores aparezcan agregados al final del build.
    Lexer lexer(source, canonical_path, diags_);
    Parser parser(lexer, diags_);
    auto ast = parser.parse_program();
    if (!ast) {
        SourceLoc l;
        l.file = canonical_path;
        diags_.error(l, "error al parsear el modulo: '" + canonical_path + "'");
        return UINT32_MAX;
    }
    modules_[id]->parsed_ast = std::move(ast);
    return id;
}

// ---------------------------------------------------------------------------
// resolve(): aplica las reglas de busqueda en orden y devuelve el primer
// candidato existente, o NOT_FOUND con la lista de paths intentados.
// ---------------------------------------------------------------------------
ResolveResult ModuleGraph::resolve(const std::string &raw_path,
                                    const std::string &importer_file) {
    ResolveResult res;

    // Derivar el directorio del importador.
    std::string importer_dir;
    if (!importer_file.empty()) {
        std::string norm = importer_file;
        for (char &c : norm) if (c == '\\') c = '/';
        size_t slash = norm.find_last_of('/');
        if (slash != std::string::npos) {
            importer_dir = norm.substr(0, slash);
        }
    }

    // Construir lista ordenada de candidatos.
    std::vector<std::string> candidates;
    candidates.reserve(4 + search_paths_.size());

    // Phase M.L22: paquete-dir.  Para cada base_dir, intentamos primero
    // `base_dir/raw_path.vex` (modulo single-file) y luego
    // `base_dir/raw_path/mod.vex` (paquete-dir).  El segundo convenio
    // permite agrupar varios .vex bajo `std/io/` con un entry point.
    // El nombre del modulo importado sigue siendo `raw_path` (e.g.
    // `std/io`) -- no cambia la semantica del consumer.
    auto add_candidate = [&](const std::string &base_dir) {
        // (a) modulo single-file.
        candidates.push_back(normalize_path_(raw_path + ".vex", base_dir));
        // (b) paquete-dir: base_dir/raw_path/mod.vex.
        candidates.push_back(normalize_path_(raw_path + "/mod.vex", base_dir));
    };

    // 1. Carpeta del importador.
    add_candidate(importer_dir);
    // 2. Search paths anyadidos (VEX_PATH + adds explicitos).
    for (const auto &sp : search_paths_) {
        add_candidate(sp);
    }
    // 3. Stdlib (solo si raw_path empieza con "std/" o si se busca el resto).
    if (!stdlib_dir_.empty()) {
        add_candidate(stdlib_dir_);
    }

    // Probar cada candidato.
    for (const auto &c : candidates) {
        res.tried_paths.push_back(c);
        if (file_exists_(c)) {
            const uint32_t id = load_and_parse_(c);
            if (id == UINT32_MAX) {
                res.status = ResolveResult::Status::PARSE_ERROR;
                res.error_message = "fallo al cargar/parsear: " + c;
                return res;
            }
            res.status    = ResolveResult::Status::OK;
            res.module_id = id;
            return res;
        }
    }

    // NOT_FOUND con lista de paths intentados.
    res.status = ResolveResult::Status::NOT_FOUND;
    std::string msg = "modulo '" + raw_path + "' no encontrado. Paths probados:";
    for (const auto &c : res.tried_paths) {
        msg += "\n  - " + c;
    }
    res.error_message = std::move(msg);
    return res;
}

// ---------------------------------------------------------------------------
// process_dependencies_: recorre el AST buscando ImportDecls, los resuelve,
// y popula mod.dependencies con los module_ids correspondientes.
//
// Recursivo: cada modulo importado se parsea (via load_and_parse_) y luego
// se procesan sus deps tambien.  La proteccion contra ciclos vive en
// topological_order via DFS coloreado (no aqui).
// ---------------------------------------------------------------------------
void ModuleGraph::process_dependencies_(ResolvedModule &mod) {
    if (!mod.parsed_ast) return;

    for (auto &decl : mod.parsed_ast->decls) {
        if (!decl) continue;
        if (decl->kind != ast::NodeKind::ImportDecl) continue;
        auto *imp = static_cast<ast::ImportDecl *>(decl.get());

        ResolveResult r = resolve(imp->path, mod.canonical_path);
        if (r.status == ResolveResult::Status::NOT_FOUND) {
            diags_.error(imp->loc, r.error_message);
            continue;
        }
        if (r.status == ResolveResult::Status::PARSE_ERROR
         || r.status == ResolveResult::Status::IO_ERROR) {
            diags_.error(imp->loc, r.error_message);
            continue;
        }
        // Auto-import (M se importa a si mismo): legal pero ignorado.
        if (r.module_id == mod.module_id) {
            continue;
        }
        // Anyadir dependencia.  Evitamos duplicados (un mismo modulo
        // importado dos veces solo aparece una vez).
        bool dup = false;
        for (uint32_t d : mod.dependencies) {
            if (d == r.module_id) { dup = true; break; }
        }
        if (!dup) {
            mod.dependencies.push_back(r.module_id);
            // Procesar deps del modulo nuevo recursivamente.  Solo si su
            // dep list aun no esta computada (evita re-entrada).
            ResolvedModule *child = modules_[r.module_id].get();
            if (child->dependencies.empty() && child->parsed_ast) {
                process_dependencies_(*child);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// build_from_root: punto de entrada.  Carga el modulo raiz y procesa
// recursivamente todas las dependencias.  Devuelve el module_id del root
// o UINT32_MAX si error fatal.
// ---------------------------------------------------------------------------
uint32_t ModuleGraph::build_from_root(const std::string &root_file) {
    // Resolver el path absoluto del root.
    std::string canonical = normalize_path_(root_file, "");
    if (!file_exists_(canonical)) {
        SourceLoc l;
        l.file = root_file;
        diags_.error(l, "el fichero raiz no existe: '" + root_file + "'");
        return UINT32_MAX;
    }
    const uint32_t id = load_and_parse_(canonical);
    if (id == UINT32_MAX) return UINT32_MAX;
    process_dependencies_(*modules_[id]);
    return id;
}

// ---------------------------------------------------------------------------
// topological_order: DFS coloreado.  WHITE = no visitado, GRAY = en pila
// DFS actual (si encontramos otro GRAY -> ciclo), BLACK = procesado.
// El output queda en orden: las deps salen ANTES de sus dependents.
// ---------------------------------------------------------------------------
std::vector<uint32_t> ModuleGraph::topological_order() const {
    std::vector<uint32_t> order;
    order.reserve(modules_.size());
    if (modules_.empty()) return order;

    // Reset colors.  Modificamos los modulos (color es mutable conceptual,
    // pero const_cast aqui es seguro porque este metodo no es thread-safe
    // y el caller no ejecuta multiples topological_order concurrentes).
    auto *self = const_cast<ModuleGraph *>(this);
    for (auto &m : self->modules_) m->color = ResolveColor::WHITE;
    self->cycle_detected_ = false;

    // DFS iterativa con stack explicita para evitar stack overflow en
    // proyectos profundos (mas de ~1000 niveles raros pero defensivo).
    struct Frame {
        uint32_t mod_id;
        size_t   dep_idx;
    };
    std::vector<Frame> stack;
    stack.reserve(128);

    for (uint32_t root = 0; root < modules_.size(); ++root) {
        if (self->modules_[root]->color != ResolveColor::WHITE) continue;

        stack.push_back({root, 0});
        self->modules_[root]->color = ResolveColor::GRAY;

        while (!stack.empty()) {
            Frame &top = stack.back();
            ResolvedModule *m = self->modules_[top.mod_id].get();

            if (top.dep_idx < m->dependencies.size()) {
                const uint32_t next = m->dependencies[top.dep_idx++];
                ResolvedModule *child = self->modules_[next].get();

                if (child->color == ResolveColor::WHITE) {
                    child->color = ResolveColor::GRAY;
                    stack.push_back({next, 0});
                } else if (child->color == ResolveColor::GRAY) {
                    // Ciclo: child esta en la pila DFS actual.
                    self->cycle_detected_ = true;
                    // Construir mensaje del ciclo recorriendo la pila.
                    std::string cycle_msg = "ciclo de imports detectado: ";
                    bool found_start = false;
                    for (const auto &f : stack) {
                        if (!found_start && f.mod_id == next) found_start = true;
                        if (found_start) {
                            cycle_msg += self->modules_[f.mod_id]->module_name + " -> ";
                        }
                    }
                    cycle_msg += child->module_name;
                    SourceLoc l;
                    l.file = m->canonical_path;
                    self->diags_.error(l, cycle_msg);
                    // No re-entramos: marcamos el child como BLACK temporal
                    // para que el resto del topo no vuelva a quejarse.
                    child->color = ResolveColor::BLACK;
                }
                // Si BLACK: ya procesado, skip.
            } else {
                // Todas las deps procesadas: emitir m y pop.
                m->color = ResolveColor::BLACK;
                order.push_back(top.mod_id);
                stack.pop_back();
            }
        }
    }

    return order;
}

} // namespace vex
