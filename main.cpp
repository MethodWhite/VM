/*
 * VestaVM - Máquina Virtual Distribuida
 *
 * Copyright © 2026 David López.T (DesmonHak) (Castilla y León, ES)
 * Licencia VMProject
 *
 * USO LIBRE NO COMERCIAL con atribución obligatoria.
 * PROHIBIDO lucro sin permiso escrito.
 *
 * Descargo: Autor no responsable por modificaciones.
 */



#include <cstdlib>
#include <iostream>
#include <iomanip>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <atomic>
#include <csignal>
#include <openssl/sha.h>

#include "cxxopts.hpp"

#include "cli/cli.h"
#include "cli/vsh.h"
#include "ir/ir_emitter.h"
#include "jit/auto_jit.h"
#include "runtime/proceso_runtime.h"
#include "cli/runtime_api_commands.h"
#include "util/assembler_multiprocess.h"
#include "vex/compiler.h"
#include "util/sqlite_singleton.h"
#include "util/fs_utils.h"
#include "runtime/manager_runtime.h"
#include "gc/gc_heap.h"
#include "loader/loader.h"
#include "profiler/timer.h"
#include "distrib/dist_runtime.h"
#include "distrib/dist_debug.h"
#include "distrib/node_registry.h"
#include "debug/debugger.h"
#include "install/install.h"

#ifdef VESTA_HAS_PREPROCESSOR
    #include "preprocessor/preprocessor.h"
#endif

// flag global para el modo --dist-server; SIGINT lo pone a false
static std::atomic<bool> g_server_running{true};
static void on_dist_sigint(int) { g_server_running.store(false); }

/**
 * @brief Construye la NodeAuthConfig a partir de los flags de autenticacion.
 *
 * Calcula SHA-256 del token en texto plano si se proporciono --dist-token.
 * Copia las rutas TLS si se activo --dist-tls.
 *
 * @param token    Token en texto plano (puede estar vacio).
 * @param use_tls  true si se activo --dist-tls.
 * @param cert     Ruta al certificado PEM del cliente.
 * @param key      Ruta a la clave privada PEM del cliente.
 * @param ca       Ruta al CA bundle PEM para verificar pares.
 * @return NodeAuthConfig relleno.
 */
static distrib::NodeAuthConfig build_node_auth(
    const std::string &token,
    bool               use_tls,
    const std::string &cert,
    const std::string &key,
    const std::string &ca)
{
    distrib::NodeAuthConfig auth{};
    if (!token.empty()) {
        auth.use_token = true;
        SHA256(reinterpret_cast<const unsigned char *>(token.c_str()),
               token.size(), auth.token_hash);
    }
    if (use_tls) {
        auth.use_tls = true;
        std::snprintf(auth.cert_path, sizeof(auth.cert_path), "%s", cert.c_str());
        std::snprintf(auth.key_path,  sizeof(auth.key_path),  "%s", key.c_str());
        std::snprintf(auth.ca_path,   sizeof(auth.ca_path),   "%s", ca.c_str());
    }
    return auth;
}

/**
 * @brief Aplica la configuracion distribuida a una instancia VM.
 *
 * Reemplaza el dist_runtime minimo creado en VM::VM() por uno completamente
 * configurado segun los flags --dist-* de la linea de comandos.
 * Si --dist-port > 0 o --dist-discover esta activo, llama a start() para
 * abrir el servidor VDP y/o el hilo de descubrimiento UDP.
 * Registra cualquier nodo estatico indicado con --dist-add-node (formato IP:PUERTO).
 *
 * @param vm     Instancia VM sobre la que se aplica la configuracion.
 * @param result Resultado del parseo de cxxopts con todos los flags.
 */
static void apply_dist_config(runtime::VM *vm, const cxxopts::ParseResult &result)
{
    const std::string token   = result["dist-token"].as<std::string>();
    const bool        use_tls = result.count("dist-tls") > 0;
    const std::string cert    = result["dist-cert"].as<std::string>();
    const std::string key     = result["dist-key"].as<std::string>();
    const std::string ca      = result["dist-ca"].as<std::string>();

    // construir configuracion del DistRuntime
    distrib::DistRuntimeConfig cfg{};
    cfg.local_node_id    = result["dist-node-id"].as<uint64_t>();
    cfg.vdp_listen_port  = result["dist-port"].as<uint16_t>();
    cfg.discover_port    = result["dist-discover-port"].as<uint16_t>();
    cfg.enable_discovery = result.count("dist-discover") > 0;

    std::string name = result["dist-name"].as<std::string>();
    if (!name.empty())
        std::snprintf(cfg.local_node_name, sizeof(cfg.local_node_name), "%s", name.c_str());
    else
        std::snprintf(cfg.local_node_name, sizeof(cfg.local_node_name), "vm-%llu",
                      static_cast<unsigned long long>(vm->id));

    cfg.server_auth = build_node_auth(token, use_tls, cert, key, ca);

    // reemplazar el dist_runtime minimal por uno completamente configurado
    vm->dist_runtime = std::make_unique<distrib::DistRuntime>(*vm, cfg);

    // arrancar el servidor VDP y/o el descubrimiento si alguno esta habilitado
    if (cfg.vdp_listen_port > 0 || cfg.enable_discovery) {
        if (vm->dist_runtime->start()) {
            std::string msg = "[dist] Servidor VDP iniciado";
            if (cfg.vdp_listen_port)
                msg += " en puerto " + std::to_string(cfg.vdp_listen_port);
            if (cfg.enable_discovery)
                msg += " con descubrimiento UDP (puerto " + std::to_string(cfg.discover_port) + ")";
            vesta::scout() << msg << "\n";
        } else {
            std::cerr << "[dist] Error al iniciar el servidor VDP\n";
        }
    }

    // registrar nodos estaticos proporcionados con --dist-add-node IP:PUERTO
    if (result.count("dist-add-node")) {
        distrib::NodeAuthConfig node_auth = build_node_auth(token, use_tls, cert, key, ca);
        for (auto &spec : result["dist-add-node"].as<std::vector<std::string>>()) {
            auto colon = spec.rfind(':');
            if (colon == std::string::npos) {
                std::cerr << "[dist] Formato invalido (esperado IP:PUERTO): " << spec << "\n";
                continue;
            }
            std::string node_ip   = spec.substr(0, colon);
            uint16_t    node_port = 0;
            try { node_port = static_cast<uint16_t>(std::stoul(spec.substr(colon + 1))); }
            catch (...) { std::cerr << "[dist] Puerto invalido en: " << spec << "\n"; continue; }

            uint32_t idx = vm->dist_runtime->add_node(
                node_ip.c_str(), node_port, node_auth, node_ip.c_str());
            vesta::scout() << "[dist] Nodo registrado: " << node_ip << ":"
                           << node_port << " (idx=" << idx << ")\n";
        }
    }
}


int main(int argc, char *argv[]) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32) && !defined(__CYGWIN__)
    asm_multi_process::run_and_capture("chcp 65001");
#endif

    /* registrar el hook auto-JIT en el runtime.
     * El runtime (vesta_rt) tiene un function pointer nulo por defecto;
     * el main aqui lo apunta al JIT trigger.  Si el env var
     * VESTA_JIT_THRESHOLD esta presente, el trigger compila funciones
     * hot a codigo nativo.  Sin env var, el
     * threshold queda en UINT32_MAX y el hook es no-op aunque se llame. */
    runtime::g_callvirt_post_hook = &jit::maybe_compile_method;

    cxxopts::Options options("VMProject", "Virtual Machine Example");

    options.add_options()
            ("h,help", "Mostrar ayuda")
            ("o,output", "Archivo de salida (sin extensión o completo)", cxxopts::value<std::string>())
            ("driver", "Compilar un directorio completo en paralelo", cxxopts::value<std::string>())
            ("worker", "Compilar un único archivo (modo interno)", cxxopts::value<std::string>())
            ("j,threads", "Número de hilos para el driver", cxxopts::value<int>()->default_value("0"))
            ("v,version", "Mostrar versión")
            ("m,mode", "Modo de ejecución (vm/jit)", cxxopts::value<std::string>()->default_value("vm"))
            ("list-arch", "Imprimir arquitecturas soportadas")
            ("asm-file", "Archivo ASM a ensamblar", cxxopts::value<std::string>())
            ("disasm-file", "Archivo binario a desensamblar", cxxopts::value<std::string>())
            ("arch", "Arquitectura para ensamblar/desensamblar", cxxopts::value<std::string>())
            ("save-output", "Guardar código ensamblado/desensamblado en archivo")
            ("output-prefix", "Prefijo/nombre base para archivos de salida",
             cxxopts::value<std::string>()->default_value("out"))
            ("run", "Ejecutar un archivo .velb en la VM", cxxopts::value<std::string>())
            ("build", "Compilar un archivo .vel a .velb", cxxopts::value<std::string>())
            ("schedulers", "Número de schedulers para el comando run", cxxopts::value<size_t>()->default_value("1"))
            ("install",   "Ejecutar el instalador interactivo (o con flags adicionales)")
            ("uninstall", "Desinstalar Vesta usando el manifest")
            ("repair",    "Reparar la instalacion existente")
            ("silent",    "Modo silencioso para install/uninstall")
            ("per-user",  "Forzar instalacion per-user")
            ("system-wide","Forzar instalacion system-wide")
            ("prefix",    "Directorio destino", cxxopts::value<std::string>())
            ("manifest",  "Ruta a install_manifest.json", cxxopts::value<std::string>())
            ("stats", "Mostrar estadísticas de ejecución al finalizar (tiempo, MIPS)")
            // ---- opciones de JIT ----
            ("jit-threshold", "Umbral de invocaciones de un metodo para disparar JIT (default: UINT32_MAX = JIT off; sugerido para test: 1)",
                cxxopts::value<uint32_t>())
            ("jit-warn",      "Imprimir warnings cada vez que el Selector encuentra una IR op no soportada (dedup por op+linea)")
            ("jit-stats",     "Imprimir snapshot final de counters del JIT: compiled/unsupported/no_ir + threshold")
            ("jit-disasm",    "Volcar hex bytes + disasm (Capstone) de cada funcion JIT-compilada a stderr")
            // ---- opciones de runtime distribuido ----
            ("dist-port",         "Puerto VDP del servidor distribuido (0 = sin servidor TCP)",
                cxxopts::value<uint16_t>()->default_value("0"))
            ("dist-discover",     "Activar descubrimiento UDP de nodos en la LAN")
            ("dist-discover-port","Puerto UDP para descubrimiento de nodos",
                cxxopts::value<uint16_t>()->default_value("7790"))
            ("dist-name",         "Nombre del nodo local (cadena identificativa)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-node-id",      "ID de 64 bits del nodo (0 = generar automaticamente)",
                cxxopts::value<uint64_t>()->default_value("0"))
            ("dist-add-node",     "Nodo estatico a registrar y conectar (formato IP:PUERTO, repetible)",
                cxxopts::value<std::vector<std::string>>())
            ("dist-token",        "Token de autenticacion en texto plano (se almacena como SHA-256)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-tls",          "Usar TLS en las conexiones VDP salientes y entrantes")
            ("dist-cert",         "Ruta al certificado TLS del nodo local (PEM)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-key",          "Ruta a la clave privada TLS del nodo local (PEM)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-ca",           "Ruta al CA bundle TLS para verificar pares (PEM)",
                cxxopts::value<std::string>()->default_value(""))
            ("dist-server",       "Modo servidor distribuido puro: espera conexiones VDP sin ejecutar bytecode")
            ("dist-debug",        "Activar trazas de depuracion del subsistema distribuido (RSPAWN, HALT, FUTURE_FULFILL)")
            ("script",            "Ejecutar un fichero VestaShell (.vsh) y salir", cxxopts::value<std::string>())
            ("interprete",        "Abrir el interprete interactivo VestaShell (REPL .vsh)")
            ("ir-file",           "Compilar archivo .ir (SSA IR) a .vel y opcionalmente a .velb",
                cxxopts::value<std::string>())
            ("ir-opt",            "Nivel de optimizacion IR: 0=O0, 1=O1, 2=O2, 3=O3 (defecto: 1)",
                cxxopts::value<int>()->default_value("1"))
            ("ir-emit-only",      "Solo emitir el texto .vel; no compilar a .velb")
            ("vex",               "Compilar archivo .vex (lenguaje Vex) a .velb",
                cxxopts::value<std::string>())
            ("vex-emit-only",     "Solo emitir el .vel intermedio del .vex; no compilar a .velb")
            ("vex-emit-ir",       "Emitir el SSA IR del .vex (pre y post optimizacion) en <output>.ir; util para debug del frontend")
            ("vex-base",          "VA base address para el modulo (hex, e.g. 0x10000000). Usado para plugins cargados via loadmodule, evita solapamiento con el caller (default 0x0).",
                cxxopts::value<std::string>()->default_value("0x0"))
            // Diagramas para debug y traceo del pipeline Vex.  Soportan dos
            // formatos seleccionables via --diagram-format:
            //   mermaid (default): escribe .mmd con bloque ```mermaid```;
            //                      listo para VS Code / GitHub / mermaid.live.
            //   graphviz:          escribe .dot con `digraph G { ... }`;
            //                      listo para `dot -Tsvg foo.dot -o foo.svg`.
            //   both:              escribe AMBOS formatos.
            // El contenido es paralelo entre formatos: misma topologia, misma
            // info por nodo (Graphviz lleva extra via tooltips/atributos DOT).
            // --diagram-all genera los 4 diagramas (AST, IR pre, IR post, VEL)
            // en el formato escogido.
            ("diagram-vex",     "Generar diagrama del AST Vex post type-check (.ast.<ext>)")
            ("diagram-ir",      "Generar diagrama del SSA IR pre-optimizacion (.ir.pre.<ext>)")
            ("diagram-ir-opt",  "Generar diagrama del SSA IR post-optimizacion (.ir.post.<ext>)")
            ("diagram-vel",     "Generar diagrama del bytecode .vel final (.vel.<ext>)")
            ("diagram-all",     "Generar los 4 diagramas (vex, ir pre, ir post, vel) con sufijos correspondientes")
            ("diagram-format",  "Formato de salida: mermaid | graphviz | both (default: mermaid). graphviz produce .dot listo para Graphviz; mermaid produce .mmd.",
                cxxopts::value<std::string>()->default_value("mermaid"))
            ("gc-debug",        "Activar trazas de debug del Garbage Collector a stderr (minor_gc, major_gc, sweep, release_handle, evacuate). Util para diagnosticar use-after-free o objetos colectados prematuramente. Alt: env VESTA_GC_DEBUG=1.")
            ("gc-debug-buffered","Activar modo BUFFERED del GC debug: ~100x mas rapido (buffer thread-local de 64KB) pero pierde las ultimas trazas en crash. Implica --gc-debug. Alt: env VESTA_GC_DEBUG_BUFFERED=1.")
            ("debug-port",      "Activar el servidor de depuracion TCP en el puerto N (default 9229 si N=0). Soporta multiples clientes simultaneos y JSON via length-prefix framing. El servidor permanece disponible mientras la VM ejecuta; los clientes (e.g. tools/dbg_client.vsh) se conectan via tcp_connect, envian comandos JSON y reciben eventos asincronos (break/exit/exception/spawned). Sin este flag, el debugger NO se instancia y el coste runtime es exactamente cero. Cuando esta presente, el proceso main arranca PAUSADO en su primera instruccion para dar tiempo al cliente a conectarse y poner breakpoints; el cliente debe enviar 'continue 0' para arrancar la ejecucion.",
                cxxopts::value<uint16_t>()->default_value("0"))
            ("vex-debug",       "Emitir comentarios `// @line N` en el .vel intermedio del compilador Vex y, cuando se integre la pipeline completa de debug section (Phase 2), embeber la tabla bytecode_offset -> (file, line) en el .velb final.  Sin este flag, el .vel/.velb no contienen info de debug -> el ejecutable es mas pequeno y el frontend NO genera datos extra.  Con el flag, el cliente del debugger puede setear breakpoints por linea Vex (`b file.vex:42`) en lugar de solo por addr.")
            ("port",            "Transpilar el IR a codigo fuente del lenguaje destino y escribir a <output>.<ext> (e.g. .c).  Valores actuales: 'c'.  Futuro: 'java', 'js', 'rust'.  Con --port=c se genera codigo C99 portable listo para compilar con gcc/clang -O3 -std=c11 SIN dependencias de VestaVM (a menos que --port-gc=vesta).  Implica --vex (se aplica al pipeline Vex post-optimizacion).",
                cxxopts::value<std::string>())
            ("port-gc",         "Modelo de memoria del codigo portado: none|vesta|boehm.  none (default): malloc/free + sin GC.  Las IR ops de objetos GC (NEWOBJ/strings) emiten stub.  vesta: enlazar contra vesta_rt.lib.  boehm: enlazar contra libgc (placeholder).",
                cxxopts::value<std::string>()->default_value("none"))
            ("port-exc",        "Manejo de excepciones del codigo portado: none|setjmp|returncode.  none: throw -> abort.  setjmp (default): longjmp portable.  returncode: codigo de retorno + propagacion explicita (no impl v1).",
                cxxopts::value<std::string>()->default_value("setjmp"))
            ("port-types",      "Estilo de tipos del codigo portado: stdint (default) usa int8_t/uint64_t/etc de stdint.h.  builtin usa char/int/long del C clasico (menos portable).",
                cxxopts::value<std::string>()->default_value("stdint"))
            ("port-strings",    "Modelo de strings: raw (const char* solo literales) | managed (default: VexString con tracking per-thread).",
                cxxopts::value<std::string>()->default_value("managed"))
            ("port-freestanding", "Generar C freestanding (sin includes automaticos de stdio/stdlib/math).  El usuario debe proveer vex_throw + vex_panic_with_str.  Para bootloaders, kernels, firmware.")
            ("port-stdlib-dir", "Path al directorio stdlib/port/c con los snippets .v.c (autodetect si vacio).",
                cxxopts::value<std::string>()->default_value(""))
            ("port-arch", "Target CPU: '' (portable, default) | native | x86-64-v2 | x86-64-v3 | x86-64-v4.  Emite #pragma GCC target con flags agresivas (AVX2/FMA/BMI2 segun nivel).",
                cxxopts::value<std::string>()->default_value(""))
            ("port-no-aggressive", "Desactivar atributos agresivos (const/cold/restrict/always_inline + __builtin_expect/unreachable).  Default: activado.")
            ("instrument", "Instrumentacion en el IR Vex (heredada por bytecode VM, JIT, port C, port futuros): none (default) | trace (calls a vex_trace:enter/exit por funcion) | profile (timing per-funcion).",
                cxxopts::value<std::string>()->default_value("none"))
#ifdef VESTA_HAS_PREPROCESSOR
            ("preprocess-only", "Solo preprocesar un .vel y mostrar/guardar el resultado (debug)", cxxopts::value<std::string>())
#endif
            ("keep-labels", "Mantener los nombres de label en el .velb (por defecto se eliminan: el loader no los usa y reducen ~9% el tamano)")
            ;


    // BUG FIX: Args posicionales y allow_unrecognised DEBEN configurarse
    // ANTES de @c options.parse(...).  El bug anterior registraba el
    // option `positional` y @c parse_positional DESPUES del parse, asi
    // que cualquier `vm --script foo.vsh arg1 arg2` perdia arg1/arg2 y
    // ARGV del script quedaba con solo el path del script.  Se mueve
    // toda la configuracion arriba; esto es prerequisito de @c parse.
    options.add_options()("positional", "Argumentos posicionales",
        cxxopts::value<std::vector<std::string>>());
    options.parse_positional({"positional"});
    options.positional_help("[args...]");
    options.allow_unrecognised_options();   // para flags sin que aun hay que parsear

    auto result = options.parse(argc, argv);


    // activar trazas de depuracion del subsistema distribuido si se paso --dist-debug
    if (result.count("dist-debug"))
        distrib::set_dist_debug(true);

    // activar trazas del GC si se paso --gc-debug.  Tambien respeta env
    // VESTA_GC_DEBUG=1 (la inicializacion estatica corre antes de main,
    // no se pierde si el flag CLI esta o no).
    if (result.count("gc-debug"))
        gc::set_gc_debug(true);
    // --gc-debug-buffered implica --gc-debug ademas de buffered mode.
    if (result.count("gc-debug-buffered")) {
        gc::set_gc_debug(true);
        gc::set_gc_debug_buffered(true);
    }

    // ---- Configuracion del JIT ----
    //
    // Orden de prioridad (mayor primero):
    //   1. --jit-threshold N   (CLI explicito)
    //   2. -m jit              (preset = threshold 1, util para tests)
    //   3. VESTA_JIT_THRESHOLD env var (default ya lazy-leida en auto_jit)
    //
    // --jit-warn activa el output detallado de IR ops no soportadas.
    // --jit-stats imprime el snapshot final al RET de main (independiente).
    if (result.count("jit-threshold")) {
        jit::set_jit_threshold(result["jit-threshold"].as<uint32_t>());
    } else if (result.count("mode")) {
        const std::string m = result["mode"].as<std::string>();
        if (m == "jit") {
            /* Preset: threshold=1 => cada metodo se JIT-compila a la
             * primera invocacion.  Equivalente a la opcion explicita
             * --jit-threshold 1.  Si el usuario quiere otro threshold,
             * que use --jit-threshold N directamente. */
            jit::set_jit_threshold(1);
        } else if (m == "vm" || m == "interp") {
            /* Desactiva el JIT explicitamente sobreescribiendo cualquier
             * env var.  UINT32_MAX = JIT off, hook devuelve sin compilar
             * en el fast path. */
            jit::set_jit_threshold(UINT32_MAX);
        }
    }
    if (result.count("jit-warn")) {
        jit::g_jit_warn_unsupported = true;
    }
    if (result.count("jit-disasm")) {
        jit::g_jit_disasm = true;
    }
    const bool jit_stats_requested = result.count("jit-stats") > 0;

    if (result.count("help")) {
        vesta::scout() << options.help() << std::endl;
        return 0;
    }

    std::string out_prefix;
    if (result.count("output")) {
        out_prefix = result["output"].as<std::string>();
    } else {
        out_prefix = result["output-prefix"].as<std::string>();
    }

    if (result.count("version")) {
        vesta::scout() << "Vesta v0.1.0" << std::endl;
        return 0;
    }

    if (result.count("list-arch")) {
        const ArchSupport &archs = get_available_architectures();
        vesta::scout() << "Capstone supported architectures:\n";
        for (auto &a: archs.capstone) vesta::scout() << "  " << a << "\n";
        vesta::scout() << "Keystone supported architectures:\n";
        for (auto &a: archs.keystone) vesta::scout() << "  " << a << "\n";
        return 0;
    }

#ifdef VESTA_HAS_PREPROCESSOR
    // Solo preprocesar: expandir macros y directivas sin ensamblar
    // vm.exe --preprocess-only src/main.vel [-o salida.vel]
    if (result.count("preprocess-only")) {
        const std::string& src_path = result["preprocess-only"].as<std::string>();

        // leer el archivo fuente
        std::ifstream ifs(src_path, std::ios::binary);
        if (!ifs) {
            std::cerr << "error: no se puede abrir: " << src_path << "\n";
            return EXIT_FAILURE;
        }
        std::string source((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

        // configurar el preprocesador igual que run_worker
        vpp::Preprocessor pp;
        std::string source_dir =
            std::filesystem::path(src_path).parent_path().string();
        if (source_dir.empty()) source_dir = ".";
        pp.options().include_paths.push_back(source_dir);
        std::string exe_dir =
            std::filesystem::path(fs::get_executable_path()).parent_path().string();
        pp.options().import_paths.push_back(exe_dir + "/preprocessor/include_lib");
        pp.options().import_paths.push_back(exe_dir + "/include_lib");
        pp.options().import_paths.push_back(source_dir);
#ifdef _WIN32
        pp.options().predefines.push_back("__VPP_WINDOWS__");
#elif defined(__linux__)
        pp.options().predefines.push_back("__VPP_LINUX__");
#elif defined(__APPLE__)
        pp.options().predefines.push_back("__VPP_MACOS__");
#endif
#if defined(__x86_64__) || defined(_M_X64)
        pp.options().predefines.push_back("__VPP_X86_64__");
#elif defined(__i386__) || defined(_M_IX86)
        pp.options().predefines.push_back("__VPP_X86_32__");
#elif defined(__aarch64__) || defined(_M_ARM64)
        pp.options().predefines.push_back("__VPP_AARCH64__");
#endif

        std::string processed = pp.process(source, src_path);

        // imprimir todos los diagnosticos
        for (const auto& d : pp.diagnostics().diagnostics()) {
            std::cerr << d.loc.file << ":" << d.loc.line << ":"
                      << d.loc.col  << ": "
                      << (d.level >= vpp::DiagLevel::ERR ? "error: " : "warning: ")
                      << d.message << "\n";
        }
        if (pp.diagnostics().has_errors()) return EXIT_FAILURE;

        // escribir a archivo si se especifico -o, si no a stdout
        if (!out_prefix.empty() && out_prefix != "out") {
            // determinar nombre del archivo de salida
            std::string out_file = out_prefix;
            if (out_file.find('.') == std::string::npos) out_file += ".vel";
            std::ofstream ofs(out_file);
            if (!ofs) {
                std::cerr << "error: no se puede escribir: " << out_file << "\n";
                return EXIT_FAILURE;
            }
            ofs << processed;
            vesta::scout() << "[preprocess-only] -> " << out_file << "\n";
        } else {
            // sin -o: imprimir a stdout para inspeccion rapida
            std::cout << processed;
        }
        return EXIT_SUCCESS;
    }
#endif

    // -----------------------------------------------------------------------
    // Modo servidor distribuido puro (sin ejecutar bytecode)
    // vm.exe --dist-server --dist-port 7789 [--dist-discover] [--dist-name nodo1]
    //        [--dist-add-node 192.168.1.100:7789] [--dist-token secreto]
    //        [--dist-tls --dist-cert cert.pem --dist-key key.pem --dist-ca ca.pem]
    // -----------------------------------------------------------------------
    if (result.count("dist-server")) {
        try {
            runtime::ManageVM dist_mgr(nullptr, 0);
            runtime::VM *vm = dist_mgr.loader.create_vm_instance(1);
            if (!vm) {
                std::cerr << "[dist-server] Error: no se pudo crear la instancia VM\n";
                return EXIT_FAILURE;
            }

            apply_dist_config(vm, result);

            // activar modo persistente para que el scheduler no termine al no haber procesos;
            // los procesos remotos llegan via rspawn despues del arranque
            vm->vm_persistent = true;
            vm->start();

            vesta::scout() << "[dist-server] Nodo distribuido activo. "
                              "Pulse Ctrl+C para detener.\n";

            // esperar hasta Ctrl+C
            std::signal(SIGINT, on_dist_sigint);
            while (g_server_running.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            vm->dist_runtime->stop();
            vm->stop();
            vesta::scout() << "[dist-server] Nodo detenido.\n";
        } catch (const std::exception &e) {
            std::cerr << "[dist-server] Error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    if (result.count("install") ||
        result.count("uninstall") ||
        result.count("repair"))
    {
        // Reconstruir argv "limpio" para el parser interno del instalador
        std::vector<std::string> args;
        if (result.count("uninstall")) args.push_back("uninstall");
        else if (result.count("repair")) args.push_back("repair");
        else args.push_back("install");

        if (result.count("silent"))      args.push_back("--silent");
        if (result.count("per-user"))    args.push_back("--per-user");
        if (result.count("system-wide")) args.push_back("--system-wide");
        if (result.count("prefix"))      args.push_back("--prefix=" + result["prefix"].as<std::string>());
        if (result.count("manifest"))    args.push_back("--manifest=" + result["manifest"].as<std::string>());

        std::vector<char*> argv2;
        argv2.push_back(argv[0]);
        for (auto& s : args) argv2.push_back(s.data());
        return install::run_install_cli((int)argv2.size(), argv2.data());
    }

    // Compilar un archivo como worker
    // vm.exe --worker src/main.vel -o main.velb
    if (result.count("worker")) {
        return asm_multi_process::run_worker(
            result["worker"].as<std::string>(),
            out_prefix,
            /*skip_preprocessor=*/false,
            /*keep_labels=*/(result.count("keep-labels") > 0)
        );
    }

    // Compilar un proyecto entero en paralelo
    // vm.exe --driver src/ -j 8 -o program.velb
    // Compilar con número automático de hilos
    // vm.exe --driver src/ -j 0 -o program.velb
    if (result.count("driver")) {
        int threads = result["threads"].as<int>();
        return asm_multi_process::run_driver(
            result["driver"].as<std::string>(),
            threads,
            out_prefix
        );
    }


    bool save_output = result.count("save-output") > 0;

    // ensamblar un solo archivo (modo clásico)
    // vm.exe --asm-file src/main.asm --arch x86_64
    if (result.count("asm-file")) {
        return assemble_file(
                   result["asm-file"].as<std::string>(),
                   result["arch"].as<std::string>(),
                   save_output,
                   out_prefix
               )
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    if (result.count("disasm-file")) {
        if (!result.count("arch")) {
            std::cerr << "--arch es requerido para desensamblar\n";
            return EXIT_FAILURE;
        }
        return disassemble_file(
                   result["disasm-file"].as<std::string>(),
                   result["arch"].as<std::string>(),
                   save_output,
                   out_prefix
               )
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    }

    // Compilar un archivo .ir (SSA IR) a .vel y opcionalmente a .velb
    // vm.exe --ir-file program.ir --ir-opt 2 -o program.velb
    if (result.count("ir-file")) {
        const std::string &ir_path = result["ir-file"].as<std::string>();
        int opt_n = result["ir-opt"].as<int>();
        bool emit_only = result.count("ir-emit-only") > 0;

        // Leer el archivo .ir
        std::ifstream ifs(ir_path);
        if (!ifs.is_open()) {
            std::cerr << "[ir] No se puede abrir: " << ir_path << "\n";
            return EXIT_FAILURE;
        }
        std::string ir_text((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());

        // Emitir .vel
        ir::EmitOptions eopts;
        eopts.opt_level     = ir::opt_level_from_int(opt_n);
        eopts.emit_comments = true;
        eopts.export_all    = true;

        ir::EmitResult er = ir::ir_emit_text(ir_text, eopts);
        if (!er.ok) {
            std::cerr << "[ir] Error de emision: " << er.error << "\n";
            return EXIT_FAILURE;
        }

        // Determinar nombre del archivo .vel de salida
        std::string vel_path = out_prefix.empty() ? "out.vel" : out_prefix + ".vel";
        {
            std::ofstream ofs(vel_path);
            if (!ofs.is_open()) {
                std::cerr << "[ir] No se puede escribir: " << vel_path << "\n";
                return EXIT_FAILURE;
            }
            ofs << er.vel_text;
        }
        vesta::scout() << "[ir] .vel generado: " << vel_path << "\n";

        if (emit_only) return EXIT_SUCCESS;

        // Compilar el .vel generado a .velb usando el pipeline existente
        return asm_multi_process::run_worker(
            vel_path, out_prefix,
            /*skip_preprocessor=*/false,
            /*keep_labels=*/(result.count("keep-labels") > 0));
    }

    // Compilar un archivo .vex (lenguaje Vex) a .velb.
    // Pipeline:
    //   .vex source
    //     -> [VPP opcional]    (metaprogramacion compartida con .vel)
    //     -> Vex frontend      (lex + parse + tipos + lowering)
    //     -> ir::IrModule
    //     -> ir_emit_module    (texto .vel)
    //     -> run_worker(.vel, skip_preprocessor=true)
    //     -> .velb
    //
    // Ejemplo: vm.exe --vex src/main.vex -o main.velb
    if (result.count("vex")) {
        const std::string &vex_path = result["vex"].as<std::string>();
        bool emit_only = result.count("vex-emit-only") > 0;
        bool emit_ir   = result.count("vex-emit-ir")   > 0;

        // Flags de diagramas (Mermaid y/o Graphviz).  --diagram-all activa
        // los 4 diagramas; --diagram-format elige el formato de salida.
        const bool diag_all      = result.count("diagram-all")     > 0;
        const bool diag_vex      = diag_all || result.count("diagram-vex")    > 0;
        const bool diag_ir_pre   = diag_all || result.count("diagram-ir")     > 0;
        const bool diag_ir_post  = diag_all || result.count("diagram-ir-opt") > 0;
        const bool diag_vel      = diag_all || result.count("diagram-vel")    > 0;

        // Parsear --diagram-format: mermaid | graphviz | both.
        // Lower-case para tolerar "Mermaid", "GraphViz", "BOTH", etc.
        std::string diag_fmt = result.count("diagram-format") > 0
                                ? result["diagram-format"].as<std::string>()
                                : std::string("mermaid");
        for (auto &c : diag_fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool emit_mermaid  = (diag_fmt == "mermaid"  || diag_fmt == "both");
        bool emit_graphviz = (diag_fmt == "graphviz" || diag_fmt == "dot" || diag_fmt == "both");
        if (!emit_mermaid && !emit_graphviz) {
            std::cerr << "[diagram] Formato desconocido: '" << diag_fmt
                      << "' (usar mermaid | graphviz | both). Defaulting a mermaid.\n";
            emit_mermaid = true;
        }

        // parsear --vex-base (VA base en hex).  0x0 = comportamiento
        // por defecto (caller).  Para plugins cargados via loadmodule usar
        // un valor distinto (ej. 0x10000000) para evitar solapamiento con
        // el caller cuyo code section vive en 0x0..N.
        uint64_t vex_base_addr = 0;
        if (result.count("vex-base")) {
            const std::string &s = result["vex-base"].as<std::string>();
            try {
                vex_base_addr = std::stoull(s, nullptr, 0); // base 0 = autodetect 0x prefix
            } catch (...) {
                std::cerr << "[vex] --vex-base invalido: " << s << "\n";
                return EXIT_FAILURE;
            }
        }

        // 1 Leer el .vex.
        std::ifstream ifs(vex_path);
        if (!ifs.is_open()) {
            std::cerr << "[vex] No se puede abrir: " << vex_path << "\n";
            return EXIT_FAILURE;
        }
        std::string vex_source((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());

        // 2 Aplicar VPP (mismo pipeline que run_worker).  Esto es
        // best-effort: si una macro genera sintaxis no soportada por Vex,
        // el frontend reportara el error con la posicion preprocesada.
#ifdef VESTA_HAS_PREPROCESSOR
        {
            vpp::Preprocessor pp;
            std::string source_dir =
                std::filesystem::path(vex_path).parent_path().string();
            pp.options().include_paths.push_back(source_dir);
            std::string exe_dir =
                std::filesystem::path(fs::get_executable_path()).parent_path().string();
            pp.options().import_paths.push_back(exe_dir + "/preprocessor/include_lib");
            pp.options().import_paths.push_back(exe_dir + "/include_lib");
            pp.options().import_paths.push_back(source_dir);
#ifdef _WIN32
            pp.options().predefines.push_back("__VPP_WINDOWS__");
#elif defined(__linux__)
            pp.options().predefines.push_back("__VPP_LINUX__");
#elif defined(__APPLE__)
            pp.options().predefines.push_back("__VPP_MACOS__");
#endif
            std::string processed = pp.process(vex_source, vex_path);
            if (pp.diagnostics().has_errors()) {
                for (const auto &d : pp.diagnostics().diagnostics()) {
                    std::cerr << d.loc.file << ":" << d.loc.line << ": "
                              << (d.level == vpp::DiagLevel::ERR ? "error: " : "warning: ")
                              << d.message << "\n";
                }
                return EXIT_FAILURE;
            }
            vex_source = std::move(processed);
        }
#endif

        // 3 Frontend Vex: source -> IR -> .vel.
        // Sanitizar el nombre del modulo: el parser .vel rechaza identificadores
        // que empiezan con digito o que contienen caracteres no [A-Za-z0-9_],
        // pero los nombres de fichero pueden tener cualquier cosa.  Aplicamos
        // dos transformaciones: (a) si empieza por digito, anteponer "m_";
        // (b) sustituir cualquier byte no alfanumerico por '_'.
        std::string raw_name = std::filesystem::path(vex_path).stem().string();
        std::string mod_name; mod_name.reserve(raw_name.size() + 2);
        if (!raw_name.empty()
         && (raw_name[0] >= '0' && raw_name[0] <= '9')) {
            mod_name = "m_";
        }
        for (char c : raw_name) {
            const bool ok = (c >= 'a' && c <= 'z')
                         || (c >= 'A' && c <= 'Z')
                         || (c >= '0' && c <= '9')
                         || c == '_';
            mod_name.push_back(ok ? c : '_');
        }
        if (mod_name.empty()) mod_name = "main";

        vex::CompileOptions copts;
        copts.module_name = mod_name;
        copts.opt_level   = 2;
        copts.dump_ir     = emit_ir;  // habilita CompileResult::ir_text
        // Instrumentacion: aplica al IR independientemente del target
        // (bytecode VM, JIT, port C, etc.).  Validar valor aqui mismo.
        copts.instrument_mode = result["instrument"].as<std::string>();
        if (copts.instrument_mode != "none"
         && copts.instrument_mode != "trace"
         && copts.instrument_mode != "profile") {
            std::cerr << "[vex] --instrument invalido: "
                      << copts.instrument_mode
                      << " (valores: none|trace|profile)\n";
            return 2;
        }
        // --vex-debug: emite `// @line N` en el .vel y genera la
        // seccion debug en el .velb final.  Por defecto OFF: el ejecutable
        // queda mas pequeno y la compilacion mas rapida.
        copts.emit_debug  = (result.count("vex-debug") > 0);
        // Flags de diagramas: cada uno habilita la generacion del diagrama
        // correspondiente en CompileResult, segun el formato elegido por
        // --diagram-format.  Se escriben a archivos al final del bloque.
        copts.dump_mermaid_ast       = emit_mermaid  && diag_vex;
        copts.dump_mermaid_ir_pre    = emit_mermaid  && diag_ir_pre;
        copts.dump_mermaid_ir_post   = emit_mermaid  && diag_ir_post;
        copts.dump_mermaid_vel       = emit_mermaid  && diag_vel;
        copts.dump_graphviz_ast      = emit_graphviz && diag_vex;
        copts.dump_graphviz_ir_pre   = emit_graphviz && diag_ir_pre;
        copts.dump_graphviz_ir_post  = emit_graphviz && diag_ir_post;
        copts.dump_graphviz_vel      = emit_graphviz && diag_vel;

        // Flag --port=<lang>: si presente, configurar el transpiler IR -> codigo.
        // El frontend Vex llama al port::Transpiler tras la fase de optimizacion
        // del IR; el resultado queda en cr.port_text para que aqui lo escribamos
        // a archivo con la extension correspondiente.
        if (result.count("port")) {
            copts.port_target = result["port"].as<std::string>();
            // Validacion temprana del target: solo 'c' en v1.
            if (copts.port_target != "c") {
                std::cerr << "[port] Target desconocido: '" << copts.port_target
                          << "'.  Soportados: c.\n";
                return EXIT_FAILURE;
            }
            // Parsear los flags --port-gc / --port-exc / --port-types.
            const std::string &gc_s = result["port-gc"].as<std::string>();
            if (!port::parse_gc_mode(gc_s, copts.port_options.gc)) {
                std::cerr << "[port] --port-gc invalido: " << gc_s
                          << " (valores: none|vesta|boehm)\n";
                return EXIT_FAILURE;
            }
            const std::string &exc_s = result["port-exc"].as<std::string>();
            if (!port::parse_exc_mode(exc_s, copts.port_options.exc)) {
                std::cerr << "[port] --port-exc invalido: " << exc_s
                          << " (valores: none|setjmp|returncode)\n";
                return EXIT_FAILURE;
            }
            const std::string &str_s = result["port-strings"].as<std::string>();
            if (!port::parse_string_mode(str_s, copts.port_options.strings)) {
                std::cerr << "[port] --port-strings invalido: " << str_s
                          << " (valores: raw|managed)\n";
                return 2;
            }
            const std::string &ty_s = result["port-types"].as<std::string>();
            if (!port::parse_type_style(ty_s, copts.port_options.types)) {
                std::cerr << "[port] --port-types invalido: " << ty_s
                          << " (valores: stdint|builtin)\n";
                return EXIT_FAILURE;
            }
            copts.port_options.freestanding = result.count("port-freestanding") > 0;
            copts.port_options.stdlib_port_c_dir =
                result["port-stdlib-dir"].as<std::string>();
            copts.port_options.arch_target =
                result["port-arch"].as<std::string>();
            copts.port_options.aggressive_opt =
                (result.count("port-no-aggressive") == 0);
            copts.port_options.module_name = mod_name;
            copts.port_options.source_path = vex_path;
        }

        vex::CompileResult cr =
            vex::compile_vex_source(vex_source, vex_path, copts);
        if (!cr.ok) {
            for (const auto &d : cr.diagnostics.all()) {
                vex::print_diagnostic(std::cerr, d);
            }
            return EXIT_FAILURE;
        }
        // Mostrar warnings (cr.ok no impide los warnings).
        for (const auto &d : cr.diagnostics.all()) {
            if (d.level != vex::DiagLevel::ERR) vex::print_diagnostic(std::cerr, d);
        }

        // si --vex-base fue especificado y es != 0, parchear el
        // texto .vel para reemplazar el @IniAddress(0x0000000000000000)
        // generado por defecto por el ir_emitter por @IniAddress(<base>).
        // Esto desplaza todo el code section a la VA solicitada, evitando
        // solapamiento con el caller cuando este modulo se carga via
        // loadmodule.  Es una solucion tactica; idealmente el ir_emitter
        // tomaria una opcion de base address directamente.
        //
        // ADEMAS: insertar `@InitPc(main)` antes del @Module(...) para que
        // el linker compute start_pc = absolute_addr_of_main = base +
        // offset(main) = base + 0 (main es siempre el primer label en
        // codigo Vex).  Sin esto, start_pc queda en 0 y loadmodule ejecuta
        // codigo del caller en vez del plugin.
        if (vex_base_addr != 0) {
            const std::string from = "@IniAddress(0x0000000000000000)";
            char buf[64];
            std::snprintf(buf, sizeof(buf), "@IniAddress(0x%016llX)",
                          static_cast<unsigned long long>(vex_base_addr));
            std::string to = buf;
            size_t pos = cr.vel_text.find(from);
            if (pos != std::string::npos) {
                cr.vel_text.replace(pos, from.size(), to);
                vesta::scout() << "[vex] @IniAddress patched -> " << to << "\n";
            } else {
                std::cerr << "[vex] aviso: no se encontro @IniAddress(0x0...) "
                             "para parchear con --vex-base\n";
            }
            // Insertar @InitPc(<base>) NUMERICO antes del @Module(...).  El
            // assembler procesa anotaciones single-pass y `main` no esta
            // definido todavia cuando @InitPc se evalua, asi que usamos el
            // valor absoluto (= base, ya que main es siempre el primer label
            // en codigo Vex y la seccion code tiene @Align(0x1000) que se
            // alinea con la base hex que pasa el usuario).
            const std::string mod_marker = "@Module(";
            size_t mod_pos = cr.vel_text.find(mod_marker);
            if (mod_pos != std::string::npos) {
                char ipbuf[64];
                std::snprintf(ipbuf, sizeof(ipbuf), "@InitPc(0x%llX)\n\n",
                              static_cast<unsigned long long>(vex_base_addr));
                cr.vel_text.insert(mod_pos, ipbuf);
                vesta::scout() << "[vex] @InitPc(0x" << std::hex << vex_base_addr
                               << std::dec << ") insertado (start_pc = base address)\n";
            } else {
                std::cerr << "[vex] aviso: no se encontro @Module(...) para insertar @InitPc\n";
            }
            // Convertir el `hlt` final del main del modulo en `ret` para que
            // sea LLAMABLE via callvm desde loadmod del caller.  Por defecto
            // main de Vex termina con `leave\nhlt` (convencion entry-point);
            // un plugin necesita main RET-able para que el push de return
            // address en loadmod resulte en flujo de vuelta al caller.
            // El standalone execution del plugin no funciona tras esta
            // conversion (RET pop'ea garbage del stack).  Aceptamos esta
            // limitacion: los plugins no se ejecutan standalone.
            const std::string main_ret_marker = "main_ret:\n    leave\n    hlt\n";
            const std::string main_ret_repl   = "main_ret:\n    leave\n    ret\n";
            size_t hlt_pos = cr.vel_text.find(main_ret_marker);
            if (hlt_pos != std::string::npos) {
                cr.vel_text.replace(hlt_pos, main_ret_marker.size(), main_ret_repl);
                vesta::scout() << "[vex] main_ret hlt -> ret (modo plugin: callable via loadmod)\n";
            } else {
                std::cerr << "[vex] aviso: no se encontro 'main_ret: leave hlt' para convertir a ret\n";
            }
        }

        // Diagramas: cada flag activo produce un archivo .mmd o .dot segun
        // el formato escogido.  Mermaid reconocido por VS Code / GitHub /
        // mermaid.live.  Graphviz se renderiza con `dot -Tsvg foo.dot -o foo.svg`.
        auto write_diagram = [&](const std::string &content,
                                  const std::string &suffix) -> bool {
            if (content.empty()) return true; // no se solicito; no es error
            std::string base = out_prefix.empty()
                                 ? copts.module_name
                                 : out_prefix;
            std::string path = base + suffix;
            std::ofstream ofs(path);
            if (!ofs.is_open()) {
                std::cerr << "[diagram] No se puede escribir: " << path << "\n";
                return false;
            }
            ofs << content;
            vesta::scout() << "[diagram] generado: " << path << "\n";
            return true;
        };
        if (emit_mermaid) {
            if (diag_vex     && !write_diagram(cr.mermaid_ast,      ".ast.mmd"))     return EXIT_FAILURE;
            if (diag_ir_pre  && !write_diagram(cr.mermaid_ir_pre,   ".ir.pre.mmd"))  return EXIT_FAILURE;
            if (diag_ir_post && !write_diagram(cr.mermaid_ir_post,  ".ir.post.mmd")) return EXIT_FAILURE;
            if (diag_vel     && !write_diagram(cr.mermaid_vel,      ".vel.mmd"))     return EXIT_FAILURE;
        }
        if (emit_graphviz) {
            if (diag_vex     && !write_diagram(cr.graphviz_ast,     ".ast.dot"))     return EXIT_FAILURE;
            if (diag_ir_pre  && !write_diagram(cr.graphviz_ir_pre,  ".ir.pre.dot"))  return EXIT_FAILURE;
            if (diag_ir_post && !write_diagram(cr.graphviz_ir_post, ".ir.post.dot")) return EXIT_FAILURE;
            if (diag_vel     && !write_diagram(cr.graphviz_vel,     ".vel.dot"))     return EXIT_FAILURE;
        }

        // Si --vex-emit-ir esta activo, escribir el dump del SSA IR
        // (pre y post optimizacion) en <out>.ir y salir.  Util para
        // debug del frontend sin tocar el .vel ni el linker.  No se
        // compila a .velb en este modo.
        if (emit_ir) {
            std::string ir_path = out_prefix.empty()
                                    ? (copts.module_name + ".ir")
                                    : (out_prefix + ".ir");
            std::ofstream ofs_ir(ir_path);
            if (!ofs_ir.is_open()) {
                std::cerr << "[vex] No se puede escribir: " << ir_path << "\n";
                return EXIT_FAILURE;
            }
            ofs_ir << cr.ir_text;
            vesta::scout() << "[vex] .ir generado: " << ir_path << "\n";
            return EXIT_SUCCESS;
        }

        // Si --port=<lang> esta activo, escribir el codigo fuente generado
        // y terminar (no compilar a .velb).  El usuario lo compila con su
        // toolchain nativa (gcc/clang/javac/node/etc).  La extension se
        // deriva del target.
        if (!copts.port_target.empty()) {
            std::string ext = ".c";  // default; futuros: .java, .js
            if (copts.port_target == "c")    ext = ".c";
            // (mas casos cuando agreguemos backends)
            std::string port_path = out_prefix.empty()
                                      ? (copts.module_name + ext)
                                      : (out_prefix + ext);
            std::ofstream ofs_port(port_path);
            if (!ofs_port.is_open()) {
                std::cerr << "[port] No se puede escribir: " << port_path << "\n";
                return EXIT_FAILURE;
            }
            ofs_port << cr.port_text;
            vesta::scout() << "[port] " << copts.port_target
                           << " generado: " << port_path
                           << " (" << cr.port_text.size() << " bytes)\n";
            for (const auto &w : cr.port_warnings) {
                std::cerr << "[port] warning: " << w << "\n";
            }
            return EXIT_SUCCESS;
        }

        // 5 Escribir el .vel intermedio.
        std::string vel_path = out_prefix.empty()
                                 ? (copts.module_name + ".vel")
                                 : (out_prefix + ".vel");
        {
            std::ofstream ofs(vel_path);
            if (!ofs.is_open()) {
                std::cerr << "[vex] No se puede escribir: " << vel_path << "\n";
                return EXIT_FAILURE;
            }
            // Si --vex-debug esta activo, prepend `// @file <vex_path>`
            // al .vel para que el lexer del .vel-to-.velb pase la info
            // al linker (Context::debug_source_file).
            if (copts.emit_debug) {
                ofs << "// @file " << vex_path << "\n";
            }
            ofs << cr.vel_text;
        }
        vesta::scout() << "[vex] .vel generado: " << vel_path << "\n";

        if (emit_only) return EXIT_SUCCESS;

        // Compilar .vel -> .velb saltando VPP
        // Opcion W: pasar el IR pre-serializado al linker via run_worker
        // para que se embeba en la seccion @c @ir del .velb v3.
        return asm_multi_process::run_worker(
            vel_path, out_prefix,
            /*skip_preprocessor=*/true,
            /*keep_labels=*/(result.count("keep-labels") > 0),
            /*ir_section_bytes=*/&cr.ir_section_bytes);
    }

    // Compilar un archivo .vel a .velb
    // vm.exe --build src/main.vel -o main.velb
    if (result.count("build")) {
        return asm_multi_process::run_worker(
            result["build"].as<std::string>(),
            out_prefix,
            /*skip_preprocessor=*/false,
            /*keep_labels=*/(result.count("keep-labels") > 0)
        );
    }

    // Abrir el REPL interactivo VestaShell (--interprete)
    if (result.count("interprete")) {
        vsh::VshInterpreter interp;
        // ARGV vacio o con [""]
        std::vector<std::string> empty_argv = { "" };
        interp.set_argv(empty_argv);
        interp.run_interactive();
        return EXIT_SUCCESS;
    }


    // Ejecutar un fichero VestaShell directamente sin abrir el REPL
    // vm.exe --script mi_script.vsh [args extra para el script...]
    if (result.count("script")) {
        const std::string &vsh_path = result["script"].as<std::string>();

        // Construir ARGV: [vsh_path, args_posicionales_extra...]
        std::vector<std::string> script_args;
        script_args.push_back(vsh_path);
        if (result.count("positional")) {
            for (const auto& a : result["positional"].as<std::vector<std::string>>()) {
                script_args.push_back(a);
            }
        }

        try {
            vsh::VshInterpreter interp;     // sin callback REPL
            interp.set_argv(script_args);   // <--- AQUI lo importante
            interp.exec_file(vsh_path);
        } catch (const vsh::VshRuntimeError &e) {
            std::cerr << "[script] Error en " << vsh_path;
            if (e.line > 0) std::cerr << ":" << e.line;
            std::cerr << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        } catch (const vsh::VshParseError &e) {
            std::cerr << "[script] Error de sintaxis en " << vsh_path;
            if (e.line > 0) std::cerr << ":" << e.line;
            std::cerr << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        } catch (const std::exception &e) {
            std::cerr << "[script] " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }


    // Ejecutar un archivo .velb en la VM
    // vm.exe --run program.velb
    if (result.count("run")) {
        const std::string &velb_path      = result["run"].as<std::string>();
        size_t             num_schedulers = result["schedulers"].as<size_t>();

        try {
            Timer t_total_run;
            Timer t_construct;
            runtime::ManageVM mgr(nullptr, 0);
            runtime::VM *     vm = mgr.loader.create_vm_instance(num_schedulers);
            if (!vm) {
                std::cerr << "Error: no se pudo crear la instancia de VM\n";
                return EXIT_FAILURE;
            }
            const long long ns_construct = t_construct.ns();

            // aplicar configuracion distribuida si el usuario paso algun flag --dist-*
            bool has_dist = result.count("dist-port")        > 0 ||
                            result.count("dist-discover")    > 0 ||
                            result.count("dist-add-node")    > 0 ||
                            result.count("dist-tls")         > 0 ||
                            result.count("dist-name")        > 0 ||
                            result.count("dist-token")       > 0 ||
                            result.count("dist-node-id")     > 0;
            if (has_dist) apply_dist_config(vm, result);

            Timer t_load;
            runtime::ProcessVM *proc = mgr.loader.load_executable(*vm, velb_path);
            if (!proc) {
                std::cerr << "Error: no se pudo cargar el ejecutable\n";
                return EXIT_FAILURE;
            }
            const long long ns_load = t_load.ns();

            // Argumentos del script Vex.  Convencion: el path del .velb es
            // el "argv[0]" implicito del programa; los positionals que el
            // usuario pasa tras `--run prog.velb` son args[0..N-1] desde el
            // punto de vista del programa Vex (mismo modelo que VSH).  Los
            // builtins Vex `args_count()` y `args_get(i)` los exponen via
            // los opcodes getargc/getarg.
            if (result.count("positional")) {
                vm->script_args =
                    result["positional"].as<std::vector<std::string>>();
            }

            vm->make_ready(proc->pid);

            // Servidor de depuracion opcional (--debug-port).  Se
            // instancia ANTES de vm.start() para que los clientes ya
            // puedan conectarse antes de que el primer proceso comience.
            // El Debugger es heap-allocado y se libera al exit del scope
            // (RAII via unique_ptr).  Pone has_hooks=true en cada
            // scheduler para activar el slow path con FSM completa que
            // invoca on_before_exec antes de cada instruccion.
            //
            // Sin --debug-port, el unique_ptr queda vacio y el coste
            // runtime es exactamente cero: ningun scheduler tiene
            // has_hooks activo y el fast path se ejecuta sin checks.
            std::unique_ptr<debug::Debugger> dbg;
            if (result.count("debug-port")) {
                uint16_t dbg_port = result["debug-port"].as<uint16_t>();
                if (dbg_port == 0) dbg_port = debug::DBG_DEFAULT_PORT;
                dbg = std::make_unique<debug::Debugger>(*vm);
                if (!dbg->start(dbg_port)) {
                    std::cerr << "Warning: no se pudo iniciar el servidor "
                                 "de depuracion en puerto " << dbg_port
                              << "; continuando sin debugger.\n";
                    dbg.reset();
                } else {
                    vm->debugger = dbg.get();
                    for (auto &sched : vm->schedulers) {
                        sched->has_hooks = true;
                    }
                    // Pausa el proceso main ANTES de su primera
                    // instruccion: sin esto, programas cortos podrian
                    // terminar antes de que el cliente conecte.  El
                    // usuario tipicamente conecta el cliente VSH, hace
                    // `attach 0`, pone breakpoints (si los necesita), y
                    // emite `continue 0` para arrancar la ejecucion.
                    dbg->pause_at_start(proc->pid.local_pid);
                    vesta::scout() << "[debugger] servidor TCP escuchando en "
                                   << "puerto " << dbg_port << "\n";
                    vesta::scout() << "[debugger] proceso main pausado al "
                                   << "inicio (pid=" << proc->pid.local_pid
                                   << "); conecta el cliente y emite "
                                   << "'continue " << proc->pid.local_pid
                                   << "' para arrancar.\n";
                }
            }

            Timer t_run;
            Timer t_start_phase;
            vm->start();
            const long long ns_start = t_start_phase.ns();

            Timer t_poll;
            // en lugar de polling con sleep_for(1ms)
            // (granularity ~15.6ms en Windows), bloquear con condition
            // variable.  El ultimo scheduler que ponga vm_running=false
            // notifica done_cv y main desbloquea inmediatamente.
            {
                std::unique_lock<std::mutex> lk(vm->done_mtx);
                vm->done_cv.wait(lk, [&] {
                    return !vm->vm_running.load(std::memory_order_acquire);
                });
            }
            const long long ns_poll = t_poll.ns();

            long long elapsed_ns = t_run.ns();

            Timer t_stop;
            vm->stop();
            const long long ns_stop = t_stop.ns();
            const long long ns_total_run = t_total_run.ns();

            if (result.count("stats")) {
                long long elapsed_ms = elapsed_ns / 1'000'000;
                long long elapsed_us = elapsed_ns / 1'000;

                uint64_t total_instrs   = 0;
                uint64_t active_time_ns = 0;
                for (const auto &sched: vm->schedulers) {
                    total_instrs += sched->profiler_instr_counter;
                    active_time_ns += sched->time_exec + sched->time_decode;
                }


                for (auto &sched: vm->schedulers) {
                    vesta::scout()
                            << "[Scheduler " << sched->id_scheduler
                            << "] Estados de procesos: " << sched->ready_queue.size()
                            << " "
                            << sched->processes.size()
                            << " "
                            << sched->is_waiting
                            << " "
                            << sched->should_kill // indica si la instancia debe morir.
                            << std::endl;
                    vesta::scout() << sched->to_string() << std::endl;
                    for (auto &p: sched->processes) {
                        vesta::scout()
                                << "\t[Process " << p->pid.local_pid
                                << "] Estados de procesos: " << runtime::vm_state_to_str(p->state)
                                << " "
                                << std::endl;
                        vesta::scout() << p->to_string() << std::endl;
                    }
                }

                // sin hooks time_decode/time_exec son 0; usar wall time como base
                uint64_t mips_base_ns = active_time_ns > 0 ? active_time_ns : (uint64_t) elapsed_ns;
                double   mips         = total_instrs > 0 && mips_base_ns > 0
                                  ? (total_instrs * 1000.0) / mips_base_ns
                                  : 0.0;

                vesta::scout() << "\n=== RUN STATS ===\n";
                vesta::scout() << "Wall time:     " << elapsed_ns << " ns  ("
                        << elapsed_us << " us, " << elapsed_ms << " ms)\n";
                if (active_time_ns > 0) {
                    vesta::scout() << "Tiempo activo: " << active_time_ns << " ns  ("
                            << active_time_ns / 1000 << " us, "
                            << active_time_ns / 1'000'000 << " ms)\n";
                }
                vesta::scout() << "Instrucciones: " << total_instrs << "\n";
                vesta::scout() << "MIPS:          " << mips
                        << (active_time_ns > 0 ? "" : "  (wall time)") << "\n";
                vesta::scout() << "\n=== OVERHEAD BREAKDOWN ===\n";
                vesta::scout() << "VM construct:    " << ns_construct/1000 << " us\n";
                vesta::scout() << "load_executable: " << ns_load/1000 << " us\n";
                vesta::scout() << "vm.start:        " << ns_start/1000 << " us  (lanzar threads)\n";
                vesta::scout() << "wait until done: " << ns_poll/1000 << " us  (cv wait)\n";
                vesta::scout() << "vm.stop:         " << ns_stop/1000 << " us  (join threads)\n";
                vesta::scout() << "Total --run:     " << ns_total_run/1000 << " us\n";
            }

            /* --jit-stats: imprimir contadores del JIT al final.
             * Independiente de --stats; util para auditar % de funciones
             * JIT-eatables en programas reales. */
            if (jit_stats_requested) {
                vesta::scout() << "\n=== JIT STATS ===\n";
                vesta::scout() << jit::get_jit_stats_summary() << "\n";
            }
        } catch (const std::exception &e) {
            std::cerr << "Error al ejecutar " << velb_path << ": " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    cli::Config cfg;
    cfg.history_file  = "my_vm_history.txt";
    cfg.history_max   = 1000;
    cfg.prompt        = "vesta> ";
    cfg.multiline_end = ";;";

    cli::VestaViewManager vm(cfg);

    vm.set_execute_callback([](const std::string &cmd) {
        auto fut = runtime::run_command_async(cmd);
        std::thread([f = std::move(fut)]() mutable {
            try {
                auto out = f.get();
                if (!out.empty()) vesta::scout() << out << std::endl;
            } catch (const std::exception &e) {
                std::cerr << "Runtime error: " << e.what() << std::endl;
            }
        }).detach();
    });

    vm.run();


    return EXIT_SUCCESS;
}
