/**
 * @file test_vex_module_interop.cpp
 * @brief Round-trip TypeChecker -> VexiModule -> serialized -> parsed
 *        -> TypeChecker (Phase M.2.d).
 *
 * Verifica el ciclo completo de compartir simbolos cross-module:
 *
 *  1. Compilar un modulo "lib" via el TypeChecker normal.
 *  2. Exportar su estado a un VexiModule via export_typechecker_to_vexi.
 *  3. Serializarlo a bytes via vexi_emit.
 *  4. Parsear los bytes con vexi_parse.
 *  5. Crear un segundo TypeChecker (modulo "main") e inyectar los
 *     simbolos via import_vexi_into_typechecker con only_symbols.
 *  6. Verificar que el segundo TypeChecker conoce los simbolos
 *     inyectados (resuelve nombres correctamente).
 *
 * Esto demuestra que la cadena M2.a -> M2.b -> M2.c -> M2.d funciona
 * end-to-end SIN aun integrar al pipeline compile_vex_source (eso es
 * M2.e en el siguiente sprint).
 */

#include "vex/module_interop.h"
#include "vex/vexi_format.h"
#include "vex/type_checker.h"
#include "vex/diagnostic.h"
#include "vex/lexer.h"
#include "vex/parser.h"
#include "vex/ast.h"

#include <cstdio>
#include <iostream>
#include <string>

namespace {

int g_pass = 0;
int g_fail = 0;

#define CHECK(cond, label)                                                     \
    do {                                                                       \
        if (cond) {                                                            \
            ++g_pass;                                                          \
            std::cout << "  PASS  " << label << "\n";                          \
        } else {                                                               \
            ++g_fail;                                                          \
            std::cout << "  FAIL  " << label << "  (" << __FILE__ << ":"      \
                      << __LINE__ << ")\n";                                    \
        }                                                                      \
    } while (0)

// Compila una fuente y devuelve un TypeChecker poblado.  Util para
// preparar el "modulo lib" del test.
struct CompiledModule {
    vex::Diagnostics                   diags;
    std::unique_ptr<vex::ast::ModuleNode> ast;
    std::unique_ptr<vex::TypeChecker>  tc;
};

std::unique_ptr<CompiledModule> compile_to_typechecker(const std::string &source,
                                                         const std::string &filename) {
    auto m = std::make_unique<CompiledModule>();
    vex::Lexer lex(source, filename, m->diags);
    vex::Parser parser(lex, m->diags);
    m->ast = parser.parse_program();
    if (!m->ast) return m;
    m->tc = std::make_unique<vex::TypeChecker>(*m->ast, m->diags);
    m->tc->run();
    return m;
}

// ------------------------------------------------------------------
// Test 1: round-trip typedef.
// El lib expone `typedef u64 user_id new;`; main importa con `only`.
// ------------------------------------------------------------------
void test_typedef_roundtrip() {
    std::cout << "\n[Test] round-trip typedef new (lib -> .vexi -> main)\n";

    // 1. Compilar el modulo lib con un typedef new.
    auto lib = compile_to_typechecker(
        "typedef u64 user_id new;\n"
        "typedef u32 port_num new;\n"
        "i32 fn_lib(i32 x) { return x + 1; }\n",
        "lib.vex");

    CHECK(lib->tc != nullptr, "lib compila");
    CHECK(!lib->diags.has_errors(), "lib sin errores");
    if (lib->tc == nullptr) return;

    // 2. Exportar a VexiModule + serializar.
    vex::VexiModule vm;
    vex::export_typechecker_to_vexi(*lib->tc, /*source_hash=*/0xABCD, vm);
    CHECK(!vm.symbols.empty(), "exported tiene simbolos");

    int td_count = 0, fn_count = 0;
    for (const auto &s : vm.symbols) {
        if (s.kind == vex::VexiSymbolKind::TYPEDEF_NEW) ++td_count;
        if (s.kind == vex::VexiSymbolKind::FUNCTION) ++fn_count;
    }
    CHECK(td_count == 2, "2 newtypes exportados (user_id, port_num)");
    CHECK(fn_count >= 1, "fn_lib exportada");

    auto bytes = vex::vexi_emit(vm);
    auto parsed = vex::vexi_parse(bytes.data(), bytes.size());
    CHECK(parsed.ok, "round-trip parse OK");
    if (!parsed.ok) return;

    // 3. Compilar un main vacio + inyectar simbolos.
    auto mainmod = compile_to_typechecker("i32 main() { return 42; }\n", "main.vex");
    CHECK(mainmod->tc != nullptr, "main compila");
    if (mainmod->tc == nullptr) return;

    // 4. Inyectar SOLO user_id y fn_lib (no port_num).
    std::vector<vex::TypeChecker::VexiOnlyEntry> only;
    only.push_back({"user_id", ""});
    only.push_back({"fn_lib",  ""});
    vex::import_vexi_into_typechecker(*mainmod->tc, parsed.module_, only);

    // 5. Verificar que main ahora conoce user_id pero NO port_num.
    {
        const auto &aliases = mainmod->tc->type_aliases();
        bool has_user = aliases.find("user_id") != aliases.end();
        bool has_port = aliases.find("port_num") != aliases.end();
        CHECK(has_user, "main conoce user_id tras import");
        CHECK(!has_port, "main NO conoce port_num (no estaba en only)");
    }
    {
        const auto &fns = mainmod->tc->function_names();
        bool has_fn = fns.find("fn_lib") != fns.end();
        CHECK(has_fn, "main conoce fn_lib tras import");
        const vex::FunctionSig *sig = mainmod->tc->function_sig_by_name("fn_lib");
        CHECK(sig != nullptr, "function_sig_by_name devuelve fn_lib");
        if (sig) {
            CHECK(sig->return_type.kind == vex::PrimitiveKind::I32,
                  "fn_lib retorna i32");
            CHECK(sig->param_types.size() == 1, "fn_lib tiene 1 param");
            if (!sig->param_types.empty()) {
                CHECK(sig->param_types[0].kind == vex::PrimitiveKind::I32,
                      "fn_lib param 0 = i32");
            }
        }
    }
}

// ------------------------------------------------------------------
// Test 2: round-trip struct con campos.
// ------------------------------------------------------------------
void test_struct_roundtrip() {
    std::cout << "\n[Test] round-trip struct (lib -> .vexi -> main)\n";

    auto lib = compile_to_typechecker(
        "struct Point { f64 x; f64 y; }\n"
        "i32 main() { return 0; }\n",
        "lib2.vex");
    CHECK(lib->tc != nullptr, "lib compila");
    if (!lib->tc) return;

    vex::VexiModule vm;
    vex::export_typechecker_to_vexi(*lib->tc, 0x1234, vm);

    auto bytes = vex::vexi_emit(vm);
    auto parsed = vex::vexi_parse(bytes.data(), bytes.size());
    CHECK(parsed.ok, "parse OK");

    auto mainmod = compile_to_typechecker("i32 main() { return 0; }\n", "main2.vex");
    std::vector<vex::TypeChecker::VexiOnlyEntry> only = {{"Point", ""}};
    vex::import_vexi_into_typechecker(*mainmod->tc, parsed.module_, only);

    const auto &structs = mainmod->tc->struct_layouts();
    auto it = structs.find("Point");
    CHECK(it != structs.end(), "main conoce struct Point tras import");
    if (it != structs.end()) {
        CHECK(it->second.fields.size() == 2, "Point tiene 2 fields");
        if (it->second.fields.size() == 2) {
            CHECK(it->second.fields[0].name == "x", "field 0 = x");
            CHECK(it->second.fields[1].name == "y", "field 1 = y");
            CHECK(it->second.fields[0].type.kind == vex::PrimitiveKind::F64,
                  "field 0 type = f64");
        }
    }
}

// ------------------------------------------------------------------
// Test 3: only con rename ("foo as bar").
// ------------------------------------------------------------------
void test_only_rename() {
    std::cout << "\n[Test] only con rename\n";

    auto lib = compile_to_typechecker(
        "typedef u64 fd new;\n"
        "i32 main() { return 0; }\n",
        "lib3.vex");
    if (!lib->tc) return;

    vex::VexiModule vm;
    vex::export_typechecker_to_vexi(*lib->tc, 0, vm);
    auto bytes = vex::vexi_emit(vm);
    auto parsed = vex::vexi_parse(bytes.data(), bytes.size());

    auto mainmod = compile_to_typechecker("i32 main() { return 0; }\n", "main3.vex");
    std::vector<vex::TypeChecker::VexiOnlyEntry> only = {{"fd", "FileDesc"}};
    vex::import_vexi_into_typechecker(*mainmod->tc, parsed.module_, only);

    const auto &aliases = mainmod->tc->type_aliases();
    CHECK(aliases.find("fd") == aliases.end(),
          "el nombre original NO esta (solo el renombrado)");
    CHECK(aliases.find("FileDesc") != aliases.end(),
          "el nombre renombrado SI esta");
}

// ------------------------------------------------------------------
// Test 4: only_symbols vacio = no inyecta nada.
// ------------------------------------------------------------------
void test_empty_only_no_inject() {
    std::cout << "\n[Test] only vacio no inyecta nada (M2 MVP)\n";

    auto lib = compile_to_typechecker(
        "typedef u64 X new;\n"
        "i32 main() { return 0; }\n",
        "lib4.vex");
    if (!lib->tc) return;

    vex::VexiModule vm;
    vex::export_typechecker_to_vexi(*lib->tc, 0, vm);
    auto bytes = vex::vexi_emit(vm);
    auto parsed = vex::vexi_parse(bytes.data(), bytes.size());

    auto mainmod = compile_to_typechecker("i32 main() { return 0; }\n", "main4.vex");
    const size_t before = mainmod->tc->type_aliases().size();

    // only vacio.
    std::vector<vex::TypeChecker::VexiOnlyEntry> only;
    vex::import_vexi_into_typechecker(*mainmod->tc, parsed.module_, only);

    const size_t after = mainmod->tc->type_aliases().size();
    CHECK(before == after, "only vacio no cambio nada");
}

} // namespace

int main() {
    std::cout << "=== test_vex_module_interop: Phase M.2.d ===\n";
    test_typedef_roundtrip();
    test_struct_roundtrip();
    test_only_rename();
    test_empty_only_no_inject();
    std::cout << "\n=== Resultado: " << g_pass << " PASS, "
              << g_fail << " FAIL ===\n";
    return (g_fail == 0) ? 0 : 1;
}
