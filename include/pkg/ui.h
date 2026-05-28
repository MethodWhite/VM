/**
 * @file ui.h
 * @brief Helper de UI para el package manager: colores ANSI + barras de
 *        progreso + spinners + mensajes formateados.
 *
 * Output con color SOLO si stdout es una terminal (TTY).  Detecta
 * Windows 10+ ANSI VT-100 support; cae a output plano en pipes/
 * redirects para no contaminar logs.
 *
 * Codigos de color usados:
 *   - rojo:    errores y mensajes destructivos
 *   - amarillo: warnings, paquetes unsafe, decisiones delicadas
 *   - verde:   exito, OK, paquetes verificados
 *   - cyan:    info, headers, hints
 *   - magenta: identificadores (nombres de paquete, hashes)
 *   - gris:    detalles secundarios, metadata
 *
 * Diseno:
 *   pkg::ui::error("mensaje")     -> rojo + prefix [ERROR]
 *   pkg::ui::warn("mensaje")      -> amarillo + prefix [WARN]
 *   pkg::ui::ok("mensaje")        -> verde + prefix [OK]
 *   pkg::ui::info("mensaje")      -> cyan + prefix [INFO]
 *   pkg::ui::step("mensaje")      -> negrita + arrow para steps de proceso
 *
 *   pkg::ui::Progress p(total);
 *   for (...) { p.tick(); }
 *   p.done();
 *
 *   pkg::ui::Spinner s("Downloading...");
 *   s.tick();      // animacion |/-\
 *   s.done("OK");  // reemplaza con check verde
 */
#ifndef VESTAVM_PKG_UI_H
#define VESTAVM_PKG_UI_H

#include <string>
#include <string_view>
#include <chrono>
#include <cstdint>

namespace pkg::ui {

    /**
     * @brief Inicializa el subsistema de UI.  Detecta TTY + habilita ANSI
     *        en consola Windows 10+.  Idempotente; se puede llamar varias
     *        veces.
     */
    void init();

    /**
     * @brief True si el output actual soporta colores ANSI.
     */
    bool color_enabled() noexcept;

    /**
     * @brief Forzar desactivacion del color (e.g. flag --no-color del CLI).
     */
    void disable_color() noexcept;

    /**
     * @brief Codigos ANSI basicos como strings; vacios si color desactivado.
     */
    const char *red() noexcept;
    const char *green() noexcept;
    const char *yellow() noexcept;
    const char *blue() noexcept;
    const char *cyan() noexcept;
    const char *magenta() noexcept;
    const char *gray() noexcept;
    const char *bold() noexcept;
    const char *dim() noexcept;
    const char *reset() noexcept;

    // ----------------------------------------------------------------
    // Mensajes con prefix y color
    // ----------------------------------------------------------------

    /** @brief @c [ERROR] en rojo + mensaje.  Va a stderr. */
    void error(std::string_view msg);

    /** @brief @c [WARN] en amarillo + mensaje.  Va a stderr. */
    void warn(std::string_view msg);

    /** @brief @c [OK] en verde + mensaje. */
    void ok(std::string_view msg);

    /** @brief @c [INFO] en cyan + mensaje. */
    void info(std::string_view msg);

    /** @brief Paso de un proceso: flecha en bold + mensaje. */
    void step(std::string_view msg);

    /** @brief Sub-paso indentado en gris. */
    void detail(std::string_view msg);

    /** @brief Encabezado destacado en cyan + bold con linea horizontal. */
    void header(std::string_view title);

    /** @brief Imprime un par clave: valor con colores. */
    void kv(std::string_view key, std::string_view value);

    /** @brief Imprime nombre de paquete formateado (magenta + bold). */
    std::string pkg_name(std::string_view name);

    /** @brief Imprime version formateada (gris). */
    std::string pkg_version(std::string_view version);

    /** @brief Imprime hash truncado a 12 chars con elipsis (gris). */
    std::string short_hash(std::string_view hash);

    // ----------------------------------------------------------------
    // Barra de progreso para descargas/operaciones con total conocido
    // ----------------------------------------------------------------

    /**
     * @brief Barra de progreso estilo `[###----] 42% (123/456 KB)`.
     *
     * Uso:
     * @code
     * pkg::ui::Progress p("Downloading buffer", 1024*100);
     * for (size_t i = 0; i < 100; ++i) {
     *     p.set(i * 1024);
     *     std::this_thread::sleep_for(std::chrono::milliseconds(50));
     * }
     * p.done();
     * @endcode
     *
     * Si stdout no es TTY, emite progreso periodico en formato simple
     * (una linea por 10% incremento; sin sobreescribir).
     */
    class Progress {
    public:
        /**
         * @param title         Label corto de la operacion.
         * @param total_bytes   Total esperado en bytes; 0 = unknown
         *                      (cambia a spinner mode).
         */
        Progress(std::string title, uint64_t total_bytes = 0);
        ~Progress();

        /** @brief Avanza la barra al valor absoluto en bytes. */
        void set(uint64_t current_bytes) noexcept;

        /** @brief Incrementa la barra por @p delta bytes. */
        void add(uint64_t delta) noexcept;

        /** @brief Cambia el label sin reset del contador. */
        void set_label(std::string label) noexcept;

        /** @brief Finaliza la barra; rellena al 100% y nueva linea. */
        void done() noexcept;

        /** @brief Finaliza con un mensaje de error en rojo. */
        void fail(std::string_view reason) noexcept;

    private:
        std::string title_;
        uint64_t    total_   = 0;
        uint64_t    current_ = 0;
        bool        is_tty_  = false;
        bool        finished_ = false;
        std::chrono::steady_clock::time_point last_redraw_;
        uint64_t    last_step_pct_ = 0;

        void redraw() noexcept;
    };

    // ----------------------------------------------------------------
    // Spinner para operaciones de duracion desconocida
    // ----------------------------------------------------------------

    /**
     * @brief Spinner animado para operaciones sin progreso medible.
     *
     * Uso:
     * @code
     * pkg::ui::Spinner s("Resolving dependencies");
     * resolve_all();
     * s.done("OK");   // verde check al final
     * @endcode
     *
     * En non-TTY emite la linea inicial sin animacion; @c done()
     * sobreescribe la misma linea.
     */
    class Spinner {
    public:
        explicit Spinner(std::string label);
        ~Spinner();

        /** @brief Avanza un frame de la animacion (llamada periodica). */
        void tick() noexcept;

        /** @brief Cambia el label sin reset. */
        void set_label(std::string label) noexcept;

        /** @brief Termina con mensaje verde de exito. */
        void done(std::string_view message = "OK") noexcept;

        /** @brief Termina con mensaje rojo de error. */
        void fail(std::string_view message) noexcept;

    private:
        std::string label_;
        size_t      frame_   = 0;
        bool        is_tty_  = false;
        bool        finished_ = false;
        std::chrono::steady_clock::time_point last_tick_;

        void redraw() noexcept;
    };

    // ----------------------------------------------------------------
    // Prompts interactivos
    // ----------------------------------------------------------------

    /**
     * @brief Pregunta yes/no al usuario.  Default @p default_yes si vacio.
     *
     * @return true si yes, false si no.
     */
    bool prompt_yes_no(std::string_view question, bool default_yes = false);

    /**
     * @brief Lee una linea del usuario con un prompt.  Vacio = default.
     */
    std::string prompt_line(std::string_view question,
                             std::string_view default_value = "");

} // namespace pkg::ui

#endif // VESTAVM_PKG_UI_H
