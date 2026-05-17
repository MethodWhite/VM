/**
 * @file vsh.cpp
 * @brief Implementacion completa del motor VestaShell (.vsh).
 *
 * Contiene en orden:
 *   1. Metodos de Value (truthy, to_string, operadores)
 *   2. VshEnv (get/define/assign/has)
 *   3. VshLexer (tokenizacion + string interpolation)
 *   4. VshParser (descenso recursivo)
 *   5. VshInterpreter (evaluacion + built-ins + integracion REPL)
 */
#include "cli/vsh.h"
#include "json.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cassert>
#include <thread>
#include <chrono>
#include <cstdio>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#  include <fcntl.h>
#else
#  include <glob.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  if defined(__APPLE__)
#    include <mach-o/dyld.h>
#  endif
#endif
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <mutex>
#include <atomic>
#include <random>
#include <numeric>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sys/types.h>
#if !defined(_WIN32)
#  include <sys/wait.h>
#  include <dlfcn.h>
#endif

// ============================================================
// Estado global de sockets para los builtins de red VSH
// ============================================================

namespace {
    // Estado interno del buffer de salida.
    static std::string  g_out_buffer;
    static const size_t OUT_FLUSH_THRESHOLD = 64 * 1024; // 64 KB

#if defined(_WIN32)
    typedef SOCKET              vsh_sock_t;
    static constexpr vsh_sock_t VSH_BAD_SOCK = INVALID_SOCKET;

    static void vsh_sock_close(vsh_sock_t s) {
        closesocket(s);
    }

    static bool vsh_sock_valid(vsh_sock_t s) {
        return s != INVALID_SOCKET;
    }

    static void ensure_wsa() {
        static bool done = false;
        if (!done) {
            WSADATA wd;
            WSAStartup(MAKEWORD(2, 2), &wd);
            done = true;
        }
    }

    // Cache del handle del console: se obtiene una sola vez
    static HANDLE g_console_handle             = INVALID_HANDLE_VALUE;
    static bool   g_is_console                 = false;
    static bool   g_console_handle_initialized = false;

    static void init_console_handle() {
        if (g_console_handle_initialized) return;
        g_console_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode;
        g_is_console = (g_console_handle != INVALID_HANDLE_VALUE) &&
                GetConsoleMode(g_console_handle, &mode);
        g_console_handle_initialized = true;
    }

    // Conversion rapida UTF-8 -> UTF-16 con MultiByteToWideChar
    static int utf8_to_utf16(const char *src, int src_len, std::wstring &out) {
        if (src_len <= 0) {
            out.clear();
            return 0;
        }
        int needed = MultiByteToWideChar(CP_UTF8, 0, src, src_len, nullptr, 0);
        if (needed <= 0) return 0;
        out.resize(needed);
        return MultiByteToWideChar(CP_UTF8, 0, src, src_len, &out[0], needed);
    }

#else
    typedef int vsh_sock_t;
    static constexpr vsh_sock_t VSH_BAD_SOCK = -1;
    static void vsh_sock_close(vsh_sock_t s) { ::close(s); }
    static bool vsh_sock_valid(vsh_sock_t s) { return s >= 0; }
    static void ensure_wsa() {}
#endif

    // Escribe el contenido de g_out_buffer a stdout y limpia el buffer.
    // Usado por todos los builtins de salida.
    static void flush_out_buffer() {
        if (g_out_buffer.empty()) return;
#ifdef _WIN32
        init_console_handle();
        if (g_is_console) {
            // Console: usar WriteConsoleW directamente, evita la doble conversion
            // de std::cout en codepage 65001.
            std::wstring wide;
            int          n = utf8_to_utf16(g_out_buffer.data(),
                                  static_cast<int>(g_out_buffer.size()),
                                  wide);
            if (n > 0) {
                DWORD written = 0;
                // Escribir en chunks porque WriteConsoleW tiene un limite de
                // ~32K wchars por llamada en algunas versiones de Windows
                const DWORD    CHUNK     = 16 * 1024;
                DWORD          remaining = static_cast<DWORD>(n);
                const wchar_t *p         = wide.data();
                while (remaining > 0) {
                    DWORD to_write = remaining > CHUNK ? CHUNK : remaining;
                    if (!WriteConsoleW(g_console_handle, p, to_write, &written, nullptr)) {
                        // Fallback: si falla, usar fwrite normal
                        fwrite(g_out_buffer.data(), 1, g_out_buffer.size(), stdout);
                        break;
                    }
                    p += written;
                    remaining -= written;
                }
            }
        } else {
            // Pipe o redireccion: fwrite directo (no conviene WriteConsole con pipes)
            fwrite(g_out_buffer.data(), 1, g_out_buffer.size(), stdout);
        }
#else
        // Linux/macOS: fwrite es ya muy rapido
        fwrite(g_out_buffer.data(), 1, g_out_buffer.size(), stdout);
#endif

        g_out_buffer.clear();
    }

    // Anade un string al buffer; flush automatico si supera el umbral
    static void buffered_write(const std::string &s) {
        g_out_buffer.append(s);
        if (g_out_buffer.size() >= OUT_FLUSH_THRESHOLD) {
            flush_out_buffer();
        }
    }

    static void buffered_write_char(char c) {
        g_out_buffer.push_back(c);
        if (g_out_buffer.size() >= OUT_FLUSH_THRESHOLD) {
            flush_out_buffer();
        }
    }


    struct VshSockState {
        vsh_sock_t fd     = VSH_BAD_SOCK;
        SSL *      ssl    = nullptr;
        bool       is_udp = false;
    };

    static std::mutex                                g_sock_mutex;
    static std::unordered_map<int64_t, VshSockState> g_sockets;
    static int64_t                                   g_sock_next_id = 1;

    static SSL_CTX *      g_ssl_ctx = nullptr;
    static std::once_flag g_ssl_init_flag;

    static void ensure_ssl_ctx() {
        std::call_once(g_ssl_init_flag, []() {
            SSL_library_init();
            SSL_load_error_strings();
            OpenSSL_add_ssl_algorithms();
            g_ssl_ctx = SSL_CTX_new(TLS_client_method());
            if (g_ssl_ctx)
                SSL_CTX_set_verify(g_ssl_ctx, SSL_VERIFY_NONE, nullptr);
        });
    }

    static int64_t vsh_alloc_sock(vsh_sock_t fd, bool is_udp = false, SSL *ssl = nullptr) {
        std::lock_guard<std::mutex> lk(g_sock_mutex);
        int64_t                     id = g_sock_next_id++;
        g_sockets[id]                  = {fd, ssl, is_udp};
        return id;
    }

    static VshSockState *vsh_get_sock(int64_t id) {
        auto it = g_sockets.find(id);
        return it == g_sockets.end() ? nullptr : &it->second;
    }

    static void vsh_free_sock(int64_t id) {
        std::lock_guard<std::mutex> lk(g_sock_mutex);
        auto                        it = g_sockets.find(id);
        if (it == g_sockets.end()) return;
        auto &s = it->second;
        if (s.ssl) {
            SSL_shutdown(s.ssl);
            SSL_free(s.ssl);
        }
        if (vsh_sock_valid(s.fd)) vsh_sock_close(s.fd);
        g_sockets.erase(it);
    }

    static void vsh_resolve(const std::string &host, uint16_t port, struct sockaddr_in &out) {
        memset(&out, 0, sizeof(out));
        out.sin_family = AF_INET;
        out.sin_port   = htons(port);
        struct addrinfo hints{};
        memset(&hints, 0, sizeof(hints));
        hints.ai_family      = AF_INET;
        hints.ai_socktype    = SOCK_STREAM;
        struct addrinfo *res = nullptr;
        int              r   = getaddrinfo(host.c_str(), nullptr, &hints, &res);
        if (r != 0 || !res) {
            if (res) freeaddrinfo(res);
            throw vsh::VshRuntimeError("no se pudo resolver host: " + host);
        }
        out.sin_addr = reinterpret_cast<struct sockaddr_in *>(res->ai_addr)->sin_addr;
        freeaddrinfo(res);
    }

    // Peticion HTTP/HTTPS generica: metodo, cuerpo y tipo de contenido opcionales.
    // Devuelve [status_code, headers_str, body_str] como tres strings concatenados
    // con separador "\x00" — o solo el body si split_response=false.
    struct VshHttpResp {
        int         status = 0;
        std::string headers;
        std::string body;
    };

    static VshHttpResp vsh_http_request(
        const std::string &method,
        const std::string &host, uint16_t port,
        const std::string &path, bool     use_tls,
        const std::string &body          = "",
        const std::string &content_type  = "application/x-www-form-urlencoded",
        const std::string &extra_headers = "") {
        ensure_wsa();
        struct sockaddr_in addr{};
        vsh_resolve(host, port, addr);
        vsh_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
        if (!vsh_sock_valid(fd)) throw vsh::VshRuntimeError("error creando socket HTTP");
        if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
            vsh_sock_close(fd);
            throw vsh::VshRuntimeError("no se pudo conectar a " + host);
        }
        SSL *ssl = nullptr;
        if (use_tls) {
            ensure_ssl_ctx();
            ssl = SSL_new(g_ssl_ctx);
            SSL_set_fd(ssl, (int) fd);
            SSL_set_tlsext_host_name(ssl, host.c_str());
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl);
                vsh_sock_close(fd);
                throw vsh::VshRuntimeError("fallo TLS handshake con " + host);
            }
        }
        std::string req = method + " " + path + " HTTP/1.0\r\n"
                + "Host: " + host + "\r\n"
                + "User-Agent: VestaShell/1.0\r\n"
                + "Accept: */*\r\n";
        if (!body.empty()) {
            req += "Content-Type: " + content_type + "\r\n";
            req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        if (!extra_headers.empty()) req += extra_headers;
        req += "Connection: close\r\n\r\n";
        if (!body.empty()) req += body;

        if (ssl) SSL_write(ssl, req.c_str(), (int) req.size());
        else send(fd, req.c_str(), (int) req.size(), 0);

        std::string raw;
        char        buf[4096];
        for (;;) {
            int n = ssl
                        ? SSL_read(ssl, buf, sizeof(buf))
                        : (int) recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, (size_t) n);
        }
        if (ssl) {
            SSL_shutdown(ssl);
            SSL_free(ssl);
        }
        vsh_sock_close(fd);

        VshHttpResp r;
        auto        sep = raw.find("\r\n\r\n");
        r.headers       = sep == std::string::npos ? raw : raw.substr(0, sep);
        r.body          = sep == std::string::npos ? "" : raw.substr(sep + 4);
        // extraer codigo de estado de la primera linea (ej: "HTTP/1.0 200 OK")
        auto sp1 = r.headers.find(' ');
        if (sp1 != std::string::npos) {
            auto        sp2      = r.headers.find(' ', sp1 + 1);
            std::string code_str = sp2 == std::string::npos
                                       ? r.headers.substr(sp1 + 1)
                                       : r.headers.substr(sp1 + 1, sp2 - sp1 - 1);
            try {
                r.status = std::stoi(code_str);
            } catch (...) {}
        }
        return r;
    }

    static std::pair<std::string, uint16_t> vsh_parse_url(const std::string &url,
                                                          bool &             use_tls,
                                                          std::string &      path_out) {
        use_tls          = false;
        std::string work = url;
        if (work.substr(0, 8) == "https://") {
            use_tls = true;
            work    = work.substr(8);
        } else if (work.substr(0, 7) == "http://") work = work.substr(7);
        auto        slash    = work.find('/');
        std::string hostport = slash == std::string::npos ? work : work.substr(0, slash);
        path_out             = slash == std::string::npos ? "/" : work.substr(slash);
        auto colon           = hostport.rfind(':');
        if (colon != std::string::npos) {
            uint16_t p = (uint16_t) std::stoi(hostport.substr(colon + 1));
            return {hostport.substr(0, colon), p};
        }
        return {hostport, use_tls ? uint16_t(443) : uint16_t(80)};
    }

    // RNG global compartido por todos los builtins de aleatoriedad
    static std::mt19937_64 g_vsh_rng(std::random_device{}());

    // ============================================================
    // FFI: tabla de handles de librerias dinamicas cargadas en tiempo de ejecucion
    // ============================================================

    struct VshFfiLib {
#if defined(_WIN32)
        HMODULE handle = nullptr;
#else
    void *handle = nullptr;
#endif
    };

    static std::mutex                             g_ffi_mutex;
    static std::unordered_map<int64_t, VshFfiLib> g_ffi_libs;
    static int64_t                                g_ffi_next_id = 1;

    // Abre una libreria dinamica y devuelve un identificador opaco.
    static int64_t vsh_ffi_open(const std::string &path) {
        std::lock_guard<std::mutex> lk(g_ffi_mutex);
#if defined(_WIN32)
        HMODULE h = LoadLibraryA(path.c_str());
        if (!h) throw vsh::VshRuntimeError("ffi_open: no se pudo cargar: " + path);
#else
    void *h = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!h) throw vsh::VshRuntimeError(std::string("ffi_open: ") + dlerror());
#endif
        int64_t id     = g_ffi_next_id++;
        g_ffi_libs[id] = {h};
        return id;
    }

    // Libera una libreria cargada con vsh_ffi_open.
    static void vsh_ffi_close(int64_t id) {
        std::lock_guard<std::mutex> lk(g_ffi_mutex);
        auto                        it = g_ffi_libs.find(id);
        if (it == g_ffi_libs.end()) return;
#if defined(_WIN32)
        FreeLibrary(it->second.handle);
#else
    dlclose(it->second.handle);
#endif
        g_ffi_libs.erase(it);
    }

    // Busca un simbolo en la libreria y devuelve su direccion como int64_t.
    static int64_t vsh_ffi_sym(int64_t lib_id, const std::string &name) {
        std::lock_guard<std::mutex> lk(g_ffi_mutex);
        auto                        it = g_ffi_libs.find(lib_id);
        if (it == g_ffi_libs.end()) throw vsh::VshRuntimeError("ffi_sym: handle invalido");
#if defined(_WIN32)
        FARPROC p = GetProcAddress(it->second.handle, name.c_str());
        if (!p) throw vsh::VshRuntimeError("ffi_sym: simbolo no encontrado: " + name);
        int64_t result = 0;
        memcpy(&result, &p, sizeof(p));
        return result;
#else
    void *p = dlsym(it->second.handle, name.c_str());
    if (!p) throw vsh::VshRuntimeError(std::string("ffi_sym: ") + dlerror());
    int64_t result = 0;
    memcpy(&result, &p, sizeof(p));
    return result;
#endif
    }

    // Llama a una funcion nativa con hasta 8 argumentos.
    // int/bool/null pasan directamente; strings se convierten a char* temporal;
    // floats pasan sus bits IEEE-754 empaquetados en int64_t.
    static int64_t vsh_ffi_call(int64_t sym_addr, const std::vector<vsh::Value> &args) {
        std::vector<std::string> str_storage;
        str_storage.reserve(args.size());
        std::vector<int64_t> iargs;
        iargs.reserve(args.size());
        for (const auto &a: args) {
            if (a.is_string()) {
                str_storage.push_back(a.as_string());
                int64_t     p  = 0;
                const void *cp = str_storage.back().c_str();
                memcpy(&p, &cp, sizeof(p));
                iargs.push_back(p);
            } else if (a.is_int()) {
                iargs.push_back(a.as_int());
            } else if (a.is_bool()) {
                iargs.push_back(a.as_bool() ? 1 : 0);
            } else if (a.is_float()) {
                double  d    = a.as_float();
                int64_t bits = 0;
                memcpy(&bits, &d, 8);
                iargs.push_back(bits);
            } else if (a.is_null()) {
                iargs.push_back(0);
            } else {
                throw vsh::VshRuntimeError("ffi_call: tipo de argumento no soportado para FFI");
            }
        }
        // Typedef de punteros a funcion tipados para evitar casting de void*
        typedef int64_t (*fn0)();
        typedef int64_t (*fn1)(int64_t);
        typedef int64_t (*fn2)(int64_t, int64_t);
        typedef int64_t (*fn3)(int64_t, int64_t, int64_t);
        typedef int64_t (*fn4)(int64_t, int64_t, int64_t, int64_t);
        typedef int64_t (*fn5)(int64_t, int64_t, int64_t, int64_t, int64_t);
        typedef int64_t (*fn6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
        typedef int64_t (*fn7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
        typedef int64_t (*fn8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
        // Recuperar el puntero de funcion desde el int64_t via memcpy (sin UB)
        fn0 raw = nullptr;
        memcpy(&raw, &sym_addr, sizeof(raw));
        size_t n = iargs.size();
        while (iargs.size() < 8) iargs.push_back(0);
        switch (n) {
            case 0: return raw();
            case 1: return reinterpret_cast<fn1>(raw)(iargs[0]);
            case 2: return reinterpret_cast<fn2>(raw)(iargs[0], iargs[1]);
            case 3: return reinterpret_cast<fn3>(raw)(iargs[0], iargs[1], iargs[2]);
            case 4: return reinterpret_cast<fn4>(raw)(iargs[0], iargs[1], iargs[2], iargs[3]);
            case 5: return reinterpret_cast<fn5>(raw)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4]);
            case 6: return reinterpret_cast<fn6>(raw)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5]);
            case 7: return reinterpret_cast<fn7>(raw)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5],
                                                      iargs[6]);
            case 8: return reinterpret_cast<fn8>(raw)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5],
                                                      iargs[6], iargs[7]);
            default: throw vsh::VshRuntimeError("ffi_call: maximo 8 argumentos soportados");
        }
    }

    // Igual que vsh_ffi_call pero el entero retornado se reinterpreta como double.
    static double vsh_ffi_call_f(int64_t sym_addr, const std::vector<vsh::Value> &args) {
        int64_t bits = vsh_ffi_call(sym_addr, args);
        double  d    = 0.0;
        memcpy(&d, &bits, 8);
        return d;
    }

    // ============================================================
    // ANSI: activacion de secuencias de escape en la consola de Windows
    // ============================================================

    // Habilita ENABLE_VIRTUAL_TERMINAL_PROCESSING en Windows para que los
    // codigos de escape ANSI se interpreten correctamente en cmd.exe / conhost.
    // En Linux es una no-op: el terminal ya los soporta de forma nativa.
    static void vsh_ansi_ensure() {
#if defined(_WIN32)
        static bool done = false;
        if (done) return;
        done        = true;
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD  mode = 0;
        if (GetConsoleMode(hOut, &mode))
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    }
} // anonymous namespace

namespace vsh {
    // ============================================================
    // Seccion 1: Value
    // ============================================================

    VshType Value::type() const noexcept {
        return static_cast<VshType>(data.index());
    }

    bool Value::truthy() const noexcept {
        switch (type()) {
            case VshType::Null: return false;
            case VshType::Bool: return as_bool();
            case VshType::Int: return as_int() != 0;
            case VshType::Float: return as_float() != 0.0;
            case VshType::String: return !as_string().empty();
            case VshType::List: return !as_list()->empty();
            case VshType::Map: return !as_map()->empty();
            case VshType::Function: return true;
            case VshType::Class: return true;
            case VshType::Instance: return true;
        }
        return false;
    }

    double Value::as_number() const {
        if (is_int()) return static_cast<double>(as_int());
        if (is_float()) return as_float();
        throw VshRuntimeError("se esperaba un numero, se obtuvo " +
            std::string(is_string() ? "string" : "otro tipo"));
    }

    std::string Value::to_string() const {
        switch (type()) {
            case VshType::Null: return "null";
            case VshType::Bool: return as_bool() ? "true" : "false";
            case VshType::Int: return std::to_string(as_int());
            case VshType::Float: {
                std::ostringstream oss;
                oss << as_float();
                std::string s = oss.str();
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                    s += ".0"; // garantizar que parezca flotante
                return s;
            }
            case VshType::String: return as_string();
            case VshType::List: {
                std::string r = "[";
                auto &      v = *as_list();
                for (size_t i = 0; i < v.size(); ++i) {
                    if (i) r += ", ";
                    r += v[i].to_repr();
                }
                return r + "]";
            }
            case VshType::Map: {
                std::string r     = "{";
                bool        first = true;
                for (auto &[k, v]: *as_map()) {
                    if (!first) r += ", ";
                    r += "\"" + k + "\": " + v.to_repr();
                    first = false;
                }
                return r + "}";
            }
            case VshType::Function: {
                auto fn = as_fn();
                return "<function " + (fn->name.empty() ? "<lambda>" : fn->name) + ">";
            }
            case VshType::Class: {
                auto cls = as_class();
                return "<class " + cls->name + ">";
            }
            case VshType::Instance: {
                auto inst = as_instance();
                // si tiene __str__, se llamara desde eval; aqui retornamos representacion por defecto
                return "<" + inst->klass->name + " object>";
            }
        }
        return "null";
    }

    std::string Value::to_repr() const {
        if (is_string()) return "\"" + as_string() + "\"";
        return to_string();
    }

    bool Value::operator==(const Value &o) const noexcept {
        if (type() != o.type()) {
            // int y float son iguales si el valor numerico coincide
            if (is_numeric() && o.is_numeric())
                return as_number() == o.as_number();
            return false;
        }
        switch (type()) {
            case VshType::Null: return true;
            case VshType::Bool: return as_bool() == o.as_bool();
            case VshType::Int: return as_int() == o.as_int();
            case VshType::Float: return as_float() == o.as_float();
            case VshType::String: return as_string() == o.as_string();
            default: return false; // list/map/fn: identidad de puntero
        }
    }

    bool Value::operator<(const Value &o) const {
        if (is_numeric() && o.is_numeric()) return as_number() < o.as_number();
        if (is_string() && o.is_string()) return as_string() < o.as_string();
        throw VshRuntimeError("tipos no comparables: " +
            std::string(is_null() ? "null" : is_bool() ? "bool" : is_list() ? "list" : is_map() ? "map" : "funcion") +
            " y " +
            std::string(o.is_null()
                            ? "null"
                            : o.is_bool()
                                  ? "bool"
                                  : o.is_list()
                                        ? "list"
                                        : o.is_map()
                                              ? "map"
                                              : "funcion"));
    }

    // ============================================================
    // Seccion 2: VshEnv
    // ============================================================

    Value &VshEnv::get(const std::string &name) {
        auto it = vars.find(name);
        if (it != vars.end()) return it->second;
        if (parent) return parent->get(name);
        throw VshRuntimeError("variable no definida: '" + name + "'");
    }

    void VshEnv::define(const std::string &name, Value v) {
        vars[name] = std::move(v);
    }

    void VshEnv::assign(const std::string &name, Value v) {
        auto it = vars.find(name);
        if (it != vars.end()) {
            it->second = std::move(v);
            return;
        }
        if (parent) {
            parent->assign(name, std::move(v));
            return;
        }
        throw VshRuntimeError("asignacion a variable no declarada: '" + name +
            "' (usa 'let " + name + " = ...' para declararla)");
    }

    bool VshEnv::has(const std::string &name) const noexcept {
        if (vars.count(name)) return true;
        return parent ? parent->has(name) : false;
    }

    std::shared_ptr<VshEnv> VshEnv::make_child() {
        return std::make_shared<VshEnv>(shared_from_this());
    }

    // ============================================================
    // Seccion 3: VshLexer
    // ============================================================

    const std::unordered_map<std::string, TK> &VshLexer::kw_map() {
        static const std::unordered_map<std::string, TK> m = {
            {"let", TK::KW_LET}, {"fn", TK::KW_FN},
            {"return", TK::KW_RETURN}, {"if", TK::KW_IF},
            {"elif", TK::KW_ELIF}, {"else", TK::KW_ELSE},
            {"while", TK::KW_WHILE}, {"for", TK::KW_FOR},
            {"in", TK::KW_IN}, {"break", TK::KW_BREAK},
            {"continue", TK::KW_CONTINUE}, {"try", TK::KW_TRY},
            {"catch", TK::KW_CATCH}, {"import", TK::KW_IMPORT},
            {"and", TK::KW_AND}, {"or", TK::KW_OR},
            {"not", TK::KW_NOT}, {"true", TK::BOOL_LIT},
            {"false", TK::BOOL_LIT}, {"null", TK::NULL_LIT},
            {"class", TK::KW_CLASS}, {"super", TK::KW_SUPER},
            {"throw", TK::KW_THROW},
        };
        return m;
    }

    VshLexer::VshLexer(std::string source, std::string filename)
        : src_(std::move(source)), filename_(std::move(filename)) {}

    char VshLexer::cur() const {
        return pos_ < src_.size() ? src_[pos_] : '\0';
    }

    char VshLexer::peek1() const {
        return (pos_ + 1) < src_.size() ? src_[pos_ + 1] : '\0';
    }

    char VshLexer::advance() {
        char c = src_[pos_++];
        if (c == '\n') {
            ++line_;
            col_ = 1;
        } else {
            ++col_;
        }
        return c;
    }

    bool VshLexer::match(char c) {
        if (cur() != c) return false;
        advance();
        return true;
    }

    void VshLexer::skip_ws_and_comments() {
        while (pos_ < src_.size()) {
            char c = cur();
            if (c == ' ' || c == '\t' || c == '\r') {
                advance();
                continue;
            }
            // comentario de linea: // hasta fin de linea
            if (c == '/' && peek1() == '/') {
                while (pos_ < src_.size() && cur() != '\n') advance();
                continue;
            }
            // comentario de bloque: /* ... */ (puede abarcar multiples lineas)
            if (c == '/' && peek1() == '*') {
                advance();
                advance(); // consume '/*'
                while (pos_ < src_.size()) {
                    if (cur() == '*' && peek1() == '/') {
                        advance();
                        advance();
                        break;
                    }
                    advance(); // advance() ya actualiza line_/col_ ante '\n'
                }
                continue;
            }
            // barra invertida + newline = continuacion de linea
            if (c == '\\' && peek1() == '\n') {
                advance();
                advance();
                continue;
            }
            break;
        }
    }

    Token VshLexer::make_tok(TK k, std::string lex) const {
        Token t;
        t.kind   = k;
        t.lexeme = std::move(lex);
        t.line   = line_;
        t.col    = col_;
        return t;
    }

    bool VshLexer::at_end() const {
        return buf_.empty() && pos_ >= src_.size();
    }

    const Token &VshLexer::peek(int offset) {
        while ((int) buf_.size() <= offset) buf_.push_back(read_one_token());
        return buf_[offset];
    }

    Token VshLexer::next() {
        if (!buf_.empty()) {
            Token t = buf_.front();
            buf_.pop_front();
            return t;
        }
        return read_one_token();
    }

    Token VshLexer::read_number() {
        int         sline = line_, scol = col_;
        std::string lex;
        bool        is_float = false;
        while (pos_ < src_.size() && (std::isdigit(cur()) || cur() == '_')) {
            if (cur() != '_') lex += cur();
            advance();
        }
        if (cur() == '.' && std::isdigit(peek1())) {
            is_float = true;
            lex += advance(); // '.'
            while (pos_ < src_.size() && (std::isdigit(cur()) || cur() == '_')) {
                if (cur() != '_') lex += cur();
                advance();
            }
        }
        if (cur() == 'e' || cur() == 'E') {
            is_float = true;
            lex += advance();
            if (cur() == '+' || cur() == '-') lex += advance();
            while (std::isdigit(cur())) {
                lex += cur();
                advance();
            }
        }
        Token t;
        t.line = sline;
        t.col  = scol;
        if (is_float) {
            t.kind    = TK::FLOAT_LIT;
            t.lexeme  = lex;
            t.flt_val = std::stod(lex);
        } else {
            t.kind    = TK::INT_LIT;
            t.lexeme  = lex;
            t.int_val = std::stoll(lex, nullptr, 0);
        }
        return t;
    }

    // empuja en buf_ la secuencia ISTR_TEXT / ISTR_EXPR_BEGIN ... ISTR_EXPR_END / ISTR_END
    void VshLexer::push_interp_tokens(const std::string &raw, int base_line, int base_col) {
        std::string text;
        size_t      i = 0;

        auto flush_text = [&]() {
            if (text.empty()) return;
            Token t;
            t.kind   = TK::ISTR_TEXT;
            t.lexeme = text;
            t.line   = base_line;
            t.col    = base_col;
            buf_.push_back(std::move(t));
            text.clear();
        };

        auto hex_val = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        while (i < raw.size()) {
            // escape sequences
            if (raw[i] == '\\' && i + 1 < raw.size()) {
                char e = raw[i + 1];
                // \xHH : byte hexadecimal de 2 digitos
                if ((e == 'x' || e == 'X') && i + 3 < raw.size()) {
                    int hi = hex_val(raw[i + 2]);
                    int lo = hex_val(raw[i + 3]);
                    if (hi >= 0 && lo >= 0) {
                        text += static_cast<char>((hi << 4) | lo);
                        i += 4;
                        continue;
                    }
                }
                switch (e) {
                    case 'n': text += '\n';
                        break;
                    case 't': text += '\t';
                        break;
                    case 'r': text += '\r';
                        break;
                    case '0': text += '\0';
                        break;
                    case 'a': text += '\a';
                        break;
                    case 'b': text += '\b';
                        break;
                    case 'f': text += '\f';
                        break;
                    case 'v': text += '\v';
                        break;
                    case 'e': text += '\x1b';
                        break; // ESC: comodidad para ANSI
                    case '"': text += '"';
                        break;
                    case '\\': text += '\\';
                        break;
                    case '$': text += '$';
                        break; // \$ literal
                    default: text += '\\';
                        text += e;
                        break;
                }
                i += 2;
                continue;
            }
            // inicio de expresion embebida
            if (raw[i] == '$' && i + 1 < raw.size() && raw[i + 1] == '{') {
                flush_text();
                // token ISTR_EXPR_BEGIN
                Token tb;
                tb.kind = TK::ISTR_EXPR_BEGIN;
                tb.line = base_line;
                tb.col  = base_col;
                buf_.push_back(tb);
                // extraer hasta el } balanceado
                i += 2;
                size_t      depth = 1;
                std::string expr_src;
                while (i < raw.size() && depth > 0) {
                    if (raw[i] == '{') ++depth;
                    else if (raw[i] == '}') {
                        --depth;
                        if (depth == 0) {
                            ++i;
                            break;
                        }
                    }
                    // cadenas dentro de la expresion: copiar opacamente
                    if ((raw[i] == '"' || raw[i] == '\'') && depth > 0) {
                        char q = raw[i++];
                        expr_src += q;
                        while (i < raw.size() && raw[i] != q) {
                            if (raw[i] == '\\' && i + 1 < raw.size()) {
                                expr_src += raw[i++];
                            }
                            expr_src += raw[i++];
                        }
                        if (i < raw.size()) {
                            expr_src += raw[i++];
                        } // cierre comilla
                        continue;
                    }
                    expr_src += raw[i++];
                }
                // sub-lexer sobre la expresion
                VshLexer sub(expr_src, filename_);
                while (!sub.at_end()) {
                    Token st = sub.next();
                    if (st.kind == TK::END_OF_FILE) break;
                    st.line = base_line;
                    st.col  = base_col;
                    buf_.push_back(std::move(st));
                }
                // token ISTR_EXPR_END
                Token te;
                te.kind = TK::ISTR_EXPR_END;
                te.line = base_line;
                te.col  = base_col;
                buf_.push_back(te);
                continue;
            }
            text += raw[i++];
        }
        flush_text();
        // token ISTR_END
        Token tend;
        tend.kind = TK::ISTR_END;
        tend.line = base_line;
        tend.col  = base_col;
        buf_.push_back(tend);
    }

    Token VshLexer::read_string_or_interp() {
        int  sline = line_, scol = col_;
        char quote = advance(); // consume '"' o '\''

        // Triple-comilla """...""": string multilínea sin interpolacion (docstring)
        if (quote == '"' && pos_ < src_.size() && cur() == '"') {
            if (pos_ + 1 < src_.size() && src_[pos_ + 1] == '"') {
                // Consumir las dos comillas restantes de apertura
                advance();
                advance();
                std::string out;
                while (pos_ < src_.size()) {
                    // Detectar cierre """
                    if (cur() == '"' && pos_ + 1 < src_.size() && src_[pos_ + 1] == '"' &&
                        pos_ + 2 < src_.size() && src_[pos_ + 2] == '"') {
                        advance();
                        advance();
                        advance();
                        break;
                    }
                    if (cur() == '\\') {
                        advance();
                        if (pos_ < src_.size()) {
                            char e = advance();
                            // \xHH : byte hexadecimal de 2 digitos
                            if ((e == 'x' || e == 'X') && pos_ + 1 < src_.size()) {
                                auto hex_v = [](char c) -> int {
                                    if (c >= '0' && c <= '9') return c - '0';
                                    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                                    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                                    return -1;
                                };
                                int hi = hex_v(src_[pos_]);
                                int lo = hex_v(src_[pos_ + 1]);
                                if (hi >= 0 && lo >= 0) {
                                    advance();
                                    advance();
                                    out += static_cast<char>((hi << 4) | lo);
                                    continue;
                                }
                            }
                            switch (e) {
                                case 'n': out += '\n';
                                    break;
                                case 't': out += '\t';
                                    break;
                                case 'r': out += '\r';
                                    break;
                                case '0': out += '\0';
                                    break;
                                case 'a': out += '\a';
                                    break;
                                case 'b': out += '\b';
                                    break;
                                case 'f': out += '\f';
                                    break;
                                case 'v': out += '\v';
                                    break;
                                case 'e': out += '\x1b';
                                    break;
                                case '"': out += '"';
                                    break;
                                case '\\': out += '\\';
                                    break;
                                default: out += '\\';
                                    out += e;
                                    break;
                            }
                        }
                    } else {
                        out += advance();
                    }
                }
                Token t;
                t.kind   = TK::STRING_LIT;
                t.lexeme = out;
                t.line   = sline;
                t.col    = scol;
                return t;
            }
            // Cadena vacia "": la segunda '"' cierra directamente
            advance();
            Token t;
            t.kind   = TK::STRING_LIT;
            t.lexeme = "";
            t.line   = sline;
            t.col    = scol;
            return t;
        }

        std::string raw;
        bool        has_interp = false;
        while (pos_ < src_.size() && cur() != quote) {
            if (cur() == '\\') {
                raw += advance();
                if (pos_ < src_.size()) raw += advance();
                continue;
            }
            if (cur() == '$' && peek1() == '{') has_interp = true;
            raw += advance();
        }
        if (pos_ < src_.size()) advance(); // consume cierre de comilla
        if (!has_interp) {
            // string simple: procesar escapes
            auto hex_v = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            std::string out;
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    char e = raw[i + 1];
                    // \xHH : byte hexadecimal de 2 digitos
                    if ((e == 'x' || e == 'X') && i + 3 < raw.size()) {
                        int hi = hex_v(raw[i + 2]);
                        int lo = hex_v(raw[i + 3]);
                        if (hi >= 0 && lo >= 0) {
                            out += static_cast<char>((hi << 4) | lo);
                            i += 3;
                            continue;
                        }
                    }
                    ++i;
                    switch (e) {
                        case 'n': out += '\n';
                            break;
                        case 't': out += '\t';
                            break;
                        case 'r': out += '\r';
                            break;
                        case '0': out += '\0';
                            break;
                        case 'a': out += '\a';
                            break;
                        case 'b': out += '\b';
                            break;
                        case 'f': out += '\f';
                            break;
                        case 'v': out += '\v';
                            break;
                        case 'e': out += '\x1b';
                            break;
                        case '"': out += '"';
                            break;
                        case '\'': out += '\'';
                            break;
                        case '\\': out += '\\';
                            break;
                        default: out += '\\';
                            out += e;
                            break;
                    }
                } else {
                    out += raw[i];
                }
            }
            Token t;
            t.kind   = TK::STRING_LIT;
            t.lexeme = out;
            t.line   = sline;
            t.col    = scol;
            return t;
        }
        // string interpolada: genera tokens en buf_ y devuelve un marcador especial
        Token marker;
        marker.kind = TK::ISTR_TEXT;
        marker.line = sline;
        marker.col  = scol;
        // marcamos con lexeme vacio para que el parser sepa que empieza aqui
        // En realidad el primer token de la interpolacion ya estara en buf_
        push_interp_tokens(raw, sline, scol);
        // El parser vera ISTR_TEXT (o ISTR_EXPR_BEGIN como primer token) seguido de ISTR_END
        // Devolvemos un token ficticio que el parser identifica como inicio de interp
        // Usaremos ISTR_TEXT con lexeme especial "" como "ya esta en buf_"
        // En realidad simplemente devolvemos el primer token del buf_
        Token first = buf_.front();
        buf_.pop_front();
        return first;
    }

    Token VshLexer::read_ident_kw() {
        int         sline = line_, scol = col_;
        std::string lex;
        while (pos_ < src_.size() && (std::isalnum(cur()) || cur() == '_')) {
            lex += advance();
        }
        auto &km = kw_map();
        auto  it = km.find(lex);
        Token t;
        t.line   = sline;
        t.col    = scol;
        t.lexeme = lex;
        if (it != km.end()) {
            t.kind = it->second;
            if (t.kind == TK::BOOL_LIT) t.int_val = (lex == "true") ? 1 : 0;
        } else {
            t.kind = TK::IDENT;
        }
        return t;
    }

    Token VshLexer::read_one_token() {
        skip_ws_and_comments();
        if (pos_ >= src_.size()) return make_tok(TK::END_OF_FILE, "<eof>");

        int  sline = line_, scol = col_;
        char c     = cur();

        // Newline: terminador de sentencia
        if (c == '\n') {
            advance();
            Token t = make_tok(TK::NEWLINE, "\\n");
            t.line  = sline;
            t.col   = scol;
            return t;
        }

        if (std::isdigit(c)) return read_number();
        if (c == '"' || c == '\'') return read_string_or_interp();
        if (std::isalpha(c) || c == '_') return read_ident_kw();

        advance(); // consumir el caracter
        Token t;
        t.line = sline;
        t.col  = scol;

        switch (c) {
            case '+': t.kind = match('=') ? TK::PLUS_EQ : TK::PLUS;
                break;
            case '-': t.kind = match('=') ? TK::MINUS_EQ : TK::MINUS;
                break;
            case '*':
                if (match('*')) t.kind = TK::STARSTAR;
                else if (match('=')) t.kind = TK::STAR_EQ;
                else t.kind                 = TK::STAR;
                break;
            case '/': t.kind = match('=') ? TK::SLASH_EQ : TK::SLASH;
                break;
            case '%': t.kind = match('=') ? TK::PERCENT_EQ : TK::PERCENT;
                break;
            case '=': t.kind = match('=') ? TK::EQ_EQ : TK::EQ;
                break;
            case '!': t.kind = match('=') ? TK::BANG_EQ : TK::LEX_ERROR;
                break;
            case '<': t.kind = match('=') ? TK::LT_EQ : TK::LT;
                break;
            case '>': t.kind = match('=') ? TK::GT_EQ : TK::GT;
                break;
            case '(': t.kind = TK::LPAREN;
                break;
            case ')': t.kind = TK::RPAREN;
                break;
            case '{': t.kind = TK::LBRACE;
                break;
            case '}': t.kind = TK::RBRACE;
                break;
            case '[': t.kind = TK::LBRACKET;
                break;
            case ']': t.kind = TK::RBRACKET;
                break;
            case ',': t.kind = TK::COMMA;
                break;
            case ':': t.kind = TK::COLON;
                break;
            case '.': t.kind = TK::DOT;
                break;
            case ';': t.kind = TK::SEMICOLON;
                break;
            default:
                t.kind = TK::LEX_ERROR;
                t.lexeme = std::string(1, c);
                break;
        }
        t.lexeme = std::string(1, c);
        return t;
    }

    // ============================================================
    // Seccion 4: VshParser
    // ============================================================

    // Acepta como nombre de tipo: IDENT o ciertas palabras clave (fn, null, bool, ...)
    static bool is_type_name_token(TK k) {
        return k == TK::IDENT || k == TK::KW_FN || k == TK::NULL_LIT ||
                k == TK::BOOL_LIT;
    }

    VshParser::VshParser(VshLexer &lex) : lex_(lex) {
        cur_  = lex_.next();
        peek_ = lex_.next();
    }

    Token VshParser::advance() {
        Token t = cur_;
        cur_    = peek_;
        peek_   = lex_.next();
        return t;
    }

    bool VshParser::check(TK k) const {
        return cur_.kind == k;
    }

    bool VshParser::match(TK k) {
        if (!check(k)) return false;
        advance();
        return true;
    }

    Token VshParser::expect(TK k, const char *msg) {
        if (!check(k)) {
            throw VshParseError(
                std::string(msg) + " (se obtuvo '" + cur_.lexeme + "')",
                cur_.line, cur_.col);
        }
        return advance();
    }

    void VshParser::skip_nl() {
        while (check(TK::NEWLINE) || check(TK::SEMICOLON)) advance();
    }

    void VshParser::end_stmt() {
        // termina sentencia con NEWLINE, ';', o implicitamente antes de '}'
        if (check(TK::NEWLINE) || check(TK::SEMICOLON)) {
            advance();
            return;
        }
        if (check(TK::RBRACE) || check(TK::END_OF_FILE)) return;
        throw VshParseError("se esperaba fin de sentencia (nueva linea o ';')",
                            cur_.line, cur_.col);
    }

    AstNodePtr VshParser::parse_program() {
        auto n = make_node(AstKind::Program, cur_.line, cur_.col);
        skip_nl();
        while (!check(TK::END_OF_FILE)) {
            n->args.push_back(stmt());
            skip_nl();
        }
        return n;
    }

    AstNodePtr VshParser::stmt() {
        skip_nl();
        if (check(TK::KW_LET)) return let_decl();
        if (check(TK::KW_FN)) return fn_decl();
        if (check(TK::KW_IF)) return if_stmt();
        if (check(TK::KW_WHILE)) return while_stmt();
        if (check(TK::KW_FOR)) return for_stmt();
        if (check(TK::KW_TRY)) return try_stmt();
        if (check(TK::KW_RETURN)) return ret_stmt();
        if (check(TK::KW_BREAK)) return brk_stmt();
        if (check(TK::KW_CONTINUE)) return cnt_stmt();
        if (check(TK::KW_IMPORT)) return import_stmt();
        if (check(TK::KW_CLASS)) return class_decl();
        if (check(TK::KW_THROW)) return throw_stmt();
        return expr_stmt();
    }

    AstNodePtr VshParser::let_decl() {
        auto n = make_node(AstKind::LetDecl, cur_.line, cur_.col);
        advance(); // 'let'
        n->name = expect(TK::IDENT, "se esperaba nombre de variable despues de 'let'").lexeme;
        if (match(TK::COLON)) {
            if (!is_type_name_token(cur_.kind))
                throw VshParseError("se esperaba nombre de tipo despues de ':'", cur_.line, cur_.col);
            n->type_hint = cur_.lexeme;
            advance();
        }
        if (match(TK::EQ)) {
            n->right = expr();
        }
        end_stmt();
        return n;
    }

    std::vector<std::string> VshParser::params_list(AstNode *n) {
        std::vector<std::string> p;
        std::vector<std::string> types;
        if (check(TK::RPAREN)) {
            if (n) n->param_types = types;
            return p;
        }
        p.push_back(expect(TK::IDENT, "se esperaba nombre de parametro").lexeme);
        std::string t;
        if (match(TK::COLON)) {
            if (!is_type_name_token(cur_.kind))
                throw VshParseError("se esperaba nombre de tipo despues de ':'", cur_.line, cur_.col);
            t = cur_.lexeme;
            advance();
        }
        types.push_back(t);
        while (match(TK::COMMA)) {
            if (check(TK::RPAREN)) break;
            p.push_back(expect(TK::IDENT, "se esperaba nombre de parametro").lexeme);
            t = {};
            if (match(TK::COLON)) {
                if (!is_type_name_token(cur_.kind))
                    throw VshParseError("se esperaba nombre de tipo despues de ':'", cur_.line, cur_.col);
                t = cur_.lexeme;
                advance();
            }
            types.push_back(t);
        }
        if (n) n->param_types = types;
        return p;
    }

    AstNodePtr VshParser::fn_decl() {
        auto n = make_node(AstKind::FnDecl, cur_.line, cur_.col);
        advance(); // 'fn'
        n->name = expect(TK::IDENT, "se esperaba nombre de funcion").lexeme;
        expect(TK::LPAREN, "se esperaba '(' en declaracion de funcion");
        n->params = params_list(n.get());
        expect(TK::RPAREN, "se esperaba ')'");
        n->body = block();
        return n;
    }

    AstNodePtr VshParser::if_stmt() {
        auto n = make_node(AstKind::IfStmt, cur_.line, cur_.col);
        advance(); // 'if'
        IfBranch main_branch;
        main_branch.cond = expr();
        main_branch.body = block();
        n->branches.push_back(std::move(main_branch));
        while (check(TK::KW_ELIF)) {
            advance();
            IfBranch b;
            b.cond = expr();
            b.body = block();
            n->branches.push_back(std::move(b));
        }
        if (match(TK::KW_ELSE)) {
            IfBranch b;
            b.cond = nullptr;
            b.body = block();
            n->branches.push_back(std::move(b));
        }
        return n;
    }

    AstNodePtr VshParser::while_stmt() {
        auto n = make_node(AstKind::WhileStmt, cur_.line, cur_.col);
        advance(); // 'while'
        n->cond = expr();
        n->body = block();
        return n;
    }

    AstNodePtr VshParser::for_stmt() {
        auto n = make_node(AstKind::ForInStmt, cur_.line, cur_.col);
        advance(); // 'for'
        n->name = expect(TK::IDENT, "se esperaba variable de iteracion").lexeme;
        expect(TK::KW_IN, "se esperaba 'in'");
        n->cond = expr(); // expresion iterable (reutiliza campo cond)
        n->body = block();
        return n;
    }

    AstNodePtr VshParser::try_stmt() {
        auto n = make_node(AstKind::TryCatch, cur_.line, cur_.col);
        advance(); // 'try'
        n->try_body = block();
        expect(TK::KW_CATCH, "se esperaba 'catch' despues del bloque 'try'");
        n->name = expect(TK::IDENT, "se esperaba nombre de variable de error en catch").lexeme;
        if (match(TK::COLON)) {
            n->catch_type_hint = expect(TK::IDENT, "se esperaba nombre de tipo en catch tipado").lexeme;
        }
        n->catch_body = block();
        return n;
    }

    AstNodePtr VshParser::ret_stmt() {
        auto n = make_node(AstKind::Return, cur_.line, cur_.col);
        advance(); // 'return'
        if (!check(TK::NEWLINE) && !check(TK::SEMICOLON) && !check(TK::RBRACE) && !check(TK::END_OF_FILE))
            n->right = expr();
        end_stmt();
        return n;
    }

    AstNodePtr VshParser::brk_stmt() {
        auto n = make_node(AstKind::Break, cur_.line, cur_.col);
        advance();
        end_stmt();
        return n;
    }

    AstNodePtr VshParser::cnt_stmt() {
        auto n = make_node(AstKind::Continue, cur_.line, cur_.col);
        advance();
        end_stmt();
        return n;
    }

    AstNodePtr VshParser::import_stmt() {
        auto n = make_node(AstKind::Import, cur_.line, cur_.col);
        advance(); // 'import'
        auto tok = expect(TK::STRING_LIT, "se esperaba ruta de fichero en 'import'");
        n->name  = tok.lexeme;
        end_stmt();
        return n;
    }



    AstNodePtr VshParser::class_decl() {
        auto n = make_node(AstKind::ClassDecl, cur_.line, cur_.col);
        advance(); // 'class'
        n->name = expect(TK::IDENT, "se esperaba nombre de clase").lexeme;
        if (match(TK::COLON)) {
            n->parent_class = expect(TK::IDENT, "se esperaba nombre de clase padre").lexeme;
        }
        skip_nl();
        expect(TK::LBRACE, "se esperaba '{' en cuerpo de clase");
        skip_nl();
        while (!check(TK::RBRACE) && !check(TK::END_OF_FILE)) {
            if (check(TK::KW_FN)) {
                n->args.push_back(fn_decl());
            } else if (check(TK::STRING_LIT) || check(TK::ISTR_TEXT) || check(TK::ISTR_EXPR_BEGIN)) {
                // docstring de clase: primer elemento string literal
                n->args.push_back(expr_stmt());
            } else {
                throw VshParseError("se esperaba 'fn' o docstring dentro del cuerpo de clase",
                                    cur_.line, cur_.col);
            }
            skip_nl();
        }
        expect(TK::RBRACE, "se esperaba '}' para cerrar cuerpo de clase");
        return n;
    }

    AstNodePtr VshParser::throw_stmt() {
        auto n = make_node(AstKind::ThrowStmt, cur_.line, cur_.col);
        advance(); // 'throw'
        n->right = expr();
        end_stmt();
        return n;
    }

    AstNodePtr VshParser::block() {
        skip_nl();
        auto n = make_node(AstKind::Block, cur_.line, cur_.col);
        expect(TK::LBRACE, "se esperaba '{' para abrir bloque");
        skip_nl();
        while (!check(TK::RBRACE) && !check(TK::END_OF_FILE)) {
            n->args.push_back(stmt());
            skip_nl();
        }
        expect(TK::RBRACE, "se esperaba '}' para cerrar bloque");
        return n;
    }

    AstNodePtr VshParser::expr_stmt() {
        auto n  = make_node(AstKind::ExprStmt, cur_.line, cur_.col);
        n->left = expr();
        end_stmt();
        return n;
    }

    // ---- Expresiones ----

    AstNodePtr VshParser::expr() {
        return assign_expr();
    }

    AstNodePtr VshParser::assign_expr() {
        auto lhs = or_expr();
        // asignacion: target = value  o  target OP= value
        static const TK compound[] = {TK::PLUS_EQ, TK::MINUS_EQ, TK::STAR_EQ, TK::SLASH_EQ, TK::PERCENT_EQ};
        for (auto op: compound) {
            if (check(op)) {
                TK  op2 = cur_.kind;
                int ln  = cur_.line, co = cur_.col;
                advance();
                auto val = expr();
                auto n   = make_node(AstKind::CompoundAssign, ln, co);
                n->op    = op2;
                n->left  = lhs;
                n->right = val;
                return n;
            }
        }
        if (check(TK::EQ)) {
            int ln = cur_.line, co = cur_.col;
            advance();
            auto val = expr();
            auto n   = make_node(AstKind::Assign, ln, co);
            n->left  = lhs;
            n->right = val;
            return n;
        }
        return lhs;
    }

    AstNodePtr VshParser::or_expr() {
        auto lhs = and_expr();
        while (check(TK::KW_OR)) {
            TK  op = cur_.kind;
            int ln = cur_.line, co = cur_.col;
            advance();
            auto rhs = and_expr();
            auto n   = make_node(AstKind::BinOp, ln, co);
            n->op    = op;
            n->left  = lhs;
            n->right = rhs;
            lhs      = n;
        }
        return lhs;
    }

    AstNodePtr VshParser::and_expr() {
        auto lhs = not_expr();
        while (check(TK::KW_AND)) {
            TK  op = cur_.kind;
            int ln = cur_.line, co = cur_.col;
            advance();
            auto rhs = not_expr();
            auto n   = make_node(AstKind::BinOp, ln, co);
            n->op    = op;
            n->left  = lhs;
            n->right = rhs;
            lhs      = n;
        }
        return lhs;
    }

    AstNodePtr VshParser::not_expr() {
        if (check(TK::KW_NOT)) {
            int ln = cur_.line, co = cur_.col;
            TK  op = cur_.kind;
            advance();
            auto n  = make_node(AstKind::UnaryOp, ln, co);
            n->op   = op;
            n->left = not_expr();
            return n;
        }
        return cmp_expr();
    }

    AstNodePtr VshParser::cmp_expr() {
        auto lhs = add_expr();
        while (check(TK::EQ_EQ) || check(TK::BANG_EQ) || check(TK::LT) || check(TK::GT) ||
            check(TK::LT_EQ) || check(TK::GT_EQ)) {
            TK  op = cur_.kind;
            int ln = cur_.line, co = cur_.col;
            advance();
            auto rhs = add_expr();
            auto n   = make_node(AstKind::BinOp, ln, co);
            n->op    = op;
            n->left  = lhs;
            n->right = rhs;
            lhs      = n;
        }
        return lhs;
    }

    AstNodePtr VshParser::add_expr() {
        auto lhs = mul_expr();
        while (check(TK::PLUS) || check(TK::MINUS)) {
            TK  op = cur_.kind;
            int ln = cur_.line, co = cur_.col;
            advance();
            auto rhs = mul_expr();
            auto n   = make_node(AstKind::BinOp, ln, co);
            n->op    = op;
            n->left  = lhs;
            n->right = rhs;
            lhs      = n;
        }
        return lhs;
    }

    AstNodePtr VshParser::mul_expr() {
        auto lhs = pow_expr();
        while (check(TK::STAR) || check(TK::SLASH) || check(TK::PERCENT)) {
            TK  op = cur_.kind;
            int ln = cur_.line, co = cur_.col;
            advance();
            auto rhs = pow_expr();
            auto n   = make_node(AstKind::BinOp, ln, co);
            n->op    = op;
            n->left  = lhs;
            n->right = rhs;
            lhs      = n;
        }
        return lhs;
    }

    AstNodePtr VshParser::pow_expr() {
        auto lhs = unary_expr();
        if (check(TK::STARSTAR)) {
            TK  op = cur_.kind;
            int ln = cur_.line, co = cur_.col;
            advance();
            auto rhs = pow_expr(); // asociatividad derecha: recursion
            auto n   = make_node(AstKind::BinOp, ln, co);
            n->op    = op;
            n->left  = lhs;
            n->right = rhs;
            return n;
        }
        return lhs;
    }

    AstNodePtr VshParser::unary_expr() {
        if (check(TK::MINUS) || check(TK::KW_NOT)) {
            TK  op = cur_.kind;
            int ln = cur_.line, co = cur_.col;
            advance();
            auto n  = make_node(AstKind::UnaryOp, ln, co);
            n->op   = op;
            n->left = unary_expr();
            return n;
        }
        return postfix_expr();
    }

    std::vector<AstNodePtr> VshParser::args_list() {
        std::vector<AstNodePtr> v;
        if (check(TK::RPAREN)) return v;
        v.push_back(expr());
        while (match(TK::COMMA)) {
            if (check(TK::RPAREN)) break; // trailing comma
            v.push_back(expr());
        }
        return v;
    }

    AstNodePtr VshParser::postfix_expr() {
        auto lhs = primary();
        for (;;) {
            if (check(TK::LPAREN)) {
                int ln = cur_.line, co = cur_.col;
                advance();
                auto n    = make_node(AstKind::Call, ln, co);
                n->callee = lhs;
                n->args   = args_list();
                expect(TK::RPAREN, "se esperaba ')' en llamada a funcion");
                lhs = n;
            } else if (check(TK::LBRACKET)) {
                int ln = cur_.line, co = cur_.col;
                advance();
                auto n   = make_node(AstKind::Index, ln, co);
                n->left  = lhs;
                n->right = expr();
                expect(TK::RBRACKET, "se esperaba ']' en indexado");
                lhs = n;
            } else if (check(TK::DOT)) {
                int ln = cur_.line, co = cur_.col;
                advance();
                std::string field = expect(TK::IDENT, "se esperaba nombre de campo despues de '.'").lexeme;
                auto        n     = make_node(AstKind::FieldAccess, ln, co);
                n->left           = lhs;
                n->name           = field;
                lhs               = n;
            } else {
                break;
            }
        }
        return lhs;
    }

    AstNodePtr VshParser::fn_expr() {
        auto n = make_node(AstKind::FnExpr, cur_.line, cur_.col);
        advance(); // 'fn'
        expect(TK::LPAREN, "se esperaba '('");
        n->params = params_list(n.get());
        expect(TK::RPAREN, "se esperaba ')'");
        n->body = block();
        return n;
    }

    AstNodePtr VshParser::list_lit() {
        auto n = make_node(AstKind::ListLit, cur_.line, cur_.col);
        advance(); // '['
        skip_nl();
        if (!check(TK::RBRACKET)) {
            n->args.push_back(expr());
            skip_nl();
            while (match(TK::COMMA)) {
                skip_nl();
                if (check(TK::RBRACKET)) break;
                n->args.push_back(expr());
                skip_nl();
            }
        }
        expect(TK::RBRACKET, "se esperaba ']' al cerrar lista");
        return n;
    }

    AstNodePtr VshParser::map_lit() {
        auto n = make_node(AstKind::MapLit, cur_.line, cur_.col);
        advance(); // '{'
        skip_nl();
        if (!check(TK::RBRACE)) {
            // pares key: valor; la key puede ser STRING_LIT o IDENT
            auto read_key = [&]() -> AstNodePtr {
                if (check(TK::STRING_LIT)) {
                    auto kn     = make_node(AstKind::Literal, cur_.line, cur_.col);
                    kn->lit_val = Value(cur_.lexeme);
                    advance();
                    return kn;
                } else if (check(TK::IDENT)) {
                    auto kn     = make_node(AstKind::Literal, cur_.line, cur_.col);
                    kn->lit_val = Value(cur_.lexeme);
                    advance();
                    return kn;
                }
                throw VshParseError("se esperaba clave de mapa (string o identificador)",
                                    cur_.line, cur_.col);
            };
            n->args.push_back(read_key());
            expect(TK::COLON, "se esperaba ':' despues de clave de mapa");
            n->args.push_back(expr());
            skip_nl();
            while (match(TK::COMMA)) {
                skip_nl();
                if (check(TK::RBRACE)) break;
                n->args.push_back(read_key());
                expect(TK::COLON, "se esperaba ':' despues de clave de mapa");
                n->args.push_back(expr());
                skip_nl();
            }
        }
        expect(TK::RBRACE, "se esperaba '}' al cerrar mapa");
        return n;
    }

    AstNodePtr VshParser::interp_string() {
        // El primer token ya esta en cur_ y es ISTR_TEXT o ISTR_EXPR_BEGIN o ISTR_END
        // Construimos un nodo InterpString cuyas partes son:
        //   - Literal(string) para ISTR_TEXT
        //   - sub-expresiones entre ISTR_EXPR_BEGIN y ISTR_EXPR_END
        auto n = make_node(AstKind::InterpString, cur_.line, cur_.col);
        while (!check(TK::ISTR_END) && !check(TK::END_OF_FILE)) {
            if (check(TK::ISTR_TEXT)) {
                auto part     = make_node(AstKind::Literal, cur_.line, cur_.col);
                part->lit_val = Value(cur_.lexeme);
                advance();
                n->args.push_back(part);
            } else if (check(TK::ISTR_EXPR_BEGIN)) {
                advance(); // consume ISTR_EXPR_BEGIN
                n->args.push_back(expr());
                expect(TK::ISTR_EXPR_END, "se esperaba '}' al cerrar expresion embebida en string");
            } else {
                break;
            }
        }
        if (check(TK::ISTR_END)) advance();
        return n;
    }

    AstNodePtr VshParser::primary() {
        // Literales escalares
        if (check(TK::INT_LIT)) {
            auto n     = make_node(AstKind::Literal, cur_.line, cur_.col);
            n->lit_val = Value(cur_.int_val);
            advance();
            return n;
        }
        if (check(TK::FLOAT_LIT)) {
            auto n     = make_node(AstKind::Literal, cur_.line, cur_.col);
            n->lit_val = Value(cur_.flt_val);
            advance();
            return n;
        }
        if (check(TK::STRING_LIT)) {
            auto n     = make_node(AstKind::Literal, cur_.line, cur_.col);
            n->lit_val = Value(cur_.lexeme);
            advance();
            return n;
        }
        if (check(TK::BOOL_LIT)) {
            auto n     = make_node(AstKind::Literal, cur_.line, cur_.col);
            n->lit_val = Value(bool(cur_.int_val != 0));
            advance();
            return n;
        }
        if (check(TK::NULL_LIT)) {
            auto n = make_node(AstKind::Literal, cur_.line, cur_.col);
            advance();
            return n; // lit_val queda Null por defecto
        }
        // String interpolada: el lexer ya emitio los sub-tokens en buf_
        if (check(TK::ISTR_TEXT) || check(TK::ISTR_EXPR_BEGIN)) return interp_string();
        // Solo ISTR_END (string interpolada vacia "")
        if (check(TK::ISTR_END)) {
            auto n     = make_node(AstKind::Literal, cur_.line, cur_.col);
            n->lit_val = Value(std::string{});
            advance();
            return n;
        }
        // Identificador o 'super'
        if (check(TK::IDENT) || check(TK::KW_SUPER)) {
            auto n  = make_node(AstKind::Ident, cur_.line, cur_.col);
            n->name = cur_.lexeme;
            advance();
            return n;
        }
        // Funcion anonima
        if (check(TK::KW_FN)) return fn_expr();
        // Lista
        if (check(TK::LBRACKET)) return list_lit();
        // Mapa: '{' puede ser bloque o mapa; si le sigue STRING/IDENT ':' es mapa.
        // Salta NEWLINEs en el lookahead para soportar mapas multi-linea:
        //   { \n  "key": val, ... }
        if (check(TK::LBRACE)) {
            bool is_map = false;
            // t1: primer token no-NEWLINE tras '{'; busca en peek_ y luego en lex_.peek(n)
            TK  t1  = peek_.kind;
            int off = 0; // indice en lex_.peek() a usar si peek_ es NEWLINE
            while (t1 == TK::NEWLINE || t1 == TK::SEMICOLON)
                t1 = lex_.peek(off++).kind;
            if (t1 == TK::STRING_LIT || t1 == TK::IDENT) {
                // t2: token que sigue a la clave (debe ser ':' para que sea mapa)
                TK t2 = lex_.peek(off).kind;
                while (t2 == TK::NEWLINE || t2 == TK::SEMICOLON)
                    t2 = lex_.peek(++off).kind;
                if (t2 == TK::COLON) is_map = true;
            }
            if (t1 == TK::RBRACE) is_map = true; // mapa vacio {}
            if (is_map) return map_lit();
            // bloque en posicion de expresion: no permitido
            throw VshParseError("'{ }' inesperado en expresion; usa map() o lista de sentencias",
                                cur_.line, cur_.col);
        }
        // Parentesis agrupador
        if (check(TK::LPAREN)) {
            advance();
            auto n = expr();
            expect(TK::RPAREN, "se esperaba ')' para cerrar expresion entre parentesis");
            return n;
        }
        throw VshParseError("expresion inesperada: '" + cur_.lexeme + "'", cur_.line, cur_.col);
    }

    // ============================================================
    // Seccion 5: VshInterpreter
    // ============================================================

    VshInterpreter::~VshInterpreter() {
        flush_out_buffer();
        fflush(stdout);
    }

    VshInterpreter::VshInterpreter(ReplDispatch dispatch)
        : global_(std::make_shared<VshEnv>()), repl_dispatch_(std::move(dispatch)) {
        register_builtins();
        std::ios::sync_with_stdio(false);
        std::cout.tie(nullptr);
        std::cin.tie(nullptr);
    }

    void VshInterpreter::register_builtin(const std::string &name, NativeFn fn) {
        builtins_[name] = std::move(fn);
    }

    void VshInterpreter::exec_file(const std::string &path) {
        std::ifstream f(path);
        if (!f) throw VshRuntimeError("no se puede abrir el fichero: " + path);
        std::string src((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

        // Guardar contexto anterior para restaurarlo si esto es una importacion
        bool  is_import = !import_stack_.empty();
        Value prev_name = global_->get("__name__");
        Value prev_file = global_->get("__file__");

        // __name__ segun si es script principal o modulo importado
        if (is_import) {
            std::string mod = std::filesystem::path(path).stem().string();
            global_->define("__name__", Value(mod));
        } else {
            global_->define("__name__", Value(std::string("__main__")));
        }
        global_->define("__file__", Value(path));

        try {
            exec_string(src, path);
        } catch (...) {
            // Restaurar contexto del importador antes de propagar la excepcion
            if (is_import) {
                global_->define("__name__", prev_name);
                global_->define("__file__", prev_file);
            }
            throw;
        }
        // Restaurar contexto del importador al terminar la importacion
        if (is_import) {
            global_->define("__name__", prev_name);
            global_->define("__file__", prev_file);
        }
    }

    void VshInterpreter::exec_string(const std::string &src, const std::string &name) {
        VshLexer  lex(src, name);
        VshParser par(lex);
        auto      ast = par.parse_program();
        exec(ast, global_);
    }

    /** @brief Extrae el docstring de un cuerpo de funcion (primera sentencia string literal). */
    static std::string extract_docstring(const std::shared_ptr<AstNode> &body) {
        if (!body || body->args.empty()) return {};
        const auto &first = body->args[0];
        // sentencia de expresion cuyo unico contenido es un literal string
        if (first->kind == AstKind::ExprStmt && first->left &&
            first->left->kind == AstKind::Literal &&
            first->left->lit_val.is_string()) {
            return first->left->lit_val.as_string();
        }
        return {};
    }

    // ---- dispatch principal ----

    Value VshInterpreter::eval(const AstNodePtr &n, std::shared_ptr<VshEnv> env) {
        switch (n->kind) {
            case AstKind::Literal: return eval_literal(*n);
            case AstKind::InterpString: return eval_interp_str(*n, env);
            case AstKind::Ident: return eval_ident(*n, env);
            case AstKind::BinOp: return eval_binop(*n, env);
            case AstKind::UnaryOp: return eval_unary(*n, env);
            case AstKind::Assign: return eval_assign(*n, env);
            case AstKind::CompoundAssign: return eval_compound(*n, env);
            case AstKind::Call: return eval_call(*n, env);
            case AstKind::Index: return eval_index(*n, env);
            case AstKind::ListLit: return eval_list_lit(*n, env);
            case AstKind::MapLit: return eval_map_lit(*n, env);
            case AstKind::FieldAccess: return eval_field(*n, env);
            case AstKind::FnExpr: {
                auto fn         = std::make_shared<VshFunction>();
                fn->params      = n->params;
                fn->param_types = n->param_types;
                fn->body        = n->body;
                fn->closure_env = env;
                fn->doc         = extract_docstring(n->body);
                return Value(fn);
            }
            default:
                throw VshRuntimeError("eval() llamado con nodo no-expresion", n->line, n->col);
        }
    }

    void VshInterpreter::exec(const AstNodePtr &n, std::shared_ptr<VshEnv> env) {
        switch (n->kind) {
            case AstKind::Program:
            case AstKind::Block: exec_block(*n, env);
                break;
            case AstKind::ExprStmt: eval(n->left, env);
                break;
            case AstKind::LetDecl: exec_let(*n, env);
                break;
            case AstKind::FnDecl: exec_fn_decl(*n, env);
                break;
            case AstKind::ClassDecl: exec_class_decl(*n, env);
                break;
            case AstKind::ThrowStmt: {
                Value thrown = n->right ? eval(n->right, env) : Value{};
                if (thrown.is_instance()) {
                    auto        inst = thrown.as_instance();
                    std::string msg  = inst->klass->name;
                    auto        it   = inst->attrs.find("message");
                    if (it != inst->attrs.end()) msg = it->second.to_string();
                    throw VshInstanceError(msg, thrown, n->line, n->col);
                }
                throw VshRuntimeError(thrown.to_string(), n->line, n->col);
            }
            case AstKind::IfStmt: exec_if(*n, env);
                break;
            case AstKind::WhileStmt: exec_while(*n, env);
                break;
            case AstKind::ForInStmt: exec_for_in(*n, env);
                break;
            case AstKind::TryCatch: exec_try(*n, env);
                break;
            case AstKind::Import: exec_import(*n, env);
                break;
            case AstKind::Return: {
                Value v = n->right ? eval(n->right, env) : Value{};
                throw ReturnSignal{std::move(v)};
            }
            case AstKind::Break: throw BreakSignal{n->line};
            case AstKind::Continue: throw ContinueSignal{n->line};
            default:
                // sentencias que son expresiones (assign, call como stmt...)
                eval(n, env);
                break;
        }
    }

    // ---- evaluadores de expresiones ----

    Value VshInterpreter::eval_literal(const AstNode &n) {
        return n.lit_val;
    }

    Value VshInterpreter::eval_interp_str(const AstNode &n, std::shared_ptr<VshEnv> env) {
        std::string result;
        for (auto &part: n.args) {
            result += eval(part, env).to_string();
        }
        return Value(result);
    }

    void VshInterpreter::set_argv(const std::vector<std::string> &argv) {
        auto lst = std::make_shared<std::vector<Value> >();
        for (auto &a: argv) lst->push_back(Value(a));
        global_->define("ARGV", Value(lst));
    }

    Value VshInterpreter::eval_ident(const AstNode &n, std::shared_ptr<VshEnv> env) {
        return env->get(n.name);
    }

    Value VshInterpreter::eval_binop(const AstNode &n, std::shared_ptr<VshEnv> env) {
        // cortocircuito para and/or (evalua derecho solo si necesario)
        if (n.op == TK::KW_OR) {
            auto lv = eval(n.left, env);
            return lv.truthy() ? lv : eval(n.right, env);
        }
        if (n.op == TK::KW_AND) {
            auto lv = eval(n.left, env);
            return !lv.truthy() ? lv : eval(n.right, env);
        }
        Value lv = eval(n.left, env);
        Value rv = eval(n.right, env);
        switch (n.op) {
            case TK::PLUS:
                if (lv.is_string() || rv.is_string())
                    return Value(lv.to_string() + rv.to_string());
                if (lv.is_int() && rv.is_int()) return Value(lv.as_int() + rv.as_int());
                if (lv.is_float() || rv.is_float()) return Value(lv.as_number() + rv.as_number());
                if (lv.is_list()) {
                    auto lst = std::make_shared<std::vector<Value> >(*lv.as_list());
                    for (auto &e: *rv.as_list()) lst->push_back(e);
                    return Value(lst);
                }
                throw VshRuntimeError("operador '+' no soportado para " + lv.to_string(), n.line);
            case TK::MINUS:
                if (lv.is_int() && rv.is_int()) return Value(lv.as_int() - rv.as_int());
                return Value(lv.as_number() - rv.as_number());
            case TK::STAR:
                if (lv.is_int() && rv.is_int()) return Value(lv.as_int() * rv.as_int());
                if (lv.is_string() && rv.is_int()) {
                    std::string r;
                    for (int64_t i = 0; i < rv.as_int(); ++i) r += lv.as_string();
                    return Value(r);
                }
                return Value(lv.as_number() * rv.as_number());
            case TK::SLASH:
                if (rv.is_int() && rv.as_int() == 0) throw VshRuntimeError("division por cero", n.line);
                if (rv.is_float() && rv.as_float() == 0.0) throw VshRuntimeError("division por cero", n.line);
                if (lv.is_int() && rv.is_int()) return Value(lv.as_int() / rv.as_int());
                return Value(lv.as_number() / rv.as_number());
            case TK::PERCENT:
                if (lv.is_int() && rv.is_int()) return Value(lv.as_int() % rv.as_int());
                return Value(std::fmod(lv.as_number(), rv.as_number()));
            case TK::STARSTAR:
                return Value(std::pow(lv.as_number(), rv.as_number()));
            case TK::EQ_EQ: return Value(lv == rv);
            case TK::BANG_EQ: return Value(lv != rv);
            case TK::LT: return Value(lv < rv);
            case TK::GT: return Value(lv > rv);
            case TK::LT_EQ: return Value(lv <= rv);
            case TK::GT_EQ: return Value(lv >= rv);
            default:
                throw VshRuntimeError("operador binario desconocido", n.line);
        }
    }

    Value VshInterpreter::eval_unary(const AstNode &n, std::shared_ptr<VshEnv> env) {
        Value v = eval(n.left, env);
        if (n.op == TK::MINUS) {
            if (v.is_int()) return Value(-v.as_int());
            if (v.is_float()) return Value(-v.as_float());
            throw VshRuntimeError("'-' unario no aplicable a " + v.to_string(), n.line);
        }
        if (n.op == TK::KW_NOT) return Value(!v.truthy());
        throw VshRuntimeError("operador unario desconocido", n.line);
    }

    Value VshInterpreter::eval_assign(const AstNode &n, std::shared_ptr<VshEnv> env) {
        Value val = eval(n.right, env);
        if (n.left->kind == AstKind::Ident) {
            env->assign(n.left->name, val);
            return val;
        }
        if (n.left->kind == AstKind::FieldAccess) {
            Value              obj   = eval(n.left->left, env);
            const std::string &field = n.left->name;
            if (!obj.is_instance())
                throw VshRuntimeError("asignacion de campo '.' solo para instancias", n.line);
            obj.as_instance()->attrs[field] = val;
            return val;
        }
        if (n.left->kind == AstKind::Index) {
            Value container = eval(n.left->left, env);
            Value key       = eval(n.left->right, env);
            if (container.is_list()) {
                if (!key.is_int()) throw VshRuntimeError("indice de lista debe ser entero", n.line);
                auto &  lst = *container.as_list();
                int64_t idx = key.as_int();
                if (idx < 0) idx += (int64_t) lst.size();
                if (idx < 0 || (size_t) idx >= lst.size())
                    throw VshRuntimeError("indice fuera de rango", n.line);
                lst[idx] = val;
                return val;
            }
            if (container.is_map()) {
                (*container.as_map())[key.to_string()] = val;
                return val;
            }
            throw VshRuntimeError("asignacion por indice solo para listas y mapas", n.line);
        }
        throw VshRuntimeError("el lado izquierdo de '=' no es asignable", n.line);
    }

    Value VshInterpreter::eval_compound(const AstNode &n, std::shared_ptr<VshEnv> env) {
        // Construir BinOp sintetico y luego asignar
        AstNode binop_node;
        binop_node.kind  = AstKind::BinOp;
        binop_node.line  = n.line;
        binop_node.col   = n.col;
        binop_node.left  = n.left;
        binop_node.right = n.right;
        switch (n.op) {
            case TK::PLUS_EQ: binop_node.op = TK::PLUS;
                break;
            case TK::MINUS_EQ: binop_node.op = TK::MINUS;
                break;
            case TK::STAR_EQ: binop_node.op = TK::STAR;
                break;
            case TK::SLASH_EQ: binop_node.op = TK::SLASH;
                break;
            case TK::PERCENT_EQ: binop_node.op = TK::PERCENT;
                break;
            default: throw VshRuntimeError("operador compuesto desconocido", n.line);
        }
        auto  bp     = std::make_shared<AstNode>(binop_node);
        Value result = eval_binop(binop_node, env);
        if (n.left->kind == AstKind::Ident) {
            env->assign(n.left->name, result);
        }
        return result;
    }

    Value VshInterpreter::eval_call(const AstNode &n, std::shared_ptr<VshEnv> env) {
        // ---- caso especial: obj.method(args) ----
        if (n.callee->kind == AstKind::FieldAccess) {
            Value              obj         = eval(n.callee->left, env);
            const std::string &method_name = n.callee->name;
            std::vector<Value> extra_args;
            for (auto &a: n.args) extra_args.push_back(eval(a, env));
            if (obj.is_instance()) {
                auto inst = obj.as_instance();
                // atributo callable (fn guardado en instancia)
                auto it = inst->attrs.find(method_name);
                if (it != inst->attrs.end()) {
                    if (it->second.is_function())
                        return call_method_impl(it->second.as_fn(), obj, std::move(extra_args), n.line);
                    throw VshRuntimeError("atributo '" + method_name + "' no es callable", n.line);
                }
                // metodo en la clase
                auto method = find_method(inst->klass, method_name);
                if (method) return call_method_impl(method, obj, std::move(extra_args), n.line);
                throw VshRuntimeError("metodo no encontrado: '" + method_name +
                                      "' en " + inst->klass->name, n.line);
            }
            if (obj.is_class()) {
                // metodo de clase sin instancia (llamada estatica)
                auto method = find_method(obj.as_class(), method_name);
                if (method) return call_fn(Value(method), std::move(extra_args), n.line);
                throw VshRuntimeError("metodo no encontrado: '" + method_name +
                                      "' en clase " + obj.as_class()->name, n.line);
            }
            throw VshRuntimeError("acceso '.' solo para instancias y clases", n.line);
        }

        bool        is_ident = (n.callee->kind == AstKind::Ident);
        std::string fn_name  = is_ident ? n.callee->name : "";

        // evaluar argumentos
        std::vector<Value> args;
        for (auto &a: n.args) args.push_back(eval(a, env));

        // 1. buscar en el scope del script
        if (is_ident && env->has(fn_name)) {
            Value callee = env->get(fn_name);
            if (callee.is_function()) return call_fn(callee, std::move(args), n.line);
            if (callee.is_class()) {
                // constructor: crear instancia y llamar __init__ si existe
                auto klass     = callee.as_class();
                auto inst      = std::make_shared<VshInstance>();
                inst->klass    = klass;
                Value inst_val = make_instance_val(inst);
                auto  init     = find_method(klass, "__init__");
                if (init) call_method_impl(init, inst_val, std::move(args), n.line);
                return inst_val;
            }
            throw VshRuntimeError("'" + fn_name + "' no es una funcion ni clase", n.line);
        }

        // 2. buscar en built-ins
        if (is_ident) {
            auto bit = builtins_.find(fn_name);
            if (bit != builtins_.end()) {
                try {
                    return bit->second(std::move(args));
                } catch (VshRuntimeError &e) {
                    if (e.line == 0) throw VshRuntimeError(e.vsh_msg, n.line, n.col);
                    throw;
                }
            }
        }

        // 3. si el callee es una expresion evaluable (no ident)
        if (!is_ident) {
            Value callee = eval(n.callee, env);
            if (callee.is_function()) return call_fn(callee, std::move(args), n.line);
            if (callee.is_class()) {
                auto klass     = callee.as_class();
                auto inst      = std::make_shared<VshInstance>();
                inst->klass    = klass;
                Value inst_val = make_instance_val(inst);
                auto  init     = find_method(klass, "__init__");
                if (init) call_method_impl(init, inst_val, std::move(args), n.line);
                return inst_val;
            }
            throw VshRuntimeError("no es una funcion ni clase llamable", n.line);
        }

        // 4. fallback: despachar como comando del REPL
        if (repl_dispatch_) {
            std::string cmd_line = fn_name;
            for (auto &v: args) {
                cmd_line += ' ';
                std::string s = v.to_string();
                // citar con espacios
                if (s.find(' ') != std::string::npos || s.find('"') != std::string::npos) {
                    std::string esc;
                    for (char c: s) {
                        if (c == '"') esc += "\\\"";
                        else esc += c;
                    }
                    cmd_line += '"' + esc + '"';
                } else {
                    cmd_line += s;
                }
            }
            repl_dispatch_(cmd_line);
            return Value{}; // los comandos del REPL devuelven null al script
        }

        throw VshRuntimeError("funcion desconocida: '" + fn_name + "'", n.line, n.col);
    }

    Value VshInterpreter::eval_index(const AstNode &n, std::shared_ptr<VshEnv> env) {
        Value container = eval(n.left, env);
        Value key       = eval(n.right, env);
        if (container.is_list()) {
            if (!key.is_int())
                throw VshRuntimeError("indice de lista debe ser entero, se obtuvo: " + key.to_string(), n.line);
            auto &  lst = *container.as_list();
            int64_t idx = key.as_int();
            if (idx < 0) idx += (int64_t) lst.size();
            if (idx < 0 || (size_t) idx >= lst.size())
                throw VshRuntimeError("indice fuera de rango: " + std::to_string(key.as_int()), n.line);
            return lst[idx];
        }
        if (container.is_map()) {
            std::string k  = key.to_string();
            auto &      m  = *container.as_map();
            auto        it = m.find(k);
            if (it == m.end())
                throw VshRuntimeError("clave no encontrada en mapa: '" + k + "'", n.line);
            return it->second;
        }
        if (container.is_string()) {
            if (!key.is_int())
                throw VshRuntimeError("indice de string debe ser entero", n.line);
            const auto &s   = container.as_string();
            int64_t     idx = key.as_int();
            if (idx < 0) idx += (int64_t) s.size();
            if (idx < 0 || (size_t) idx >= s.size())
                throw VshRuntimeError("indice de string fuera de rango", n.line);
            return Value(std::string(1, s[idx]));
        }
        throw VshRuntimeError("indexado no soportado para " + container.to_string(), n.line);
    }

    Value VshInterpreter::eval_list_lit(const AstNode &n, std::shared_ptr<VshEnv> env) {
        auto lst = std::make_shared<std::vector<Value> >();
        for (auto &e: n.args) lst->push_back(eval(e, env));
        return Value(lst);
    }

    Value VshInterpreter::eval_map_lit(const AstNode &n, std::shared_ptr<VshEnv> env) {
        auto mp = std::make_shared<std::unordered_map<std::string, Value> >();
        // args contiene pares (key_node, value_node)
        for (size_t i = 0; i + 1 < n.args.size(); i += 2) {
            std::string k = eval(n.args[i], env).to_string();
            (*mp)[k]      = eval(n.args[i + 1], env);
        }
        return Value(mp);
    }

    // ---- ejecutores de sentencias ----

    void VshInterpreter::exec_block(const AstNode &n, std::shared_ptr<VshEnv> env) {
        // Program ejecuta en el env dado; Block crea un scope hijo
        auto child = (n.kind == AstKind::Block) ? env->make_child() : env;
        for (auto &s: n.args) exec(s, child);
    }

    void VshInterpreter::exec_let(const AstNode &n, std::shared_ptr<VshEnv> env) {
        Value v = n.right ? eval(n.right, env) : Value{};
        if (!n.type_hint.empty()) {
            check_type_hint(n.type_hint, v, "variable '" + n.name + "'", n.line, env);
        }
        env->define(n.name, std::move(v));
    }

    void VshInterpreter::exec_fn_decl(const AstNode &n, std::shared_ptr<VshEnv> env) {
        auto fn         = std::make_shared<VshFunction>();
        fn->name        = n.name;
        fn->params      = n.params;
        fn->param_types = n.param_types;
        fn->body        = n.body; // AstNodePtr (Block)
        fn->closure_env = env;
        fn->doc         = extract_docstring(std::static_pointer_cast<AstNode>(fn->body));
        env->define(n.name, Value(fn));
    }

    void VshInterpreter::exec_if(const AstNode &n, std::shared_ptr<VshEnv> env) {
        for (auto &branch: n.branches) {
            if (branch.cond == nullptr || eval(branch.cond, env).truthy()) {
                exec(branch.body, env);
                return;
            }
        }
    }

    void VshInterpreter::exec_while(const AstNode &n, std::shared_ptr<VshEnv> env) {
        while (eval(n.cond, env).truthy()) {
            try {
                exec(n.body, env);
            } catch (BreakSignal &) {
                break;
            }
            catch (ContinueSignal &) {
                continue;
            }
        }
    }

    void VshInterpreter::exec_for_in(const AstNode &n, std::shared_ptr<VshEnv> env) {
        Value iter     = eval(n.cond, env); // n.cond reutilizado como iterable
        auto  loop_env = env->make_child();

        auto run_body = [&](Value v) {
            loop_env->vars[n.name] = std::move(v); // define/redefine la var del bucle
            try {
                exec(n.body, loop_env);
            } catch (ContinueSignal &) {
                /* siguiente iteracion */
            }
        };

        try {
            if (iter.is_list()) {
                for (auto &v: *iter.as_list()) run_body(v);
            } else if (iter.is_map()) {
                for (auto &[k, _]: *iter.as_map()) run_body(Value(k));
            } else if (iter.is_string()) {
                for (char c: iter.as_string()) run_body(Value(std::string(1, c)));
            } else {
                throw VshRuntimeError("'for in' requiere un iterable (lista, mapa o string)",
                                      n.line, n.col);
            }
        } catch (BreakSignal &) {
            /* terminar el bucle */
        }
    }

    void VshInterpreter::exec_try(const AstNode &n, std::shared_ptr<VshEnv> env) {
        try {
            exec(n.try_body, env);
        } catch (VshInstanceError &e) {
            // catch tipado: comprobar jerarquia de clases
            if (!n.catch_type_hint.empty()) {
                bool matches = false;
                if (e.thrown_val.is_instance()) {
                    auto klass = e.thrown_val.as_instance()->klass;
                    while (klass) {
                        if (klass->name == n.catch_type_hint) {
                            matches = true;
                            break;
                        }
                        klass = klass->parent;
                    }
                }
                if (!matches) throw; // tipo no coincide: relanzar
            }
            auto catch_env = env->make_child();
            catch_env->define(n.name, e.thrown_val); // la variable catch recibe la instancia
            exec(n.catch_body, catch_env);
        } catch (VshRuntimeError &e) {
            if (!n.catch_type_hint.empty()) throw; // catch tipado no captura errores de string
            auto catch_env = env->make_child();
            catch_env->define(n.name, Value(std::string(e.vsh_msg)));
            exec(n.catch_body, catch_env);
        }
        // ReturnSignal / BreakSignal / ContinueSignal se propagan sin capturar
    }

    // =============================================================================
    // Resolucion de imports VSH
    // =============================================================================
    //
    // `import "ruta"` busca el fichero en el siguiente orden, devolviendo el
    // PRIMER candidato que exista en disco.  Si ninguno existe, error con la
    // lista completa de rutas probadas para facilitar el diagnostico:
    //
    //   1. La ruta literal tal cual (relativa al cwd o absoluta).
    //   2. Resuelta relativa al directorio del SCRIPT QUE HACE EL IMPORT
    //      (i.e. dirname(__file__)).  Permite que un script main.vsh haga
    //      `import "lib/util.vsh"` sin importar desde donde se llamo.
    //   3. Resuelta relativa al directorio del INTERPRETE (vm[.exe]).
    //      Permite que vm.exe + stdlib/ se distribuyan juntos en /Program
    //      Files/Vesta/ y que `import "stdlib/vsh/build_lib.vsh"` funcione
    //      desde cualquier cwd.
    //   4. Resuelta relativa a `<dir interprete>/stdlib/`.  Atajo para los
    //      modulos canonicos del runtime: `import "vsh/build_lib.vsh"`.
    //   5. Cada entrada de la variable de entorno VESTAVM_VSH_PATH
    //      (separada por ; en Windows, : en POSIX) anteponiendola al path.
    //
    // Devuelve el path resuelto o cadena vacia si no se encuentra.

    namespace {
        // Devuelve el path absoluto del binario en ejecucion (vm[.exe]).
        // Usa GetModuleFileNameW en Windows, /proc/self/exe en Linux y
        // _NSGetExecutablePath en macOS.  Devuelve cadena vacia ante fallo.
        std::string get_executable_path() {
#if defined(_WIN32)
            wchar_t buf[MAX_PATH * 2];
            DWORD   n = GetModuleFileNameW(nullptr, buf, sizeof(buf) / sizeof(buf[0]));
            if (n == 0) return {};
            // Convertir UTF-16 -> UTF-8 sin depender de WideCharToMultiByte
            // (basta con narrowing ASCII para paths del sistema).
            std::string out;
            out.reserve(n);
            for (DWORD i = 0; i < n; ++i) {
                wchar_t c = buf[i];
                out.push_back(c < 128 ? static_cast<char>(c) : '?');
            }
            return out;
#elif defined(__APPLE__)
        char buf[4096];
        uint32_t sz = sizeof(buf);
        if (_NSGetExecutablePath(buf, &sz) != 0) return {};
        return std::string(buf);
#else
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0) return {};
        buf[n] = '\0';
        return std::string(buf);
#endif
        }

        // Path separator de la variable de entorno PATH-like.
        constexpr char path_list_sep() {
#if defined(_WIN32)
            return ';';
#else
        return ':';
#endif
        }

        // Split de una cadena por separador (sin std::ranges para C++17).
        std::vector<std::string> split_paths(const std::string &s, char sep) {
            std::vector<std::string> out;
            std::string              cur;
            for (char c: s) {
                if (c == sep) {
                    if (!cur.empty()) out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) out.push_back(cur);
            return out;
        }
    } // namespace

    std::string VshInterpreter::resolve_import_path(const std::string &path,
                                                    const std::string &importer_file) const {
        namespace fs = std::filesystem;
        std::vector<std::string> tried;

        auto try_candidate = [&](const fs::path &p) -> std::string {
            std::error_code ec;
            if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                return p.string();
            }
            tried.push_back(p.string());
            return {};
        };

        // 1. Path literal tal cual.
        if (auto r = try_candidate(fs::path(path)); !r.empty()) return r;

        // 2. Relativo al script importador.
        if (!importer_file.empty()) {
            fs::path importer_dir = fs::path(importer_file).parent_path();
            if (!importer_dir.empty()) {
                if (auto r = try_candidate(importer_dir / path); !r.empty()) return r;
            }
        }

        // 3. Relativo al directorio del intérprete (vm[.exe]).
        std::string exe = get_executable_path();
        fs::path    exe_dir;
        if (!exe.empty()) exe_dir = fs::path(exe).parent_path();
        if (!exe_dir.empty()) {
            if (auto r = try_candidate(exe_dir / path); !r.empty()) return r;
            // 4. Relativo a <dir interprete>/stdlib/.
            if (auto r = try_candidate(exe_dir / "stdlib" / path); !r.empty()) return r;
            // 5. Layout build-tree:  parent_del_exe + path  (e.g. para imports
            //    `stdlib/vsh/X` cuando vm.exe vive en cmake-build-XXX/ y la
            //    stdlib esta en la raiz del repo en stdlib/).
            fs::path parent = exe_dir.parent_path();
            if (!parent.empty()) {
                if (auto r = try_candidate(parent / path); !r.empty()) return r;
                // 6. Layout estilo Unix prefix:  bin/vm + stdlib/...  o
                //    bin/vm + share/vm/stdlib/...  (para imports `vsh/X`
                //    sin el prefijo stdlib).
                if (auto r = try_candidate(parent / "stdlib" / path); !r.empty()) return r;
                if (auto r = try_candidate(parent / "share" / "vm" / "stdlib" / path); !r.empty()) return r;
            }
        }

        // 5. VESTAVM_VSH_PATH.
        if (const char *envp = std::getenv("VESTAVM_VSH_PATH")) {
            for (const auto &dir: split_paths(envp, path_list_sep())) {
                fs::path d(dir);
                if (auto r = try_candidate(d / path); !r.empty()) return r;
            }
        }

        // No encontrado: lanzar error con la lista de rutas probadas.
        std::string msg = "import: no se encuentra '" + path + "'.  Probado:";
        for (const auto &t: tried) msg += "\n  - " + t;
        throw VshRuntimeError(msg);
    }

    void VshInterpreter::exec_import(const AstNode &n, std::shared_ptr<VshEnv> /*env*/) {
        std::string requested = n.name;
        // Path del importador: leemos __file__ del scope global ANTES de
        // que exec_file lo sobreescriba con el path del modulo importado.
        std::string importer_file;
        if (global_->has("__file__")) {
            Value fv = global_->get("__file__");
            if (fv.type() == VshType::String) importer_file = fv.as_string();
        }
        std::string resolved = resolve_import_path(requested, importer_file);

        // Detección de ciclo usando el path RESUELTO (evita que dos imports
        // del mismo fichero por rutas distintas pasen el chequeo).
        for (auto &p: import_stack_)
            if (p == resolved)
                throw VshRuntimeError("importacion circular detectada: " + resolved, n.line);
        import_stack_.push_back(resolved);
        try {
            exec_file(resolved); // ejecuta en el scope global (efecto: define funciones globales)
        } catch (...) {
            import_stack_.pop_back();
            throw;
        }
        import_stack_.pop_back();
    }

    // ---- llamada a funcion VSH ----

    Value VshInterpreter::call_fn(const Value &callee, std::vector<Value> args, int line) {
        auto fn = callee.as_fn();
        // stub de builtin nativo (body == nullptr): despachar directamente
        if (!fn->body) {
            auto bit = builtins_.find(fn->name);
            if (bit != builtins_.end()) {
                try {
                    return bit->second(std::move(args));
                } catch (VshRuntimeError &e) {
                    if (e.line == 0) throw VshRuntimeError(e.vsh_msg, line);
                    throw;
                }
            }
            return Value{};
        }
        if (args.size() < fn->params.size())
            throw VshRuntimeError("'" + fn->name + "': se esperaban " +
                                  std::to_string(fn->params.size()) + " argumentos, se recibieron " +
                                  std::to_string(args.size()), line);
        auto fn_env = fn->closure_env->make_child();
        for (size_t i = 0; i < fn->params.size(); ++i) {
            if (i < fn->param_types.size() && !fn->param_types[i].empty()) {
                check_type_hint(fn->param_types[i], args[i],
                                "parametro '" + fn->params[i] + "'", line, fn->closure_env);
            }
            fn_env->define(fn->params[i], std::move(args[i]));
        }
        auto body = std::static_pointer_cast<AstNode>(fn->body);
        try {
            exec(body, fn_env);
            return Value{}; // funciones sin return explicito devuelven null
        } catch (ReturnSignal &rs) {
            return std::move(rs.val);
        }
    }

    // ---- nuevos metodos OOP/tipado ----

    Value VshInterpreter::eval_field(const AstNode &n, std::shared_ptr<VshEnv> env) {
        Value              obj   = eval(n.left, env);
        const std::string &field = n.name;
        if (obj.is_instance()) {
            auto inst = obj.as_instance();
            auto it   = inst->attrs.find(field);
            if (it != inst->attrs.end()) return it->second;
            // devolver metodo como funcion sin enlazar (se enlaza en eval_call)
            auto method = find_method(inst->klass, field);
            if (method) return Value(method);
            throw VshRuntimeError("atributo no encontrado: '" + field +
                                  "' en " + inst->klass->name, n.line);
        }
        if (obj.is_class()) {
            auto method = find_method(obj.as_class(), field);
            if (method) return Value(method);
            throw VshRuntimeError("metodo no encontrado: '" + field +
                                  "' en clase " + obj.as_class()->name, n.line);
        }
        throw VshRuntimeError("acceso '.' solo para instancias y clases", n.line);
    }

    void VshInterpreter::exec_class_decl(const AstNode &n, std::shared_ptr<VshEnv> env) {
        auto klass  = std::make_shared<VshClass>();
        klass->name = n.name;
        // resolver clase padre
        if (!n.parent_class.empty()) {
            if (!env->has(n.parent_class))
                throw VshRuntimeError("clase padre no definida: '" + n.parent_class + "'", n.line);
            Value parent_val = env->get(n.parent_class);
            if (!parent_val.is_class())
                throw VshRuntimeError("'" + n.parent_class + "' no es una clase", n.line);
            klass->parent = parent_val.as_class();
        }
        // procesar metodos y docstring del cuerpo
        for (auto &child: n.args) {
            if (child->kind == AstKind::FnDecl) {
                auto fn                  = std::make_shared<VshFunction>();
                fn->name                 = child->name;
                fn->params               = child->params;
                fn->param_types          = child->param_types;
                fn->body                 = child->body;
                fn->closure_env          = env;
                fn->doc                  = extract_docstring(std::static_pointer_cast<AstNode>(fn->body));
                klass->methods[fn->name] = fn;
            } else if (child->kind == AstKind::ExprStmt && child->left &&
                child->left->kind == AstKind::Literal &&
                child->left->lit_val.is_string() && klass->doc.empty()) {
                klass->doc = child->left->lit_val.as_string(); // docstring de clase
            }
        }
        env->define(n.name, make_class_val(klass));
    }

    std::shared_ptr<VshFunction> VshInterpreter::find_method(
        const std::shared_ptr<VshClass> &klass, const std::string &name) {
        if (!klass) return nullptr;
        auto it = klass->methods.find(name);
        if (it != klass->methods.end()) return it->second;
        return find_method(klass->parent, name);
    }

    Value VshInterpreter::call_method_impl(const std::shared_ptr<VshFunction> &method,
                                           const Value &                       self_val,
                                           std::vector<Value>                  extra_args,
                                           int                                 line) {
        if (method->params.empty())
            throw VshRuntimeError("metodo '" + method->name + "' requiere al menos 'self'", line);
        auto fn_env = method->closure_env->make_child();
        fn_env->define(method->params[0], self_val); // primer param = self
        // exponer 'super' si la clase tiene padre
        if (self_val.is_instance()) {
            auto inst = self_val.as_instance();
            if (inst->klass->parent)
                fn_env->define("super", make_class_val(inst->klass->parent));
        }
        if (extra_args.size() < method->params.size() - 1)
            throw VshRuntimeError("metodo '" + method->name + "': argumentos insuficientes", line);
        for (size_t i = 1; i < method->params.size(); ++i) {
            size_t ai = i - 1;
            if (i < method->param_types.size() && !method->param_types[i].empty()) {
                check_type_hint(method->param_types[i], extra_args[ai],
                                "parametro '" + method->params[i] + "'", line, method->closure_env);
            }
            fn_env->define(method->params[i], std::move(extra_args[ai]));
        }
        auto body = std::static_pointer_cast<AstNode>(method->body);
        try {
            exec(body, fn_env);
            return Value{};
        } catch (ReturnSignal &rs) {
            return std::move(rs.val);
        }
    }

    void VshInterpreter::check_type_hint(const std::string &hint, const Value &val,
                                         const std::string &ctx, int           line,
                                         std::shared_ptr<VshEnv> /*env*/) {
        if (hint.empty()) return;
        bool ok = false;
        if (hint == "null") ok = val.is_null();
        else if (hint == "bool") ok = val.is_bool();
        else if (hint == "int") ok = val.is_int();
        else if (hint == "float") ok = val.is_float();
        else if (hint == "str" || hint == "string") ok = val.is_string();
        else if (hint == "list") ok = val.is_list();
        else if (hint == "map") ok = val.is_map();
        else if (hint == "fn" || hint == "function") ok = val.is_function();
        else if (hint == "class") ok = val.is_class();
        else if (hint == "instance") ok = val.is_instance();
        else {
            // nombre de clase: recorrer jerarquia de herencia
            if (val.is_instance()) {
                auto klass = val.as_instance()->klass;
                while (klass) {
                    if (klass->name == hint) {
                        ok = true;
                        break;
                    }
                    klass = klass->parent;
                }
            }
        }
        if (!ok) {
            throw VshRuntimeError(ctx + ": se esperaba tipo '" + hint +
                                  "', se obtuvo '" + val.to_string() + "'", line);
        }
    }

    void VshInterpreter::run_interactive(ReadlineFn readline_fn) {
        std::string buffer;
        int         depth = 0;

        auto read_line = [&](const std::string &prompt) -> std::string {
            if (readline_fn) return readline_fn(prompt);
            std::cout << prompt << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) return "\x04"; // EOF
            return line;
        };

        std::cout << "VestaShell REPL interactivo - escribe 'exit' o 'quit' para salir\n";

        for (;;) {
            std::string prompt = buffer.empty() ? ">>> " : "... ";
            std::string line   = read_line(prompt);

            if (line == "\x04") break; // EOF
            if (buffer.empty() && (line == "exit" || line == "quit")) break;

            // contar { } para detectar bloque multilinea
            for (char c: line) {
                if (c == '{') ++depth;
                else if (c == '}' && depth > 0) --depth;
            }

            if (!buffer.empty()) buffer += '\n';
            buffer += line;

            if (depth > 0) continue; // esperar cierre del bloque

            if (buffer.find_first_not_of(" \t\n\r") == std::string::npos) {
                buffer.clear();
                depth = 0;
                continue;
            }

            try {
                VshLexer  lex(buffer, "<repl>");
                VshParser par(lex);
                auto      ast = par.parse_program();
                // si es una sola ExprStmt, imprimir resultado (al estilo Python)
                if (ast->args.size() == 1 && ast->args[0]->kind == AstKind::ExprStmt) {
                    Value result = eval(ast->args[0]->left, global_);
                    // auto-invocar stubs de builtins (ej: 'help' sin () ejecuta help())
                    if (result.is_function() && !result.as_fn()->body && !result.as_fn()->name.empty()) {
                        auto bit = builtins_.find(result.as_fn()->name);
                        if (bit != builtins_.end()) {
                            result = bit->second({});
                        }
                    }
                    if (!result.is_null()) std::cout << result.to_repr() << '\n';
                } else {
                    exec(ast, global_);
                }
            } catch (VshParseError &e) {
                std::cerr << "SyntaxError: " << e.what() << '\n';
            } catch (VshInstanceError &e) {
                std::cerr << e.thrown_val.as_instance()->klass->name << ": " << e.what() << '\n';
            } catch (VshRuntimeError &e) {
                std::cerr << "RuntimeError: " << e.what() << '\n';
            } catch (std::exception &e) {
                std::cerr << "Error: " << e.what() << '\n';
            }

            buffer.clear();
            depth = 0;
        }
    }

    // ============================================================
    // Seccion 6: Built-ins
    // ============================================================

    void VshInterpreter::register_builtins() {
        // ---- salida ----
        /*builtins_["echo"] = [](std::vector<Value> args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) std::cout << ' ';
                std::cout << args[i].to_string();
            }
            std::cout << '\n'; return {};
        };
        builtins_["print"] = [](std::vector<Value> args) -> Value {
            for (auto &a : args) std::cout << a.to_string();
            return {};
        };
        builtins_["println"] = [](std::vector<Value> args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) std::cout << ' ';
                std::cout << args[i].to_string();
            }
            std::cout << '\n'; return {};
        };*/

        builtins_["echo"] = [](std::vector<Value> args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) buffered_write_char(' ');
                buffered_write(args[i].to_string());
            }
            buffered_write_char('\n');
            // No flush automatico aqui; lo hace el threshold o el shutdown.
            // Si se usa echo para debug interactivo, puede convenir flushear:
            flush_out_buffer();
            return {};
        };

        // print: sin separador, sin newline (el caso comun de vnano)
        builtins_["print"] = [](std::vector<Value> args) -> Value {
            for (auto &a: args) {
                buffered_write(a.to_string());
            }
            // OJO: NO hacemos flush aqui. El codigo VSH (vnano.render) llama print
            // muchas veces seguidas y un flush por llamada arruina la optimizacion.
            // El flush ocurre por threshold (64KB) o explicitamente con flush_output().
            return {};
        };

        // println: con espacios entre args y \n al final, con flush
        builtins_["println"] = [](std::vector<Value> args) -> Value {
            for (size_t i = 0; i < args.size(); ++i) {
                if (i) buffered_write_char(' ');
                buffered_write(args[i].to_string());
            }
            buffered_write_char('\n');
            flush_out_buffer(); // println es para output del usuario, flush es razonable
            return {};
        };

        // flush_output: builtin nuevo para forzar el flush desde codigo VSH
        builtins_["flush_output"] = [](std::vector<Value> args) -> Value {
            flush_out_buffer();
            fflush(stdout);
            return {};
        };

        // ---- conversiones de tipo ----
        builtins_["str"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            return Value(args[0].to_string());
        };
        builtins_["int"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(int64_t(0));
            if (args[0].is_int()) return args[0];
            if (args[0].is_float()) return Value(int64_t(args[0].as_float()));
            if (args[0].is_bool()) return Value(int64_t(args[0].as_bool() ? 1 : 0));
            if (args[0].is_string()) {
                // Soporte de base opcional (segundo argumento) y auto-deteccion
                // de prefijo "0x"/"0b"/"0o" cuando no se da base.  Sin esto el
                // cliente del debugger tiene que parsear hex a mano.
                int base = 0; // 0 = auto-detect via std::stoll
                if (args.size() >= 2 && args[1].is_int()) {
                    base = static_cast<int>(args[1].as_int());
                    if (base != 0 && (base < 2 || base > 36)) {
                        throw VshRuntimeError("int(): base fuera de rango [2..36]");
                    }
                }
                const std::string &s = args[0].as_string();
                // std::stoll con base 0 auto-detecta "0x" como hex.  Anadimos
                // soporte explicito para prefijos "0b" (binario) y "0o" (octal)
                // cuando base = 0, ya que stoll no los reconoce.
                std::string trimmed = s;
                size_t      pos0    = trimmed.find_first_not_of(" \t\n\r");
                if (pos0 == std::string::npos) {
                    throw VshRuntimeError("int(): cadena vacia");
                }
                trimmed  = trimmed.substr(pos0);
                int sign = 1;
                if (!trimmed.empty() && (trimmed[0] == '+' || trimmed[0] == '-')) {
                    if (trimmed[0] == '-') sign = -1;
                    trimmed = trimmed.substr(1);
                }
                int eff_base = base;
                if (eff_base == 0) {
                    if (trimmed.size() >= 2
                        && trimmed[0] == '0'
                        && (trimmed[1] == 'b' || trimmed[1] == 'B')) {
                        eff_base = 2;
                        trimmed  = trimmed.substr(2);
                    } else if (trimmed.size() >= 2
                        && trimmed[0] == '0'
                        && (trimmed[1] == 'o' || trimmed[1] == 'O')) {
                        eff_base = 8;
                        trimmed  = trimmed.substr(2);
                    }
                }
                try {
                    size_t  consumed = 0;
                    int64_t v        = static_cast<int64_t>(
                        std::stoll(trimmed, &consumed, eff_base));
                    return Value(sign * v);
                } catch (...) {
                    throw VshRuntimeError("int(): no se puede convertir '" + s + "'");
                }
            }
            throw VshRuntimeError("int(): tipo no convertible");
        };
        builtins_["float"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(0.0);
            if (args[0].is_float()) return args[0];
            if (args[0].is_int()) return Value(double(args[0].as_int()));
            if (args[0].is_bool()) return Value(double(args[0].as_bool() ? 1 : 0));
            if (args[0].is_string()) {
                try {
                    return Value(std::stod(args[0].as_string()));
                } catch (...) {
                    throw VshRuntimeError("float(): no se puede convertir");
                }
            }
            throw VshRuntimeError("float(): tipo no convertible");
        };
        builtins_["bool"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].truthy());
        };
        builtins_["type"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string("null"));
            static const char *names[] = {
                "null", "bool", "int", "float", "string", "list", "map", "function", "class", "instance"
            };
            return Value(std::string(names[int(args[0].type())]));
        };

        // ---- colecciones ----
        builtins_["len"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("len() requiere un argumento");
            if (args[0].is_string()) return Value(int64_t(args[0].as_string().size()));
            if (args[0].is_list()) return Value(int64_t(args[0].as_list()->size()));
            if (args[0].is_map()) return Value(int64_t(args[0].as_map()->size()));
            throw VshRuntimeError("len() no soportado para " + args[0].to_string());
        };
        builtins_["append"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_list())
                throw VshRuntimeError("Uso: append(lista, valor)");
            args[0].as_list()->push_back(args[1]);
            return {};
        };
        builtins_["pop"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list())
                throw VshRuntimeError("Uso: pop(lista)");
            auto &v = *args[0].as_list();
            if (v.empty()) throw VshRuntimeError("pop(): lista vacia");
            Value last = v.back();
            v.pop_back();
            return last;
        };
        builtins_["keys"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_map())
                throw VshRuntimeError("Uso: keys(mapa)");
            auto lst = std::make_shared<std::vector<Value> >();
            for (auto &[k,_]: *args[0].as_map()) lst->push_back(Value(k));
            return Value(lst);
        };
        builtins_["values"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_map())
                throw VshRuntimeError("Uso: values(mapa)");
            auto lst = std::make_shared<std::vector<Value> >();
            for (auto &[_,v]: *args[0].as_map()) lst->push_back(v);
            return Value(lst);
        };
        builtins_["contains"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: contains(coleccion, valor)");
            if (args[0].is_list()) {
                for (auto &v: *args[0].as_list()) if (v == args[1]) return Value(true);
                return Value(false);
            }
            if (args[0].is_map()) {
                return Value(args[0].as_map()->count(args[1].to_string()) > 0);
            }
            if (args[0].is_string()) {
                return Value(args[0].as_string().find(args[1].to_string()) != std::string::npos);
            }
            throw VshRuntimeError("contains() no soportado para " + args[0].to_string());
        };

        // ---- strings ----
        builtins_["split"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string())
                throw VshRuntimeError("Uso: split(string, separador)");
            std::string s   = args[0].as_string();
            std::string sep = args[1].as_string();
            auto        lst = std::make_shared<std::vector<Value> >();
            if (sep.empty()) {
                for (char c: s) lst->push_back(Value(std::string(1, c)));
                return Value(lst);
            }
            size_t pos = 0, found;
            while ((found = s.find(sep, pos)) != std::string::npos) {
                lst->push_back(Value(s.substr(pos, found - pos)));
                pos = found + sep.size();
            }
            lst->push_back(Value(s.substr(pos)));
            return Value(lst);
        };
        builtins_["join"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_list())
                throw VshRuntimeError("Uso: join(lista, separador)");
            std::string sep = args[1].is_string() ? args[1].as_string() : "";
            std::string r;
            auto &      v = *args[0].as_list();
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) r += sep;
                r += v[i].to_string();
            }
            return Value(r);
        };
        builtins_["trim"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) throw VshRuntimeError("Uso: trim(string)");
            std::string s = args[0].as_string();
            auto        l = s.find_first_not_of(" \t\r\n");
            auto        r = s.find_last_not_of(" \t\r\n");
            return Value(l == std::string::npos ? std::string{} : s.substr(l, r - l + 1));
        };
        builtins_["upper"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) throw VshRuntimeError("Uso: upper(string)");
            std::string s = args[0].as_string();
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return std::toupper(c);
            });
            return Value(s);
        };
        builtins_["lower"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) throw VshRuntimeError("Uso: lower(string)");
            std::string s = args[0].as_string();
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
                return std::tolower(c);
            });
            return Value(s);
        };
        builtins_["starts_with"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: starts_with(s, prefijo)");
            return Value(args[0].as_string().rfind(args[1].as_string(), 0) == 0);
        };
        builtins_["ends_with"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: ends_with(s, sufijo)");
            auto &s = args[0].as_string();
            auto &p = args[1].as_string();
            return Value(s.size() >= p.size() && s.substr(s.size() - p.size()) == p);
        };
        builtins_["replace"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 3 || !args[0].is_string()) throw VshRuntimeError("Uso: replace(s, viejo, nuevo)");
            std::string s   = args[0].as_string(), viejo = args[1].as_string(), nuevo = args[2].as_string();
            size_t      pos = 0;
            while ((pos = s.find(viejo, pos)) != std::string::npos) {
                s.replace(pos, viejo.size(), nuevo);
                pos += nuevo.size();
            }
            return Value(s);
        };
        builtins_["substr"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: substr(s, inicio [, len])");
            const auto &s     = args[0].as_string();
            int64_t     start = args[1].as_int();
            if (start < 0) start += (int64_t) s.size();
            size_t len = args.size() >= 3 ? (size_t) args[2].as_int() : std::string::npos;
            return Value(s.substr((size_t) start, len));
        };

        // ---- iteracion ----
        builtins_["range"] = [](std::vector<Value> args) -> Value {
            int64_t start = 0, end = 0, step = 1;
            if (args.size() == 1) {
                end = args[0].is_int() ? args[0].as_int() : (int64_t) args[0].as_float();
            } else if (args.size() >= 2) {
                start = args[0].is_int() ? args[0].as_int() : (int64_t) args[0].as_float();
                end   = args[1].is_int() ? args[1].as_int() : (int64_t) args[1].as_float();
            }
            if (args.size() >= 3) step = args[2].is_int() ? args[2].as_int() : (int64_t) args[2].as_float();
            if (step == 0) throw VshRuntimeError("range(): step no puede ser 0");
            auto lst = std::make_shared<std::vector<Value> >();
            for (int64_t i = start; (step > 0 ? i < end : i > end); i += step) lst->push_back(Value(i));
            return Value(lst);
        };

        // ---- sistema de ficheros ----
        builtins_["exists"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: exists(ruta)");
            std::error_code ec;
            return Value(std::filesystem::exists(args[0].as_string(), ec));
        };
        builtins_["is_dir"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: is_dir(ruta)");
            std::error_code ec;
            return Value(std::filesystem::is_directory(args[0].as_string(), ec));
        };
        builtins_["is_file"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: is_file(ruta)");
            std::error_code ec;
            return Value(std::filesystem::is_regular_file(args[0].as_string(), ec));
        };
        builtins_["basename"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            return Value(std::filesystem::path(args[0].as_string()).filename().string());
        };
        builtins_["dirname"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            auto p = std::filesystem::path(args[0].as_string()).parent_path();
            return Value(p.empty() ? std::string(".") : p.string());
        };
        builtins_["stem"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            return Value(std::filesystem::path(args[0].as_string()).stem().string());
        };
        builtins_["extension"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            return Value(std::filesystem::path(args[0].as_string()).extension().string());
        };
        builtins_["glob"] = [](std::vector<Value> args) -> Value {
            auto lst = std::make_shared<std::vector<Value> >();
            if (args.empty()) return Value(lst);
            std::string pattern = args[0].as_string();
            namespace fs = std::filesystem;
            try {
                fs::path    base = fs::path(pattern).parent_path();
                std::string pat  = fs::path(pattern).filename().string();
                if (base.empty()) base = ".";
                // matching simple: * ? como wildcards
                auto match_pat = [](const std::string &name, const std::string &p) -> bool {
                    size_t ni           = 0,                 pi           = 0;
                    size_t last_star_pi = std::string::npos, last_star_ni = 0;
                    while (ni < name.size()) {
                        if (pi < p.size() && (p[pi] == '?' || p[pi] == name[ni])) {
                            ++ni;
                            ++pi;
                        } else if (pi < p.size() && p[pi] == '*') {
                            last_star_pi = pi++;
                            last_star_ni = ni;
                        } else if (last_star_pi != std::string::npos) {
                            pi = last_star_pi + 1;
                            ni = ++last_star_ni;
                        } else return false;
                    }
                    while (pi < p.size() && p[pi] == '*') ++pi;
                    return pi == p.size();
                };
                std::error_code ec;
                for (auto &e: fs::directory_iterator(base, ec)) {
                    std::string fname = e.path().filename().string();
                    if (pat == "*" || match_pat(fname, pat))
                        lst->push_back(Value(e.path().string()));
                }
                // ordenar para resultados deterministas
                std::sort(lst->begin(), lst->end(), [](const Value &a, const Value &b) {
                    return a.as_string() < b.as_string();
                });
            } catch (...) {}
            return Value(lst);
        };
        builtins_["read_file"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: read_file(ruta)");
            std::ifstream f(args[0].as_string());
            if (!f) throw VshRuntimeError("read_file(): no se puede abrir: " + args[0].as_string());
            std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            return Value(s);
        };
        builtins_["write_file"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: write_file(ruta, contenido)");
            std::ofstream f(args[0].as_string());
            if (!f) throw VshRuntimeError("write_file(): no se puede abrir: " + args[0].as_string());
            f << args[1].to_string();
            return {};
        };

        // ---- sistema de ficheros (operaciones mutables y navegacion) ----
        builtins_["getcwd"] = [](std::vector<Value> /*args*/) -> Value {
            std::error_code ec;
            auto            p = std::filesystem::current_path(ec);
            if (ec) throw VshRuntimeError("getcwd(): " + ec.message());
            return Value(p.string());
        };
        builtins_["listdir"] = [](std::vector<Value> args) -> Value {
            namespace fs = std::filesystem;
            std::string     dir = args.empty() ? "." : args[0].as_string();
            auto            lst = std::make_shared<std::vector<Value> >();
            std::error_code ec;
            for (auto &e: fs::directory_iterator(dir, ec))
                lst->push_back(Value(e.path().filename().string()));
            if (ec) throw VshRuntimeError("listdir(): " + ec.message());
            std::sort(lst->begin(), lst->end(), [](const Value &a, const Value &b) {
                return a.as_string() < b.as_string();
            });
            return Value(lst);
        };
        builtins_["listdir_full"] = [](std::vector<Value> args) -> Value {
            namespace fs = std::filesystem;
            std::string     dir = args.empty() ? "." : args[0].as_string();
            auto            lst = std::make_shared<std::vector<Value> >();
            std::error_code ec;
            for (auto &e: fs::directory_iterator(dir, ec))
                lst->push_back(Value(e.path().string()));
            if (ec) throw VshRuntimeError("listdir_full(): " + ec.message());
            std::sort(lst->begin(), lst->end(), [](const Value &a, const Value &b) {
                return a.as_string() < b.as_string();
            });
            return Value(lst);
        };
        builtins_["mkdir"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: mkdir(ruta)");
            std::error_code ec;
            std::filesystem::create_directory(args[0].as_string(), ec);
            if (ec) throw VshRuntimeError("mkdir(): " + ec.message());
            return {};
        };
        builtins_["makedirs"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: makedirs(ruta)");
            std::error_code ec;
            std::filesystem::create_directories(args[0].as_string(), ec);
            if (ec) throw VshRuntimeError("makedirs(): " + ec.message());
            return {};
        };
        builtins_["rmdir"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: rmdir(ruta)");
            std::error_code ec;
            std::filesystem::remove(args[0].as_string(), ec);
            if (ec) throw VshRuntimeError("rmdir(): " + ec.message());
            return {};
        };
        builtins_["remove_file"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: remove_file(ruta)");
            std::error_code ec;
            std::filesystem::remove(args[0].as_string(), ec);
            if (ec) throw VshRuntimeError("remove_file(): " + ec.message());
            return {};
        };
        builtins_["remove_all"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: remove_all(ruta)");
            std::error_code ec;
            std::filesystem::remove_all(args[0].as_string(), ec);
            if (ec) throw VshRuntimeError("remove_all(): " + ec.message());
            return {};
        };
        builtins_["rename_path"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: rename_path(origen, destino)");
            std::error_code ec;
            std::filesystem::rename(args[0].as_string(), args[1].as_string(), ec);
            if (ec) throw VshRuntimeError("rename_path(): " + ec.message());
            return {};
        };
        builtins_["copy_file"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: copy_file(origen, destino)");
            std::error_code ec;
            std::filesystem::copy_file(args[0].as_string(), args[1].as_string(),
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) throw VshRuntimeError("copy_file(): " + ec.message());
            return {};
        };
        builtins_["abspath"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: abspath(ruta)");
            std::error_code ec;
            auto            p = std::filesystem::absolute(args[0].as_string());
            return Value(p.lexically_normal().string());
        };
        builtins_["normpath"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: normpath(ruta)");
            return Value(std::filesystem::path(args[0].as_string()).lexically_normal().string());
        };
        builtins_["join_path"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            std::filesystem::path p(args[0].as_string());
            for (size_t i = 1; i < args.size(); ++i)
                p /= args[i].as_string();
            return Value(p.string());
        };
        builtins_["file_size"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: file_size(ruta)");
            std::error_code ec;
            auto            sz = std::filesystem::file_size(args[0].as_string(), ec);
            if (ec) throw VshRuntimeError("file_size(): " + ec.message());
            return Value(int64_t(sz));
        };
        builtins_["chdir"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: chdir(ruta)");
            std::error_code ec;
            std::filesystem::current_path(args[0].as_string(), ec);
            if (ec) throw VshRuntimeError("chdir(): " + ec.message());
            return {};
        };

        // ---- shell y misc ----
        builtins_["shell"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            std::string cmd = args[0].as_string();
            std::string out;
#if defined(_WIN32)
            FILE *f = _popen(("cmd.exe /C \"" + cmd + "\" 2>&1").c_str(), "r");
#else
        FILE *f = popen((cmd + " 2>&1").c_str(), "r");
#endif
            if (!f) throw VshRuntimeError("shell(): no se pudo ejecutar: " + cmd);
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) out += buf;
#if defined(_WIN32)
            _pclose(f);
#else
        pclose(f);
#endif
            if (!out.empty() && out.back() == '\n') out.pop_back();
            return Value(out);
        };
        builtins_["shell_ex"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: shell_ex(cmd)");
            std::string cmd = args[0].as_string();
            std::string out;
            int         code = 0;
#if defined(_WIN32)
            FILE *f = _popen(("cmd.exe /C \"" + cmd + "\" 2>&1").c_str(), "r");
#else
        FILE *f = popen((cmd + " 2>&1").c_str(), "r");
#endif
            if (!f) throw VshRuntimeError("shell_ex(): no se pudo ejecutar: " + cmd);
            char buf[256];
            while (fgets(buf, sizeof(buf), f)) out += buf;
            if (!out.empty() && out.back() == '\n') out.pop_back();
#if defined(_WIN32)
            code = _pclose(f);
#else
        int status = pclose(f);
        code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
            auto m         = std::make_shared<std::unordered_map<std::string, Value> >();
            (*m)["output"] = Value(out);
            (*m)["code"]   = Value(int64_t(code));
            return Value(m);
        };
        builtins_["sleep"] = [](std::vector<Value> args) -> Value {
            int64_t ms = 0;
            if (!args.empty()) ms = args[0].is_int() ? args[0].as_int() : (int64_t) (args[0].as_number());
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            return {};
        };
        builtins_["input"] = [](std::vector<Value> args) -> Value {
            if (!args.empty()) std::cout << args[0].to_string() << std::flush;
            std::string line;
            if (!std::getline(std::cin, line)) {
                // EOF o error: devolver null para que el script pueda detectarlo.
                return Value(); // Value() = null en VSH
            }
            return Value(line);
        };
        builtins_["exit"] = [](std::vector<Value> args) -> Value {
            int code = args.empty() ? 0 : (int) (args[0].is_int() ? args[0].as_int() : (int64_t) args[0].as_number());
            std::exit(code);
            return {};
        };
        builtins_["assert"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("assert(): se requiere condicion");
            if (!args[0].truthy()) {
                std::string msg = args.size() >= 2 ? args[1].to_string() : "asercion fallida";
                throw VshRuntimeError(msg);
            }
            return {};
        };
        builtins_["error"] = [](std::vector<Value> args) -> Value {
            std::string msg = args.empty() ? "error en el script" : args[0].to_string();
            throw VshRuntimeError(msg);
            return {};
        };

        // ---- matematicas ----
        builtins_["abs"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("abs() requiere argumento");
            if (args[0].is_int()) return Value(std::abs(args[0].as_int()));
            return Value(std::abs(args[0].as_float()));
        };
        builtins_["min"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("min() requiere 2 argumentos");
            return args[0] < args[1] ? args[0] : args[1];
        };
        builtins_["max"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("max() requiere 2 argumentos");
            return args[0] > args[1] ? args[0] : args[1];
        };
        builtins_["floor"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("floor() requiere argumento");
            return Value(int64_t(std::floor(args[0].as_number())));
        };
        builtins_["ceil"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("ceil() requiere argumento");
            return Value(int64_t(std::ceil(args[0].as_number())));
        };
        builtins_["round"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("round() requiere argumento");
            return Value(int64_t(std::round(args[0].as_number())));
        };
        builtins_["sqrt"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("sqrt() requiere argumento");
            return Value(std::sqrt(args[0].as_number()));
        };
        builtins_["pow"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("pow() requiere 2 argumentos");
            return Value(std::pow(args[0].as_number(), args[1].as_number()));
        };
        builtins_["log"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("log() requiere argumento");
            return Value(std::log(args[0].as_number()));
        };

        // ---- utilidades de tipo ----
        builtins_["is_null"] = [](std::vector<Value> args) -> Value {
            return Value(args.empty() || args[0].is_null());
        };
        builtins_["is_bool"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_bool());
        };
        builtins_["is_int"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_int());
        };
        builtins_["is_float"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_float());
        };
        builtins_["is_str"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_string());
        };
        builtins_["is_list"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_list());
        };
        builtins_["is_map"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_map());
        };
        builtins_["is_fn"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_function());
        };
        builtins_["is_class"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_class());
        };
        builtins_["is_instance"] = [](std::vector<Value> args) -> Value {
            return Value(!args.empty() && args[0].is_instance());
        };

        // ---- OOP ----
        builtins_["classname"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("classname() requiere argumento");
            if (args[0].is_instance()) return Value(args[0].as_instance()->klass->name);
            if (args[0].is_class()) return Value(args[0].as_class()->name);
            throw VshRuntimeError("classname(): se esperaba instancia o clase");
        };
        builtins_["isinstance"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: isinstance(obj, clase)");
            if (!args[0].is_instance()) return Value(false);
            if (!args[1].is_class()) throw VshRuntimeError("isinstance(): segundo arg debe ser clase");
            const std::string &target = args[1].as_class()->name;
            auto               klass  = args[0].as_instance()->klass;
            while (klass) {
                if (klass->name == target) return Value(true);
                klass = klass->parent;
            }
            return Value(false);
        };

        // ---- documentacion (docstrings) ----
        builtins_["doc"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            if (args[0].is_function()) return Value(args[0].as_fn()->doc);
            if (args[0].is_class()) return Value(args[0].as_class()->doc);
            return Value(std::string{});
        };
        builtins_["help"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) {
                std::cout << "Uso: help(funcion_o_clase)  ->  muestra firma y docstring\n";
                return Value{};
            }
            if (args[0].is_function()) {
                auto        fn  = args[0].as_fn();
                std::string sig = fn->name.empty() ? "<lambda>" : fn->name;
                sig += "(";
                for (size_t i = 0; i < fn->params.size(); ++i) {
                    if (i) sig += ", ";
                    sig += fn->params[i];
                    if (i < fn->param_types.size() && !fn->param_types[i].empty())
                        sig += ": " + fn->param_types[i];
                }
                sig += ")";
                std::string out = sig + "\n";
                out += fn->doc.empty() ? "\n(sin documentacion)\n" : "\n" + fn->doc + "\n";
                std::cout << out;
                return Value(out);
            }
            if (args[0].is_class()) {
                auto        klass = args[0].as_class();
                std::string out   = "class " + klass->name;
                if (klass->parent) out += " : " + klass->parent->name;
                out += "\n";
                if (!klass->doc.empty()) out += "\n" + klass->doc + "\n";
                out += "\nMetodos:\n";
                for (auto &[mname, fn]: klass->methods) {
                    out += "  " + mname + "(";
                    for (size_t i = 0; i < fn->params.size(); ++i) {
                        if (i) out += ", ";
                        out += fn->params[i];
                        if (i < fn->param_types.size() && !fn->param_types[i].empty())
                            out += ": " + fn->param_types[i];
                    }
                    out += ")\n";
                    if (!fn->doc.empty()) out += "    " + fn->doc + "\n";
                }
                std::cout << out;
                return Value(out);
            }
            throw VshRuntimeError("help() requiere funcion o clase");
        };

        // ---- numeros aleatorios ----
        builtins_["rand_seed"] = [](std::vector<Value> args) -> Value {
            uint64_t seed = args.empty()
                                ? (uint64_t) std::random_device{}()
                                : (uint64_t) (args[0].is_int() ? args[0].as_int() : (int64_t) args[0].as_number());
            g_vsh_rng.seed(seed);
            return {};
        };
        builtins_["rand"] = [](std::vector<Value> /*args*/) -> Value {
            return Value(std::uniform_real_distribution<double>(0.0, 1.0)(g_vsh_rng));
        };
        builtins_["rand_int"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: rand_int(min, max)");
            int64_t lo = args[0].is_int() ? args[0].as_int() : (int64_t) args[0].as_number();
            int64_t hi = args[1].is_int() ? args[1].as_int() : (int64_t) args[1].as_number();
            if (lo > hi) throw VshRuntimeError("rand_int(): min > max");
            return Value(std::uniform_int_distribution<int64_t>(lo, hi)(g_vsh_rng));
        };
        builtins_["rand_float"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: rand_float(min, max)");
            double lo = args[0].as_number(), hi = args[1].as_number();
            return Value(std::uniform_real_distribution<double>(lo, hi)(g_vsh_rng));
        };
        builtins_["rand_choice"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: rand_choice(lista)");
            auto &lst = *args[0].as_list();
            if (lst.empty()) throw VshRuntimeError("rand_choice(): lista vacia");
            size_t idx = std::uniform_int_distribution<size_t>(0, lst.size() - 1)(g_vsh_rng);
            return lst[idx];
        };
        builtins_["rand_shuffle"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: rand_shuffle(lista)");
            auto copy = std::make_shared<std::vector<Value> >(*args[0].as_list());
            std::shuffle(copy->begin(), copy->end(), g_vsh_rng);
            return Value(copy);
        };

        // ---- colecciones de orden superior ----
        builtins_["sort"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: sort(lista)");
            auto copy = std::make_shared<std::vector<Value> >(*args[0].as_list());
            std::sort(copy->begin(), copy->end());
            return Value(copy);
        };
        builtins_["reverse"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: reverse(lista)");
            auto copy = std::make_shared<std::vector<Value> >(*args[0].as_list());
            std::reverse(copy->begin(), copy->end());
            return Value(copy);
        };
        builtins_["slice"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: slice(lista, inicio [, fin])");
            auto &  src = *args[0].as_list();
            int64_t n   = (int64_t) src.size();
            int64_t lo  = args.size() >= 2 ? args[1].as_int() : 0;
            int64_t hi  = args.size() >= 3 ? args[2].as_int() : n;
            if (lo < 0) lo += n;
            if (hi < 0) hi += n;
            lo       = std::max<int64_t>(0, lo);
            hi       = std::min<int64_t>(n, hi);
            auto out = std::make_shared<std::vector<Value> >();
            for (int64_t i = lo; i < hi; ++i) out->push_back(src[(size_t) i]);
            return Value(out);
        };
        builtins_["flat"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: flat(lista)");
            auto out = std::make_shared<std::vector<Value> >();
            for (auto &v: *args[0].as_list()) {
                if (v.is_list()) for (auto &e: *v.as_list()) out->push_back(e);
                else out->push_back(v);
            }
            return Value(out);
        };
        builtins_["zip"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_list() || !args[1].is_list())
                throw VshRuntimeError("Uso: zip(lista_a, lista_b)");
            auto & a   = *args[0].as_list();
            auto & b   = *args[1].as_list();
            size_t n   = std::min(a.size(), b.size());
            auto   out = std::make_shared<std::vector<Value> >();
            for (size_t i = 0; i < n; ++i) {
                auto pair = std::make_shared<std::vector<Value> >();
                pair->push_back(a[i]);
                pair->push_back(b[i]);
                out->push_back(Value(pair));
            }
            return Value(out);
        };
        builtins_["enumerate"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: enumerate(lista)");
            auto    out = std::make_shared<std::vector<Value> >();
            int64_t i   = 0;
            for (auto &v: *args[0].as_list()) {
                auto pair = std::make_shared<std::vector<Value> >();
                pair->push_back(Value(i++));
                pair->push_back(v);
                out->push_back(Value(pair));
            }
            return Value(out);
        };
        builtins_["sum"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: sum(lista)");
            bool   has_float = false;
            double acc       = 0.0;
            for (auto &v: *args[0].as_list()) {
                if (v.is_float()) {
                    has_float = true;
                    acc += v.as_float();
                } else if (v.is_int()) acc += double(v.as_int());
                else throw VshRuntimeError("sum(): elemento no numerico: " + v.to_string());
            }
            return has_float ? Value(acc) : Value(int64_t(acc));
        };
        builtins_["any_of"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: any_of(lista)");
            for (auto &v: *args[0].as_list()) if (v.truthy()) return Value(true);
            return Value(false);
        };
        builtins_["all_of"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: all_of(lista)");
            for (auto &v: *args[0].as_list()) if (!v.truthy()) return Value(false);
            return Value(true);
        };
        builtins_["unique"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_list()) throw VshRuntimeError("Uso: unique(lista)");
            auto out = std::make_shared<std::vector<Value> >();
            for (auto &v: *args[0].as_list()) {
                bool found = false;
                for (auto &u: *out)
                    if (u == v) {
                        found = true;
                        break;
                    }
                if (!found) out->push_back(v);
            }
            return Value(out);
        };
        builtins_["index_of"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_list()) throw VshRuntimeError("Uso: index_of(lista, valor)");
            auto &lst = *args[0].as_list();
            for (size_t i = 0; i < lst.size(); ++i) if (lst[i] == args[1]) return Value(int64_t(i));
            return Value(int64_t(-1));
        };

        // ---- utilidades de string adicionales ----
        builtins_["lstrip"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) throw VshRuntimeError("Uso: lstrip(s)");
            std::string s = args[0].as_string();
            auto        i = s.find_first_not_of(" \t\r\n");
            return Value(i == std::string::npos ? std::string{} : s.substr(i));
        };
        builtins_["rstrip"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) throw VshRuntimeError("Uso: rstrip(s)");
            std::string s = args[0].as_string();
            auto        i = s.find_last_not_of(" \t\r\n");
            return Value(i == std::string::npos ? std::string{} : s.substr(0, i + 1));
        };
        builtins_["find_str"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: find_str(s, sub [, inicio])");
            const auto &s     = args[0].as_string(), &sub = args[1].as_string();
            size_t      start = args.size() >= 3 ? (size_t) args[2].as_int() : 0;
            auto        pos   = s.find(sub, start);
            return Value(pos == std::string::npos ? int64_t(-1) : int64_t(pos));
        };
        builtins_["count_str"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: count_str(s, sub)");
            const auto &s = args[0].as_string(), &sub = args[1].as_string();
            if (sub.empty()) return Value(int64_t(0));
            int64_t n   = 0;
            size_t  pos = 0;
            while ((pos = s.find(sub, pos)) != std::string::npos) {
                ++n;
                pos += sub.size();
            }
            return Value(n);
        };
        builtins_["repeat"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: repeat(s, n)");
            std::string s = args[0].as_string();
            int64_t     n = args[1].as_int();
            if (n <= 0) return Value(std::string{});
            std::string r;
            r.reserve(s.size() * (size_t) n);
            for (int64_t i = 0; i < n; ++i) r += s;
            return Value(r);
        };
        builtins_["pad_left"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: pad_left(s, n [, char])");
            std::string s  = args[0].as_string();
            int64_t     n  = args[1].as_int();
            char        ch = args.size() >= 3 && args[2].is_string() && !args[2].as_string().empty()
                          ? args[2].as_string()[0]
                          : ' ';
            while ((int64_t) s.size() < n) s.insert(s.begin(), ch);
            return Value(s);
        };
        builtins_["pad_right"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string()) throw VshRuntimeError("Uso: pad_right(s, n [, char])");
            std::string s  = args[0].as_string();
            int64_t     n  = args[1].as_int();
            char        ch = args.size() >= 3 && args[2].is_string() && !args[2].as_string().empty()
                          ? args[2].as_string()[0]
                          : ' ';
            while ((int64_t) s.size() < n) s.push_back(ch);
            return Value(s);
        };
        builtins_["char_code"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string() || args[0].as_string().empty())
                throw VshRuntimeError("Uso: char_code(s)");
            return Value(int64_t((unsigned char) args[0].as_string()[0]));
        };
        builtins_["from_char"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: from_char(n)");
            char c = (char) (args[0].as_int() & 0xFF);
            return Value(std::string(1, c));
        };
        builtins_["is_numeric"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) return Value(false);
            const auto &s = args[0].as_string();
            if (s.empty()) return Value(false);
            try {
                std::stod(s);
                return Value(true);
            } catch (...) {
                return Value(false);
            }
        };
        builtins_["hex"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: hex(n)");
            int64_t n = args[0].is_int() ? args[0].as_int() : (int64_t) args[0].as_number();
            char    buf[32];
            snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long) (uint64_t) n);
            return Value(std::string(buf));
        };
        builtins_["bin_str"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: bin_str(n)");
            uint64_t n = (uint64_t) (args[0].is_int() ? args[0].as_int() : (int64_t) args[0].as_number());
            if (n == 0) return Value(std::string("0b0"));
            std::string r;
            while (n) {
                r = (char) ('0' + (n & 1)) + r;
                n >>= 1;
            }
            return Value("0b" + r);
        };

        // ---- matematicas adicionales ----
        builtins_["clamp"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 3) throw VshRuntimeError("Uso: clamp(x, min, max)");
            if (args[0].is_int() && args[1].is_int() && args[2].is_int()) {
                int64_t x = args[0].as_int(), lo = args[1].as_int(), hi = args[2].as_int();
                return Value(x < lo ? lo : (x > hi ? hi : x));
            }
            double x = args[0].as_number(), lo = args[1].as_number(), hi = args[2].as_number();
            return Value(x < lo ? lo : (x > hi ? hi : x));
        };
        builtins_["sign"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: sign(x)");
            if (args[0].is_int()) {
                int64_t x = args[0].as_int();
                return Value(x > 0 ? int64_t(1) : (x < 0 ? int64_t(-1) : int64_t(0)));
            }
            double x = args[0].as_number();
            return Value(x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0));
        };
        builtins_["gcd"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: gcd(a, b)");
            int64_t a = std::abs(args[0].as_int()), b = std::abs(args[1].as_int());
            while (b) {
                int64_t t = b;
                b         = a % b;
                a         = t;
            }
            return Value(a);
        };
        builtins_["lcm"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: lcm(a, b)");
            int64_t a = std::abs(args[0].as_int()), b = std::abs(args[1].as_int());
            if (!a || !b) return Value(int64_t(0));
            int64_t g  = a;
            int64_t tb = b;
            while (tb) {
                int64_t t = tb;
                tb        = g % tb;
                g         = t;
            }
            return Value(a / g * b);
        };
        builtins_["log2"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: log2(x)");
            return Value(std::log2(args[0].as_number()));
        };
        builtins_["log10"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: log10(x)");
            return Value(std::log10(args[0].as_number()));
        };
        builtins_["sin"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: sin(x)");
            return Value(std::sin(args[0].as_number()));
        };
        builtins_["cos"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: cos(x)");
            return Value(std::cos(args[0].as_number()));
        };
        builtins_["tan"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: tan(x)");
            return Value(std::tan(args[0].as_number()));
        };
        builtins_["pi"] = [](std::vector<Value> /*args*/) -> Value {
            return Value(3.141592653589793);
        };
        builtins_["inf"] = [](std::vector<Value> /*args*/) -> Value {
            return Value(std::numeric_limits<double>::infinity());
        };
        builtins_["is_nan"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].is_float() && std::isnan(args[0].as_float()));
        };
        builtins_["is_inf"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(false);
            return Value(args[0].is_float() && std::isinf(args[0].as_float()));
        };

        // ---- tiempo ----
        builtins_["time_ms"] = [](std::vector<Value> /*args*/) -> Value {
            using namespace std::chrono;
            return Value(int64_t(duration_cast<milliseconds>(
                system_clock::now().time_since_epoch()).count()));
        };
        builtins_["time_s"] = [](std::vector<Value> /*args*/) -> Value {
            using namespace std::chrono;
            return Value(double(duration_cast<microseconds>(
                system_clock::now().time_since_epoch()).count()) / 1e6);
        };
        builtins_["time_now"] = [](std::vector<Value> args) -> Value {
            std::time_t t   = std::time(nullptr);
            const char *fmt = args.empty() ? "%Y-%m-%d %H:%M:%S" : args[0].as_string().c_str();
            char        buf[128];
            std::strftime(buf, sizeof(buf), fmt, std::localtime(&t));
            return Value(std::string(buf));
        };

        // ---- entorno y proceso ----
        builtins_["getenv"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: getenv(nombre)");
            const char *v = std::getenv(args[0].as_string().c_str());
            return v ? Value(std::string(v)) : Value{};
        };
        builtins_["setenv"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: setenv(nombre, valor)");
#if defined(_WIN32)
            _putenv_s(args[0].as_string().c_str(), args[1].as_string().c_str());
#else
        setenv(args[0].as_string().c_str(), args[1].as_string().c_str(), 1);
#endif
            return {};
        };
        builtins_["platform"] = [](std::vector<Value> /*args*/) -> Value {
#if defined(_WIN32)
            return Value(std::string("windows"));
#elif defined(__APPLE__)
        return Value(std::string("macos"));
#else
        return Value(std::string("linux"));
#endif
        };
        // Devuelve el path absoluto del directorio que contiene el binario
        // del interprete (vm[.exe]).  Util para builders portables que
        // necesitan localizar la stdlib o el propio vm desde un script
        // ejecutado con cwd arbitrario.
        builtins_["interpreter_dir"] = [](std::vector<Value> /*args*/) -> Value {
            std::string exe = get_executable_path();
            if (exe.empty()) return Value(std::string{});
            return Value(std::filesystem::path(exe).parent_path().string());
        };
        // Devuelve el path absoluto del binario del interprete (incluyendo
        // el nombre del .exe en Windows).  Cadena vacia si no se puede
        // resolver.
        builtins_["interpreter_path"] = [](std::vector<Value> /*args*/) -> Value {
            return Value(get_executable_path());
        };
        builtins_["pid"] = [](std::vector<Value> /*args*/) -> Value {
#if defined(_WIN32)
            return Value(int64_t(GetCurrentProcessId()));
#else
        return Value(int64_t(getpid()));
#endif
        };
        builtins_["cpu_count"] = [](std::vector<Value> /*args*/) -> Value {
            return Value(int64_t(std::thread::hardware_concurrency()));
        };

        // ---- formato de texto ----
        builtins_["format"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string{});
            std::string tmpl = args[0].as_string();
            std::string out;
            size_t      arg_idx = 1;
            for (size_t i = 0; i < tmpl.size(); ++i) {
                if (tmpl[i] == '{' && i + 1 < tmpl.size()) {
                    size_t j = i + 1;
                    while (j < tmpl.size() && tmpl[j] != '}') ++j;
                    if (j < tmpl.size()) {
                        std::string key = tmpl.substr(i + 1, j - i - 1);
                        size_t      idx = key.empty() ? arg_idx++ : (size_t) std::stoul(key) + 1;
                        out += (idx < args.size()) ? args[idx].to_string() : "";
                        i = j;
                        continue;
                    }
                }
                out += tmpl[i];
            }
            return Value(out);
        };

        // ---- json simple ----
        // Implementacion recursiva sin dependencias externas.
        // Soporta null, bool, int, float, string, list, map.
        builtins_["json_str"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) return Value(std::string("null"));
            std::function<std::string(const Value &)> to_json = [&](const Value &v) -> std::string {
                if (v.is_null()) return "null";
                if (v.is_bool()) return v.as_bool() ? "true" : "false";
                if (v.is_int()) return std::to_string(v.as_int());
                if (v.is_float()) {
                    std::ostringstream os;
                    os << v.as_float();
                    return os.str();
                }
                if (v.is_string()) {
                    std::string s = v.as_string(), r = "\"";
                    for (char c: s) {
                        if (c == '"') r += "\\\"";
                        else if (c == '\\') r += "\\\\";
                        else if (c == '\n') r += "\\n";
                        else if (c == '\r') r += "\\r";
                        else if (c == '\t') r += "\\t";
                        else r += c;
                    }
                    return r + "\"";
                }
                if (v.is_list()) {
                    std::string r   = "[";
                    auto &      lst = *v.as_list();
                    for (size_t i = 0; i < lst.size(); ++i) {
                        if (i)r += ",";
                        r += to_json(lst[i]);
                    }
                    return r + "]";
                }
                if (v.is_map()) {
                    std::string r     = "{";
                    bool        first = true;
                    for (auto &[k,val]: *v.as_map()) {
                        if (!first)r += ",";
                        first = false;
                        r += "\"" + k + "\":" + to_json(val);
                    }
                    return r + "}";
                }
                return "null";
            };
            return Value(to_json(args[0]));
        };

        // ---- ffi: llamadas a librerias nativas (Win API, .so, .dll) ----
        // ffi_open(ruta) -> handle: carga la libreria dinamica y devuelve su id opaco.
        builtins_["ffi_open"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: ffi_open(ruta_lib)");
            return Value(vsh_ffi_open(args[0].as_string()));
        };

        // ffi_sym(handle, nombre) -> dir: busca el simbolo y devuelve su direccion como int.
        builtins_["ffi_sym"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: ffi_sym(handle, nombre)");
            return Value(vsh_ffi_sym(args[0].as_int(), args[1].as_string()));
        };

        // ffi_call(sym [, arg1, ...]) -> int: llama con args int/str/float/bool/null.
        builtins_["ffi_call"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: ffi_call(sym_handle [, arg1, ...])");
            int64_t            sym = args[0].as_int();
            std::vector<Value> rest(args.begin() + 1, args.end());
            return Value(vsh_ffi_call(sym, rest));
        };

        // ffi_call_f(sym [, arg1, ...]) -> float: igual que ffi_call pero retorno double.
        builtins_["ffi_call_f"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: ffi_call_f(sym_handle [, arg1, ...])");
            int64_t            sym = args[0].as_int();
            std::vector<Value> rest(args.begin() + 1, args.end());
            return Value(vsh_ffi_call_f(sym, rest));
        };

        // ffi_close(handle): descarga la libreria.
        builtins_["ffi_close"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: ffi_close(handle)");
            vsh_ffi_close(args[0].as_int());
            return Value();
        };

        // ffi_call_sym(ruta, nombre [, arg1, ...]): atajo open+sym+call+close en una llamada.
        builtins_["ffi_call_sym"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: ffi_call_sym(ruta, nombre [, arg1, ...])");
            int64_t            lib = vsh_ffi_open(args[0].as_string());
            int64_t            sym = vsh_ffi_sym(lib, args[1].as_string());
            std::vector<Value> rest(args.begin() + 2, args.end());
            int64_t            ret = vsh_ffi_call(sym, rest);
            vsh_ffi_close(lib);
            return Value(ret);
        };

        // ffi_str(ptr_int) -> string: interpreta el entero como char* y copia el contenido.
        // Util para leer strings de retorno de funciones Win32/POSIX.
        builtins_["ffi_str"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: ffi_str(ptr_int64)");
            int64_t addr = args[0].as_int();
            if (!addr) return Value(); // puntero nulo -> null
            const char *p = nullptr;
            memcpy(&p, &addr, sizeof(p));
            return Value(std::string(p));
        };

        // ---- ansi: secuencias de escape y colores ANSI/VT100 ----
        // Activa VT100 en Windows automaticamente al cargar los builtins.
        vsh_ansi_ensure();

        // Mapa ANSI con todas las constantes de color y estilo.
        // Uso: echo(ANSI["RED"] + "texto" + ANSI["RESET"])
        {
            auto ansi_map                = std::make_shared<std::unordered_map<std::string, Value> >();
            (*ansi_map)["RESET"]         = Value(std::string("\033[0m"));
            (*ansi_map)["BOLD"]          = Value(std::string("\033[1m"));
            (*ansi_map)["DIM"]           = Value(std::string("\033[2m"));
            (*ansi_map)["ITALIC"]        = Value(std::string("\033[3m"));
            (*ansi_map)["UNDERLINE"]     = Value(std::string("\033[4m"));
            (*ansi_map)["BLINK"]         = Value(std::string("\033[5m"));
            (*ansi_map)["REVERSE"]       = Value(std::string("\033[7m"));
            (*ansi_map)["STRIKE"]        = Value(std::string("\033[9m"));
            (*ansi_map)["BLACK"]         = Value(std::string("\033[30m"));
            (*ansi_map)["RED"]           = Value(std::string("\033[31m"));
            (*ansi_map)["GREEN"]         = Value(std::string("\033[32m"));
            (*ansi_map)["YELLOW"]        = Value(std::string("\033[33m"));
            (*ansi_map)["BLUE"]          = Value(std::string("\033[34m"));
            (*ansi_map)["MAGENTA"]       = Value(std::string("\033[35m"));
            (*ansi_map)["CYAN"]          = Value(std::string("\033[36m"));
            (*ansi_map)["WHITE"]         = Value(std::string("\033[37m"));
            (*ansi_map)["BR_BLACK"]      = Value(std::string("\033[90m"));
            (*ansi_map)["BR_RED"]        = Value(std::string("\033[91m"));
            (*ansi_map)["BR_GREEN"]      = Value(std::string("\033[92m"));
            (*ansi_map)["BR_YELLOW"]     = Value(std::string("\033[93m"));
            (*ansi_map)["BR_BLUE"]       = Value(std::string("\033[94m"));
            (*ansi_map)["BR_MAGENTA"]    = Value(std::string("\033[95m"));
            (*ansi_map)["BR_CYAN"]       = Value(std::string("\033[96m"));
            (*ansi_map)["BR_WHITE"]      = Value(std::string("\033[97m"));
            (*ansi_map)["BG_BLACK"]      = Value(std::string("\033[40m"));
            (*ansi_map)["BG_RED"]        = Value(std::string("\033[41m"));
            (*ansi_map)["BG_GREEN"]      = Value(std::string("\033[42m"));
            (*ansi_map)["BG_YELLOW"]     = Value(std::string("\033[43m"));
            (*ansi_map)["BG_BLUE"]       = Value(std::string("\033[44m"));
            (*ansi_map)["BG_MAGENTA"]    = Value(std::string("\033[45m"));
            (*ansi_map)["BG_CYAN"]       = Value(std::string("\033[46m"));
            (*ansi_map)["BG_WHITE"]      = Value(std::string("\033[47m"));
            (*ansi_map)["BG_BR_BLACK"]   = Value(std::string("\033[100m"));
            (*ansi_map)["BG_BR_RED"]     = Value(std::string("\033[101m"));
            (*ansi_map)["BG_BR_GREEN"]   = Value(std::string("\033[102m"));
            (*ansi_map)["BG_BR_YELLOW"]  = Value(std::string("\033[103m"));
            (*ansi_map)["BG_BR_BLUE"]    = Value(std::string("\033[104m"));
            (*ansi_map)["BG_BR_MAGENTA"] = Value(std::string("\033[105m"));
            (*ansi_map)["BG_BR_CYAN"]    = Value(std::string("\033[106m"));
            (*ansi_map)["BG_BR_WHITE"]   = Value(std::string("\033[107m"));
            (*ansi_map)["CLEAR"]         = Value(std::string("\033[2J\033[H"));
            (*ansi_map)["CLEAR_LINE"]    = Value(std::string("\033[2K\r"));
            (*ansi_map)["HOME"]          = Value(std::string("\033[H"));
            (*ansi_map)["CURSOR_HIDE"]   = Value(std::string("\033[?25l"));
            (*ansi_map)["CURSOR_SHOW"]   = Value(std::string("\033[?25h"));
            (*ansi_map)["SAVE_POS"]      = Value(std::string("\033[s"));
            (*ansi_map)["RESTORE_POS"]   = Value(std::string("\033[u"));
            global_->define("ANSI", Value(ansi_map));
        }

        // ansi_enable(): activa VT100 en Windows; siempre retorna true.
        builtins_["ansi_enable"] = [](std::vector<Value> /*args*/) -> Value {
            vsh_ansi_ensure();
            return Value(true);
        };

        // ansi_code(n): devuelve "\033[{n}m" para cualquier codigo ANSI arbitrario.
        builtins_["ansi_code"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: ansi_code(n)");
            return Value("\033[" + args[0].to_string() + "m");
        };

        // ansi_rgb(r,g,b): color de primer plano en True Color (24 bits).
        builtins_["ansi_rgb"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 3) throw VshRuntimeError("Uso: ansi_rgb(r, g, b)");
            return Value("\033[38;2;" + std::to_string(args[0].as_int()) + ";" +
                std::to_string(args[1].as_int()) + ";" +
                std::to_string(args[2].as_int()) + "m");
        };

        // ansi_rgb_bg(r,g,b): color de fondo en True Color (24 bits).
        builtins_["ansi_rgb_bg"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 3) throw VshRuntimeError("Uso: ansi_rgb_bg(r, g, b)");
            return Value("\033[48;2;" + std::to_string(args[0].as_int()) + ";" +
                std::to_string(args[1].as_int()) + ";" +
                std::to_string(args[2].as_int()) + "m");
        };

        // colorize(texto, fg [, bg [, negrita]]): envuelve el texto con escapes de color.
        // fg/bg son nombres de color en minusculas: "red", "green", "br_blue", etc.
        builtins_["colorize"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: colorize(texto, fg [, bg [, bold]])");
            static const std::unordered_map<std::string, int> fg_map = {
                {"black", 30}, {"red", 31}, {"green", 32}, {"yellow", 33}, {"blue", 34},
                {"magenta", 35}, {"cyan", 36}, {"white", 37},
                {"br_black", 90}, {"br_red", 91}, {"br_green", 92}, {"br_yellow", 93},
                {"br_blue", 94}, {"br_magenta", 95}, {"br_cyan", 96}, {"br_white", 97}
            };
            static const std::unordered_map<std::string, int> bg_map = {
                {"black", 40}, {"red", 41}, {"green", 42}, {"yellow", 43}, {"blue", 44},
                {"magenta", 45}, {"cyan", 46}, {"white", 47},
                {"br_black", 100}, {"br_red", 101}, {"br_green", 102}, {"br_yellow", 103},
                {"br_blue", 104}, {"br_magenta", 105}, {"br_cyan", 106}, {"br_white", 107}
            };
            std::string text = args[0].to_string();
            std::string out;
            std::string fg_name = args[1].to_string();
            for (auto &c: fg_name) c = (char) tolower((unsigned char) c);
            auto it = fg_map.find(fg_name);
            if (it != fg_map.end()) out += "\033[" + std::to_string(it->second) + "m";
            if (args.size() >= 3) {
                std::string bg_name = args[2].to_string();
                for (auto &c: bg_name) c = (char) tolower((unsigned char) c);
                auto jt = bg_map.find(bg_name);
                if (jt != bg_map.end()) out += "\033[" + std::to_string(jt->second) + "m";
            }
            if (args.size() >= 4 && args[3].truthy()) out += "\033[1m";
            out += text + "\033[0m";
            return Value(out);
        };

        // Funciones rapidas de color y estilo:
        //   red("hola")    -> "\033[31mhola\033[0m"
        //   red()          -> "\033[31m"  (solo el codigo, sin reset)
        {
            static const std::pair<const char *, const char *> color_list[] = {
                {"red", "\033[31m"}, {"green", "\033[32m"},
                {"yellow", "\033[33m"}, {"blue", "\033[34m"},
                {"magenta", "\033[35m"}, {"cyan", "\033[36m"},
                {"white", "\033[37m"}, {"bold", "\033[1m"},
                {"dim", "\033[2m"}, {"italic", "\033[3m"},
                {"underline", "\033[4m"}, {"strike", "\033[9m"},
            };
            for (const auto &entry: color_list) {
                std::string seq(entry.second);
                builtins_[entry.first] = [seq](std::vector<Value> args) -> Value {
                    return args.empty() ? Value(seq) : Value(seq + args[0].to_string() + "\033[0m");
                };
            }
        }

        // Movimiento de cursor ANSI.
        builtins_["ansi_cursor_up"] = [](std::vector<Value> args) -> Value {
            int64_t n = args.empty() ? 1 : args[0].as_int();
            return Value("\033[" + std::to_string(n) + "A");
        };
        builtins_["ansi_cursor_down"] = [](std::vector<Value> args) -> Value {
            int64_t n = args.empty() ? 1 : args[0].as_int();
            return Value("\033[" + std::to_string(n) + "B");
        };
        builtins_["ansi_cursor_right"] = [](std::vector<Value> args) -> Value {
            int64_t n = args.empty() ? 1 : args[0].as_int();
            return Value("\033[" + std::to_string(n) + "C");
        };
        builtins_["ansi_cursor_left"] = [](std::vector<Value> args) -> Value {
            int64_t n = args.empty() ? 1 : args[0].as_int();
            return Value("\033[" + std::to_string(n) + "D");
        };
        builtins_["ansi_cursor_pos"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: ansi_cursor_pos(fila, col)");
            return Value("\033[" + std::to_string(args[0].as_int()) + ";" +
                std::to_string(args[1].as_int()) + "H");
        };
        builtins_["ansi_clear"] = [](std::vector<Value> /*args*/) -> Value {
            return Value(std::string("\033[2J\033[H"));
        };
        builtins_["ansi_clear_line"] = [](std::vector<Value> /*args*/) -> Value {
            return Value(std::string("\033[2K\r"));
        };

        // strip_ansi(texto): elimina todas las secuencias de escape ANSI de un string.
        builtins_["strip_ansi"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: strip_ansi(texto)");
            std::string s = args[0].to_string();
            std::string out;
            for (size_t i = 0; i < s.size();) {
                if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '[') {
                    i += 2;
                    while (i < s.size() && s[i] != 'm' && s[i] != 'A' &&
                        s[i] != 'B' && s[i] != 'C' && s[i] != 'D' &&
                        s[i] != 'H' && s[i] != 'J' && s[i] != 'K')
                        ++i;
                    if (i < s.size()) ++i;
                } else {
                    out += s[i++];
                }
            }
            return Value(out);
        };

        // ---- sockets de red ----
        builtins_["tcp_connect"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: tcp_connect(host, port)");
            ensure_wsa();
            std::string        host = args[0].as_string();
            uint16_t           port = (uint16_t) args[1].as_int();
            struct sockaddr_in addr{};
            vsh_resolve(host, port, addr);
            vsh_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
            if (!vsh_sock_valid(fd)) throw VshRuntimeError("tcp_connect: error creando socket");
            if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
                vsh_sock_close(fd);
                throw VshRuntimeError("tcp_connect: no se pudo conectar a " + host + ":" + std::to_string(port));
            }
            return Value(vsh_alloc_sock(fd, false));
        };
        builtins_["tcp_listen"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: tcp_listen(port [, backlog])");
            ensure_wsa();
            uint16_t   port    = (uint16_t) args[0].as_int();
            int        backlog = args.size() >= 2 ? (int) args[1].as_int() : 10;
            vsh_sock_t fd      = socket(AF_INET, SOCK_STREAM, 0);
            if (!vsh_sock_valid(fd)) throw VshRuntimeError("tcp_listen: error creando socket");
            int opt = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
            struct sockaddr_in addr{};
            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port        = htons(port);
            if (bind(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
                vsh_sock_close(fd);
                throw VshRuntimeError("tcp_listen: bind fallido en puerto " + std::to_string(port));
            }
            listen(fd, backlog);
            return Value(vsh_alloc_sock(fd, false));
        };
        builtins_["tcp_accept"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: tcp_accept(handle)");
            int64_t id = args[0].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s) throw VshRuntimeError("tcp_accept: handle invalido");
            struct sockaddr_in remote{};
            socklen_t          len    = sizeof(remote);
            vsh_sock_t         client = accept(s->fd, reinterpret_cast<struct sockaddr *>(&remote), &len);
            if (!vsh_sock_valid(client)) throw VshRuntimeError("tcp_accept: error al aceptar conexion");
            return Value(vsh_alloc_sock(client, false));
        };
        builtins_["socket_send"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: socket_send(handle, datos)");
            int64_t id = args[0].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s) throw VshRuntimeError("socket_send: handle invalido");
            std::string data = args[1].to_string();
            // perf+correctness: bucle send-all para evitar
            // envios parciales que rompen el framing length-prefix de
            // protocolos como el del debugger TCP.  TCP NORMALMENTE no
            // hace partial send para mensajes pequenos en buffer libre,
            // pero la API NO lo garantiza: bajo presion del buffer del
            // peer (clientes lentos) `send` puede devolver menos bytes
            // que los pedidos.  El loop garantiza que el frame completo
            // sale en una sola llamada VSH `socket_send`.
            size_t       total = 0;
            const size_t need  = data.size();
            while (total < need) {
                int chunk;
                if (s->ssl)
                    chunk = SSL_write(s->ssl,
                                      data.c_str() + total,
                                      (int) (need - total));
                else
                    chunk = (int) send(s->fd,
                                       data.c_str() + total,
                                       (int) (need - total), 0);
                if (chunk < 0) throw VshRuntimeError("socket_send: error al enviar datos");
                if (chunk == 0) break; // peer cerro
                total += static_cast<size_t>(chunk);
            }
            return Value(int64_t(total));
        };
        builtins_["socket_recv"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: socket_recv(handle [, maxlen])");
            int64_t id = args[0].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s) throw VshRuntimeError("socket_recv: handle invalido");
            int         maxlen = args.size() >= 2 ? (int) args[1].as_int() : 4096;
            std::string buf(maxlen, '\0');
            int         n;
            if (s->ssl) n = SSL_read(s->ssl, &buf[0], maxlen);
            else n        = (int) recv(s->fd, &buf[0], (size_t) maxlen, 0);
            if (n < 0) throw VshRuntimeError("socket_recv: error al recibir datos");
            buf.resize(n);
            return Value(buf);
        };
        builtins_["socket_close"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: socket_close(handle)");
            vsh_free_sock(args[0].as_int());
            return {};
        };
        builtins_["socket_poll"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: socket_poll(handle, ms)");
            int64_t id = args[0].as_int();
            int64_t ms = args[1].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s) throw VshRuntimeError("socket_poll: handle invalido");
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(s->fd, &rfds);
            struct timeval tv;
            tv.tv_sec  = (long) (ms / 1000);
            tv.tv_usec = (long) ((ms % 1000) * 1000);
            int rc     = select((int) (s->fd + 1), &rfds, nullptr, nullptr, &tv);
            if (rc < 0) return Value((int64_t) -1);
            return Value((int64_t) (rc > 0 ? 1 : 0));
        };
        builtins_["udp_socket"] = [](std::vector<Value> args) -> Value {
            (void) args;
            ensure_wsa();
            vsh_sock_t fd = socket(AF_INET, SOCK_DGRAM, 0);
            if (!vsh_sock_valid(fd)) throw VshRuntimeError("udp_socket: error creando socket UDP");
            return Value(vsh_alloc_sock(fd, true));
        };
        builtins_["udp_bind"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: udp_bind(handle, port)");
            int64_t id = args[0].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s || !s->is_udp) throw VshRuntimeError("udp_bind: handle invalido o no es UDP");
            uint16_t           port = (uint16_t) args[1].as_int();
            struct sockaddr_in addr{};
            memset(&addr, 0, sizeof(addr));
            addr.sin_family      = AF_INET;
            addr.sin_addr.s_addr = INADDR_ANY;
            addr.sin_port        = htons(port);
            if (bind(s->fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0)
                throw VshRuntimeError("udp_bind: bind fallido en puerto " + std::to_string(port));
            return {};
        };
        builtins_["udp_sendto"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 4) throw VshRuntimeError("Uso: udp_sendto(handle, datos, host, port)");
            int64_t id = args[0].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s || !s->is_udp) throw VshRuntimeError("udp_sendto: handle invalido o no es UDP");
            std::string        data = args[1].to_string();
            struct sockaddr_in dest{};
            vsh_resolve(args[2].as_string(), (uint16_t) args[3].as_int(), dest);
            int n = (int) sendto(s->fd, data.c_str(), (int) data.size(), 0,
                                 reinterpret_cast<struct sockaddr *>(&dest), sizeof(dest));
            if (n < 0) throw VshRuntimeError("udp_sendto: error al enviar");
            return Value(int64_t(n));
        };
        builtins_["udp_recvfrom"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: udp_recvfrom(handle [, maxlen])");
            int64_t id = args[0].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s || !s->is_udp) throw VshRuntimeError("udp_recvfrom: handle invalido o no es UDP");
            int                maxlen = args.size() >= 2 ? (int) args[1].as_int() : 65536;
            std::string        buf(maxlen, '\0');
            struct sockaddr_in from{};
            socklen_t          fromlen = sizeof(from);
            int                n       = (int) recvfrom(s->fd, &buf[0], (size_t) maxlen, 0,
                                   reinterpret_cast<struct sockaddr *>(&from), &fromlen);
            if (n < 0) throw VshRuntimeError("udp_recvfrom: error al recibir");
            buf.resize(n);
            char ip_buf[INET_ADDRSTRLEN] = {};
            // inet_ntoa pasa el valor por copia (no puntero) asi que es seguro
            const char *ipstr = inet_ntoa(from.sin_addr);
            if (ipstr) strncpy(ip_buf, ipstr, sizeof(ip_buf) - 1);
            auto lst = std::make_shared<std::vector<Value> >();
            lst->push_back(Value(buf));
            lst->push_back(Value(std::string(ip_buf)));
            lst->push_back(Value(int64_t(ntohs(from.sin_port))));
            return Value(lst);
        };
        builtins_["tls_connect"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: tls_connect(host, port)");
            ensure_wsa();
            ensure_ssl_ctx();
            std::string        host = args[0].as_string();
            uint16_t           port = (uint16_t) args[1].as_int();
            struct sockaddr_in addr{};
            vsh_resolve(host, port, addr);
            vsh_sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
            if (!vsh_sock_valid(fd)) throw VshRuntimeError("tls_connect: error creando socket");
            if (connect(fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) != 0) {
                vsh_sock_close(fd);
                throw VshRuntimeError("tls_connect: no se pudo conectar a " + host);
            }
            SSL *ssl = SSL_new(g_ssl_ctx);
            SSL_set_fd(ssl, (int) fd);
            SSL_set_tlsext_host_name(ssl, host.c_str());
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl);
                vsh_sock_close(fd);
                throw VshRuntimeError("tls_connect: fallo TLS handshake con " + host);
            }
            return Value(vsh_alloc_sock(fd, false, ssl));
        };
        builtins_["socket_recv_all"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: socket_recv_all(handle)");
            int64_t id = args[0].as_int();
            auto *  s  = vsh_get_sock(id);
            if (!s) throw VshRuntimeError("socket_recv_all: handle invalido");
            std::string result;
            char        buf[4096];
            for (;;) {
                int n = s->ssl
                            ? SSL_read(s->ssl, buf, sizeof(buf))
                            : (int) recv(s->fd, buf, sizeof(buf), 0);
                if (n <= 0) break;
                result.append(buf, (size_t) n);
            }
            return Value(result);
        };
        builtins_["http_get"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: http_get(url)");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            return Value(vsh_http_request("GET", host, port, path, false).body);
        };
        builtins_["https_get"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: https_get(url)");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            return Value(vsh_http_request("GET", host, port, path, true).body);
        };
        builtins_["http_post"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: http_post(url, body [, content_type])");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            std::string body         = args.size() >= 2 ? args[1].to_string() : "";
            std::string ct           = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
            return Value(vsh_http_request("POST", host, port, path, false, body, ct).body);
        };
        builtins_["https_post"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: https_post(url, body [, content_type])");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            std::string body         = args.size() >= 2 ? args[1].to_string() : "";
            std::string ct           = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
            return Value(vsh_http_request("POST", host, port, path, true, body, ct).body);
        };
        builtins_["http_put"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: http_put(url, body [, content_type])");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            std::string body         = args.size() >= 2 ? args[1].to_string() : "";
            std::string ct           = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
            return Value(vsh_http_request("PUT", host, port, path, false, body, ct).body);
        };
        builtins_["https_put"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: https_put(url, body [, content_type])");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            std::string body         = args.size() >= 2 ? args[1].to_string() : "";
            std::string ct           = args.size() >= 3 ? args[2].as_string() : "application/x-www-form-urlencoded";
            return Value(vsh_http_request("PUT", host, port, path, true, body, ct).body);
        };
        builtins_["http_delete"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: http_delete(url)");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            return Value(vsh_http_request("DELETE", host, port, path, false).body);
        };
        builtins_["https_delete"] = [](std::vector<Value> args) -> Value {
            if (args.empty()) throw VshRuntimeError("Uso: https_delete(url)");
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[0].as_string(), use_tls, path);
            return Value(vsh_http_request("DELETE", host, port, path, true).body);
        };
        // http_request(method, url [, body [, content_type]]) -> map {status, body}
        builtins_["http_request"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2) throw VshRuntimeError("Uso: http_request(method, url [, body [, content_type]])");
            std::string method = args[0].as_string();
            bool        use_tls;
            std::string path;
            auto        [host, port] = vsh_parse_url(args[1].as_string(), use_tls, path);
            std::string body         = args.size() >= 3 ? args[2].to_string() : "";
            std::string ct           = args.size() >= 4 ? args[3].as_string() : "application/json";
            auto        r            = vsh_http_request(method, host, port, path, use_tls, body, ct);
            auto        m            = std::make_shared<std::unordered_map<std::string, Value> >();
            (*m)["status"]           = Value(int64_t(r.status));
            (*m)["headers"]          = Value(r.headers);
            (*m)["body"]             = Value(r.body);
            return Value(m);
        };

        // ---- variables de contexto de ejecucion (estilo Python __name__) ----
        // __name__ == "__main__"   -> script ejecutado directamente (--script o REPL script)
        // __name__ == "<modulo>"   -> script cargado via import
        // __name__ == "__repl__"   -> sesion interactiva (--interprete o CLI vsh)
        // __file__                 -> ruta del fichero en ejecucion ("" en REPL)
        global_->define("__name__", Value(std::string("__repl__")));
        global_->define("__file__", Value(std::string("")));

        // ===== JSON parse/stringify (anadido para soporte del cliente VSH del debugger) =====
        // json_parse(str) -> Value:  parsea texto JSON a Null/Bool/Int/Float/String/List/Map.
        // json_stringify(value)        -> str: serializa el mismo arbol.
        // json_stringify(value, indent) -> str: con indentacion (entero >= 0).
        // En errores de parse lanza VshRuntimeError con mensaje claro.
        {
            // Funcion recursiva: nlohmann::json -> Value (VSH).
            // No es lambda por la auto-referencia recursiva.
            struct JsonConv {
                static Value to_vsh(const nlohmann::json &j) {
                    if (j.is_null()) return Value();
                    if (j.is_boolean())return Value(j.get<bool>());
                    if (j.is_number_integer() || j.is_number_unsigned())
                        return Value(j.get<int64_t>());
                    if (j.is_number_float())
                        return Value(j.get<double>());
                    if (j.is_string())
                        return Value(j.get<std::string>());
                    if (j.is_array()) {
                        auto lp = std::make_shared<std::vector<Value> >();
                        lp->reserve(j.size());
                        for (const auto &e: j) lp->push_back(to_vsh(e));
                        return Value(lp);
                    }
                    if (j.is_object()) {
                        auto mp = std::make_shared<std::unordered_map<std::string, Value> >();
                        mp->reserve(j.size());
                        for (auto it = j.begin(); it != j.end(); ++it) {
                            (*mp)[it.key()] = to_vsh(it.value());
                        }
                        return Value(mp);
                    }
                    return Value();
                }

                static nlohmann::json from_vsh(const Value &v) {
                    if (v.is_null()) return nullptr;
                    if (v.is_bool()) return v.as_bool();
                    if (v.is_int()) return v.as_int();
                    if (v.is_float()) return v.as_float();
                    if (v.is_string()) return v.as_string();
                    if (v.is_list()) {
                        nlohmann::json a = nlohmann::json::array();
                        for (const auto &e: *v.as_list()) a.push_back(from_vsh(e));
                        return a;
                    }
                    if (v.is_map()) {
                        nlohmann::json o = nlohmann::json::object();
                        for (const auto &kv: *v.as_map())
                            o[kv.first] = from_vsh(kv.second);
                        return o;
                    }
                    // Function/Class/Instance -> string descriptiva (no serializable).
                    return v.to_string();
                }
            };

            builtins_["json_parse"] = [](std::vector<Value> args) -> Value {
                if (args.empty() || !args[0].is_string()) {
                    throw VshRuntimeError("Uso: json_parse(str)");
                }
                try {
                    auto j = nlohmann::json::parse(args[0].as_string());
                    return JsonConv::to_vsh(j);
                } catch (const nlohmann::json::exception &e) {
                    throw VshRuntimeError(std::string("json_parse: ") + e.what());
                }
            };

            builtins_["json_stringify"] = [](std::vector<Value> args) -> Value {
                if (args.empty()) {
                    throw VshRuntimeError("Uso: json_stringify(value [, indent])");
                }
                int indent = -1; // -1 = sin pretty-print
                if (args.size() >= 2) {
                    if (!args[1].is_int()) {
                        throw VshRuntimeError("json_stringify: 'indent' debe ser int");
                    }
                    indent = static_cast<int>(args[1].as_int());
                    if (indent < 0) indent = -1;
                }
                try {
                    auto j = JsonConv::from_vsh(args[0]);
                    return Value(j.dump(indent));
                } catch (const std::exception &e) {
                    throw VshRuntimeError(std::string("json_stringify: ") + e.what());
                }
            };
        }

        // ===== Helpers binario little-endian (utiles para protocolo length-prefix u32) =====
        // u32_le_pack(n)        -> string de 4 bytes con n en LE.
        // u32_le_unpack(s)      -> int64 (lee primeros 4 bytes de s en LE).
        // bytes_concat(a, b)    -> string con a ++ b (binary safe).
        // bytes_len(s)          -> int64 con longitud en bytes (== len(s)).
        // bytes_substr(s, off, len) -> string (substring binary-safe).
        builtins_["u32_le_pack"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_int()) {
                throw VshRuntimeError("Uso: u32_le_pack(int)");
            }
            uint32_t    n = static_cast<uint32_t>(args[0].as_int());
            std::string out(4, '\0');
            out[0] = static_cast<char>(n & 0xFF);
            out[1] = static_cast<char>((n >> 8) & 0xFF);
            out[2] = static_cast<char>((n >> 16) & 0xFF);
            out[3] = static_cast<char>((n >> 24) & 0xFF);
            return Value(out);
        };
        builtins_["u32_le_unpack"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) {
                throw VshRuntimeError("Uso: u32_le_unpack(str)");
            }
            const std::string &s = args[0].as_string();
            if (s.size() < 4) {
                throw VshRuntimeError("u32_le_unpack: necesita al menos 4 bytes");
            }
            uint32_t n = (static_cast<uint32_t>(static_cast<uint8_t>(s[0]))) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 8) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 16) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(s[3])) << 24);
            return Value(static_cast<int64_t>(n));
        };
        builtins_["bytes_concat"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 2 || !args[0].is_string() || !args[1].is_string()) {
                throw VshRuntimeError("Uso: bytes_concat(str_a, str_b)");
            }
            std::string r = args[0].as_string();
            r += args[1].as_string();
            return Value(r);
        };
        builtins_["bytes_len"] = [](std::vector<Value> args) -> Value {
            if (args.empty() || !args[0].is_string()) {
                throw VshRuntimeError("Uso: bytes_len(str)");
            }
            return Value(static_cast<int64_t>(args[0].as_string().size()));
        };
        builtins_["bytes_substr"] = [](std::vector<Value> args) -> Value {
            if (args.size() < 3 || !args[0].is_string()
                || !args[1].is_int() || !args[2].is_int()) {
                throw VshRuntimeError("Uso: bytes_substr(str, off, len)");
            }
            const std::string &s   = args[0].as_string();
            int64_t            off = args[1].as_int();
            int64_t            len = args[2].as_int();
            if (off < 0 || off > static_cast<int64_t>(s.size())) {
                throw VshRuntimeError("bytes_substr: offset fuera de rango");
            }
            if (len < 0) len = 0;
            if (off + len > static_cast<int64_t>(s.size())) {
                len = static_cast<int64_t>(s.size()) - off;
            }
            return Value(s.substr(static_cast<size_t>(off),
                                  static_cast<size_t>(len)));
        };

        // ---- argumentos del script (estilo Python sys.argv) ----
        // ARGV[0] = ruta del script, ARGV[1..] = argumentos pasados
        auto argv_list = std::make_shared<std::vector<Value> >();
        global_->define("ARGV", Value(argv_list));

        // ---- exponer todos los builtins en el scope global como stubs (estilo Python) ----
        // Esto permite usar 'help' sin parentesis igual que en Python
        for (auto &kv: builtins_) {
            const std::string &bname = kv.first;
            auto               stub  = std::make_shared<VshFunction>();
            stub->name               = bname;
            stub->closure_env        = global_;
            // body queda nullptr -> call_fn lo despachara al builtin nativo
            global_->define(bname, Value(stub));
        }

        // ---- clase Error incorporada (base para errores personalizados) ----
        exec_string(
            "class Error {\n"
            "    fn __init__(self, message) {\n"
            "        self.message = message\n"
            "    }\n"
            "    fn __str__(self) {\n"
            "        return self.message\n"
            "    }\n"
            "}\n",
            "<builtin>"
        );
    }
} // namespace vsh
