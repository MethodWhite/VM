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
 * @file sqlite_singleton.h
 * @brief Singleton thread-safe para gestion de la base de datos SQLite de VestaVM.
 *
 * Proporciona un punto de acceso global y unico a una instancia de SQLite con:
 *  - Inicializacion lazy (primera llamada a get_instance()).
 *  - Thread-safety garantizada mediante mutex interno.
 *  - Gestion automatica de la conexion mediante RAII (SqliteDeleter).
 *  - Consultas simples (execute) y consultas con resultado en JSON (execute_json).
 *  - Impresion de tablas ASCII con print_table() y table_to_string().
 *
 * Uso tipico:
 * @code
 *   Sqlite::SqliteSingleton::execute(
 *       "CREATE TABLE IF NOT EXISTS bytecode(id INTEGER PRIMARY KEY, code BLOB)");
 *
 *   auto rows = Sqlite::SqliteSingleton::query_json_rows_map(
 *       "SELECT id, code FROM bytecode WHERE id=1");
 *   for (const auto &row : rows) {
 *       std::string id  = row.at("id");
 *       std::string cod = row.at("code");
 *   }
 * @endcode
 */

#ifndef SQLITESINGLETON_H
#define SQLITESINGLETON_H

#include <mutex>
#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <sstream>

#include "sqlite3.h"
#include "json.hpp"

namespace Sqlite {

    using json = nlohmann::json;

    /**
     * @class SqliteSingleton
     * @brief Singleton thread-safe para gestion de base de datos SQLite en VestaVM.
     *
     * Solo puede existir una instancia de la conexion SQLite en todo el proceso.
     * La inicializacion es lazy: la conexion se abre la primera vez que se llama a
     * @c get_instance() y permanece abierta hasta que se llama a @c cleanup() o
     * el proceso termina.
     *
     * No puede copiarse ni moverse; todos sus metodos son estaticos.
     *
     * @note El nivel de thread-safety depende tambien del modo de compilacion de
     *       SQLite.  Para uso multiproceso se recomienda compilar SQLite en modo
     *       "serialized" (SQLITE_THREADSAFE=1).
     */
    class SqliteSingleton {
    private:
        /* constructor privado: impide instanciacion directa */
        SqliteSingleton() = default;

        /**
         * @struct SqliteDeleter
         * @brief Deleter personalizado para @c std::unique_ptr<sqlite3>.
         *
         * Cierra la conexion SQLite mediante @c sqlite3_close() cuando el
         * @c unique_ptr es destruido, garantizando la liberacion de recursos
         * incluso en presencia de excepciones.
         */
        struct SqliteDeleter {
            /**
             * @brief Cierra la conexion SQLite si el puntero no es nulo.
             * @param db Puntero a la estructura sqlite3 gestionada.
             */
            void operator()(sqlite3 *db) const {
                if (db) sqlite3_close(db);
            }
        };

        static std::unique_ptr<sqlite3, SqliteDeleter> db_instance; ///< Instancia unica de la BD.
        static std::mutex db_mutex;   ///< Mutex que serializa el acceso a la instancia.
        static bool initialized;      ///< true una vez que la BD ha sido abierta con exito.

        /// Ruta por defecto del archivo de base de datos.
        static constexpr const char *DEFAULT_DB_PATH = "vm_data.db";

    public:
        /* --- Prohibir copia y movimiento --- */
        SqliteSingleton(const SqliteSingleton &)            = delete;
        SqliteSingleton &operator=(const SqliteSingleton &) = delete;
        SqliteSingleton(SqliteSingleton &&)                 = delete;
        SqliteSingleton &operator=(SqliteSingleton &&)      = delete;

        /**
         * @brief Devuelve el puntero a la instancia unica de SQLite (inicializacion lazy).
         *
         * La primera llamada abre la base de datos en @p db_path.  Las siguientes
         * llamadas devuelven directamente el puntero ya almacenado sin volver a abrir.
         *
         * Thread-safe: solo un hilo inicializa la BD; los demas esperan en el mutex y
         * reutilizan la instancia ya creada.
         *
         * @param db_path Ruta al archivo .db (por defecto "vm_data.db").
         * @return Puntero a @c sqlite3, o @c nullptr si la apertura fallo.
         */
        static sqlite3 *get_instance(const std::string &db_path = DEFAULT_DB_PATH);

        /**
         * @brief Cierra la conexion SQLite y resetea el singleton.
         *
         * Llama a @c sqlite3_close() a traves del deleter del @c unique_ptr.
         * Despues de esta llamada, @c get_instance() volvera a abrir la BD
         * si se invoca de nuevo.
         *
         * Thread-safe: protegido por el mutex interno.
         */
        static void cleanup();

        /**
         * @brief Ejecuta una sentencia SQL sin devolver filas (DDL, DML).
         *
         * Adecuado para @c CREATE, @c INSERT, @c UPDATE, @c DELETE.
         * Para consultas @c SELECT que devuelven datos usa @c execute_json().
         *
         * @param sql Sentencia SQL a ejecutar.
         * @param err Puntero opcional donde se almacena el mensaje de error si ocurre;
         *            si es @c nullptr los errores se imprimen en @c stderr.
         * @return @c true si la ejecucion fue correcta; @c false si hubo error.
         *
         * @code
         *   SqliteSingleton::execute(
         *       "CREATE TABLE IF NOT EXISTS lexer_tokens (token TEXT, line INTEGER)");
         * @endcode
         */
        static bool execute(const std::string &sql, std::string *err = nullptr);

        /**
         * @brief Ejecuta una sentencia SQL y devuelve las filas como un array JSON.
         *
         * Cada fila se representa como un objeto JSON cuyas claves son los nombres
         * de columna y cuyos valores son cadenas (o la representacion textual del tipo).
         *
         * Ejemplo de retorno para @c "SELECT id,val FROM kv":
         * @code
         *   [
         *     {"id": "0", "val": "v0"},
         *     {"id": "1", "val": "v1"}
         *   ]
         * @endcode
         *
         * Si ocurre un error se devuelve un objeto JSON con la clave @c "error":
         * @code
         *   {"error": "mensaje de error de SQLite"}
         * @endcode
         *
         * @param sql Sentencia SQL a ejecutar (normalmente un @c SELECT).
         * @return Objeto @c nlohmann::json con el array de filas o con el error.
         *
         * @warning La seguridad en concurrencia depende del modo de compilacion de
         *          SQLite.  Si no es "serialized", no es seguro compartir la misma
         *          @c sqlite3* entre hilos sin sincronizacion externa adicional.
         *
         * @see query_json_rows_map() para convertir la salida en un vector de mapas C++.
         */
        static nlohmann::json execute_json(const std::string &sql);

        /**
         * @brief Ejecuta una consulta SQL y devuelve las filas como vector de mapas.
         *
         * Convierte la salida de @c execute_json() en una estructura C++ mas comoda:
         * cada fila es un @c std::map<string,string> donde la clave es el nombre de
         * columna y el valor es la representacion textual del dato.
         *
         * Reglas de conversion de tipos JSON:
         *  - @c null  -> cadena vacia (@c "").
         *  - @c string -> el string sin comillas adicionales.
         *  - otros    -> representacion textual via @c json::dump().
         *
         * @param sql Sentencia SQL a ejecutar (normalmente un @c SELECT).
         * @param err Puntero opcional; si no es @c nullptr y ocurre un error, se
         *            escribe el mensaje de error en @c *err.
         * @return Vector de filas; vacio si hubo error o la consulta no devolvio datos.
         *
         * @note No lanza excepciones por columnas ausentes; verifica la existencia de
         *       claves antes de usar @c at() si el esquema puede variar.
         *
         * @code
         *   std::string err;
         *   auto rows = SqliteSingleton::query_json_rows_map(
         *       "SELECT id, val FROM kv2;", &err);
         *   if (!err.empty()) { manejar error }
         *   for (const auto &row : rows) {
         *       std::string id  = row.at("id");
         *       std::string val = row.at("val");
         *   }
         * @endcode
         */
        static std::vector<std::map<std::string, std::string> > query_json_rows_map(
            const std::string &sql, std::string *err = nullptr) {
            json j = SqliteSingleton::execute_json(sql);

            /* si el JSON contiene la clave "error" la consulta fallo */
            if (j.is_object() && j.contains("error")) {
                if (err) *err = j["error"].get<std::string>();
                return {};
            }

            std::vector<std::map<std::string, std::string> > rows;
            for (const auto &r: j) {
                if (!r.is_object()) continue;

                std::map<std::string, std::string> rowmap;
                for (auto it = r.begin(); it != r.end(); ++it) {
                    if      (it.value().is_null())   rowmap[it.key()] = "";
                    else if (it.value().is_string()) rowmap[it.key()] = it.value().get<std::string>();
                    else                             rowmap[it.key()] = it.value().dump();
                }
                rows.push_back(std::move(rowmap));
            }
            return rows;
        }
    };

    /// Mutex dedicado a serializar la impresion de tablas en la consola.
    static std::mutex print_mutex;

    /**
     * @brief Imprime un vector de filas como una tabla ASCII en el stream indicado.
     *
     * Dibuja bordes con caracteres '+', '-' y '|' al estilo de una tabla SQL.
     * Las columnas se ajustan al ancho maximo de sus valores (truncando a @p max_width).
     * Los valores que parecen numericos se alinean a la derecha si @p right_align_numbers
     * es @c true.
     *
     * Thread-safe: usa @c print_mutex para serializar escrituras concurrentes.
     *
     * @param out                 Stream de salida (p.ej. @c std::cout o un @c ostringstream).
     * @param rows                Vector de filas; cada fila es un mapa columna->valor.
     * @param columns_order       Orden explicito de columnas a mostrar; si esta vacio se
     *                            usa la union ordenada de todas las claves presentes.
     * @param right_align_numbers Si @c true, los valores que solo contengan digitos, '.'
     *                            o '-' se alinean a la derecha.
     * @param max_width           Ancho maximo en caracteres de una celda; los valores mas
     *                            largos se truncan con "...".
     */
    static void print_table(std::ostream &out,
                            const std::vector<std::map<std::string, std::string> > &rows,
                            const std::vector<std::string> &columns_order = {},
                            bool right_align_numbers = true,
                            size_t max_width = 60) {
        std::lock_guard<std::mutex> lk(print_mutex);

        /* determinar orden de columnas */
        std::vector<std::string> cols;
        if (!columns_order.empty()) {
            cols = columns_order;
        } else {
            std::set<std::string> colset;
            for (const auto &r: rows) for (const auto &p: r) colset.insert(p.first);
            cols.assign(colset.begin(), colset.end());
        }

        /* calcular el ancho de cada columna (minimo = longitud del nombre) */
        std::vector<size_t> width(cols.size());
        for (size_t i = 0; i < cols.size(); ++i) width[i] = cols[i].size();
        for (const auto &r: rows) {
            for (size_t i = 0; i < cols.size(); ++i) {
                auto it = r.find(cols[i]);
                if (it != r.end())
                    width[i] = std::max(width[i], std::min(max_width, it->second.size()));
            }
        }

        /* lambda auxiliar para imprimir una linea separadora */
        auto print_sep = [&](char left, char mid, char right, char fill = '-') {
            out << left;
            for (size_t i = 0; i < cols.size(); ++i) {
                out << std::string(width[i] + 2, fill);
                if (i + 1 < cols.size()) out << mid;
            }
            out << right << "\n";
        };

        /* cabecera de la tabla */
        print_sep('+', '+', '+');
        out << "|";
        for (size_t i = 0; i < cols.size(); ++i)
            out << " " << std::left << std::setw((int)width[i]) << cols[i] << " " << "|";
        out << "\n";
        print_sep('+', '+', '+');

        /* filas de datos */
        for (const auto &r: rows) {
            out << "|";
            for (size_t i = 0; i < cols.size(); ++i) {
                auto it = r.find(cols[i]);
                std::string val = (it != r.end()) ? it->second : "";

                /* truncar valores demasiado largos */
                if (val.size() > max_width) val = val.substr(0, max_width - 3) + "...";

                /* determinar si el valor parece un numero para alinearlo a la derecha */
                bool is_number = !val.empty() && std::all_of(val.begin(), val.end(),
                    [](char c){ return (c >= '0' && c <= '9') || c == '.' || c == '-'; });

                if (right_align_numbers && is_number)
                    out << " " << std::right << std::setw((int)width[i]) << val << " " << "|";
                else
                    out << " " << std::left  << std::setw((int)width[i]) << val << " " << "|";
            }
            out << "\n";
        }

        /* pie de la tabla */
        print_sep('+', '+', '+');
    }

    /**
     * @brief Devuelve la representacion de tabla ASCII como cadena de texto.
     *
     * Variante de @c print_table() que escribe en un @c ostringstream interno y
     * devuelve su contenido como @c std::string.  Util para logs o pruebas.
     *
     * @param rows                Vector de filas a representar.
     * @param columns_order       Orden explicito de columnas; vacio = orden alfabetico.
     * @param right_align_numbers Alinea a la derecha los valores numericos si es @c true.
     * @param max_width           Ancho maximo de cada celda en caracteres.
     * @return Cadena con la tabla ASCII completa, incluyendo cabecera y pie.
     */
    inline std::string table_to_string(
        const std::vector<std::map<std::string, std::string> > &rows,
        const std::vector<std::string> &columns_order = {},
        bool right_align_numbers = true,
        size_t max_width = 60) {
        std::ostringstream ss;
        print_table(ss, rows, columns_order, right_align_numbers, max_width);
        return ss.str();
    }

} // namespace Sqlite

#endif // SQLITESINGLETON_H
