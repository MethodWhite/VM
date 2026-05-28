/**
 * @file toml_lite.h
 * @brief Mini-parser de TOML para el manifest de paquetes Vesta.
 *
 * Subset de TOML que cubre lo necesario para vex.toml y vex.lock:
 *   - Tablas: @c [section] y @c [[array_of_tables]]
 *   - Sub-tablas: @c [section.subsection]
 *   - Tablas inline: @c key = { a = 1, b = "..." }
 *   - Tipos: string ("..."), int, bool (true/false), array ([...]),
 *            inline-table ({...}).
 *   - Comentarios: @c # hasta fin de linea.
 *   - String literal con backslash escapes (\n \t \" \\) y \xHH.
 *
 * NO soporta (no necesario para Vesta):
 *   - Strings multilinea ("""...""").
 *   - Strings raw ('...').
 *   - Fechas/timestamps.
 *   - Numeros flotantes.
 *   - Numeros hex/oct/bin.
 *
 * El parser devuelve un AST simple via @c TomlValue (variant-like).
 */
#ifndef VESTAVM_PKG_TOML_LITE_H
#define VESTAVM_PKG_TOML_LITE_H

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <variant>
#include <cstdint>

namespace pkg::toml {

    class TomlValue;
    using TomlTable = std::unordered_map<std::string, TomlValue>;
    using TomlArray = std::vector<TomlValue>;

    /**
     * @brief Valor TOML polimorfico.  Variant-like sobre los tipos soportados.
     */
    class TomlValue {
    public:
        enum class Kind { Null, String, Int, Bool, Array, Table };

        TomlValue() : kind_(Kind::Null) {}
        TomlValue(std::string s) : kind_(Kind::String), str_(std::move(s)) {}
        TomlValue(const char *s) : kind_(Kind::String), str_(s) {}
        TomlValue(int64_t v) : kind_(Kind::Int), int_(v) {}
        TomlValue(bool v) : kind_(Kind::Bool), bool_(v) {}
        TomlValue(TomlArray a) : kind_(Kind::Array),
                                   arr_(std::make_shared<TomlArray>(std::move(a))) {}
        TomlValue(TomlTable t) : kind_(Kind::Table),
                                   tab_(std::make_shared<TomlTable>(std::move(t))) {}

        Kind kind() const noexcept { return kind_; }
        bool is_null()   const noexcept { return kind_ == Kind::Null; }
        bool is_string() const noexcept { return kind_ == Kind::String; }
        bool is_int()    const noexcept { return kind_ == Kind::Int; }
        bool is_bool()   const noexcept { return kind_ == Kind::Bool; }
        bool is_array()  const noexcept { return kind_ == Kind::Array; }
        bool is_table()  const noexcept { return kind_ == Kind::Table; }

        const std::string &as_string() const { return str_; }
        int64_t as_int() const noexcept { return int_; }
        bool as_bool() const noexcept { return bool_; }
        const TomlArray &as_array() const { return *arr_; }
        TomlArray &as_array() { return *arr_; }
        const TomlTable &as_table() const { return *tab_; }
        TomlTable &as_table() { return *tab_; }

        /**
         * @brief Acceso defensivo: devuelve null TomlValue si no encuentra.
         */
        const TomlValue &get(const std::string &key) const;

        /**
         * @brief Helper: obtiene un string o devuelve @p def.
         */
        std::string get_string(const std::string &key,
                                const std::string &def = "") const;

        /**
         * @brief Helper: obtiene un int o devuelve @p def.
         */
        int64_t get_int(const std::string &key, int64_t def = 0) const;

        /**
         * @brief Helper: obtiene un bool o devuelve @p def.
         */
        bool get_bool(const std::string &key, bool def = false) const;

    private:
        Kind        kind_;
        std::string str_;
        int64_t     int_ = 0;
        bool        bool_ = false;
        std::shared_ptr<TomlArray> arr_;
        std::shared_ptr<TomlTable> tab_;
    };

    /**
     * @brief Resultado de un parse: tabla raiz si ok, error_msg si no.
     */
    struct ParseResult {
        bool        ok = false;
        TomlTable   root;
        std::string error_msg;
        int         error_line = 0;
        int         error_col  = 0;
    };

    /**
     * @brief Parsea un buffer TOML completo.
     */
    ParseResult parse(std::string_view input);

    /**
     * @brief Parsea un archivo TOML.
     */
    ParseResult parse_file(const std::string &path);

    // ----------------------------------------------------------------
    // Serializer (para escribir vex.toml/vex.lock canonicos)
    // ----------------------------------------------------------------

    /**
     * @brief Serializa una @c TomlTable a string TOML canonico.
     *
     * Reglas:
     *   - Strings con escape estandar (\" \\ \n \t).
     *   - Tablas inline solo para dependency specs ({git=, sha256=, ...}).
     *   - Arrays cortos en una linea; largos multi-linea.
     *   - Tablas anidadas como @c [parent.child] secciones planas.
     *   - Orden: bloques en orden de insercion del map raiz.
     */
    std::string serialize(const TomlTable &root);

    /**
     * @brief Escapa un string segun reglas TOML (basic string).
     */
    std::string escape_string(std::string_view s);

} // namespace pkg::toml

#endif // VESTAVM_PKG_TOML_LITE_H
