/**
 * @file toml_lite.cpp
 * @brief Implementacion del mini-parser TOML para vex.toml/vex.lock.
 */
#include "pkg/toml_lite.h"

#include <fstream>
#include <sstream>
#include <cctype>
#include <cstring>

namespace pkg::toml {

    static const TomlValue NULL_VAL;

    const TomlValue &TomlValue::get(const std::string &key) const {
        if (!is_table()) return NULL_VAL;
        auto it = tab_->find(key);
        return (it != tab_->end()) ? it->second : NULL_VAL;
    }

    std::string TomlValue::get_string(const std::string &key,
                                        const std::string &def) const {
        const TomlValue &v = get(key);
        return v.is_string() ? v.as_string() : def;
    }

    int64_t TomlValue::get_int(const std::string &key, int64_t def) const {
        const TomlValue &v = get(key);
        return v.is_int() ? v.as_int() : def;
    }

    bool TomlValue::get_bool(const std::string &key, bool def) const {
        const TomlValue &v = get(key);
        return v.is_bool() ? v.as_bool() : def;
    }

    // ----------------------------------------------------------------
    // Parser
    // ----------------------------------------------------------------

    namespace {

        struct Lexer {
            std::string_view src;
            size_t           pos = 0;
            int              line = 1;
            int              col = 1;
            std::string      err;
            int              err_line = 0;
            int              err_col = 0;

            void error(std::string msg) {
                if (err.empty()) {
                    err = std::move(msg);
                    err_line = line;
                    err_col  = col;
                }
            }
            bool eof() const { return pos >= src.size(); }
            char peek(size_t off = 0) const {
                return (pos + off < src.size()) ? src[pos + off] : '\0';
            }
            char advance() {
                char c = src[pos++];
                if (c == '\n') { ++line; col = 1; } else { ++col; }
                return c;
            }
            void skip_ws_and_comments() {
                while (!eof()) {
                    char c = peek();
                    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                        advance();
                    } else if (c == '#') {
                        while (!eof() && peek() != '\n') advance();
                    } else break;
                }
            }
            void skip_hspace() {
                while (!eof() && (peek() == ' ' || peek() == '\t')) advance();
            }
            void skip_to_eol() {
                while (!eof() && peek() != '\n') advance();
                if (!eof()) advance();
            }
            bool match(char c) {
                if (peek() == c) { advance(); return true; }
                return false;
            }
        };

        // Parsea identificador "bare": letras/digitos/_/-/. (despues split en .)
        // Tambien acepta strings "..." como key (TOML estandar).
        std::string parse_key(Lexer &lx) {
            lx.skip_hspace();
            std::string s;
            if (lx.peek() == '"') {
                // "quoted key"
                lx.advance();
                while (!lx.eof() && lx.peek() != '"') {
                    char c = lx.advance();
                    if (c == '\\' && !lx.eof()) {
                        char e = lx.advance();
                        switch (e) {
                            case 'n': s.push_back('\n'); break;
                            case 't': s.push_back('\t'); break;
                            case 'r': s.push_back('\r'); break;
                            case '"': s.push_back('"');  break;
                            case '\\': s.push_back('\\'); break;
                            default: s.push_back(e);
                        }
                    } else {
                        s.push_back(c);
                    }
                }
                if (lx.peek() == '"') lx.advance();
            } else {
                while (!lx.eof()) {
                    char c = lx.peek();
                    if (std::isalnum(static_cast<unsigned char>(c))
                        || c == '_' || c == '-') {
                        s.push_back(c);
                        lx.advance();
                    } else break;
                }
            }
            if (s.empty()) lx.error("se esperaba un identificador o nombre de seccion");
            return s;
        }

        // Parsea una serie de keys separadas por '.' (e.g. "package.name").
        std::vector<std::string> parse_dotted_key(Lexer &lx) {
            std::vector<std::string> parts;
            parts.push_back(parse_key(lx));
            while (true) {
                lx.skip_hspace();
                if (lx.peek() == '.') {
                    lx.advance();
                    parts.push_back(parse_key(lx));
                } else break;
            }
            return parts;
        }

        TomlValue parse_value(Lexer &lx);

        std::string parse_string(Lexer &lx) {
            // peek == '"'
            lx.advance();
            std::string s;
            while (!lx.eof() && lx.peek() != '"') {
                char c = lx.advance();
                if (c == '\\' && !lx.eof()) {
                    char e = lx.advance();
                    switch (e) {
                        case 'n':  s.push_back('\n'); break;
                        case 't':  s.push_back('\t'); break;
                        case 'r':  s.push_back('\r'); break;
                        case '0':  s.push_back('\0'); break;
                        case '"':  s.push_back('"');  break;
                        case '\\': s.push_back('\\'); break;
                        case '/':  s.push_back('/');  break;
                        case 'b':  s.push_back('\b'); break;
                        case 'f':  s.push_back('\f'); break;
                        case 'x': {
                            if (lx.pos + 1 < lx.src.size()) {
                                auto hex = [](char ch) {
                                    if (ch >= '0' && ch <= '9') return ch - '0';
                                    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
                                    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
                                    return 0;
                                };
                                char h1 = lx.advance();
                                char h2 = lx.advance();
                                s.push_back(static_cast<char>((hex(h1) << 4) | hex(h2)));
                            }
                            break;
                        }
                        default: s.push_back(e);
                    }
                } else if (c == '\n') {
                    lx.error("string literal sin cerrar (encontrado newline)");
                    return s;
                } else {
                    s.push_back(c);
                }
            }
            if (lx.peek() == '"') lx.advance();
            else lx.error("string literal sin cerrar");
            return s;
        }

        TomlArray parse_array(Lexer &lx) {
            // peek == '['
            lx.advance();
            TomlArray arr;
            while (true) {
                lx.skip_ws_and_comments();
                if (lx.peek() == ']') { lx.advance(); break; }
                arr.push_back(parse_value(lx));
                lx.skip_ws_and_comments();
                if (lx.peek() == ',') { lx.advance(); continue; }
                if (lx.peek() == ']') { lx.advance(); break; }
                lx.error("se esperaba ',' o ']' en array");
                break;
            }
            return arr;
        }

        TomlTable parse_inline_table(Lexer &lx) {
            // peek == '{'
            lx.advance();
            TomlTable tab;
            while (true) {
                lx.skip_hspace();
                if (lx.peek() == '}') { lx.advance(); break; }
                auto parts = parse_dotted_key(lx);
                lx.skip_hspace();
                if (lx.peek() != '=') {
                    lx.error("se esperaba '=' en tabla inline");
                    break;
                }
                lx.advance();
                lx.skip_hspace();
                TomlValue v = parse_value(lx);
                // Construir nested si dotted.
                TomlTable *cur = &tab;
                for (size_t i = 0; i + 1 < parts.size(); ++i) {
                    auto it = cur->find(parts[i]);
                    if (it == cur->end() || !it->second.is_table()) {
                        (*cur)[parts[i]] = TomlValue(TomlTable{});
                    }
                    cur = &(*cur)[parts[i]].as_table();
                }
                (*cur)[parts.back()] = std::move(v);
                lx.skip_hspace();
                if (lx.peek() == ',') { lx.advance(); continue; }
                if (lx.peek() == '}') { lx.advance(); break; }
                lx.error("se esperaba ',' o '}' en tabla inline");
                break;
            }
            return tab;
        }

        TomlValue parse_value(Lexer &lx) {
            lx.skip_hspace();
            char c = lx.peek();
            if (c == '"') {
                return TomlValue(parse_string(lx));
            }
            if (c == '[') {
                return TomlValue(parse_array(lx));
            }
            if (c == '{') {
                return TomlValue(parse_inline_table(lx));
            }
            // bool
            if (c == 't' && lx.src.substr(lx.pos, 4) == "true") {
                lx.pos += 4; lx.col += 4;
                return TomlValue(true);
            }
            if (c == 'f' && lx.src.substr(lx.pos, 5) == "false") {
                lx.pos += 5; lx.col += 5;
                return TomlValue(false);
            }
            // int (decimal con signo opcional + posibles separadores _)
            if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c))) {
                std::string s;
                bool seen_digit = false;
                if (c == '-' || c == '+') { s.push_back(c); lx.advance(); }
                while (!lx.eof()) {
                    char d = lx.peek();
                    if (std::isdigit(static_cast<unsigned char>(d))) {
                        s.push_back(d); lx.advance(); seen_digit = true;
                    } else if (d == '_') {
                        lx.advance();  // separador legible, se ignora
                    } else break;
                }
                if (!seen_digit) {
                    lx.error("se esperaba un numero");
                    return {};
                }
                try {
                    return TomlValue(static_cast<int64_t>(std::stoll(s)));
                } catch (...) {
                    lx.error("numero invalido: " + s);
                    return {};
                }
            }
            lx.error("valor desconocido");
            return {};
        }

        // Asegura que la ruta de keys exista como Tables anidadas y
        // devuelve el puntero a la ultima.
        TomlTable *ensure_path(TomlTable &root,
                                const std::vector<std::string> &path) {
            TomlTable *cur = &root;
            for (const auto &k : path) {
                auto it = cur->find(k);
                if (it == cur->end()) {
                    (*cur)[k] = TomlValue(TomlTable{});
                    cur = &(*cur)[k].as_table();
                } else if (!it->second.is_table()) {
                    // colision: ya hay un valor escalar con ese nombre.
                    // Lo sustituimos por table vacia (parser tolerante;
                    // el validator semantico reportara conflictos).
                    it->second = TomlValue(TomlTable{});
                    cur = &it->second.as_table();
                } else {
                    cur = &it->second.as_table();
                }
            }
            return cur;
        }

    } // namespace

    ParseResult parse(std::string_view input) {
        ParseResult res;
        Lexer lx;
        lx.src = input;

        TomlTable root;
        TomlTable *cur_table = &root;
        // Para [[array.of.tables]] necesitamos saber a que array push:
        std::vector<std::string> cur_array_path;
        bool cur_is_array_of_tables = false;

        auto push_into_aot = [&](const std::vector<std::string> &path) -> TomlTable * {
            // Encuentra (o crea) un array en root[path] y push una nueva tabla.
            TomlTable *parent = &root;
            for (size_t i = 0; i + 1 < path.size(); ++i) {
                auto it = parent->find(path[i]);
                if (it == parent->end() || !it->second.is_table()) {
                    (*parent)[path[i]] = TomlValue(TomlTable{});
                }
                parent = &(*parent)[path[i]].as_table();
            }
            const std::string &last = path.back();
            auto it = parent->find(last);
            if (it == parent->end()) {
                (*parent)[last] = TomlValue(TomlArray{});
            } else if (!it->second.is_array()) {
                // colision; sobreescribe
                (*parent)[last] = TomlValue(TomlArray{});
            }
            TomlArray &arr = (*parent)[last].as_array();
            arr.push_back(TomlValue(TomlTable{}));
            return &arr.back().as_table();
        };

        while (true) {
            lx.skip_ws_and_comments();
            if (lx.eof()) break;
            if (!lx.err.empty()) break;

            char c = lx.peek();
            if (c == '[') {
                lx.advance();
                bool aot = false;
                if (lx.peek() == '[') { aot = true; lx.advance(); }
                auto path = parse_dotted_key(lx);
                lx.skip_hspace();
                if (aot) {
                    if (lx.peek() != ']' || lx.src[lx.pos+1] != ']') {
                        lx.error("se esperaba ']]' al cerrar array-of-tables");
                        break;
                    }
                    lx.advance(); lx.advance();
                    cur_table = push_into_aot(path);
                    cur_array_path = path;
                    cur_is_array_of_tables = true;
                } else {
                    if (lx.peek() != ']') {
                        lx.error("se esperaba ']' al cerrar seccion");
                        break;
                    }
                    lx.advance();
                    cur_table = ensure_path(root, path);
                    cur_array_path.clear();
                    cur_is_array_of_tables = false;
                }
                lx.skip_hspace();
                if (!lx.eof() && lx.peek() != '\n' && lx.peek() != '#') {
                    if (lx.peek() != '\r') {
                        lx.error("contenido inesperado tras encabezado de seccion");
                        break;
                    }
                }
                lx.skip_to_eol();
                continue;
            }

            // key = value
            auto parts = parse_dotted_key(lx);
            lx.skip_hspace();
            if (lx.peek() != '=') {
                lx.error("se esperaba '=' tras clave");
                break;
            }
            lx.advance();
            lx.skip_hspace();
            TomlValue v = parse_value(lx);

            TomlTable *target = cur_table;
            for (size_t i = 0; i + 1 < parts.size(); ++i) {
                auto it = target->find(parts[i]);
                if (it == target->end() || !it->second.is_table()) {
                    (*target)[parts[i]] = TomlValue(TomlTable{});
                }
                target = &(*target)[parts[i]].as_table();
            }
            (*target)[parts.back()] = std::move(v);

            lx.skip_hspace();
            if (!lx.eof() && lx.peek() == '#') lx.skip_to_eol();
            else if (!lx.eof()) lx.skip_to_eol();
        }

        if (!lx.err.empty()) {
            res.ok = false;
            res.error_msg  = lx.err;
            res.error_line = lx.err_line;
            res.error_col  = lx.err_col;
            return res;
        }
        res.ok = true;
        res.root = std::move(root);
        return res;
    }

    ParseResult parse_file(const std::string &path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            ParseResult r;
            r.ok = false;
            r.error_msg = "no se puede abrir el archivo: " + path;
            return r;
        }
        std::ostringstream buf;
        buf << f.rdbuf();
        return parse(buf.str());
    }

    // ----------------------------------------------------------------
    // Serializer
    // ----------------------------------------------------------------

    std::string escape_string(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\t': out += "\\t";  break;
                case '\r': out += "\\r";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\x%02X",
                                       static_cast<unsigned>(c) & 0xFF);
                        out += buf;
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
        return out;
    }

    // Detect if a key needs quoting (contiene chars no-bare).
    static bool needs_quoting(const std::string &k) {
        if (k.empty()) return true;
        for (char c : k) {
            if (!(std::isalnum(static_cast<unsigned char>(c))
                  || c == '_' || c == '-')) return true;
        }
        return false;
    }

    static std::string format_key(const std::string &k) {
        return needs_quoting(k) ? escape_string(k) : k;
    }

    // Serializa un valor (NO tabla ni AoT al top-level; esos los maneja
    // serialize_table_section).  Tablas inline si is_table.
    static std::string serialize_value(const TomlValue &v);

    static std::string serialize_array(const TomlArray &a) {
        std::ostringstream s;
        s << "[";
        for (size_t i = 0; i < a.size(); ++i) {
            if (i) s << ", ";
            s << serialize_value(a[i]);
        }
        s << "]";
        return s.str();
    }

    static std::string serialize_inline_table(const TomlTable &t) {
        std::ostringstream s;
        s << "{ ";
        size_t i = 0;
        for (const auto &kv : t) {
            if (i++) s << ", ";
            s << format_key(kv.first) << " = " << serialize_value(kv.second);
        }
        s << " }";
        return s.str();
    }

    static std::string serialize_value(const TomlValue &v) {
        switch (v.kind()) {
            case TomlValue::Kind::Null:    return "\"\"";
            case TomlValue::Kind::String:  return escape_string(v.as_string());
            case TomlValue::Kind::Int:     return std::to_string(v.as_int());
            case TomlValue::Kind::Bool:    return v.as_bool() ? "true" : "false";
            case TomlValue::Kind::Array:   return serialize_array(v.as_array());
            case TomlValue::Kind::Table:   return serialize_inline_table(v.as_table());
        }
        return "";
    }

    // Detecta si una tabla es "scalar-leaf" (todas las entries no son
    // arrays-of-tables y no son nested tables grandes).  Heuristica
    // simple: es scalar-leaf si NINGUNA entry es Table o (Array of Tables).
    static bool is_scalar_leaf_table(const TomlTable &t) {
        for (const auto &kv : t) {
            if (kv.second.is_table()) return false;
            if (kv.second.is_array()) {
                for (const auto &el : kv.second.as_array()) {
                    if (el.is_table()) return false;
                }
            }
        }
        return true;
    }

    static void serialize_section(std::ostringstream &out,
                                    const std::string &header,
                                    const TomlTable &t) {
        if (!header.empty()) {
            out << "[" << header << "]\n";
        }
        // Escalares y arrays escalares primero.
        for (const auto &kv : t) {
            if (kv.second.is_table()) continue;
            if (kv.second.is_array()) {
                bool has_tables = false;
                for (const auto &el : kv.second.as_array()) {
                    if (el.is_table()) { has_tables = true; break; }
                }
                if (has_tables) continue;
            }
            out << format_key(kv.first) << " = "
                << serialize_value(kv.second) << "\n";
        }
        out << "\n";
        // Tablas anidadas (recursive).
        for (const auto &kv : t) {
            if (kv.second.is_table()) {
                const TomlTable &sub = kv.second.as_table();
                std::string subhdr = header.empty()
                    ? format_key(kv.first)
                    : header + "." + format_key(kv.first);
                if (is_scalar_leaf_table(sub)) {
                    serialize_section(out, subhdr, sub);
                } else {
                    serialize_section(out, subhdr, sub);
                }
            }
        }
        // Array-of-tables.
        for (const auto &kv : t) {
            if (kv.second.is_array()) {
                bool aot = !kv.second.as_array().empty()
                            && kv.second.as_array().front().is_table();
                if (!aot) continue;
                std::string aothdr = header.empty()
                    ? format_key(kv.first)
                    : header + "." + format_key(kv.first);
                for (const auto &el : kv.second.as_array()) {
                    out << "[[" << aothdr << "]]\n";
                    if (el.is_table()) {
                        // Serializar la tabla pero SIN el header (ya escrito).
                        const TomlTable &sub = el.as_table();
                        for (const auto &skv : sub) {
                            if (skv.second.is_table()) continue;
                            out << format_key(skv.first) << " = "
                                << serialize_value(skv.second) << "\n";
                        }
                        // Sub-tablas anidadas dentro de la AoT entry:
                        for (const auto &skv : sub) {
                            if (skv.second.is_table()) {
                                std::string subhdr = aothdr + "." + format_key(skv.first);
                                serialize_section(out, subhdr, skv.second.as_table());
                            }
                        }
                    }
                    out << "\n";
                }
            }
        }
    }

    std::string serialize(const TomlTable &root) {
        std::ostringstream out;
        serialize_section(out, "", root);
        return out.str();
    }

} // namespace pkg::toml
