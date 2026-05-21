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
 * @file cli.h
 * @brief Interfaz del interprete interactivo (REPL) y del gestor de vista de la CLI de VestaVM.
 *
 * Define:
 *   - CmdParams: estructura con los campos parseados de un comando de usuario.
 *   - extract_token(): extrae un token de una cadena respetando comillas y escapes.
 *   - parse_cmd_params(): parsea name + path + rest de una linea de comando.
 *   - VestaInterprete: REPL basico que ensambla y ejecuta fragmentos de codigo Vesta.
 *   - Config: opciones de configuracion del REPL (historial, prompt, terminador multilinea).
 *   - VestaViewManager: gestor del REPL con historial, callback de ejecucion y soporte multihilo.
 */

#ifndef CLI_H
#define CLI_H

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <atomic>
#include <algorithm>
#include <csignal>
#include <functional>
#include <sstream>
#include <filesystem>
#include <cstdlib>
#include <optional>
#include <iostream>

#include "runtime/manager_runtime.h"
#include "runtime/runtime.h"
#include "util/fs_utils.h"

namespace cli {

    struct Impl; ///< Implementacion interna de VestaViewManager (opaque pointer)

    /**
     * @brief Resultado del parseo de una linea de comando con nombre, ruta y argumentos extra.
     *
     * Se rellena por parse_cmd_params() y se usa para pasar parametros a los
     * distintos manejadores de comandos del REPL.
     */
    struct CmdParams {
        std::string name;          ///< Primer token: nombre del comando o identificador
        std::string path;          ///< Segundo token: ruta al ejecutable o argumento principal
        std::string rest;          ///< Resto de la cadena tras name y path (puede estar vacio)
        bool        valid = false; ///< true si se extrajeron al menos name y path correctamente
    };

    /**
     * @brief Extrae un token de la cadena @p s a partir de la posicion @p pos.
     *
     * Salta espacios iniciales y acumula caracteres hasta encontrar un separador de token.
     * Respeta comillas simples y dobles: los caracteres dentro de comillas se toman literalmente.
     * Soporta escapes con backslash tanto dentro como fuera de comillas.
     *
     * @param s   Cadena de entrada completa.
     * @param pos Posicion de inicio en @p s (se actualiza a la posicion posterior al token).
     * @return Token extraido; cadena vacia si la posicion esta al final de la cadena.
     */
    static std::string extract_token(const std::string &s, size_t &pos) {
        // saltar espacios iniciales antes del token
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
        if (pos >= s.size()) return {};

        std::string out;
        char        quote = 0; // caracter de comilla activo (0 si no hay comillas)

        // detectar apertura de comillas
        if (s[pos] == '"' || s[pos] == '\'') {
            quote = s[pos++];
        }

        while (pos < s.size()) {
            char c = s[pos++];
            if (quote) {
                if (c == '\\' && pos < s.size()) {
                    // secuencia de escape dentro de comillas: tomar el siguiente caracter literalmente
                    char next = s[pos++];
                    out.push_back(next);
                } else if (c == quote) {
                    break; // cierre de comillas: terminar token
                } else {
                    out.push_back(c);
                }
            } else {
                if (std::isspace(static_cast<unsigned char>(c))) {
                    break; // espacio fuera de comillas: fin de token
                } else if (c == '\\' && pos < s.size()) {
                    // escape fuera de comillas: permite espacios escapados dentro del token
                    char next = s[pos++];
                    out.push_back(next);
                } else {
                    out.push_back(c);
                }
            }
        }

        return out;
    }

    /**
     * @brief Interprete REPL basico para fragmentos de codigo Vesta.
     *
     * Permite introducir lineas de codigo Vesta de forma interactiva, ensamblarlas
     * y ejecutarlas en una instancia VM interna.  Usado principalmente para depuracion
     * y pruebas rapidas de instrucciones.
     */
    class VestaInterprete {
        bool              running       = true;        ///< Indica si el interprete sigue activo
        bool              show_bytecode = true;        ///< Si true imprime el bytecode generado
        runtime::ManageVM manager       = runtime::ManageVM(nullptr, 0); ///< Gestor VM interno
        std::string       code;                        ///< Buffer de codigo acumulado

    public:
        /**
         * @brief Construye el interprete e inicializa el gestor VM interno.
         */
        VestaInterprete();

        /**
         * @brief Ensambla el codigo acumulado en @p code y lo ejecuta en la VM interna.
         *
         * @return Vector de bytes del bytecode generado por el ensamblado.
         */
        std::vector<uint8_t> run_interprete_execute_code();

        /**
         * @brief Divide la cadena @p input en tokens separados por espacios.
         *
         * @param input Linea de entrada del usuario.
         * @return Vector de tokens extraidos de la linea.
         */
        std::vector<std::string> tokenize(const std::string &input);

        /**
         * @brief Inicia el bucle interactivo del interprete (bloqueante hasta que el usuario sale).
         */
        void run_interprete();
    };

    /**
     * @brief Parsea la linea @p cmd extrayendo name (primer token) y path (segundo token).
     *
     * El resto de la cadena (tras saltarse los espacios que siguen a path) se almacena
     * en CmdParams::rest tal cual, para que el manejador del comando lo procese segun necesite.
     *
     * @code{.cpp}
     * auto p1 = parse_cmd_params(R"(myname /home/user/vm.bin)");
     * // p1.name == "myname", p1.path == "/home/user/vm.bin"
     *
     * auto p2 = parse_cmd_params(R"("My Name" "C:\Archivos de programa\VM\vm.exe" --flag)");
     * // p2.name == "My Name", p2.path == "C:\Archivos de programa\VM\vm.exe", p2.rest == "--flag"
     * @endcode
     *
     * @param cmd Cadena completa del comando introducido por el usuario.
     * @return CmdParams con los campos rellenos; valid=false si no se pudo extraer name+path.
     */
    static CmdParams parse_cmd_params(const std::string &cmd) {
        CmdParams out;
        size_t    pos = 0;

        out.name = extract_token(cmd, pos);  // primer token: nombre
        if (out.name.empty()) return out;    // comando invalido sin nombre

        out.path = extract_token(cmd, pos);  // segundo token: ruta
        if (out.path.empty()) return out;    // comando invalido sin ruta

        // saltar espacios y capturar el resto de la cadena sin modificar
        while (pos < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[pos]))) ++pos;
        if (pos < cmd.size()) out.rest = cmd.substr(pos);
        out.valid = true;
        return out;
    }

    /**
     * @brief Opciones de configuracion del gestor de vista del REPL.
     */
    struct Config {
        std::string history_file  = "vm_history.txt"; ///< Ruta del fichero de historial de comandos
        std::string aliases_file  = "vm_aliases.txt"; ///< Ruta del fichero de persistencia de aliases
        size_t      history_max   = 2000;              ///< Numero maximo de entradas en el historial
        std::string prompt        = "vm> ";            ///< Cadena mostrada como prompt del REPL (legacy; no usado con prompt dinamico)
        std::string multiline_end = ";;";              ///< Terminador de bloque multilinea
    };

    /**
     * @brief Gestor interactivo del REPL con historial y soporte multihilo.
     *
     * VestaViewManager implementa el bucle de lectura de comandos con historial persistente,
     * un callback de ejecucion inyectable y deteccion de modo multilinea.  Internamente usa
     * un hilo de trabajo para procesar comandos sin bloquear el hilo principal.
     *
     * Ciclo de uso tipico:
     * @code
     *   cli::VestaViewManager mgr(config);
     *   mgr.set_execute_callback([](const std::string &cmd) { ... });
     *   mgr.run();   // bloqueante hasta que el usuario escribe "exit"
     * @endcode
     */
    class VestaViewManager {
    public:
        /**
         * @brief Construye el gestor con la configuracion indicada.
         *
         * @param cfg Opciones de configuracion (prompt, historial, terminador multilinea).
         */
        explicit VestaViewManager(Config cfg = Config());

        /**
         * @brief Destructor: detiene el worker y libera la implementacion interna.
         */
        ~VestaViewManager();

        /**
         * @brief Registra el callback que se invoca cada vez que el usuario introduce un comando.
         *
         * El callback recibe la linea completa (o el bloque multilinea completo) ya validada.
         *
         * @param cb Funcion con la firma void(const std::string &).
         */
        void set_execute_callback(std::function<void(const std::string &)> cb);

        /**
         * @brief Inicia el REPL y bloquea hasta que el usuario lo abandona.
         *
         * Lee lineas de stdin, gestiona el historial y delega la ejecucion al callback.
         */
        void run();

        /**
         * @brief Detiene el gestor y espera a que el hilo worker termine.
         */
        void stop();

    private:
        /// No copiar ni mover: el estado interno contiene hilos y mutexes
        VestaViewManager(const VestaViewManager &) = delete;
        VestaViewManager &operator=(const VestaViewManager &) = delete;

        Impl *impl_; ///< Implementacion interna (opaque pointer, definida en cli.cpp)
    };

    /**
     * @brief Ejecuta una linea del REPL (igual que el prompt `vesta>`) y
     *        captura todo lo que el comando imprimiria a stdout en un
     *        string que se devuelve al caller.
     *
     * Reemplaza el @c rdbuf de @c std::cout temporalmente por un buffer
     * en memoria mientras dispacha el comando, asi todas las llamadas a
     * @c std::cout / @c vesta::scout() / @c printf van al string.  La
     * funcion es thread-safe via un mutex global porque @c std::cout es
     * un recurso compartido del proceso.
     *
     * @param line Linea cruda igual a la que el operador escribiria en
     *             el prompt.  Acepta comentarios (#), aliases, etc.
     * @return Cadena con la salida capturada del comando (puede ser
     *         vacia para comandos silenciosos).  Si la linea no
     *         corresponde a ningun comando registrado, devuelve un
     *         mensaje de error claro.
     */
    std::string execute_repl_line(const std::string &line);

} // namespace cli

#endif // CLI_H
