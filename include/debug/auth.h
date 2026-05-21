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
 * @file auth.h
 * @brief Sistema de autenticacion y autorizacion para el servidor de depuracion.
 *
 * Provee un singleton @c AuthManager que gestiona:
 *   - Almacen de usuarios en SQLite (tabla @c vm_users), via la conexion
 *     compartida con @c SqliteSingleton.
 *   - Hashing seguro de contrasenyas con PBKDF2-HMAC-SHA256 (200000 iter,
 *     32 bytes de hash + 16 bytes de salt random por usuario).
 *   - Tokens de sesion criptograficamente aleatorios (32 bytes hex) con
 *     expiracion configurable.
 *   - Roles fijos: @c admin (acceso total), @c developer (cargar/inspeccionar
 *     codigo), @c viewer (solo lectura).
 *
 * El esquema de la tabla SQLite es:
 *   CREATE TABLE IF NOT EXISTS vm_users (
 *     id            INTEGER PRIMARY KEY AUTOINCREMENT,
 *     username      TEXT    UNIQUE NOT NULL,
 *     password_hash BLOB    NOT NULL,
 *     salt          BLOB    NOT NULL,
 *     role          TEXT    NOT NULL,
 *     created_at    INTEGER NOT NULL
 *   );
 *
 * Las sesiones viven solo en memoria; un reinicio del servidor invalida
 * todos los tokens.  Esto es intencional: no se persisten credenciales
 * activas en disco.
 */

#ifndef DEBUG_AUTH_H
#define DEBUG_AUTH_H

#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <chrono>

namespace debug {

    /**
     * @brief Roles soportados por el servidor de depuracion.
     *
     * Cada comando del protocolo declara su rol minimo requerido.  El
     * AuthManager permite el comando si el rol del usuario es >= al
     * minimo en la siguiente jerarquia:
     *     VIEWER (0) < DEVELOPER (1) < ADMIN (2)
     */
    enum class Role : uint8_t {
        VIEWER    = 0, ///< solo lectura (registros, list_procs, server_info)
        DEVELOPER = 1, ///< puede cargar codigo y manipular procesos
        ADMIN     = 2, ///< control total (apagado, gestion de usuarios, fs_*)
    };

    /**
     * @brief Convierte un string a Role.  Acepta @c "admin", @c "developer",
     *        @c "viewer" (case-insensitive).  Devuelve @c VIEWER si no
     *        coincide con ningun rol conocido.
     */
    Role role_from_string(const std::string &s);

    /**
     * @brief Devuelve el nombre canonico (lowercase) del rol.
     */
    const char *role_to_string(Role r);

    /**
     * @brief Resultado de una autenticacion.  Si @c ok es true, @c token
     *        contiene el session token de 64 caracteres hex y @c role el
     *        rol del usuario.  Si @c ok es false, @c error contiene el
     *        mensaje de error.
     */
    struct AuthResult {
        bool        ok = false;
        std::string token;
        std::string username;
        Role        role = Role::VIEWER;
        std::string error;
    };

    /**
     * @brief Sesion activa de un usuario autenticado.
     */
    struct Session {
        std::string username;
        Role        role;
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point expires_at;
    };

    /**
     * @brief Singleton de autenticacion para el servidor de depuracion.
     *
     * Uso tipico:
     * @code
     *   auto &auth = AuthManager::instance();
     *   auth.init("vm_auth.db");
     *   auth.create_user_if_missing("admin", "secret", Role::ADMIN);
     *   AuthResult r = auth.login("admin", "secret");
     *   if (r.ok) { ... usar r.token ... }
     * @endcode
     */
    class AuthManager {
    public:
        /**
         * @brief Devuelve la instancia singleton.
         */
        static AuthManager &instance();

        /**
         * @brief Inicializa la base de datos de autenticacion.
         *
         * Abre (o crea) el archivo SQLite en @p db_path, crea la tabla
         * @c vm_users si no existe y prepara el RNG de OpenSSL.  Es
         * idempotente: invocaciones posteriores con el mismo path se
         * ignoran; con un path distinto cierran la conexion anterior.
         *
         * @param db_path Ruta al fichero @c .db de autenticacion.
         * @return true si la inicializacion fue correcta.
         */
        bool init(const std::string &db_path);

        /**
         * @brief Devuelve el mensaje del ultimo error de init().
         *
         * Util cuando @c init devuelve false: contiene el mensaje de
         * SQLite (e.g. "unable to open database file") + la ruta
         * absoluta que se intento abrir.  Vacio si no hubo error.
         */
        const std::string &last_init_error() const { return last_init_error_; }

        /**
         * @brief Comprueba si hay al menos un usuario registrado.
         *
         * Util al arranque para decidir si hay que crear un admin por
         * defecto cuando la base de datos esta vacia.
         */
        bool has_any_user();

        /**
         * @brief Crea un usuario nuevo.
         *
         * Falla si @p username ya existe.  La contrasenya se almacena
         * como hash PBKDF2-HMAC-SHA256 con salt random de 16 bytes y
         * 200000 iteraciones.
         *
         * @param username Nombre del usuario (1-64 caracteres ASCII).
         * @param password Contrasenya en claro (no se almacena).
         * @param role     Rol asignado.
         * @param err      Salida opcional con mensaje de error.
         * @return true si se creo el usuario.
         */
        bool create_user(const std::string &username,
                         const std::string &password,
                         Role role,
                         std::string *err = nullptr);

        /**
         * @brief Crea el usuario solo si NO existe ya.
         *
         * Equivalente a @c create_user pero idempotente: si el usuario
         * ya esta en la base de datos, retorna true sin modificar nada.
         */
        bool create_user_if_missing(const std::string &username,
                                    const std::string &password,
                                    Role role);

        /**
         * @brief Elimina un usuario por nombre.  Devuelve true si se
         *        elimino o si no existia.  Las sesiones activas de ese
         *        usuario quedan invalidadas en la proxima validacion.
         */
        bool delete_user(const std::string &username, std::string *err = nullptr);

        /**
         * @brief Cambia la contrasenya de un usuario.
         */
        bool change_password(const std::string &username,
                             const std::string &new_password,
                             std::string *err = nullptr);

        /**
         * @brief Lista todos los usuarios (sin hashes ni salts).
         *
         * @return Vector de tuplas @c (username, role) ordenadas por id.
         */
        std::vector<std::pair<std::string, Role>> list_users();

        /**
         * @brief Verifica credenciales y crea una sesion.
         *
         * Si las credenciales son validas, genera un token de 32 bytes
         * aleatorios (formato hex de 64 chars), lo registra con TTL
         * @p session_ttl_sec y lo devuelve en el AuthResult.
         */
        AuthResult login(const std::string &username,
                         const std::string &password,
                         uint32_t session_ttl_sec = 3600);

        /**
         * @brief Invalida una sesion existente.  No falla si el token
         *        ya no existe.
         */
        void logout(const std::string &token);

        /**
         * @brief Valida un token de sesion.
         *
         * @param token Token de sesion.
         * @param out_session Salida opcional con datos de la sesion.
         * @return true si el token existe y no ha expirado.
         */
        bool validate_token(const std::string &token, Session *out_session = nullptr);

        /**
         * @brief Comprueba si el token pertenece a un usuario con rol >= @p min.
         */
        bool token_has_role(const std::string &token, Role min);

        /**
         * @brief Indica si AuthManager esta operativo (init() exitoso).
         *
         * Cuando es false (no se invoco init), el modo --server-mode
         * debe abrirse SIN exigir autenticacion (back-compat).
         */
        bool is_enabled() const { return enabled_; }

    private:
        AuthManager() = default;
        ~AuthManager() = default;
        AuthManager(const AuthManager &) = delete;
        AuthManager &operator=(const AuthManager &) = delete;

        /**
         * @brief Genera bytes criptograficamente aleatorios via @c RAND_bytes.
         */
        bool rand_bytes(uint8_t *out, size_t n);

        /**
         * @brief Calcula PBKDF2-HMAC-SHA256(password, salt, 200000) -> 32 bytes.
         */
        bool derive_key(const std::string &password,
                        const uint8_t     *salt,
                        size_t             salt_len,
                        uint8_t           *out_hash,
                        size_t             out_len);

        /**
         * @brief Compara dos buffers en tiempo constante.
         */
        static bool const_time_eq(const uint8_t *a, const uint8_t *b, size_t n);

        bool        enabled_ = false;          ///< true si init() fue OK
        std::string db_path_;                  ///< path activo de la BD
        std::string last_init_error_;          ///< ultimo error de init()
        std::mutex  db_mutex_;                 ///< serializa accesos al SQLite

        std::mutex                              session_mutex_;
        std::unordered_map<std::string, Session> sessions_;
    };

} // namespace debug

#endif // DEBUG_AUTH_H
