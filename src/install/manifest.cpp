/*
 * VestaVM - src/install/manifest.cpp
 *
 * Serializador JSON ad-hoc para el manifest. Sin dependencias externas:
 * el manifest es estructurado y conocido, asi que nos basta con un emisor
 * y un parser pequenos.
 */

#include "install/manifest.h"

#include <fstream>
#include <sstream>
#include <iomanip>

namespace install {

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------

    static std::string esc(const std::string& s) {
        std::string r; r.reserve(s.size() + 4);
        for (char c : s) {
            switch (c) {
                case '\\': r += "\\\\"; break;
                case '"':  r += "\\\""; break;
                case '\n': r += "\\n";  break;
                case '\r': r += "\\r";  break;
                case '\t': r += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                        r += buf;
                    } else r += c;
            }
        }
        return r;
    }

    static std::string Q(const std::string& s) { return "\"" + esc(s) + "\""; }
    static std::string Q(const std::filesystem::path& p) {
        return Q(p.string());
    }

    // ------------------------------------------------------------------
    // manifest_path()
    // ------------------------------------------------------------------

    std::filesystem::path Manifest::manifest_path() const {
        return prefix / "install_manifest.json";
    }

    // ------------------------------------------------------------------
    // save()
    // ------------------------------------------------------------------

    bool Manifest::save_to(const std::filesystem::path& path) const {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);

        std::ofstream f(path, std::ios::binary);
        if (!f) return false;

        f << "{\n";
        f << "  \"version\": "        << Q(version)        << ",\n";
        f << "  \"vesta_version\": "  << Q(vesta_version)  << ",\n";
        f << "  \"install_date\": "   << Q(install_date)   << ",\n";
        f << "  \"platform\": "       << Q(platform)       << ",\n";
        f << "  \"scope\": "          << Q(scope)          << ",\n";
        f << "  \"prefix\": "         << Q(prefix)         << ",\n";

        // files
        f << "  \"files\": [\n";
        for (size_t i = 0; i < files.size(); ++i) {
            const auto& e = files[i];
            f << "    { \"path\": " << Q(e.path)
              << ", \"size\": " << e.size
              << ", \"sha256\": " << Q(e.sha256) << " }";
            if (i + 1 < files.size()) f << ",";
            f << "\n";
        }
        f << "  ],\n";

        // registry
        f << "  \"registry\": [\n";
        for (size_t i = 0; i < registry.size(); ++i) {
            const auto& e = registry[i];
            f << "    { \"hive\": " << Q(e.hive)
              << ", \"key\": "        << Q(e.key)
              << ", \"value_name\": " << Q(e.value_name)
              << ", \"data\": "       << Q(e.data) << " }";
            if (i + 1 < registry.size()) f << ",";
            f << "\n";
        }
        f << "  ],\n";

        // path_entries
        f << "  \"path_entries\": [\n";
        for (size_t i = 0; i < path_entries.size(); ++i) {
            f << "    { \"entry\": " << Q(path_entries[i].entry)
              << ", \"scope\": " << Q(path_entries[i].scope) << " }";
            if (i + 1 < path_entries.size()) f << ",";
            f << "\n";
        }
        f << "  ],\n";

        // symlinks
        f << "  \"symlinks\": [\n";
        for (size_t i = 0; i < symlinks.size(); ++i) {
            f << "    { \"link\": " << Q(symlinks[i].link)
              << ", \"target\": " << Q(symlinks[i].target) << " }";
            if (i + 1 < symlinks.size()) f << ",";
            f << "\n";
        }
        f << "  ],\n";

        // shortcuts
        f << "  \"shortcuts\": [\n";
        for (size_t i = 0; i < shortcuts.size(); ++i) {
            f << "    { \"kind\": " << Q(shortcuts[i].kind)
              << ", \"path\": " << Q(shortcuts[i].path) << " }";
            if (i + 1 < shortcuts.size()) f << ",";
            f << "\n";
        }
        f << "  ],\n";

        // desktop_files
        f << "  \"desktop_files\": [\n";
        for (size_t i = 0; i < desktop_files.size(); ++i) {
            f << "    { \"path\": " << Q(desktop_files[i].path) << " }";
            if (i + 1 < desktop_files.size()) f << ",";
            f << "\n";
        }
        f << "  ],\n";

        // mime_files
        f << "  \"mime_files\": [\n";
        for (size_t i = 0; i < mime_files.size(); ++i) {
            f << "    { \"path\": " << Q(mime_files[i].path) << " }";
            if (i + 1 < mime_files.size()) f << ",";
            f << "\n";
        }
        f << "  ]\n";
        f << "}\n";
        return f.good();
    }

    bool Manifest::save() const {
        return save_to(manifest_path());
    }

    // ------------------------------------------------------------------
    // load_from(): parser JSON minimalista pensado para el formato propio
    //
    // No es un parser JSON general; aprovecha que el formato es siempre el
    // mismo. Si algun dia quieres robustez maxima, sustituye por nlohmann/json.
    // ------------------------------------------------------------------

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\n\r");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\n\r");
        return s.substr(a, b - a + 1);
    }

    static std::string unesc(const std::string& s) {
        std::string r; r.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char n = s[i + 1];
                ++i;
                switch (n) {
                    case '\\': r += '\\'; break;
                    case '"':  r += '"';  break;
                    case 'n':  r += '\n'; break;
                    case 'r':  r += '\r'; break;
                    case 't':  r += '\t'; break;
                    default: r += n;
                }
            } else r += s[i];
        }
        return r;
    }

    /// Extrae el valor de una clave "k": "v" o "k": N de una linea.
    static bool extract_str(const std::string& line, const char* key, std::string& out) {
        std::string needle = std::string("\"") + key + "\"";
        auto p = line.find(needle);
        if (p == std::string::npos) return false;
        auto q = line.find('"', p + needle.size() + 1);   // abre comilla
        if (q == std::string::npos) return false;
        auto r = line.find('"', q + 1);                   // cierra comilla (sin escapes complejos)
        // Manejo simple de escapes: avanzar hasta comilla sin backslash previa
        size_t i = q + 1;
        while (i < line.size()) {
            if (line[i] == '\\' && i + 1 < line.size()) { i += 2; continue; }
            if (line[i] == '"') { r = i; break; }
            ++i;
        }
        if (r == std::string::npos) return false;
        out = unesc(line.substr(q + 1, r - q - 1));
        return true;
    }

    static bool extract_u64(const std::string& line, const char* key, uint64_t& out) {
        std::string needle = std::string("\"") + key + "\"";
        auto p = line.find(needle);
        if (p == std::string::npos) return false;
        auto colon = line.find(':', p);
        if (colon == std::string::npos) return false;
        std::stringstream ss(line.substr(colon + 1));
        ss >> out;
        return !ss.fail();
    }

    bool Manifest::load_from(const std::filesystem::path& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream buf;
        buf << f.rdbuf();
        std::string text = buf.str();

        // Cabecera (claves planas, una por linea)
        std::stringstream ls(text);
        std::string line;
        std::string section;
        while (std::getline(ls, line)) {
            std::string t = trim(line);
            if (t.empty()) continue;

            // detectar inicio de seccion array
            if (t.find("\"files\":")         != std::string::npos) { section = "files"; continue; }
            if (t.find("\"registry\":")      != std::string::npos) { section = "registry"; continue; }
            if (t.find("\"path_entries\":")  != std::string::npos) { section = "path"; continue; }
            if (t.find("\"symlinks\":")      != std::string::npos) { section = "symlinks"; continue; }
            if (t.find("\"shortcuts\":")     != std::string::npos) { section = "shortcuts"; continue; }
            if (t.find("\"desktop_files\":") != std::string::npos) { section = "desktop"; continue; }
            if (t.find("\"mime_files\":")    != std::string::npos) { section = "mime"; continue; }
            if (t == "]," || t == "]")  { section = ""; continue; }
            if (t == "{" || t == "}")  continue;

            // claves de cabecera
            if (section.empty()) {
                std::string v;
                if      (extract_str(t, "version",       v)) version       = v;
                else if (extract_str(t, "vesta_version", v)) vesta_version = v;
                else if (extract_str(t, "install_date",  v)) install_date  = v;
                else if (extract_str(t, "platform",      v)) platform      = v;
                else if (extract_str(t, "scope",         v)) scope         = v;
                else if (extract_str(t, "prefix",        v)) prefix        = v;
                continue;
            }

            // entradas de array (una por linea, formato { "k1": v1, ... })
            if (section == "files") {
                ManifestFile e;
                std::string p; if (extract_str(t, "path", p)) e.path = p;
                uint64_t sz = 0; extract_u64(t, "size", sz); e.size = sz;
                std::string sh; extract_str(t, "sha256", sh); e.sha256 = sh;
                if (!e.path.empty()) files.push_back(e);
            }
            else if (section == "registry") {
                ManifestRegistry e;
                extract_str(t, "hive",       e.hive);
                extract_str(t, "key",        e.key);
                extract_str(t, "value_name", e.value_name);
                extract_str(t, "data",       e.data);
                if (!e.key.empty()) registry.push_back(e);
            }
            else if (section == "path") {
                ManifestPathEntry e;
                extract_str(t, "entry", e.entry);
                extract_str(t, "scope", e.scope);
                if (!e.entry.empty()) path_entries.push_back(e);
            }
            else if (section == "symlinks") {
                ManifestSymlink e;
                std::string a, b;
                extract_str(t, "link",   a); e.link   = a;
                extract_str(t, "target", b); e.target = b;
                if (!a.empty()) symlinks.push_back(e);
            }
            else if (section == "shortcuts") {
                ManifestShortcut e;
                extract_str(t, "kind", e.kind);
                std::string p; extract_str(t, "path", p); e.path = p;
                if (!e.path.empty()) shortcuts.push_back(e);
            }
            else if (section == "desktop") {
                ManifestDesktopFile e;
                std::string p; extract_str(t, "path", p); e.path = p;
                if (!p.empty()) desktop_files.push_back(e);
            }
            else if (section == "mime") {
                ManifestMimeFile e;
                std::string p; extract_str(t, "path", p); e.path = p;
                if (!p.empty()) mime_files.push_back(e);
            }
        }
        return true;
    }

} // namespace install
