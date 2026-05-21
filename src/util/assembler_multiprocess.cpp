/**
 * @file assembler_multiprocess.cpp
 * @brief Infraestructura para ensamblado paralelo usando multiproceso + multihilo.
 *
 * Este modulo implementa el modelo "driver + workers" de VestaVM:
 *
 *  - El **driver** ejecuta multiples hilos (ThreadPool).
 *  - Cada hilo lanza un **worker** como proceso independiente (`vm.exe --worker`).
 *  - El driver captura la salida de cada worker, registra tiempos y errores,
 *    y finalmente ejecuta el linker global.
 *
 * Ventajas del modelo:
 *  - Aislamiento total entre workers (cada uno es un proceso separado).
 *  - Uso eficiente de todos los nucleos (multihilo en el driver).
 *  - Seguridad: un worker que falla no afecta al driver.
 *  - Escalabilidad: miles de archivos pueden ensamblarse en paralelo.
 *
 * @note Este header solo declara la interfaz; la implementacion esta en el .cpp.
 */
#include "util/assembler_multiprocess.h"
#include "profiler/timer.h"
#include "util/fs_utils.h"
#include "linker/velb_linker_bytecode.h"
#ifdef VESTA_HAS_PREPROCESSOR
    #include "preprocessor/preprocessor.h"
#endif


namespace asm_multi_process {
    int run_worker(const std::string &file_name,
                   const std::string &output_prefix,
                   bool skip_preprocessor,
                   bool keep_labels,
                   const std::vector<uint8_t> *ir_section_bytes,
                   bool emit_map) {
        //if (arch.empty()) {
        //    std::cerr << "--arch es requerido en modo --worker\n";
        //    return EXIT_FAILURE;
        //}

        Timer global;

        // Leer archivo fuente
        const std::string name_file(file_name);
        std::ifstream file(name_file);
        if (!file.is_open()) {
            std::cerr << "ERROR: No se pudo abrir: " << name_file << "\n";
            return 1;
        }

        std::string code((std::istreambuf_iterator<char>(file)),
                         std::istreambuf_iterator<char>());

        if (code.empty()) {
            std::cerr << "ERROR: Archivo vacio\n";
            return 1;
        }

#ifdef VESTA_HAS_PREPROCESSOR
        // Preprocesado: expandir macros, directivas #define/#if/#foreach/#import, etc.
        // Saltable si el caller lo pidio (p.ej. el .vel proviene del lowering
        // de Vex y ya ha sido preprocesado al nivel del .vex original).
        if (!skip_preprocessor) {
            vpp::Preprocessor pp;

            // configurar rutas de busqueda para #include y #import
            std::string source_dir =
                std::filesystem::path(file_name).parent_path().string();
            pp.options().include_paths.push_back(source_dir);

            // ruta de la libreria estandar de macros vpp (junto al ejecutable)
            std::string exe_dir =
                std::filesystem::path(fs::get_executable_path()).parent_path().string();
            pp.options().import_paths.push_back(exe_dir + "/preprocessor/include_lib");
            pp.options().import_paths.push_back(exe_dir + "/include_lib");
            pp.options().import_paths.push_back(source_dir);

            // macros de plataforma predefinidas para vesta/platform.vph
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

            std::string processed = pp.process(code, file_name);
            if (pp.diagnostics().has_errors()) {
                for (const auto& d : pp.diagnostics().diagnostics()) {
                    std::cerr << d.loc.file << ":" << d.loc.line << ": "
                              << (d.level == vpp::DiagLevel::ERR ? "error: " : "warning: ")
                              << d.message << "\n";
                }
                return 1;
            }
            code = std::move(processed);
        }
#endif

        // Lexer + Parser

        vm::Lexer lexer(code);
        vm::Parser parser(lexer);


        std::vector<std::unique_ptr<vm::ASTNode> > program;
        Timer t_parser;
        try {
            program = parser.parse();
        } catch (const vm::ParseError &e) {
            std::cerr << "Parse error: " << e.what() << "\n";
            return 1;
        }
        vesta::scout() << "[Tiempo parser] " << t_parser.us() << " us " << t_parser.ms() << " ms\n";

        // Resolver imports con orden de busqueda:
        //   1. Directorio del archivo fuente (relativo al .vel que se compila)
        //   2. Directorio de trabajo actual (CWD, compatibilidad con invocaciones legacy)
        //   3. Directorio del ejecutable (libreria estandar instalada junto al binario)
        std::unordered_set<std::string> imported_files;
        std::string source_base =
            std::filesystem::path(file_name).parent_path().string();
        std::vector<std::string> import_search_paths = {
            std::filesystem::current_path().string(),
            std::filesystem::path(fs::get_executable_path()).parent_path().string(),
        };
        Assembly::Bytecode::resolve_imports(
            program, imported_files, source_base, import_search_paths);

        // Ensamblar
        Assembler asmblr;
        // Propagar el path del archivo fuente Vex (capturado por el
        // lexer del marcador `// @file <path>` al inicio del .vel) al
        // Context para que el linker lo emita en la seccion debug del
        // .velb.  Si no hay marcador (compilacion sin --vex-debug),
        // queda vacio y el linker simplemente no emite la seccion.
        if (!lexer.last_src_file.empty()) {
            asmblr.ctx.debug_source_file = lexer.last_src_file;
        }
        Timer t_asm;
        std::vector<uint8_t> bytecode;
        try {
            bytecode = asmblr.assemble(program);
        } catch (const std::exception &e) {
            std::cerr << "Assembler error: " << e.what() << "\n";
            return EXIT_FAILURE;
        }
        vesta::scout() << "[Tiempo Assembler] " << t_asm.us() << " us " << t_asm.ms() << " ms\n";


        // LINKER
        Assembly::Bytecode::Linker::LinkerOptions opts;
        opts.optimize_bytecode = true;
        /* Map file es DEBUG-only y muy costoso de generar (~64% del linker_us
         * para programas reales).  Off por defecto; opt-in via flag CLI
         * --emit-map (propagado como `emit_map=true`). */
        opts.generate_map_file = emit_map;
        opts.strip_labels      = !keep_labels;  // por defecto strip
        opts.output_path = output_prefix + ".velb";
        opts.map_file_path = output_prefix + ".velb-map";
        opts.verbose = true;

        Timer t_linker;
        const bool prof_link = []() {
            const char *v = std::getenv("VESTA_LINKER_PROFILE");
            return v && v[0] == '1';
        }();

        Timer t_link_ctor;
        Linker::Linker linker(opts);
        if (prof_link) vesta::scout() << "[linker-prof-outer] ctor                  "
                                       << t_link_ctor.us() << " us\n";

        // Anadir el ensamblado crudo
        Timer t_link_addunit;
        linker.add_assembly_unit(bytecode, &asmblr.ctx);
        if (prof_link) vesta::scout() << "[linker-prof-outer] add_assembly_unit     "
                                       << t_link_addunit.us() << " us\n";

        // anadir objetos externos
        // linker.add_object_file("libmath.velo");
        // linker.add_static_library("stdlib.vela");

        // pasar IR section bytes pre-serializados al linker.
        // El frontend Vex los produjo via @c ir::emit_ir_section.
        // El linker los appendea a la seccion @c @ir del .velb v3.
        if (ir_section_bytes && !ir_section_bytes->empty()) {
            linker.set_ir_section_bytes(*ir_section_bytes);
        }

        // Construir ejecutable
        Timer t_link_write;
        linker.write_to_file(opts.output_path);
        if (prof_link) vesta::scout() << "[linker-prof-outer] write_to_file (build+disk) "
                                       << t_link_write.us() << " us\n";

        // Generar map file
        if (opts.generate_map_file) {
            Timer t_link_map;
            linker.write_map_file(opts.map_file_path);
            if (prof_link) vesta::scout() << "[linker-prof-outer] write_map_file        "
                                           << t_link_map.us() << " us\n";
        }

        // Reporte
        const auto &report = linker.get_report();
        vesta::scout() << "[Tiempo Linker] " << t_linker.us() << " us " << t_linker.ms() << " ms\n";

        vesta::scout() << "\n=== LINKER REPORT ===\n";
        vesta::scout() << "Modulos enlazados: " << report.modules_linked << "\n";
        vesta::scout() << "Simbolos resueltos: " << report.symbols_resolved << "\n";
        vesta::scout() << "Relocaciones aplicadas: " << report.relocations_applied << "\n";
        vesta::scout() << "Optimizaciones aplicadas: " << report.optimizations_applied << "\n";

        if (!report.errors.empty()) {
            vesta::scout() << "\n=== ERRORES ===\n";
            for (auto &e: report.errors) vesta::scout() << " - " << e << "\n";
        }

        if (!report.warnings.empty()) {
            vesta::scout() << "\n=== WARNINGS ===\n";
            for (auto &w: report.warnings) vesta::scout() << " - " << w << "\n";
        }

        vesta::scout() << std::dec;
        vesta::scout() << "\n[Tiempo total] " << global.us() << " us "
                << global.ms() << " ms\n";

        struct WorkerTimes {
            long parser_us;
            long assembler_us;
            long linker_us;
            long total_us;
        };
        json j;
        j["parser_us"] = t_parser.us();
        j["assembler_us"] = t_asm.us();
        j["linker_us"] = t_linker.us();
        j["total_us"] = global.us();
        std::cout << "\n__VESTA_TIMES__ " << j.dump() << "\n";

        return EXIT_SUCCESS;
    }

    std::string run_and_capture(const std::string &cmd) {
        std::string result;
        char buffer[256];

        // En Windows, _popen ejecuta "cmd.exe /C <cmd>".
        // cmd.exe /C aplica quote-stripping: si el primer y ultimo caracter de <cmd>
        // son comillas dobles, las elimina. Esto rompe rutas con espacios como
        // "C:\Program Files\vesta.exe" --worker "f.vel" -> C:\Program Files\... (mal).
        // La solucion es envolver el comando completo en un par extra de comillas para
        // que tras el stripping quede: "C:\Program Files\vesta.exe" --worker "f.vel".
#ifdef _WIN32
        std::string w32_cmd = "\"" + cmd + "\"";
        FILE *pipe = _popen(w32_cmd.c_str(), "r");
#else
        FILE *pipe = popen(cmd.c_str(), "r");
#endif
        if (!pipe) return "ERROR: no se pudo ejecutar el comando\n";

        while (fgets(buffer, sizeof(buffer), pipe)) {
            result += buffer;
        }

#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
        return result;
    }

    int run_driver(const std::string &folder, int threads, const std::string &output) {
        Timer global;
        // Buscar archivos .vel
        std::vector<std::string> files;
        for (auto &entry: std::filesystem::directory_iterator(folder)) {
            if (entry.path().extension() == ".vel") {
                files.push_back(entry.path().string());
            }
        }

        if (files.empty()) {
            std::cerr << "No hay archivos .vel en " << folder << "\n";
            return EXIT_FAILURE;
        }

        // Crear ThreadPool
        ThreadPool pool(threads);

        std::mutex results_m;
        std::vector<std::string> obj_files;
        bool error = false;

        total_workers = files.size();
        completed_workers = 0;
        worker_current_file.resize(total_workers);

        progress_running = true;
        std::thread progress_thread(progress_thread_func);

        struct WorkerLog {
            std::string file;
            std::string output;
            bool failed;
            long parser_us;
            long assembler_us;
            long linker_us;
            long total_us;
        };
        std::vector<WorkerLog> logs;

        // Encolar tareas
        for (int i = 0; i < files.size(); i++) {
            std::string f = files[i];
            int index = i; // i es el indice del archivo en el vector files
            pool.enqueue([&, index, f] {
                worker_current_file[index] = f; {
                    std::lock_guard lk(progress_mutex);
                }

                std::string out = f + ".obj";
                // comillas obligatorias: la ruta del ejecutable puede contener espacios
                // (p.ej. C:\Program Files\VestaVM\vesta.exe)
                std::string cmd = "\"" + fs::get_executable_path() + "\"";
                cmd += (" --worker \"" + f + "\" -o \"" + out + "\"");
                // redirigir stderr al stdout capturado para que los errores del worker
                // no se mezclen con la barra de progreso del driver en la consola
                cmd += " 2>&1";

                std::string output_comand = run_and_capture(cmd); {
                    std::lock_guard lk(results_m);

                    bool failed =
                            output_comand.find("ERROR") != std::string::npos ||
                            output_comand.find("Parse error") != std::string::npos ||
                            output_comand.find("terminate called") != std::string::npos ||
                            output_comand.find("No se pudo abrir") != std::string::npos;

                    size_t pos = output_comand.find("__VESTA_TIMES__");
                    long parser_us = 0, assembler_us = 0, linker_us = 0, total_us = 0;

                    if (pos != std::string::npos) {
                        std::string json_str = output_comand.substr(pos + 16);
                        try {
                            auto j = json::parse(json_str);
                            parser_us = j["parser_us"];
                            assembler_us = j["assembler_us"];
                            linker_us = j["linker_us"];
                            total_us = j["total_us"];
                        } catch (...) {
                            // si falla el parseo, no pasa nada
                        }
                    }

                    logs.push_back({
                        f,
                        output_comand,
                        failed,
                        parser_us,
                        assembler_us,
                        linker_us,
                        total_us,
                    });

                    if (!failed) {
                        obj_files.push_back(out);
                    } else {
                        error = true;
                    }
                }
                // Actualizar progreso
                last_finished = index;
                completed_workers++;
            });
        }

        pool.shutdown();
        progress_running = false;
        progress_thread.join();

        vesta::scout() << "\n\n===== LOGS DE COMPILACION =====\n";

        for (auto &log: logs) {
            vesta::scout() << "\n=== OUTPUT de " << log.file << " ===\n";
            vesta::scout() << log.output << "\n";

            if (log.failed) {
                vesta::scout() << "[FALLO]\n";
            } else {
                vesta::scout() << "[OK]\n";
            }
        }
        vesta::scout() << "\n===== TIEMPOS POR ARCHIVO =====\n";

        for (auto &s: logs) {
            vesta::scout() << "\n" << s.file << "\n";
            vesta::scout() << "  parser:    " << s.parser_us << " us\n";
            vesta::scout() << "  assembler: " << s.assembler_us << " us\n";
            vesta::scout() << "  linker:    " << s.linker_us << " us\n";
            vesta::scout() << "  total:     " << s.total_us << " us\n";
        }


        if (error) {
            std::cerr << "Fallo la compilacion paralela.\n";
            std::cout << "\n[Tiempo total driver] " << global.us() << " us "
                    << global.ms() << " ms\n";
            return EXIT_FAILURE;
        }

        // Invocar el linker directamente via C++ para evitar spawnar un subproceso
        // sin flag reconocido (que arrancaria el REPL y bloquearia indefinidamente).
        try {
            Assembly::Bytecode::Linker::Linker linker;
            for (auto &o : obj_files) linker.add_object_file(o);
            linker.build_executable();
            linker.write_to_file(output);
            vesta::scout() << output << "\n";
        } catch (const std::exception &e) {
            std::cerr << "ERROR: Linker: " << e.what() << "\n";
            std::cout << "\n[Tiempo total driver] " << global.us() << " us "
                      << global.ms() << " ms\n";
            return EXIT_FAILURE;
        }

        std::cout << "\n[Tiempo total driver] " << global.us() << " us "
                << global.ms() << " ms\n";
        return EXIT_SUCCESS;
    }

    void print_progress() {
        std::lock_guard lock(progress_mutex);

        int done = completed_workers.load();
        int total = total_workers;

        if (total == 0) return;

        float pct = (float) done / (float) total;
        int width = 40;
        int filled = (int) (pct * width);

        std::cout << "\r\033[1;32m["; // verde brillante

        for (int i = 0; i < filled; i++) std::cout << "#";
        std::cout << "\033[1;31m"; // rojo brillante
        for (int i = filled; i < width; i++) std::cout << "-";

        std::cout << "\033[0m] "; // reset

        std::cout << (int) (pct * 100) << "%  ("
                << done << "/" << total << ")  ";

        int idx = last_finished.load();
        std::string file = (idx >= 0 ? worker_current_file[idx] : "");
        // archivo actual
        std::cout << "\033[36m" << file << "\033[0m";

        std::cout << std::flush;
    }
}
