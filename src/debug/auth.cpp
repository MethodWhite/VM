/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribucion obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 */

/**
 * @file auth.cpp
 * @brief Implementacion del AuthManager (autenticacion y autorizacion).
 *
 * Usa una conexion SQLite DEDICADA al fichero de autenticacion (separada
 * del SqliteSingleton de @c vm_data.db).  Asi el operador puede colocar
 * la base de datos de credenciales en un volumen distinto y los SQL del
 * resto del runtime no pueden tocarla por accidente.
 *
 * Hashing: PBKDF2-HMAC-SHA256, 200000 iteraciones, salt aleatorio de 16
 * bytes, hash de 32 bytes.  Tanto el salt como el hash se persisten como
 * BLOB en SQLite.
 */

#include "debug/auth.h"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace debug {

    /* =====================================================================
     * Constantes del esquema de hashing.
     *
     * Los valores estan ALINEADOS con la columna BLOB en SQLite: si se
     * cambian aqui se debe migrar la tabla o las contrasenyas viejas
     * dejaran de validarse.
     * ===================================================================== */
    static constexpr size_t   PBKDF2_SALT_LEN = 16;
    static constexpr size_t   PBKDF2_HASH_LEN = 32;
    static constexpr int      PBKDF2_ITERS    = 200000;
    static constexpr size_t   TOKEN_RAW_BYTES = 32;

    /* =====================================================================
     * role_from_string / role_to_string
     * ===================================================================== */

    Role role_from_string(const std::string &s) {
        // comparacion case-insensitive con copia local en lowercase
        std::string lo = s;
        std::transform(lo.begin(), lo.end(), lo.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lo == "admin")     return Role::ADMIN;
        if (lo == "developer") return Role::DEVELOPER;
        if (lo == "dev")       return Role::DEVELOPER;
        if (lo == "viewer")    return Role::VIEWER;
        if (lo == "read")      return Role::VIEWER;
        return Role::VIEWER; // default conservador
    }

    const char *role_to_string(Role r) {
        switch (r) {
            case Role::ADMIN:     return "admin";
            case Role::DEVELOPER: return "developer";
            case Role::VIEWER:    return "viewer";
        }
        return "viewer";
    }

    /* =====================================================================
     * Helpers internos (no exportados)
     * ===================================================================== */

    /**
     * @brief Convierte un buffer de bytes a string hex (lowercase).
     */
    static std::string to_hex(const uint8_t *buf, size_t n) {
        static const char *digits = "0123456789abcdef";
        std::string out;
        out.resize(n * 2);
        for (size_t i = 0; i < n; ++i) {
            out[2 * i]     = digits[(buf[i] >> 4) & 0xF];
            out[2 * i + 1] = digits[buf[i] & 0xF];
        }
        return out;
    }

    /* =====================================================================
     * AuthManager
     * ===================================================================== */

    /* La conexion SQLite vive como puntero estatico interno al .cpp para
     * no contaminar el header con dependencias de sqlite3.h. */
    static sqlite3 *g_auth_db = nullptr;

    AuthManager &AuthManager::instance() {
        static AuthManager inst;
        return inst;
    }

    bool AuthManager::init(const std::string &db_path) {
        std::lock_guard<std::mutex> lk(db_mutex_);
        last_init_error_.clear();

        // Si ya hay una conexion abierta al mismo path, no reabrir.
        if (g_auth_db && db_path_ == db_path) return true;

        if (g_auth_db) {
            sqlite3_close(g_auth_db);
            g_auth_db = nullptr;
        }

        // Resolver el path a absoluto para que los mensajes de error y la
        // creacion del directorio padre funcionen aunque cwd cambie.
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path resolved = fs::absolute(fs::path(db_path), ec);
        if (ec) resolved = fs::path(db_path);

        // Auto-crear el directorio padre.  Esto resuelve el caso
        // habitual "no se pudo abrir users.db" cuando el cwd
        // (e.g. C:\Program Files\VestaVM) no es escribible: el
        // operador puede pasar un path absoluto a un directorio que
        // todavia no existe, y nosotros lo creamos primero.
        if (resolved.has_parent_path()) {
            std::error_code mec;
            fs::create_directories(resolved.parent_path(), mec);
            // Si create_directories falla, dejamos que sqlite3_open
            // produzca el mensaje real abajo.
        }

        // Abrir con flags explicitos: lectura + escritura + crear.  Esto
        // evita ambiguedad sobre si se intentara crear el archivo
        // (sqlite3_open por defecto si abre con flags equivalentes).
        const int flags =
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
        int rc = sqlite3_open_v2(resolved.string().c_str(),
                                 &g_auth_db, flags, nullptr);
        if (rc != SQLITE_OK) {
            std::ostringstream m;
            m << "no se pudo abrir SQLite en '" << resolved.string() << "': "
              << (g_auth_db ? sqlite3_errmsg(g_auth_db)
                            : sqlite3_errstr(rc));
            last_init_error_ = m.str();
            if (g_auth_db) { sqlite3_close(g_auth_db); g_auth_db = nullptr; }
            enabled_ = false;
            return false;
        }

        // Crear la tabla si no existe.
        const char *schema =
            "CREATE TABLE IF NOT EXISTS vm_users ("
            "  id            INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  username      TEXT    UNIQUE NOT NULL,"
            "  password_hash BLOB    NOT NULL,"
            "  salt          BLOB    NOT NULL,"
            "  role          TEXT    NOT NULL,"
            "  created_at    INTEGER NOT NULL"
            ");";
        char *err_msg = nullptr;
        rc = sqlite3_exec(g_auth_db, schema, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            std::ostringstream m;
            m << "fallo CREATE TABLE en '" << resolved.string() << "': "
              << (err_msg ? err_msg : sqlite3_errmsg(g_auth_db));
            last_init_error_ = m.str();
            if (err_msg) sqlite3_free(err_msg);
            sqlite3_close(g_auth_db);
            g_auth_db = nullptr;
            enabled_ = false;
            return false;
        }

        db_path_ = resolved.string();
        enabled_ = true;
        return true;
    }

    bool AuthManager::has_any_user() {
        std::lock_guard<std::mutex> lk(db_mutex_);
        if (!g_auth_db) return false;
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT COUNT(*) FROM vm_users;";
        if (sqlite3_prepare_v2(g_auth_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        bool any = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            any = sqlite3_column_int(stmt, 0) > 0;
        }
        sqlite3_finalize(stmt);
        return any;
    }

    bool AuthManager::rand_bytes(uint8_t *out, size_t n) {
        // RAND_bytes devuelve 1 si exito, 0 o -1 si fallo.
        return RAND_bytes(out, static_cast<int>(n)) == 1;
    }

    bool AuthManager::derive_key(const std::string &password,
                                 const uint8_t     *salt,
                                 size_t             salt_len,
                                 uint8_t           *out_hash,
                                 size_t             out_len) {
        // PKCS5_PBKDF2_HMAC con SHA-256 esta disponible en OpenSSL 1.1+.
        int rc = PKCS5_PBKDF2_HMAC(
            password.data(), static_cast<int>(password.size()),
            salt, static_cast<int>(salt_len),
            PBKDF2_ITERS,
            EVP_sha256(),
            static_cast<int>(out_len), out_hash);
        return rc == 1;
    }

    bool AuthManager::const_time_eq(const uint8_t *a, const uint8_t *b, size_t n) {
        // Comparacion bit a bit sin early-exit para evitar timing attacks.
        uint8_t diff = 0;
        for (size_t i = 0; i < n; ++i) diff |= a[i] ^ b[i];
        return diff == 0;
    }

    bool AuthManager::create_user(const std::string &username,
                                  const std::string &password,
                                  Role role,
                                  std::string *err) {
        if (username.empty() || username.size() > 64) {
            if (err) *err = "username vacio o demasiado largo";
            return false;
        }
        if (password.empty()) {
            if (err) *err = "password vacia";
            return false;
        }
        std::lock_guard<std::mutex> lk(db_mutex_);
        if (!g_auth_db) {
            if (err) *err = "auth no inicializado";
            return false;
        }

        uint8_t salt[PBKDF2_SALT_LEN];
        uint8_t hash[PBKDF2_HASH_LEN];
        if (!rand_bytes(salt, sizeof(salt))) {
            if (err) *err = "RAND_bytes fallo";
            return false;
        }
        if (!derive_key(password, salt, sizeof(salt), hash, sizeof(hash))) {
            if (err) *err = "PBKDF2 fallo";
            return false;
        }

        const char *sql =
            "INSERT INTO vm_users (username, password_hash, salt, role, created_at) "
            "VALUES (?, ?, ?, ?, ?);";
        sqlite3_stmt *stmt = nullptr;
        if (sqlite3_prepare_v2(g_auth_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            if (err) *err = sqlite3_errmsg(g_auth_db);
            return false;
        }
        sqlite3_bind_text(stmt, 1, username.c_str(),
                          static_cast<int>(username.size()), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, hash, sizeof(hash), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, salt, sizeof(salt), SQLITE_TRANSIENT);
        const char *role_s = role_to_string(role);
        sqlite3_bind_text(stmt, 4, role_s, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 5,
            static_cast<sqlite3_int64>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            if (err) *err = sqlite3_errmsg(g_auth_db);
            return false;
        }
        return true;
    }

    bool AuthManager::create_user_if_missing(const std::string &username,
                                             const std::string &password,
                                             Role role) {
        {
            std::lock_guard<std::mutex> lk(db_mutex_);
            if (!g_auth_db) return false;
            sqlite3_stmt *stmt = nullptr;
            const char *sql = "SELECT 1 FROM vm_users WHERE username = ? LIMIT 1;";
            if (sqlite3_prepare_v2(g_auth_db, sql, -1, &stmt, nullptr) != SQLITE_OK)
                return false;
            sqlite3_bind_text(stmt, 1, username.c_str(),
                              static_cast<int>(username.size()),
                              SQLITE_TRANSIENT);
            int rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc == SQLITE_ROW) return true; // ya existe
        }
        return create_user(username, password, role);
    }

    bool AuthManager::delete_user(const std::string &username, std::string *err) {
        std::lock_guard<std::mutex> lk(db_mutex_);
        if (!g_auth_db) {
            if (err) *err = "auth no inicializado";
            return false;
        }
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "DELETE FROM vm_users WHERE username = ?;";
        if (sqlite3_prepare_v2(g_auth_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            if (err) *err = sqlite3_errmsg(g_auth_db);
            return false;
        }
        sqlite3_bind_text(stmt, 1, username.c_str(),
                          static_cast<int>(username.size()), SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }

    bool AuthManager::change_password(const std::string &username,
                                      const std::string &new_password,
                                      std::string *err) {
        if (new_password.empty()) {
            if (err) *err = "password vacia";
            return false;
        }
        std::lock_guard<std::mutex> lk(db_mutex_);
        if (!g_auth_db) {
            if (err) *err = "auth no inicializado";
            return false;
        }
        uint8_t salt[PBKDF2_SALT_LEN];
        uint8_t hash[PBKDF2_HASH_LEN];
        if (!rand_bytes(salt, sizeof(salt))) {
            if (err) *err = "RAND_bytes fallo";
            return false;
        }
        if (!derive_key(new_password, salt, sizeof(salt), hash, sizeof(hash))) {
            if (err) *err = "PBKDF2 fallo";
            return false;
        }
        sqlite3_stmt *stmt = nullptr;
        const char *sql =
            "UPDATE vm_users SET password_hash = ?, salt = ? WHERE username = ?;";
        if (sqlite3_prepare_v2(g_auth_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            if (err) *err = sqlite3_errmsg(g_auth_db);
            return false;
        }
        sqlite3_bind_blob(stmt, 1, hash, sizeof(hash), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, salt, sizeof(salt), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, username.c_str(),
                          static_cast<int>(username.size()), SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        int changes = sqlite3_changes(g_auth_db);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            if (err) *err = sqlite3_errmsg(g_auth_db);
            return false;
        }
        if (changes == 0) {
            if (err) *err = "usuario no encontrado";
            return false;
        }
        return true;
    }

    std::vector<std::pair<std::string, Role>> AuthManager::list_users() {
        std::vector<std::pair<std::string, Role>> out;
        std::lock_guard<std::mutex> lk(db_mutex_);
        if (!g_auth_db) return out;
        sqlite3_stmt *stmt = nullptr;
        const char *sql = "SELECT username, role FROM vm_users ORDER BY id;";
        if (sqlite3_prepare_v2(g_auth_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return out;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char *u = sqlite3_column_text(stmt, 0);
            const unsigned char *r = sqlite3_column_text(stmt, 1);
            if (!u || !r) continue;
            out.emplace_back(reinterpret_cast<const char *>(u),
                             role_from_string(reinterpret_cast<const char *>(r)));
        }
        sqlite3_finalize(stmt);
        return out;
    }

    AuthResult AuthManager::login(const std::string &username,
                                  const std::string &password,
                                  uint32_t session_ttl_sec) {
        AuthResult res;
        if (username.empty() || password.empty()) {
            res.error = "credenciales vacias";
            return res;
        }

        uint8_t stored_hash[PBKDF2_HASH_LEN];
        uint8_t stored_salt[PBKDF2_SALT_LEN];
        std::string role_str;

        {
            std::lock_guard<std::mutex> lk(db_mutex_);
            if (!g_auth_db) {
                res.error = "auth no inicializado";
                return res;
            }
            sqlite3_stmt *stmt = nullptr;
            const char *sql =
                "SELECT password_hash, salt, role FROM vm_users WHERE username = ?;";
            if (sqlite3_prepare_v2(g_auth_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                res.error = "prepare fallo";
                return res;
            }
            sqlite3_bind_text(stmt, 1, username.c_str(),
                              static_cast<int>(username.size()),
                              SQLITE_TRANSIENT);
            int rc = sqlite3_step(stmt);
            if (rc != SQLITE_ROW) {
                sqlite3_finalize(stmt);
                // Mensaje neutro para no filtrar la existencia del usuario.
                res.error = "credenciales invalidas";
                return res;
            }
            const void *h_blob = sqlite3_column_blob(stmt, 0);
            int          h_n   = sqlite3_column_bytes(stmt, 0);
            const void *s_blob = sqlite3_column_blob(stmt, 1);
            int          s_n   = sqlite3_column_bytes(stmt, 1);
            const unsigned char *r_text = sqlite3_column_text(stmt, 2);

            if (h_n != static_cast<int>(PBKDF2_HASH_LEN) ||
                s_n != static_cast<int>(PBKDF2_SALT_LEN) ||
                !h_blob || !s_blob || !r_text) {
                sqlite3_finalize(stmt);
                res.error = "registro corrupto";
                return res;
            }
            std::memcpy(stored_hash, h_blob, PBKDF2_HASH_LEN);
            std::memcpy(stored_salt, s_blob, PBKDF2_SALT_LEN);
            role_str = reinterpret_cast<const char *>(r_text);
            sqlite3_finalize(stmt);
        }

        // Derivar y comparar fuera del lock para minimizar contention.
        uint8_t computed[PBKDF2_HASH_LEN];
        if (!derive_key(password, stored_salt, PBKDF2_SALT_LEN,
                        computed, PBKDF2_HASH_LEN)) {
            res.error = "PBKDF2 fallo";
            return res;
        }
        if (!const_time_eq(computed, stored_hash, PBKDF2_HASH_LEN)) {
            res.error = "credenciales invalidas";
            return res;
        }

        // Crear sesion: token = 32 bytes random en hex (64 chars).
        uint8_t token_bytes[TOKEN_RAW_BYTES];
        if (!rand_bytes(token_bytes, sizeof(token_bytes))) {
            res.error = "RAND_bytes fallo";
            return res;
        }
        std::string token = to_hex(token_bytes, TOKEN_RAW_BYTES);

        Session sess;
        sess.username   = username;
        sess.role       = role_from_string(role_str);
        sess.created_at = std::chrono::steady_clock::now();
        sess.expires_at = sess.created_at + std::chrono::seconds(session_ttl_sec);

        {
            std::lock_guard<std::mutex> lk(session_mutex_);
            sessions_[token] = sess;
        }

        res.ok       = true;
        res.token    = std::move(token);
        res.username = sess.username;
        res.role     = sess.role;
        return res;
    }

    void AuthManager::logout(const std::string &token) {
        std::lock_guard<std::mutex> lk(session_mutex_);
        sessions_.erase(token);
    }

    bool AuthManager::validate_token(const std::string &token, Session *out_session) {
        std::lock_guard<std::mutex> lk(session_mutex_);
        auto it = sessions_.find(token);
        if (it == sessions_.end()) return false;
        if (std::chrono::steady_clock::now() > it->second.expires_at) {
            sessions_.erase(it);
            return false;
        }
        if (out_session) *out_session = it->second;
        return true;
    }

    bool AuthManager::token_has_role(const std::string &token, Role min) {
        Session sess;
        if (!validate_token(token, &sess)) return false;
        return static_cast<uint8_t>(sess.role) >= static_cast<uint8_t>(min);
    }

} // namespace debug
