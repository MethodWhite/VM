/**
 * @file compiler_project.cpp
 * @brief Compilador multi-modulo (Phase M.2.e).
 *
 * Detecta @c import declaraciones en el fichero raiz, construye el dep
 * graph via @c ModuleGraph, ordena topologicamente, compila cada modulo
 * en orden (con .vexi compartido cross-modulo) y mergea las IrFunctions
 * en un solo @c IrModule final que el emitter produce como un unico
 * @c .vel.  El linker se encarga (M5 futuro) de empaquetar al .velb.
 *
 * Diseno:
 *   - Reusa @c ModuleGraph (M1) para paths + topo + ciclos.
 *   - Reusa @c export_typechecker_to_vexi (M2.d) + @c vexi_emit (M2.c)
 *     para producir las interfaces.
 *   - Reusa @c import_vexi_into_typechecker (M2.d) para inyectar en
 *     consumidores.
 *   - Cada modulo del path se compila con su propio @c TypeChecker y
 *     @c Lowering, produciendo un @c IrModule local.  Al final, todos
 *     los modulos se mergean en uno solo.
 *
 * MVP (M2.e):
 *   - Solo @c "only" imports inyectan simbolos (alias/namespace son M2.x).
 *   - Las @c IrFunctions de cada dep se appendea al IrModule final.
 *   - El @c static_data y los @c globals tambien se merge.
 *   - Sin checks de colision de nombres cross-module todavia (M5).
 */

#include "vex/compiler.h"
#include <unordered_set>

#include "ir/ir_emitter.h"
#include "ir/ir_optimizer.h"
#include "ir/ssa_ir.h"
#include "ir/ssa_ir_serialize.h"
#include "vex/lexer.h"
#include "vex/lowering.h"
#include "vex/module_interop.h"
#include "vex/module_resolver.h"
#include "vex/parser.h"
#include "vex/type_checker.h"
#include "vex/vexi_format.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>

// Phase M5.A: cabeceras para PID + atomic rename portable.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <unistd.h>
#endif

namespace vex {

namespace {

/// Convierte enum int -> ir::OptLevel.  Duplicado de compiler.cpp para
/// no exponer la helper privada.
ir::OptLevel opt_level_from_int_(int n) noexcept {
    switch (n) {
        case 0: return ir::OptLevel::O0;
        case 1: return ir::OptLevel::O1;
        case 2: return ir::OptLevel::O2;
        case 3: return ir::OptLevel::O3;
        default: return ir::OptLevel::O1;
    }
}

/// Verifica si la cache esta deshabilitada via env var.  Por defecto
/// activa (escribe + lee de disco).  El usuario puede setear
/// `VEX_NO_CACHE=1` para forzar rebuild completo (util en CI / debug).
bool vexi_cache_disabled_() noexcept {
    const char *p = std::getenv("VEX_NO_CACHE");
    return p != nullptr && p[0] != 0 && p[0] != '0';
}

/// Escribe @p bytes al fichero @p path (binary).  Devuelve true si OK.
/// Crea el directorio padre si no existe.
bool write_file_(const std::string &path, const std::vector<uint8_t> &bytes) {
    try {
        std::filesystem::create_directories(
            std::filesystem::path(path).parent_path());
    } catch (...) { /* ignorar; el ofstream tambien fallara */ }
    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    if (!bytes.empty()) {
        f.write(reinterpret_cast<const char *>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    }
    return f.good();
}

/// Phase M5.A L.17: escritura atomica de fichero.  Escribe a un .tmp
/// unico (PID + tid + counter para evitar colisiones de procesos
/// concurrentes) y hace rename al destino.  En la mayoria de sistemas
/// (Windows NTFS, Linux ext4/btrfs, macOS APFS) el rename es atomic:
/// el destino o tiene el contenido viejo o el nuevo, nunca un parcial.
/// Esto cierra L.17: dos compilaciones simultaneas del mismo proyecto
/// no corrompen los archivos de cache compartidos (.vexi, .vexir, .velb).
bool write_file_atomic_(const std::string &path,
                         const std::vector<uint8_t> &bytes) {
    namespace fs = std::filesystem;
    static std::atomic<uint64_t> tmp_counter{0};
    try {
        fs::create_directories(fs::path(path).parent_path());
    } catch (...) { /* ignorar */ }
    // Sufijo unico por proceso + thread + counter.  Asi multiples
    // compilaciones concurrentes nunca colisionan en el tmp.
    std::ostringstream suffix;
    suffix << ".tmp."
#ifdef _WIN32
           << static_cast<uint64_t>(GetCurrentProcessId())
#else
           << static_cast<uint64_t>(getpid())
#endif
           << "." << tmp_counter.fetch_add(1, std::memory_order_relaxed);
    const std::string tmp_path = path + suffix.str();
    {
        std::ofstream f(tmp_path, std::ios::binary);
        if (!f.is_open()) return false;
        if (!bytes.empty()) {
            f.write(reinterpret_cast<const char *>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        }
        if (!f.good()) {
            f.close();
            std::error_code ec;
            fs::remove(tmp_path, ec);
            return false;
        }
        f.close();
    }
    // rename atomico (Windows: MoveFileExA con MOVEFILE_REPLACE_EXISTING;
    // POSIX: rename(2)).  std::filesystem::rename hace lo correcto en
    // ambos.  Si el destino existe, lo reemplaza atomicamente.
    std::error_code ec;
    fs::rename(tmp_path, path, ec);
    if (ec) {
        // Fallback: en Windows hay race rara donde MoveFileEx falla con
        // ERROR_ACCESS_DENIED si otro proceso tiene el destino abierto.
        // Intentar copy + delete como segundo recurso.
        fs::copy_file(tmp_path, path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp_path, ec);
        return !ec;
    }
    return true;
}

/// Lee el fichero entero a bytes.  Devuelve true si OK.
bool read_file_bytes_(const std::string &path, std::vector<uint8_t> &out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return false;
    const std::streamsize sz = f.tellg();
    if (sz < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(reinterpret_cast<char *>(out.data()), sz);
    return f.good();
}

/// Calcula el path del .vexi cacheado para un .vex.  Convencion:
/// `path/to/lib.vex` -> `path/to/lib.vexi`.  Mantener el cache junto al
/// source es la ruta mas predecible (vs un .cache/ global): facilita
/// distribucion (publicar la libreria = copiar .vex + .vexi + .ir
/// juntos) y limpieza (borrar la carpeta del modulo lo limpia todo).
/// Phase M.L16: cache global opt-in via @c VEX_CACHE_DIR .  Si la env
/// var esta definida, los caches (@c .vexi / @c .vexir / @c .vel ) se
/// redirigen a @c "$VEX_CACHE_DIR/<hash_64>_<basename><ext>" donde
/// @c hash_64 es FNV-1a 64 del path canonico completo.  Esto permite
/// que multiples proyectos compartan el mismo cache de libs comunes
/// (e.g. stdlib en read-only).  Sin la env var, comportamiento default:
/// cache junto al source (estable + facilita distribucion bundle).
static std::string global_cache_dir_() {
    const char *v = std::getenv("VEX_CACHE_DIR");
    return (v && v[0]) ? std::string(v) : std::string();
}

static std::string global_cache_path_(const std::string &source_path,
                                       const std::string &ext) {
    namespace fs = std::filesystem;
    const std::string dir = global_cache_dir_();
    if (dir.empty()) return std::string();   // no global cache
    // hash 64 del path canonico para que multiples sources con mismo
    // basename no colisionen.
    uint64_t h = 0xcbf29ce484222325ULL;
    for (char c : source_path) {
        h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        h *= 0x100000001b3ULL;
    }
    std::string base = fs::path(source_path).stem().string();
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx",
                    static_cast<unsigned long long>(h));
    return (fs::path(dir) / (std::string(hex) + "_" + base + ext)).string();
}

std::string vexi_path_for_(const std::string &source_path) {
    if (!global_cache_dir_().empty()) {
        return global_cache_path_(source_path, ".vexi");
    }
    size_t dot = source_path.find_last_of('.');
    if (dot == std::string::npos) return source_path + ".vexi";
    return source_path.substr(0, dot) + ".vexi";
}

/// Idem para el cache de IR del dep (.vexir).
std::string vexir_path_for_(const std::string &source_path) {
    if (!global_cache_dir_().empty()) {
        return global_cache_path_(source_path, ".vexir");
    }
    size_t dot = source_path.find_last_of('.');
    if (dot == std::string::npos) return source_path + ".vexir";
    return source_path.substr(0, dot) + ".vexir";
}

/// Phase M5.C: path del @c .vel cacheado per-dep (output secundario
/// junto al @c .vexi para distribucion de libs precompiladas).
std::string dep_vel_path_for_(const std::string &source_path) {
    if (!global_cache_dir_().empty()) {
        return global_cache_path_(source_path, ".vel");
    }
    size_t dot = source_path.find_last_of('.');
    if (dot == std::string::npos) return source_path + ".vel";
    return source_path.substr(0, dot) + ".vel";
}

/// Lee el fichero a string.  Devuelve cadena vacia en error (el caller
/// detecta el error via @c diags).
std::string read_source_(const std::string &path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    const std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::string s;
    s.resize(static_cast<size_t>(sz));
    if (sz > 0) f.read(s.data(), sz);
    return s;
}

/// Estructura de trabajo por modulo durante la compilacion del proyecto.
struct ProjectModuleWork {
    uint32_t                              module_id = 0;
    std::string                           canonical_path;
    std::string                           module_name;
    std::string                           source;
    std::unique_ptr<ast::ModuleNode>      ast;
    std::unique_ptr<TypeChecker>          tc;
    ir::IrModule                          ir;
    VexiModule                            vexi;
    bool                                  ok = false;
    /// Phase M.L20-full: Diagnostics local del modulo.  Cuando se
    /// paraleliza el compile (VEX_PARALLEL_COMPILE=1), cada thread
    /// usa este diags propio en lugar del res.diagnostics compartido,
    /// evitando race conditions.  Post-join se mergean al global.
    Diagnostics                           diags;
};

/// Extrae los ImportDecl del AST en orden de declaracion.  Util para
/// procesar los `only` imports tras tener las VexiModule de los deps.
struct ImportRequest {
    std::string                                       module_name;
    std::string                                       local_name;      // alias o module_name
    std::vector<TypeChecker::VexiOnlyEntry>           only_symbols;
    bool                                              is_plain = false; // sin only -> registra namespace
    bool                                              is_public_reexport = false; // L.23: public import
    SourceLoc                                         loc{};            // posicion del ImportDecl (M6.a.3 diags)
};

/// Phase M.5: renombrar las top-level FunctionDecl y GlobalVarDecl del
/// modulo con un prefijo `<modname>__`.  Esto evita colisiones de
/// nombres cuando dos modulos definen una funcion con el mismo nombre
/// (e.g. ambos `lib_a` y `lib_b` declaran `i32 init();`).
///
/// IMPORTANTE: las llamadas DENTRO del modulo a esas mismas funciones
/// tambien necesitan referenciar el nombre mangled.  Como el lowering
/// resuelve el nombre via @c IdentExpr::name, una segunda pasada
/// recorre las CALL exprs y reescribe sus nombres si encajan con un
/// simbolo renombrado del propio modulo.
///
/// NO renombramos:
///   - Identifiers que comienzan con `__` (estan reservados, p. ej.
///     `__module_init`, `__new_<X>`, `__macro_<X>` que el lowering
///     genera automaticamente).
///   - `main` (entry point unico del programa: solo el root lo define
///     y se invoca via su nombre canonico).
void mangle_top_level_(ast::ModuleNode &mod, const std::string &module_name) {
    const std::string prefix = module_name + "__";

    // Recopilar los nombres a renombrar.
    std::unordered_map<std::string, std::string> rename_map;
    for (auto &decl : mod.decls) {
        if (!decl) continue;
        if (decl->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            if (fd->name.empty()) continue;
            if (fd->name == "main") continue;
            if (fd->name.size() >= 2 && fd->name[0] == '_' && fd->name[1] == '_')
                continue;
            // Mangle: lib::foo -> lib__foo.
            const std::string newn = prefix + fd->name;
            rename_map.emplace(fd->name, newn);
            fd->name = newn;
        } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
            auto *gd = static_cast<ast::GlobalVarDecl *>(decl.get());
            if (gd->name.empty()) continue;
            if (gd->name.size() >= 2 && gd->name[0] == '_' && gd->name[1] == '_')
                continue;
            const std::string newn = prefix + gd->name;
            rename_map.emplace(gd->name, newn);
            gd->name = newn;
        }
    }
    if (rename_map.empty()) return;

    // Walker recursivo que reescribe identificadores en cada Expr.
    std::function<void(ast::Stmt *)> walk_stmt;
    std::function<void(ast::Expr *)> walk_expr;
    walk_expr = [&](ast::Expr *e) {
        if (!e) return;
        switch (e->kind) {
            case ast::NodeKind::IdentExpr: {
                auto *id = static_cast<ast::IdentExpr *>(e);
                auto it = rename_map.find(id->name);
                if (it != rename_map.end()) id->name = it->second;
                break;
            }
            case ast::NodeKind::CallExpr: {
                auto *c = static_cast<ast::CallExpr *>(e);
                walk_expr(c->callee.get());
                for (auto &a : c->args) walk_expr(a.get());
                break;
            }
            case ast::NodeKind::BinaryExpr: {
                auto *b = static_cast<ast::BinaryExpr *>(e);
                walk_expr(b->lhs.get());
                walk_expr(b->rhs.get());
                break;
            }
            case ast::NodeKind::UnaryExpr: {
                auto *u = static_cast<ast::UnaryExpr *>(e);
                walk_expr(u->operand.get());
                break;
            }
            case ast::NodeKind::AssignExpr: {
                auto *a = static_cast<ast::AssignExpr *>(e);
                walk_expr(a->target.get());
                walk_expr(a->value.get());
                break;
            }
            case ast::NodeKind::IndexExpr: {
                auto *ix = static_cast<ast::IndexExpr *>(e);
                walk_expr(ix->base.get());
                walk_expr(ix->index.get());
                break;
            }
            case ast::NodeKind::FieldAccessExpr: {
                auto *fa = static_cast<ast::FieldAccessExpr *>(e);
                walk_expr(fa->base.get());
                break;
            }
            case ast::NodeKind::CastExpr: {
                auto *ce = static_cast<ast::CastExpr *>(e);
                walk_expr(ce->operand.get());
                break;
            }
            case ast::NodeKind::TernaryExpr: {
                auto *tn = static_cast<ast::TernaryExpr *>(e);
                walk_expr(tn->cond.get());
                walk_expr(tn->then_expr.get());
                walk_expr(tn->else_expr.get());
                break;
            }
            case ast::NodeKind::NewExpr: {
                auto *ne = static_cast<ast::NewExpr *>(e);
                for (auto &a : ne->args) walk_expr(a.get());
                break;
            }
            case ast::NodeKind::StringLitExpr: {
                // Interpolaciones `"...${expr}..."`: recorrer cada expr
                // interna.  Sin esto, identificadores referenciados desde
                // dentro de un string interpolado no se manglan cuando el
                // modulo se compila como dep.
                auto *sl = static_cast<ast::StringLitExpr *>(e);
                for (auto &ie : sl->interp_exprs) walk_expr(ie.get());
                break;
            }
            case ast::NodeKind::LambdaExpr: {
                auto *la = static_cast<ast::LambdaExpr *>(e);
                if (la->body) walk_stmt(la->body.get());
                break;
            }
            case ast::NodeKind::MatchExpr: {
                auto *me = static_cast<ast::MatchExpr *>(e);
                walk_expr(me->scrutinee.get());
                for (auto &arm : me->arms) {
                    if (arm.guard) walk_expr(arm.guard.get());
                    if (arm.body)  walk_stmt(arm.body.get());
                }
                break;
            }
            // Otros expr-kinds que pueden contener idents se cubren
            // conforme aparezcan en tests.
            default: break;
        }
    };
    walk_stmt = [&](ast::Stmt *s) {
        if (!s) return;
        switch (s->kind) {
            case ast::NodeKind::BlockStmt: {
                auto *b = static_cast<ast::BlockStmt *>(s);
                for (auto &c : b->body) walk_stmt(c.get());
                break;
            }
            case ast::NodeKind::ExprStmt: {
                auto *es = static_cast<ast::ExprStmt *>(s);
                walk_expr(es->expr.get());
                break;
            }
            case ast::NodeKind::VarDeclStmt: {
                auto *vd = static_cast<ast::VarDeclStmt *>(s);
                if (vd->init) walk_expr(vd->init.get());
                break;
            }
            case ast::NodeKind::IfStmt: {
                auto *ifs = static_cast<ast::IfStmt *>(s);
                walk_expr(ifs->cond.get());
                walk_stmt(ifs->then_branch.get());
                walk_stmt(ifs->else_branch.get());
                break;
            }
            case ast::NodeKind::WhileStmt: {
                auto *w = static_cast<ast::WhileStmt *>(s);
                walk_expr(w->cond.get());
                walk_stmt(w->body.get());
                break;
            }
            case ast::NodeKind::ForStmt: {
                auto *fr = static_cast<ast::ForStmt *>(s);
                walk_stmt(fr->init.get());
                walk_expr(fr->cond.get());
                walk_expr(fr->step.get());
                walk_stmt(fr->body.get());
                break;
            }
            case ast::NodeKind::ReturnStmt: {
                auto *r = static_cast<ast::ReturnStmt *>(s);
                walk_expr(r->value.get());
                break;
            }
            default: break;
        }
    };

    // Aplicar el walker a los bodies de cada FunctionDecl + GlobalVarDecl init,
    // mas los bodies de TODOS los metodos de cada ClassDecl (Bug Fix M.cm).
    // Sin esto, los metodos de clase no ven los simbolos mangled del
    // propio modulo cuando se compila como dep.
    for (auto &decl : mod.decls) {
        if (!decl) continue;
        if (decl->kind == ast::NodeKind::FunctionDecl) {
            auto *fd = static_cast<ast::FunctionDecl *>(decl.get());
            walk_stmt(fd->body.get());
        } else if (decl->kind == ast::NodeKind::GlobalVarDecl) {
            auto *gd = static_cast<ast::GlobalVarDecl *>(decl.get());
            walk_expr(gd->init.get());
        } else if (decl->kind == ast::NodeKind::ClassDecl) {
            auto *cd = static_cast<ast::ClassDecl *>(decl.get());
            for (auto &m : cd->methods) {
                if (m && m->body) walk_stmt(m->body.get());
            }
        }
    }
}

std::vector<ImportRequest> collect_imports_(const ast::ModuleNode &mod) {
    std::vector<ImportRequest> out;
    for (const auto &d : mod.decls) {
        if (!d || d->kind != ast::NodeKind::ImportDecl) continue;
        const auto *im = static_cast<const ast::ImportDecl *>(d.get());
        ImportRequest req;
        // Extraer el nombre del modulo del path (ultimo segmento).
        size_t slash = im->path.find_last_of('/');
        req.module_name = (slash == std::string::npos)
            ? im->path
            : im->path.substr(slash + 1);
        // local_name: alias o module_name si no hay alias.
        req.local_name = im->alias.empty() ? req.module_name : im->alias;
        // Mapear OnlySymbol AST -> TypeChecker::VexiOnlyEntry.
        req.only_symbols.reserve(im->only_symbols.size());
        for (const auto &os : im->only_symbols) {
            req.only_symbols.push_back({os.name, os.rename});
        }
        // Plain import = sin only.  Registra namespace en lugar de inyectar.
        req.is_plain = im->only_symbols.empty();
        req.is_public_reexport = im->is_public_reexport;
        req.loc      = im->loc;
        out.push_back(std::move(req));
    }
    return out;
}

} // namespace

/// Phase M.L20: calcula el nivel topologico de cada modulo.  Nivel 0 =
/// sin imports.  Nivel N = 1 + max(niveles de sus deps).  Modulos del
/// MISMO nivel son independientes entre si (sus interfaces solo
/// dependen de niveles menores), por lo que pueden compilarse en
/// paralelo.  El root siempre tiene el nivel maximo.
std::vector<int> compute_module_levels_(
        const std::vector<ProjectModuleWork> &work,
        const std::unordered_map<std::string, size_t> &by_name) {
    std::vector<int> levels(work.size(), 0);
    // Procesamos en orden topologico (work ya esta en topo).  Para cada
    // modulo, recogemos los imports de su AST + calculamos su nivel
    // como 1 + max(nivel de cada dep).
    for (size_t i = 0; i < work.size(); ++i) {
        const auto &pm = work[i];
        if (!pm.ast) continue;
        int max_dep_level = -1;
        auto imports = collect_imports_(*pm.ast);
        for (const auto &req : imports) {
            auto itd = by_name.find(req.module_name);
            if (itd == by_name.end()) continue;
            if (itd->second >= work.size()) continue;
            if (static_cast<int>(levels[itd->second]) > max_dep_level) {
                max_dep_level = levels[itd->second];
            }
        }
        levels[i] = max_dep_level + 1;  // -1 + 1 = 0 si no hay deps
    }
    return levels;
}

CompileResult compile_vex_project(const std::string &root_path,
                                    const CompileOptions &opts) {
    CompileResult res;

    // 1. Construir el dep graph + topo sort.
    ModuleGraph graph(res.diagnostics);
    // Permitir override del directorio de busqueda via env var VEX_PATH.
    graph.add_vex_path_env();
    // Anyadir como search path implicito la carpeta del modulo root.  Asi
    // los modulos hermanos pueden importarse con paths relativos al root
    // (`import "modules/foo"` desde @c src/modules/bar.vex resuelve a
    // @c src/modules/foo.vex aunque el importer dir sea @c src/modules/).
    // Sin esto, cada modulo tendria que usar paths siblings (`import "foo"`)
    // que cambian segun donde vive el archivo -- frustrante a escala.
    {
        std::string norm = root_path;
        for (char &c : norm) if (c == '\\') c = '/';
        size_t slash = norm.find_last_of('/');
        if (slash != std::string::npos) {
            graph.add_search_path(norm.substr(0, slash));
        }
    }
    const uint32_t root_id = graph.build_from_root(root_path);
    if (root_id == UINT32_MAX || res.diagnostics.has_errors()) {
        res.ok = false;
        return res;
    }
    auto topo = graph.topological_order();
    if (graph.has_cycle()) {
        res.ok = false;
        return res;
    }

    // 2. Mover los AST parseados del graph a estructuras de trabajo.
    std::vector<ProjectModuleWork> work(topo.size());
    std::unordered_map<std::string, size_t> by_name;     // module_name -> idx
    for (size_t i = 0; i < topo.size(); ++i) {
        const uint32_t mid = topo[i];
        const ResolvedModule *rm = graph.module(mid);
        // El ModuleGraph mantiene el AST como unique_ptr.  Necesitamos
        // tomarlo prestado SIN const_cast del puntero, asi que pedimos
        // que el graph nos lo entregue via un getter que mueva el unique_ptr.
        // Como simplificacion, hacemos const_cast aqui (el ModuleGraph
        // no se usa mas despues de este punto).
        ResolvedModule *rm_mut = const_cast<ResolvedModule *>(rm);
        work[i].module_id      = mid;
        work[i].canonical_path = rm_mut->canonical_path;
        work[i].module_name    = rm_mut->module_name;
        work[i].ast            = std::move(rm_mut->parsed_ast);
        // Cargar source de disco para el lexer (necesario para el
        // diagnostics: queremos preservar locs).
        work[i].source = read_source_(rm_mut->canonical_path);
        by_name.emplace(rm_mut->module_name, i);
    }

    // 3. Compilar cada modulo en orden topologico (deps primero).
    //
    // Estrategia con cache (M3+M4):
    //   - El ROOT (work.back()) SIEMPRE se compila (es lo que el usuario
    //     pidio compilar; su .velb es el output del comando).
    //   - Los DEPS comprueban el cache `.vexi` + `.vexir` junto al source:
    //     si ambos existen y source_hash coincide con el del .vexi, se
    //     SKIPEAN el lex+parse+typecheck+lower del dep y se carga el IR
    //     directo desde .vexir.  Speedup esperado: 5-20x en builds
    //     incrementales con cache hit.
    //   - Cache desactivable via env VEX_NO_CACHE=1.
    const bool cache_enabled = !vexi_cache_disabled_();
    const bool verbose_cache = std::getenv("VEX_VERBOSE_CACHE") != nullptr;
    // Phase M.L21: progreso/feedback al usuario durante compile de
    // proyectos grandes.  Activado via @c VEX_VERBOSE_COMPILE=1 .
    // Emit @c [i/N] compiling <name>... al iniciar cada modulo +
    // @c (hit/wrote) al cerrar.  Util para identificar cuellos de
    // botella en build incrementales.
    const bool verbose_compile = []() {
        const char *v = std::getenv("VEX_VERBOSE_COMPILE");
        return v && v[0] == '1';
    }();
    // Phase M.L20: computar niveles topologicos para reporting.  Modulos
    // del MISMO nivel son independientes (pueden paralelizarse).  Esto
    // detecta el potencial paralelismo del proyecto; la ejecucion
    // paralela queda como infraestructura para M8 (ThreadPool dedicado
    // al compiler) -- el refactor del loop a paralelo requiere thread
    // safety review del TypeChecker compartido + file lock cache que
    // M5.A ya cubre via atomic write.
    const std::vector<int> module_levels = compute_module_levels_(work, by_name);
    int max_level = 0;
    for (int L : module_levels) {
        if (L > max_level) max_level = L;
    }
    if (verbose_compile && max_level > 0) {
        // Reporte de paralelismo potencial.
        std::vector<int> per_level_count(max_level + 1, 0);
        for (int L : module_levels) per_level_count[L]++;
        int parallel_opportunities = 0;
        for (int c : per_level_count) {
            if (c > 1) parallel_opportunities += c - 1;
        }
        std::cerr << "[topo] " << work.size() << " modulos en "
                   << (max_level + 1) << " niveles, "
                   << parallel_opportunities << " modulos paralelizables\n";
    }
    // Phase M8: refactor del loop body a lambda para enable dispatch paralelo
    // por nivel topo.  La lambda captura todo el entorno por referencia.
    // Cada thread tiene su propio @c pm = work[i] por diseno (slots distintos
    // del vector, no overlap).  Lectura cross-thread de @c work[dep_idx] es
    // safe porque los deps estan en niveles topologicos ANTERIORES y ya
    // finalizaron antes de que este nivel empiece (barrier por nivel).
    // Mutex para verbose output: en modo paralelo, los @c std::cerr de
    // multiples threads pueden interleaved a nivel de byte (cerr no es
    // line-buffered atomic).  Construimos la linea en un string local y
    // hacemos una sola @c cerr<< con lock.  En modo secuencial, el lock
    // es un no-op virtual (un solo thread nunca contiende).
    std::mutex verbose_mtx;
    auto compile_one_module = [&](size_t i) -> void {
        auto &pm = work[i];
        const bool is_root = (i + 1 == work.size());
        if (verbose_compile) {
            std::ostringstream ln;
            ln << "[L" << module_levels[i] << "][" << (i + 1)
               << "/" << work.size() << "] compiling " << pm.module_name
               << (is_root ? " (root)" : "") << "...\n";
            std::lock_guard<std::mutex> lk(verbose_mtx);
            std::cerr << ln.str();
        }

        const uint64_t source_hash = vexi_fnv1a(pm.source);

        // ---- M4: cache hit path ----
        // Solo se aplica a DEPS, no al root.  Verifica:
        //   1. Existe `<source>.vexi`.
        //   2. .vexi.source_hash == source_hash actual.
        //   3. Existe `<source>.vexir` para reusar el IR.
        // Si los 3 se cumplen, se skipea el recompile del dep.
        if (cache_enabled && !is_root) {
            const std::string vp = vexi_path_for_(pm.canonical_path);
            const std::string ip = vexir_path_for_(pm.canonical_path);
            std::vector<uint8_t> vbytes;
            if (read_file_bytes_(vp, vbytes)) {
                auto pr = vexi_parse(vbytes.data(), vbytes.size());
                if (pr.ok && pr.module_.source_hash == source_hash) {
                    // Phase M4.ext L.13: cache transitivo.  El source_hash
                    // del modulo coincide, pero alguno de sus deps directos
                    // podria haber cambiado.  Verificar que cada DepRecord
                    // del .vexi cacheado matchea el abi_hash actual del dep
                    // (que ya viene populated en work[] por topo order).
                    bool deps_match = true;
                    for (const auto &dep_rec : pr.module_.deps) {
                        auto itd = by_name.find(dep_rec.name);
                        if (itd == by_name.end()) {
                            // El dep ya no existe -> miss.
                            deps_match = false;
                            if (verbose_cache) {
                                std::ostringstream tmp; tmp << "[vex-cache] miss (transitivo): dep '"
                                          << dep_rec.name << "' no encontrado\n";
                                std::lock_guard<std::mutex> lk(verbose_mtx);
                                std::cerr << tmp.str();
                            }
                            break;
                        }
                        const uint64_t actual = work[itd->second].vexi.abi_hash;
                        if (actual != dep_rec.abi_hash) {
                            deps_match = false;
                            if (verbose_cache) {
                                std::ostringstream tmp; tmp << "[vex-cache] miss (transitivo): dep '"
                                          << dep_rec.name
                                          << "' cambio (abi_hash old=0x"
                                          << std::hex << dep_rec.abi_hash
                                          << " new=0x" << actual << std::dec
                                          << ")\n";
                                std::lock_guard<std::mutex> lk(verbose_mtx);
                                std::cerr << tmp.str();
                            }
                            break;
                        }
                    }
                    if (deps_match) {
                        // Hash match -> intentar cargar tambien el .vexir.
                        std::vector<uint8_t> ibytes;
                        if (read_file_bytes_(ip, ibytes) && !ibytes.empty()) {
                            std::vector<ir::IrFunction> fns;
                            if (ir::parse_ir_section(ibytes, 0,
                                                      ibytes.size(), fns)) {
                                pm.vexi = std::move(pr.module_);
                                pm.ir.functions = std::move(fns);
                                pm.ok = true;
                                if (verbose_cache) {
                                    std::ostringstream tmp; tmp << "[vex-cache] hit: "
                                              << pm.canonical_path << "\n";
                                    std::lock_guard<std::mutex> lk(verbose_mtx);
                                    std::cerr << tmp.str();
                                }
                                return; // skip rest of compile (lambda)
                            }
                        }
                    }
                }
            }
            if (verbose_cache) {
                std::ostringstream tmp; tmp << "[vex-cache] miss: " << pm.canonical_path << "\n";
                std::lock_guard<std::mutex> lk(verbose_mtx);
                std::cerr << tmp.str();
            }
        }

        // ---- Compile path (cache miss o root) ----
        // Phase M.5: si es DEP (no root), mangle top-level fns y globals
        // con prefijo `<module>__` para evitar colisiones cross-module.
        // El root NO se mangla (mantiene `main` y demas nombres tal cual).
        if (!is_root) {
            mangle_top_level_(*pm.ast, pm.module_name);
        }

        pm.tc = std::make_unique<TypeChecker>(*pm.ast, pm.diags);

        // ANTES de typecheck: inyectar simbolos de los deps via .vexi.
        // Dos modos:
        //   - `import "x" only A, B;`   -> inyecta A, B directos en scope.
        //   - `import "x" [as alias];`  -> registra namespace para `x.A` o
        //                                   `alias.A` (Phase M.7).
        auto imports = collect_imports_(*pm.ast);

        // LANG.fix-3: pre-importar las .vexi de los deps TRANSITIVOS
        // antes de procesar los imports explicitos.  Si main tiene
        // `import "outer";` + outer depende de inner, main necesita
        // inner.vexi en su TC ANTES de procesar outer (porque outer.vexi
        // referencia tipos como `inner__Bar`).  Sin esto el resolver de
        // fields falla con "void" para tipos del dep transitivo.
        //
        // Estrategia: recorrer la cadena topo de los deps directos y
        // pre-importar cada dep que NO este ya en los imports explicitos.
        // Solo registramos namespaces (no inyectamos al scope) para no
        // contaminar el namespace global del consumer.
        // Recolectar deps TRANSITIVOS (no incluidos en imports explicitos)
        // y pre-importarlos como namespaces silenciosos ANTES del loop de
        // imports explicitos.  Asi cuando outer.vexi se procesa, las
        // referencias a tipos de inner ya estan en class_layouts_ via la
        // pre-importacion de inner.  Cada namespace solo se registra una
        // vez (los explicitos van por el loop normal mas abajo).
        // Recolectar TODOS los deps (directos + transitivos) y pre-importar
        // en orden topo inverso (mas profundo primero).  El register_*
        // usa operator[] (overwrite) y register_imported_namespace ahora
        // dedupea por local_name, asi que registrar el mismo dep dos
        // veces es no-op.
        {
            std::unordered_set<std::string> seen;
            std::vector<std::string> queue;
            for (const auto &req : imports) {
                if (seen.insert(req.module_name).second) {
                    queue.push_back(req.module_name);
                }
            }
            for (size_t qi = 0; qi < queue.size(); ++qi) {
                const std::string &mn = queue[qi];
                auto itd = by_name.find(mn);
                if (itd == by_name.end()) continue;
                const ProjectModuleWork &transit = work[itd->second];
                for (const auto &de : transit.vexi.deps) {
                    if (seen.insert(de.name).second) {
                        queue.push_back(de.name);
                    }
                }
            }
            for (auto it = queue.rbegin(); it != queue.rend(); ++it) {
                const std::string &mn = *it;
                auto itd = by_name.find(mn);
                if (itd == by_name.end()) continue;
                const ProjectModuleWork &transit = work[itd->second];
                register_namespace_for_import(*pm.tc, mn, mn, transit.vexi);
            }
        }

        for (const auto &req : imports) {
            auto itd = by_name.find(req.module_name);
            if (itd == by_name.end()) continue;
            const ProjectModuleWork &dep = work[itd->second];
            if (req.is_plain) {
                // M.7: registrar namespace.
                register_namespace_for_import(*pm.tc, req.local_name,
                                                req.module_name, dep.vexi);
                // M.reexport ext: para `public import "base";` (sin only),
                // inyectar TAMBIEN cada simbolo publico del dep como si
                // fuera un `only A, B, ...` sintetico Y marcarlo
                // re-export.  Sin esto, `mid` con `public import "base"`
                // no reexpone ningun simbolo de base a sus propios
                // consumidores.
                if (req.is_public_reexport) {
                    std::vector<TypeChecker::VexiOnlyEntry> synth_only;
                    for (const auto &sym : dep.vexi.symbols) {
                        // skip simbolos privados o synthetic (mangled).
                        if (sym.name.empty()) continue;
                        if (sym.name[0] == '_') continue;
                        synth_only.push_back({sym.name, ""});
                    }
                    auto missing = import_vexi_into_typechecker_with_missing(
                            *pm.tc, dep.vexi, synth_only);
                    (void)missing;  // best-effort; los privados ya fueron
                                    //              filtrados al construir el .vexi.
                    for (const auto &os : synth_only) {
                        pm.tc->mark_imported(os.name, /*is_reexport=*/true);
                    }
                }
            } else {
                // M2.d: inyeccion directa via only.  M6.a.3: usar la variante
                // que devuelve los missing para emitir diagnostico claro.
                auto missing = import_vexi_into_typechecker_with_missing(
                        *pm.tc, dep.vexi, req.only_symbols);
                for (const auto &m : missing) {
                    std::string msg = "el modulo '";
                    msg += req.module_name;
                    msg += "' no exporta '";
                    msg += m;
                    msg += "' (es privado o no existe)";
                    pm.diags.error(req.loc, std::move(msg));
                }
                if (!missing.empty()) { pm.ok = false; return; }
                // Phase M.L23: marcar cada simbolo importado como
                // (imported, is_reexport).  El export del .vexi del
                // modulo actual filtra los importados NO marcados como
                // public.
                for (const auto &os : req.only_symbols) {
                    const std::string &local =
                        os.rename.empty() ? os.name : os.rename;
                    pm.tc->mark_imported(local, req.is_public_reexport);
                }
            }
        }

        if (!pm.tc->run()) { pm.ok = false; return; }

        // Phase M.L26: warning de imports sin usar.  Tras el check, el
        // TypeChecker tiene un set de nombres referenciados.  Cada
        // `import "lib" only A, B, C;` declara nombres en el scope
        // global; si alguno no aparece en @c referenced_names() , el
        // usuario lo importo pero no lo uso -> warning suave (NO error;
        // imports pueden documentar API surface intencionalmente).
        // Solo aplicable al root (no a deps; sus imports solo importan
        // si el dep luego los reexporta, pero el reexport plain no esta
        // soportado todavia en MVP).  Cualquier modulo con @c warning
        // ya promovido se cubrira en M.reexport.
        if (is_root) {
            const auto &refs = pm.tc->referenced_names();
            for (const auto &req : imports) {
                if (req.is_plain) {
                    // Plain `import "x";` registra @c x como Symbol::Namespace.
                    // Si el namespace alias nunca se accedio, warning.
                    if (refs.find(req.local_name) == refs.end()) {
                        std::string msg = "import '";
                        msg += req.module_name;
                        if (!req.local_name.empty()
                         && req.local_name != req.module_name) {
                            msg += "' as '";
                            msg += req.local_name;
                        }
                        msg += "' no se usa";
                        pm.diags.warning(req.loc, std::move(msg));
                    }
                } else {
                    // `only A, B`: chequear cada A, B individualmente.
                    for (const auto &os : req.only_symbols) {
                        const std::string &local =
                            os.rename.empty() ? os.name : os.rename;
                        if (refs.find(local) == refs.end()) {
                            std::string msg = "simbolo importado '";
                            msg += local;
                            msg += "' de '";
                            msg += req.module_name;
                            msg += "' no se usa";
                            pm.diags.warning(req.loc, std::move(msg));
                        }
                    }
                }
            }
        }

        Lowering lo(*pm.ast, *pm.tc, pm.diags);
        if (!opts.instrument_mode.empty() && opts.instrument_mode != "none") {
            lo.set_instrument_mode(opts.instrument_mode);
        }
        const std::string mod_name = pm.module_name;
        if (!lo.run(pm.ir, mod_name)) { pm.ok = false; return; }

        // Phase M.5: export con strip_prefix = `<module>__` para que el
        // .vexi exponga nombres publicos sin el mangle.  El consumidor
        // importa "sumar" pero la FunctionSig lleva mangled_label="lib__sumar".
        const std::string strip_prefix = is_root
            ? std::string()           // root: sin prefix (no se exporta)
            : (pm.module_name + "__");
        export_typechecker_to_vexi(*pm.tc, source_hash, pm.vexi, strip_prefix);

        // Phase M4.ext L.13: poblar dep table con los (name, abi_hash) de
        // los deps directos del modulo.  El loader del cache verifica
        // estos hashes al cache hit para invalidacion transitiva: si
        // cualquier dep cambio su .vexi (distinto abi_hash), este modulo
        // tambien debe recompilarse.
        pm.vexi.deps.clear();
        for (const auto &req : imports) {
            auto itd = by_name.find(req.module_name);
            if (itd == by_name.end()) continue;
            const ProjectModuleWork &dep = work[itd->second];
            VexiModule::DepRecord drec;
            drec.name     = req.module_name;
            drec.abi_hash = dep.vexi.abi_hash;
            pm.vexi.deps.push_back(std::move(drec));
        }

        // ---- M3: persistir .vexi + .vexir a disco para futuro cache ----
        if (cache_enabled && !is_root) {
            const std::string vp = vexi_path_for_(pm.canonical_path);
            const std::string ip = vexir_path_for_(pm.canonical_path);
            auto vbytes = vexi_emit(pm.vexi);
            // Phase M4.ext L.13: capturar el abi_hash recien calculado
            // por vexi_emit (lo escribio en offset 8 del header) para
            // que los modulos sucesores en topo order que importen este
            // puedan apuntarlo en su dep table.
            if (vbytes.size() >= 16) {
                uint64_t h = 0;
                for (int i = 0; i < 8; ++i) {
                    h |= static_cast<uint64_t>(vbytes[8 + i]) << (i * 8);
                }
                pm.vexi.abi_hash = h;
            }
            // Phase M5.A L.17: escritura atomica (rename temp file).
            (void)write_file_atomic_(vp, vbytes);
            auto ibytes = ir::emit_ir_section(pm.ir.functions);
            (void)write_file_atomic_(ip, ibytes);
            // Phase M5.C L.18: ademas del .vexi (interfaz) + .vexir (IR
            // serializado), emitir el .vel del dep solo (sin merge) para
            // que la libreria sea distribuible standalone.  El
            // consumidor puede tomar lib.vex + lib.vexi + lib.vel y
            // armar su .velb directamente con vm --asm-file lib.vel.
            ir::EmitOptions dep_emit_opts;
            dep_emit_opts.opt_level   = opt_level_from_int_(opts.opt_level);
            dep_emit_opts.emit_debug  = opts.emit_debug;
            dep_emit_opts.module_name = pm.module_name;
            ir::EmitResult dep_eres      = ir::ir_emit_module(pm.ir, dep_emit_opts);
            std::string dvel_path = dep_vel_path_for_(pm.canonical_path);
            if (dep_eres.ok) {
                std::vector<uint8_t> velb_bytes(
                    dep_eres.vel_text.begin(), dep_eres.vel_text.end());
                (void)write_file_atomic_(dvel_path, velb_bytes);
            }
            if (verbose_cache) {
                std::ostringstream tmp; tmp << "[vex-cache] wrote: " << vp
                          << " (" << vbytes.size() << " B) + "
                          << ip << " (" << ibytes.size() << " B) + "
                          << dvel_path << " ("
                          << (dep_eres.ok ? dep_eres.vel_text.size() : 0)
                          << " B)\n";
                std::lock_guard<std::mutex> lk(verbose_mtx);
                std::cerr << tmp.str();
            }
        }

        pm.ok = true;
    }; // end of compile_one_module lambda

    // Phase M8: dispatch.  Por defecto secuencial (preserve cache hit
    // determinism y el orden de @c verbose_compile output).  Activado via
    // env @c VEX_PARALLEL_COMPILE=N (N >= 1).  N=1 fuerza secuencial (util
    // para diagnostico).  N >= 2 corre hasta N modulos del MISMO nivel
    // topologico en paralelo via @c std::thread + join al final de cada
    // nivel (barrier natural).  Modulos en niveles distintos NUNCA se
    // solapan: el barrier garantiza que los deps esten finalizados antes
    // de que un consumer empiece.
    int parallel_threads = 0;
    if (const char *p = std::getenv("VEX_PARALLEL_COMPILE")) {
        try { parallel_threads = std::stoi(p); }
        catch (...) { parallel_threads = 0; }
        if (parallel_threads < 0) parallel_threads = 0;
    }
    if (parallel_threads <= 1) {
        // Path secuencial: identico al comportamiento pre-M8.
        for (size_t i = 0; i < work.size(); ++i) {
            compile_one_module(i);
        }
    } else {
        // Path paralelo: agrupar modulos por nivel topologico.
        std::vector<std::vector<size_t>> by_level(max_level + 1);
        for (size_t i = 0; i < module_levels.size(); ++i) {
            by_level[module_levels[i]].push_back(i);
        }
        if (verbose_compile) {
            std::cerr << "[parallel] threads=" << parallel_threads
                       << " niveles=" << (max_level + 1) << "\n";
        }
        for (int L = 0; L <= max_level; ++L) {
            const auto &mods = by_level[L];
            if (mods.empty()) continue;
            // Si el nivel tiene 1 solo modulo, no merece thread.
            if (mods.size() == 1) {
                compile_one_module(mods[0]);
                continue;
            }
            // Particionar @c mods en lotes de tamano @c parallel_threads .
            // Cada lote se ejecuta concurrentemente; los lotes son
            // secuenciales entre si.  Ejemplo: 5 modulos en L0 con
            // threads=2 -> lotes [0,1], [2,3], [4].
            const size_t chunk = static_cast<size_t>(parallel_threads);
            for (size_t base = 0; base < mods.size(); base += chunk) {
                const size_t end_idx = std::min(base + chunk, mods.size());
                std::vector<std::thread> threads;
                threads.reserve(end_idx - base);
                for (size_t k = base; k < end_idx; ++k) {
                    threads.emplace_back([&compile_one_module, idx = mods[k]]() {
                        compile_one_module(idx);
                    });
                }
                // Barrier: esperar todos los threads del lote antes de
                // pasar al siguiente.  Necesario porque modulos del mismo
                // nivel pueden compartir cache writes que deben terminar
                // antes de que el siguiente nivel intente cache hit.
                for (auto &t : threads) t.join();
            }
        }
    }

    // Phase M.L20-full: mergear @c pm.diags al res.diagnostics + abortar
    // si algun modulo fallo.  Hacemos esto post-loop para que las
    // versiones paralelas no compitan por el @c res.diagnostics global.
    // NOTA: @c res.ok default es @c false ; setear early-abort solo si
    // hubo un @c pm.ok==false real, no por inicializacion.
    bool any_pm_failed = false;
    for (auto &pm : work) {
        for (const auto &d : pm.diags.all()) {
            res.diagnostics.emit(d);
        }
        if (!pm.ok) any_pm_failed = true;
    }
    if (any_pm_failed) {
        res.ok = false;
        return res;
    }

    // 4. Merge IR de todos los modulos en uno solo.
    //
    // Estrategia: el modulo ROOT (work.back() en topo order, ya que el
    // root es el ultimo) se usa como destino base.  Los demas modulos
    // contribuyen sus IrFunctions, static_data y globals.
    //
    // Nota: el orden topologico garantiza que el root es el ULTIMO.
    // Lo verificamos defensivamente.
    if (work.empty()) {
        res.ok = false;
        return res;
    }
    ir::IrModule &merged = work.back().ir;

    // Phase M.L25: tree-shaking opt-in via VEX_TREE_SHAKE=1 .  Si el root
    // hace `import "lib" only X, Y` y NINGUNO de X, Y aparece en
    // referenced_names() del root, el dep NO se mergea al .velb final.
    // Reduce el tamano de programas con muchos deps opcionales.
    //
    // SEMANTICA: solo se tree-shakean deps cuyos imports son TODOS `only`
    // sin usar Y el dep NO declara clases (las clases requieren ejecutar
    // __module_init para registrarse en el ClassRegistry runtime; saltar
    // el merge perderia eso).  Plain imports (`import "x";` sin only)
    // jamas se shake-an (son referencia opaca, dificil de demostrar
    // unused).
    const bool tree_shake = []() {
        const char *v = std::getenv("VEX_TREE_SHAKE");
        return v && v[0] == '1';
    }();
    std::unordered_set<size_t> shaken_indices;
    if (tree_shake && work.size() >= 2) {
        const auto &root_pm = work.back();
        const auto &root_refs = root_pm.tc
            ? root_pm.tc->referenced_names()
            : std::unordered_set<std::string>{};
        auto root_imports = collect_imports_(*root_pm.ast);
        for (const auto &req : root_imports) {
            if (req.is_plain) continue;        // namespace -> nunca shake
            if (req.is_public_reexport) continue;  // re-export consume el dep
            auto itd = by_name.find(req.module_name);
            if (itd == by_name.end()) continue;
            // Verificar si el dep declara clases (no shake-able).
            const auto &dep_pm = work[itd->second];
            bool dep_has_classes = false;
            if (dep_pm.ast) {
                for (const auto &d : dep_pm.ast->decls) {
                    if (d && d->kind == ast::NodeKind::ClassDecl) {
                        dep_has_classes = true;
                        break;
                    }
                }
            }
            if (dep_has_classes) continue;
            // Verificar si TODOS los only simbolos estan sin usar.
            bool all_unused = true;
            for (const auto &os : req.only_symbols) {
                const std::string &local = os.rename.empty() ? os.name : os.rename;
                if (root_refs.find(local) != root_refs.end()) {
                    all_unused = false;
                    break;
                }
            }
            if (all_unused && !req.only_symbols.empty()) {
                shaken_indices.insert(itd->second);
                if (verbose_compile) {
                    std::cerr << "[tree-shake] dep '" << req.module_name
                               << "' eliminado (ninguno de "
                               << req.only_symbols.size()
                               << " simbolos importados se usa)\n";
                }
            }
        }
    }

    for (size_t i = 0; i + 1 < work.size(); ++i) {
        if (shaken_indices.count(i)) continue;  // L.25: skip dep no usado
        auto &dep_ir = work[i].ir;
        // BugFix M.sd: remapeo de STR_LIT_ADDR.imm al mergear static_data.
        // Cada modulo usa indices locales 0..N-1 para sus literales.  Al
        // concatenar el static_data del dep tras el del root, los indices
        // del dep deben desplazarse en @c offset = merged.static_data.size()
        // ANTES de mover las funciones.  Sin esto, las STR_LIT_ADDR del dep
        // (`s_0`, `s_1`, ...) apuntan a las strings del root tras el merge
        // y se imprime garbage (cross-module string aliasing).
        // BugFix M.mi: renombrar el `__module_init` del dep a un nombre
        // unico (`__module_init_<modname>`) y NO permitir colision con la
        // del root.  Sin esto, el merge genera multiples labels
        // `__module_init` y solo se ejecuta la primera -- las clases de
        // los deps no se registran y los `new dep.Class()` crashean.  El
        // root encadena llamadas a las del dep via injeccion de CALLs en
        // su propia __module_init (mas abajo).
        const std::string dep_mod_init = "__module_init_" + work[i].module_name;
        for (auto &fn : dep_ir.functions) {
            if (fn.name == "__module_init") {
                fn.name = dep_mod_init;
            }
        }
        const uint64_t sd_offset = static_cast<uint64_t>(merged.static_data.size());
        if (sd_offset != 0) {
            // Helper: en cualquier string que contenga subcadenas
            // `code.s_<N>` (referencias a static_data del dep), reemplaza
            // <N> por <N + sd_offset>.  Cubre RAW_ASM, etiquetas de
            // findclass, cache slots de clase y otros literales s_*.
            auto remap_static_refs = [sd_offset](std::string &s) {
                std::string out;
                out.reserve(s.size());
                size_t i = 0;
                while (i < s.size()) {
                    size_t p = s.find("code.s_", i);
                    if (p == std::string::npos) {
                        out.append(s, i, s.size() - i);
                        break;
                    }
                    out.append(s, i, p - i);
                    out.append("code.s_");
                    p += 7;
                    // Parsear los digitos.
                    size_t j = p;
                    uint64_t num = 0;
                    while (j < s.size()
                        && s[j] >= '0' && s[j] <= '9') {
                        num = num * 10 + static_cast<uint64_t>(s[j] - '0');
                        ++j;
                    }
                    out.append(std::to_string(num + sd_offset));
                    i = j;
                }
                s = std::move(out);
            };
            for (auto &fn : dep_ir.functions) {
                for (auto &bb : fn.blocks) {
                    for (auto &ins : bb.instrs) {
                        if (ins.op == ir::IrOp::STR_LIT_ADDR) {
                            ins.imm += sd_offset;
                            continue;
                        }
                        if (ins.op == ir::IrOp::RAW_ASM) {
                            // El texto del RAW_ASM esta en func_name.
                            if (!ins.func_name.empty()) {
                                remap_static_refs(ins.func_name);
                            }
                        }
                    }
                }
            }
        }
        for (auto &fn : dep_ir.functions) {
            merged.functions.push_back(std::move(fn));
        }
        // M.staticdata-pool: el storage canonico es ahora un pool unico
        // por modulo.  El merge usa @c append_raw_entries para concatenar
        // bytes + reescribir offsets en un solo paso O(total_bytes).
        merged.static_data.append_raw_entries(std::move(dep_ir.static_data));
        // Globals: merge insertando entradas del dep en el mapa del root.
        // Si una entrada ya existe en root, dep gana? No -- dep no
        // sobrescribe (root tiene prioridad).  En la practica los nombres
        // no colisionan en MVP.
        for (auto &gv : dep_ir.globals) {
            merged.globals.emplace(gv.first, gv.second);
        }
        // BugFix M.ni: native_imports cross-module.  Sin este merge,
        // los CALLN emitidos desde un dep (e.g. `vesta_io:vio_print` al
        // usar @c print desde un metodo de clase) generan un simbolo no
        // resuelto en el linker.  La funcion @c register_native_import
        // ya deduplica internamente, asi que llamarla directo es seguro.
        for (auto &ni : dep_ir.native_imports) {
            merged.register_native_import(ni.lib, ni.name);
        }
    }

    // BugFix M.mi: injetar al inicio del @c __module_init del root un
    // CALL a cada @c __module_init_<dep> mergeado.  Sin esto, las
    // clases declaradas en deps nunca se registran (sus defclass viven
    // dentro de su propio @c __module_init , que el main no llama
    // directamente).  Se ejecutan en topo order (deps primero).
    {
        std::vector<std::string> dep_init_names;
        for (size_t i = 0; i + 1 < work.size(); ++i) {
            if (shaken_indices.count(i) != 0) continue;
            // Verifica que existe un @c __module_init_<dep> entre las
            // funciones mergeadas (algunos deps sin clases/globals no
            // tienen uno; saltar silente).
            const std::string nm = "__module_init_" + work[i].module_name;
            for (const auto &fn : merged.functions) {
                if (fn.name == nm) {
                    dep_init_names.push_back(nm);
                    break;
                }
            }
        }
        if (!dep_init_names.empty()) {
            bool found = false;
            for (auto &fn : merged.functions) {
                if (fn.name != "__module_init") continue;
                if (fn.blocks.empty()) continue;
                auto &entry = fn.blocks.front();
                std::vector<ir::IrInstr> head;
                head.reserve(dep_init_names.size());
                for (const auto &dn : dep_init_names) {
                    ir::IrInstr c{};
                    c.op        = ir::IrOp::CALL;
                    c.type      = ir::IrType::I64;
                    c.dst       = ir::IR_NO_VALUE;
                    c.func_name = dn;
                    head.push_back(std::move(c));
                }
                entry.instrs.insert(entry.instrs.begin(),
                                     std::make_move_iterator(head.begin()),
                                     std::make_move_iterator(head.end()));
                found = true;
                break;
            }
            // Si el root no genero su propio __module_init pero hay deps
            // con classes, creamos un stub que solo encadena llamadas.
            if (!found) {
                ir::IrFunction stub;
                stub.name     = "__module_init";
                stub.ret_type = ir::IrType::I64;
                const ir::IrBlockId entry = stub.new_block("entry");
                for (const auto &dn : dep_init_names) {
                    ir::IrInstr c{};
                    c.op        = ir::IrOp::CALL;
                    c.type      = ir::IrType::I64;
                    c.dst       = ir::IR_NO_VALUE;
                    c.func_name = dn;
                    stub.append(entry, std::move(c));
                }
                ir::IrInstr ret{};
                ret.op   = ir::IrOp::RET;
                ret.type = ir::IrType::I64;
                ret.dst  = ir::IR_NO_VALUE;
                stub.append(entry, std::move(ret));
                merged.functions.push_back(std::move(stub));
            }
        }
    }

    // M.staticdata-pool full: dedup de @c static_data por content_hash
    // + remap de STR_LIT_ADDR.imm a los indices unificados.  Indices
    // estables y deterministicos: dos builds del mismo source producen el
    // mismo mapping (JIT cache friendly).  Tambien colapsa bytes
    // duplicados cuando dos modulos importan el mismo string.
    if (merged.static_data.size() >= 2) {
        // Construir mapping old_idx -> new_idx via first-occurrence
        // por content_hash.  Mantener orden estable de aparicion.
        const size_t N = merged.static_data.size();
        std::vector<uint64_t> remap(N);
        std::unordered_map<uint64_t, uint64_t> first_by_hash;
        ir::IrModule::StaticDataStore new_store;
        new_store.alignment_default = merged.static_data.alignment_default;
        new_store.reserve(N);
        for (size_t i = 0; i < N; ++i) {
            auto [bp, bn] = merged.static_data.bytes_at(i);
            auto &m = merged.static_data.meta_at(i);
            // Compute hash si no esta (defensa: push_back ya lo calcula,
            // pero entries movidas del dep podrian tenerlo en 0 si el
            // dep no lo hizo).
            if (m.content_hash == 0 && bn != 0) {
                uint64_t h = 0xcbf29ce484222325ull;
                for (size_t k = 0; k < bn; ++k) {
                    h ^= static_cast<uint64_t>(bp[k]);
                    h *= 0x100000001b3ull;
                }
                m.content_hash = h;
            }
            // Dos entries colapsan SOLO si tienen el mismo hash + bytes
            // identicos (defensa contra colision hash) + mismas flags +
            // mismo alignment.  Diferencias en cualquiera de esos
            // campos preservan ambas entradas.
            //
            // SD_FLAG_NON_DEDUP: globals mutables zero-init colapsarian
            // entre si por bytes identicos {0,0,...,0} -- el flag los
            // marca como "siempre distintos" para preservar storage real.
            const uint64_t hkey = m.content_hash;
            bool merged_in = false;
            const bool is_non_dedup =
                (m.flags & ir::IrModule::SD_FLAG_NON_DEDUP) != 0;
            if (!is_non_dedup) {
                auto it = first_by_hash.find(hkey);
                if (it != first_by_hash.end()) {
                    const uint64_t prev_idx = it->second;
                    if (new_store.equals(prev_idx, bp, bn)
                     && new_store.meta_at(prev_idx).flags == m.flags
                     && new_store.meta_at(prev_idx).alignment == m.alignment) {
                        remap[i] = prev_idx;
                        merged_in = true;
                    }
                }
            }
            if (!merged_in) {
                const uint64_t new_idx = new_store.push_back(bp, bn);
                new_store.meta_at(new_idx) = m;     // copiar flags/section
                first_by_hash.emplace(hkey, new_idx);
                remap[i] = new_idx;
            }
        }
        // Aplicar remap a TODAS las instrucciones STR_LIT_ADDR + a las
        // referencias `code.s_<N>` dentro de RAW_ASM.
        if (new_store.size() < N) {
            for (auto &fn : merged.functions) {
                for (auto &bb : fn.blocks) {
                    for (auto &ins : bb.instrs) {
                        if (ins.op == ir::IrOp::STR_LIT_ADDR) {
                            if (ins.imm < remap.size()) {
                                ins.imm = remap[ins.imm];
                            }
                            continue;
                        }
                        if (ins.op == ir::IrOp::RAW_ASM
                         && !ins.func_name.empty()) {
                            std::string &s = ins.func_name;
                            std::string out;
                            out.reserve(s.size());
                            size_t i = 0;
                            while (i < s.size()) {
                                const size_t p = s.find("code.s_", i);
                                if (p == std::string::npos) {
                                    out.append(s, i, s.size() - i);
                                    break;
                                }
                                out.append(s, i, p - i);
                                out.append("code.s_");
                                size_t j = p + 7;
                                uint64_t num = 0;
                                while (j < s.size()
                                    && s[j] >= '0' && s[j] <= '9') {
                                    num = num * 10
                                        + static_cast<uint64_t>(s[j] - '0');
                                    ++j;
                                }
                                const uint64_t nidx =
                                    (num < remap.size()) ? remap[num] : num;
                                out.append(std::to_string(nidx));
                                i = j;
                            }
                            s = std::move(out);
                        }
                    }
                }
            }
            merged.static_data = std::move(new_store);
        }
    }

    // 5. Optimizar el IR mergeado.
    ir::ir_optimize(merged, opt_level_from_int_(opts.opt_level));

    // 6. Emitir .vel desde el IR mergeado.
    ir::EmitOptions emit_opts;
    emit_opts.opt_level     = opt_level_from_int_(opts.opt_level);
    emit_opts.emit_comments = true;
    emit_opts.emit_debug    = opts.emit_debug;
    emit_opts.module_name   = opts.module_name.empty()
        ? work.back().module_name
        : opts.module_name;
    ir::EmitResult eres = ir::ir_emit_module(merged, emit_opts);
    if (!eres.ok) {
        SourceLoc loc;
        loc.file = root_path;
        res.diagnostics.error(std::move(loc),
            std::string("emisor IR fallo: ") + eres.error);
        res.ok = false;
        return res;
    }
    res.vel_text          = std::move(eres.vel_text);
    res.ir_section_bytes  = ir::emit_ir_section(merged.functions);
    res.ok                = !res.diagnostics.has_errors();

    // Phase M5.B: poblar dep_paths con los paths canonicos de TODOS los
    // modulos compilados (incluido root).  main.cpp persistira el project
    // cache asociando (paths, hashes, .velb final).
    res.dep_paths.reserve(work.size());
    for (const auto &pm : work) {
        res.dep_paths.push_back(pm.canonical_path);
    }
    return res;
}

bool vex_source_has_imports(const std::string &source) {
    // Scanner muy simple: busca `import` en posicion top-level (al
    // inicio de linea o tras whitespace, no dentro de un string ni
    // comentario).  Es heuristica permisiva: si el matcher dice "tiene
    // imports" pero el parser no los encuentra, no pasa nada (cae al
    // path normal).  Si dice "no" cuando si los hay, el path single-file
    // los rechaza con error claro.
    enum { NORMAL, IN_LINE_COMMENT, IN_BLOCK_COMMENT, IN_STRING } state = NORMAL;
    size_t i = 0;
    auto at_word_boundary = [&](size_t k) {
        if (k > 0) {
            char p = source[k - 1];
            if ((p >= 'a' && p <= 'z') || (p >= 'A' && p <= 'Z')
             || (p >= '0' && p <= '9') || p == '_') return false;
        }
        if (k + 6 > source.size()) return false;
        if (source.compare(k, 6, "import") != 0) return false;
        if (k + 6 < source.size()) {
            char n = source[k + 6];
            if ((n >= 'a' && n <= 'z') || (n >= 'A' && n <= 'Z')
             || (n >= '0' && n <= '9') || n == '_') return false;
        }
        return true;
    };
    while (i < source.size()) {
        char c = source[i];
        switch (state) {
            case NORMAL:
                if (c == '/' && i + 1 < source.size() && source[i+1] == '/') {
                    state = IN_LINE_COMMENT;
                    i += 2;
                    continue;
                }
                if (c == '/' && i + 1 < source.size() && source[i+1] == '*') {
                    state = IN_BLOCK_COMMENT;
                    i += 2;
                    continue;
                }
                if (c == '"') {
                    state = IN_STRING;
                    ++i;
                    continue;
                }
                if (at_word_boundary(i)) return true;
                break;
            case IN_LINE_COMMENT:
                if (c == '\n') state = NORMAL;
                break;
            case IN_BLOCK_COMMENT:
                if (c == '*' && i + 1 < source.size() && source[i+1] == '/') {
                    state = NORMAL;
                    i += 2;
                    continue;
                }
                break;
            case IN_STRING:
                if (c == '\\' && i + 1 < source.size()) { i += 2; continue; }
                if (c == '"') state = NORMAL;
                break;
        }
        ++i;
    }
    return false;
}

} // namespace vex
