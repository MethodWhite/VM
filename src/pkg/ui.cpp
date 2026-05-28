/**
 * @file ui.cpp
 * @brief Implementacion del helper de UI con color y progress bars.
 */
#include "pkg/ui.h"

#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <cstdio>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <io.h>
#  define isatty _isatty
#  define fileno _fileno
#else
#  include <unistd.h>
#endif

namespace pkg::ui {

    namespace {
        std::atomic<bool> g_initialized{false};
        std::atomic<bool> g_color_enabled{false};
        std::atomic<bool> g_force_disabled{false};

        bool stdout_is_tty() {
            return ::isatty(::fileno(stdout)) != 0;
        }

        bool stderr_is_tty() {
            return ::isatty(::fileno(stderr)) != 0;
        }

#ifdef _WIN32
        bool enable_vt_processing(HANDLE h) {
            DWORD mode = 0;
            if (!GetConsoleMode(h, &mode)) return false;
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            return SetConsoleMode(h, mode) != 0;
        }
#endif
    } // namespace

    void init() {
        if (g_initialized.exchange(true)) return;
        if (g_force_disabled.load()) {
            g_color_enabled.store(false);
            return;
        }
        bool tty = stdout_is_tty() || stderr_is_tty();
#ifdef _WIN32
        if (tty) {
            // Activa procesamiento VT-100 en Windows 10+.  En consolas
            // antiguas (cmd.exe pre-VT) falla y dejamos color desactivado.
            HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
            HANDLE herr = GetStdHandle(STD_ERROR_HANDLE);
            bool ok = true;
            if (hout != INVALID_HANDLE_VALUE) ok &= enable_vt_processing(hout);
            if (herr != INVALID_HANDLE_VALUE) ok &= enable_vt_processing(herr);
            g_color_enabled.store(ok);
        } else {
            g_color_enabled.store(false);
        }
#else
        // Detectar NO_COLOR estandar de https://no-color.org/
        const char *no_color = std::getenv("NO_COLOR");
        if (no_color && *no_color) {
            g_color_enabled.store(false);
        } else {
            g_color_enabled.store(tty);
        }
#endif
    }

    bool color_enabled() noexcept {
        if (!g_initialized.load()) init();
        return g_color_enabled.load();
    }

    void disable_color() noexcept {
        g_force_disabled.store(true);
        g_color_enabled.store(false);
    }

    // ----------------------------------------------------------------
    // ANSI codes
    // ----------------------------------------------------------------

#define DEF_COLOR(name, code) \
    const char *name() noexcept { \
        return color_enabled() ? "\033[" code "m" : ""; \
    }

    DEF_COLOR(red,     "31")
    DEF_COLOR(green,   "32")
    DEF_COLOR(yellow,  "33")
    DEF_COLOR(blue,    "34")
    DEF_COLOR(magenta, "35")
    DEF_COLOR(cyan,    "36")
    DEF_COLOR(gray,    "90")
    DEF_COLOR(bold,    "1")
    DEF_COLOR(dim,     "2")
    DEF_COLOR(reset,   "0")

#undef DEF_COLOR

    // ----------------------------------------------------------------
    // Mensajes
    // ----------------------------------------------------------------

    static void emit(std::ostream &os, const char *col, const char *tag,
                      std::string_view msg) {
        os << col << tag << reset() << " " << msg << "\n";
    }

    void error(std::string_view msg) {
        emit(std::cerr, red(),    "[ERROR]", msg);
    }

    void warn(std::string_view msg) {
        emit(std::cerr, yellow(), "[WARN] ", msg);
    }

    void ok(std::string_view msg) {
        emit(std::cout, green(),  "[OK]   ", msg);
    }

    void info(std::string_view msg) {
        emit(std::cout, cyan(),   "[INFO] ", msg);
    }

    void step(std::string_view msg) {
        std::cout << bold() << cyan() << "==> " << reset()
                  << bold() << msg << reset() << "\n";
    }

    void detail(std::string_view msg) {
        std::cout << gray() << "    " << msg << reset() << "\n";
    }

    void header(std::string_view title) {
        std::cout << "\n" << cyan() << bold() << title << reset() << "\n";
        std::cout << cyan();
        for (size_t i = 0; i < title.size(); ++i) std::cout << "=";
        std::cout << reset() << "\n";
    }

    void kv(std::string_view key, std::string_view value) {
        std::cout << "  " << gray() << key << ":" << reset() << " "
                  << value << "\n";
    }

    std::string pkg_name(std::string_view name) {
        std::ostringstream s;
        s << magenta() << bold() << name << reset();
        return s.str();
    }

    std::string pkg_version(std::string_view version) {
        std::ostringstream s;
        s << gray() << version << reset();
        return s.str();
    }

    std::string short_hash(std::string_view hash) {
        std::ostringstream s;
        s << gray();
        if (hash.size() <= 12) {
            s << hash;
        } else {
            s << hash.substr(0, 10) << "..";
        }
        s << reset();
        return s.str();
    }

    // ----------------------------------------------------------------
    // Progress bar
    // ----------------------------------------------------------------

    Progress::Progress(std::string title, uint64_t total_bytes)
        : title_(std::move(title)),
          total_(total_bytes),
          is_tty_(stdout_is_tty()) {
        if (!g_initialized.load()) init();
        last_redraw_ = std::chrono::steady_clock::now();
        if (is_tty_) {
            redraw();
        } else {
            std::cout << "[ ] " << title_;
            if (total_) std::cout << " (0 B)";
            std::cout << "...\n";
        }
    }

    Progress::~Progress() {
        if (!finished_) done();
    }

    static std::string format_bytes(uint64_t n) {
        if (n < 1024) return std::to_string(n) + " B";
        double v = static_cast<double>(n);
        const char *units[] = {"B", "KB", "MB", "GB"};
        size_t u = 0;
        while (v >= 1024.0 && u + 1 < sizeof(units)/sizeof(units[0])) {
            v /= 1024.0;
            ++u;
        }
        std::ostringstream s;
        s << std::fixed << std::setprecision(1) << v << " " << units[u];
        return s.str();
    }

    void Progress::redraw() noexcept {
        if (!is_tty_ || finished_) return;
        constexpr size_t BAR_WIDTH = 30;
        uint64_t pct = 0;
        if (total_ > 0) {
            pct = (current_ * 100) / total_;
            if (pct > 100) pct = 100;
        }
        size_t filled = total_ > 0
            ? static_cast<size_t>(BAR_WIDTH * current_ / total_)
            : 0;
        if (filled > BAR_WIDTH) filled = BAR_WIDTH;

        std::ostringstream s;
        s << "\r" << cyan() << "[" << reset() << green();
        for (size_t i = 0; i < filled; ++i) s << "#";
        s << reset() << gray();
        for (size_t i = filled; i < BAR_WIDTH; ++i) s << "-";
        s << reset() << cyan() << "]" << reset();
        s << " " << bold() << std::setw(3) << pct << "%" << reset();
        if (total_ > 0) {
            s << " " << gray() << "("
              << format_bytes(current_) << "/" << format_bytes(total_)
              << ")" << reset();
        } else {
            s << " " << gray() << "(" << format_bytes(current_) << ")"
              << reset();
        }
        s << " " << title_ << "          ";  // padding to overwrite
        std::cout << s.str() << std::flush;
    }

    void Progress::set(uint64_t current_bytes) noexcept {
        current_ = current_bytes;
        if (is_tty_) {
            auto now = std::chrono::steady_clock::now();
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - last_redraw_).count();
            if (dt >= 50) {
                redraw();
                last_redraw_ = now;
            }
        } else if (total_ > 0) {
            uint64_t pct = (current_ * 100) / total_;
            if (pct / 10 != last_step_pct_ / 10) {
                std::cout << "  " << title_ << " " << pct << "% ("
                          << format_bytes(current_) << ")\n";
                last_step_pct_ = pct;
            }
        }
    }

    void Progress::add(uint64_t delta) noexcept {
        set(current_ + delta);
    }

    void Progress::set_label(std::string label) noexcept {
        title_ = std::move(label);
        if (is_tty_) redraw();
    }

    void Progress::done() noexcept {
        if (finished_) return;
        finished_ = true;
        if (is_tty_) {
            current_ = (total_ > 0) ? total_ : current_;
            redraw();
            std::cout << "\n";
        } else {
            std::cout << "  done: " << title_ << " (" << format_bytes(current_)
                      << ")\n";
        }
    }

    void Progress::fail(std::string_view reason) noexcept {
        if (finished_) return;
        finished_ = true;
        if (is_tty_) {
            std::cout << "\r" << red() << "[FAIL]" << reset() << " "
                      << title_ << ": " << reason << "                       \n";
        } else {
            std::cerr << "  failed: " << title_ << ": " << reason << "\n";
        }
    }

    // ----------------------------------------------------------------
    // Spinner
    // ----------------------------------------------------------------

    static const char *SPIN_FRAMES[] = {"|", "/", "-", "\\"};
    static constexpr size_t SPIN_N = 4;

    Spinner::Spinner(std::string label)
        : label_(std::move(label)),
          is_tty_(stdout_is_tty()) {
        if (!g_initialized.load()) init();
        last_tick_ = std::chrono::steady_clock::now();
        if (is_tty_) {
            redraw();
        } else {
            std::cout << "... " << label_ << "\n";
        }
    }

    Spinner::~Spinner() {
        if (!finished_) done();
    }

    void Spinner::redraw() noexcept {
        if (!is_tty_ || finished_) return;
        std::cout << "\r" << cyan() << SPIN_FRAMES[frame_ % SPIN_N]
                  << reset() << " " << label_ << "         " << std::flush;
    }

    void Spinner::tick() noexcept {
        if (!is_tty_ || finished_) return;
        auto now = std::chrono::steady_clock::now();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_tick_).count();
        if (dt >= 100) {
            ++frame_;
            redraw();
            last_tick_ = now;
        }
    }

    void Spinner::set_label(std::string label) noexcept {
        label_ = std::move(label);
        if (is_tty_) redraw();
    }

    void Spinner::done(std::string_view message) noexcept {
        if (finished_) return;
        finished_ = true;
        if (is_tty_) {
            std::cout << "\r" << green() << bold() << "[OK]" << reset()
                      << " " << label_ << ": " << green() << message
                      << reset() << "            \n";
        } else {
            std::cout << "  ok: " << label_ << ": " << message << "\n";
        }
    }

    void Spinner::fail(std::string_view message) noexcept {
        if (finished_) return;
        finished_ = true;
        if (is_tty_) {
            std::cout << "\r" << red() << bold() << "[FAIL]" << reset()
                      << " " << label_ << ": " << red() << message
                      << reset() << "            \n";
        } else {
            std::cerr << "  fail: " << label_ << ": " << message << "\n";
        }
    }

    // ----------------------------------------------------------------
    // Prompts interactivos
    // ----------------------------------------------------------------

    bool prompt_yes_no(std::string_view question, bool default_yes) {
        const char *yn = default_yes ? "[Y/n]" : "[y/N]";
        std::cout << bold() << cyan() << "?" << reset() << " "
                  << question << " " << gray() << yn << reset() << " ";
        std::string line;
        if (!std::getline(std::cin, line)) {
            return default_yes;
        }
        if (line.empty()) return default_yes;
        char c = static_cast<char>(std::tolower(static_cast<unsigned char>(line[0])));
        return c == 'y';
    }

    std::string prompt_line(std::string_view question,
                              std::string_view default_value) {
        std::cout << bold() << cyan() << "?" << reset() << " " << question;
        if (!default_value.empty()) {
            std::cout << " " << gray() << "[" << default_value << "]" << reset();
        }
        std::cout << " ";
        std::string line;
        if (!std::getline(std::cin, line) || line.empty()) {
            return std::string(default_value);
        }
        return line;
    }

} // namespace pkg::ui
