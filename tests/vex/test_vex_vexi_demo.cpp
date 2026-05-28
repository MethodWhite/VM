// Demo: muestra como se ve un fichero .vexi (Phase M.2).
//
// Construye a mano un VexiModule pequeno y representativo (typedef new,
// struct, function) y vuelca:
//   - El "source virtual" que representa.
//   - El layout decodificado.
//   - El hex dump del binario.
//   - El round-trip parse -> verificacion.

#include "vex/vexi_format.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

namespace {

void hex_dump_annotated(const std::vector<uint8_t> &b) {
    static const char *annot[32] = {nullptr};
    annot[0] = "MAGIC + ver/_pad + abi_hash (low 4 bytes)";
    annot[1] = "abi_hash (hi 4) + source_hash";
    annot[2] = "symbol_count(=3) + string_pool_offset";
    annot[3] = "ENTRY[0]=TYPEDEF_NEW user_id";
    annot[4] = "ENTRY[1]=STRUCT Point";
    annot[5] = "ENTRY[2]=FUNCTION doblar + payloads";

    for (size_t i = 0; i < b.size(); i += 16) {
        std::printf("  %04zx  ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < b.size())
                std::printf("%02x ", b[i + j]);
            else
                std::printf("   ");
            if (j == 7) std::printf(" ");
        }
        std::printf(" |");
        for (size_t j = 0; j < 16 && i + j < b.size(); ++j) {
            uint8_t c = b[i + j];
            std::printf("%c", (c >= 32 && c < 127) ? c : '.');
        }
        std::printf("|");
        size_t idx = i / 16;
        if (idx < 32 && annot[idx]) {
            std::printf("  <- %s", annot[idx]);
        }
        std::printf("\n");
    }
}

} // namespace

int main() {
    std::cout << "================================================================\n";
    std::cout << "Modulo Vex (lo que el usuario escribio):\n";
    std::cout << "================================================================\n";
    std::cout <<
        "// buffer.vex\n"
        "public typedef u64 user_id new @opaque {\n"
        "    public explicit from u64;\n"
        "    explicit to u64;\n"
        "}\n"
        "\n"
        "public struct Point { f64 x; f64 y; }\n"
        "\n"
        "public i32 doblar(i32 n) { return n + n; }\n";
    std::cout << "================================================================\n\n";

    // VexiModule equivalente (construido a mano para que la demo sea
    // independiente del TypeChecker y se vea limpia).
    vex::VexiModule m;
    m.source_hash = vex::vexi_fnv1a(std::string("// buffer.vex\n..."));

    {
        vex::VexiSymbol s;
        s.kind            = vex::VexiSymbolKind::TYPEDEF_NEW;
        s.name            = "user_id";
        s.underlying_type = "u64";
        s.is_opaque       = true;
        s.nominal_abi     = vex::vexi_fnv1a(s.name);
        m.symbols.push_back(std::move(s));
    }
    {
        vex::VexiSymbol s;
        s.kind        = vex::VexiSymbolKind::STRUCT;
        s.name        = "Point";
        s.size_bytes  = 16;
        s.align_bytes = 8;
        s.fields = {
            {"x", "f64", 0, 8, 0, 0},
            {"y", "f64", 8, 8, 0, 0},
        };
        m.symbols.push_back(std::move(s));
    }
    {
        vex::VexiSymbol s;
        s.kind         = vex::VexiSymbolKind::FUNCTION;
        s.name         = "doblar";
        s.return_type  = "i32";
        s.param_types  = {"i32"};
        s.param_names  = {"n"};
        m.symbols.push_back(std::move(s));
    }

    auto bytes = vex::vexi_emit(m);
    auto parsed = vex::vexi_parse(bytes.data(), bytes.size());

    std::printf("Total bytes emitidos: %zu\n", bytes.size());
    std::printf("Magic:              'VEXI' (0x49584556 LE)\n");
    std::printf("Format version:     %u\n", parsed.module_.format_version);
    std::printf("ABI hash:           0x%016llx\n",
                (unsigned long long)parsed.module_.abi_hash);
    std::printf("Source hash:        0x%016llx\n",
                (unsigned long long)parsed.module_.source_hash);
    std::printf("Symbol count:       %zu\n\n", parsed.module_.symbols.size());

    std::cout << "Simbolos decodificados:\n";
    std::cout << "----------------------------------------------------------------\n";
    for (const auto &s : parsed.module_.symbols) {
        const char *k = "?";
        switch (s.kind) {
            case vex::VexiSymbolKind::TYPEDEF_ALIAS: k = "TYPEDEF    "; break;
            case vex::VexiSymbolKind::TYPEDEF_NEW:   k = "TYPEDEF_NEW"; break;
            case vex::VexiSymbolKind::STRUCT:        k = "STRUCT     "; break;
            case vex::VexiSymbolKind::CLASS:         k = "CLASS      "; break;
            case vex::VexiSymbolKind::ENUM:          k = "ENUM       "; break;
            case vex::VexiSymbolKind::FUNCTION:      k = "FUNCTION   "; break;
            case vex::VexiSymbolKind::GLOBAL_VAR:    k = "GLOBAL     "; break;
        }
        std::printf("  [%s] %-10s", k, s.name.c_str());
        if (s.kind == vex::VexiSymbolKind::TYPEDEF_NEW
         || s.kind == vex::VexiSymbolKind::TYPEDEF_ALIAS) {
            std::printf(" : %s", s.underlying_type.c_str());
            if (s.is_opaque) std::printf("  @opaque");
            if (s.align_override) std::printf("  @align(%u)", s.align_override);
        } else if (s.kind == vex::VexiSymbolKind::STRUCT
                || s.kind == vex::VexiSymbolKind::CLASS) {
            std::printf(" { ");
            for (size_t i = 0; i < s.fields.size(); ++i) {
                if (i) std::printf(", ");
                std::printf("%s %s", s.fields[i].type_str.c_str(),
                            s.fields[i].name.c_str());
            }
            std::printf(" }  size=%u align=%u", s.size_bytes, s.align_bytes);
        } else if (s.kind == vex::VexiSymbolKind::FUNCTION) {
            std::printf("(");
            for (size_t i = 0; i < s.param_types.size(); ++i) {
                if (i) std::printf(", ");
                std::printf("%s", s.param_types[i].c_str());
                if (i < s.param_names.size() && !s.param_names[i].empty()) {
                    std::printf(" %s", s.param_names[i].c_str());
                }
            }
            std::printf(") -> %s", s.return_type.c_str());
        }
        std::printf("\n");
    }
    std::cout << "----------------------------------------------------------------\n\n";

    std::cout << "Hex dump del fichero .vexi:\n";
    std::cout << "----------------------------------------------------------------\n";
    hex_dump_annotated(bytes);
    std::cout << "----------------------------------------------------------------\n";

    return 0;
}
