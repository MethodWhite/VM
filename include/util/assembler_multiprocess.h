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
 * @file assembler_multiprocess.h
 * @brief Infraestructura para ensamblado paralelo usando multiproceso y multihilo.
 *
 * Implementa el modelo "driver + workers" de VestaVM:
 *
 *   - El **driver** ejecuta multiples hilos via ThreadPool.
 *   - Cada hilo lanza un **worker** como proceso independiente ("vm.exe --worker").
 *   - El driver captura la salida de cada worker, registra tiempos y errores,
 *     y finalmente invoca el linker global sobre todos los objetos generados.
 *
 * Ventajas del modelo:
 *   - Aislamiento total: un worker que falla no afecta al driver ni al resto.
 *   - Escalabilidad: miles de archivos pueden ensamblarse en paralelo.
 *   - Uso eficiente de todos los nucleos del sistema.
 *
 * Flujo tipico:
 * @code
 *   // Modo driver: compila todos los .vel del directorio con 8 hilos
 *   asm_multi_process::run_driver("src/", 8, "program.velb");
 *
 *   // Modo worker (invocado por el driver como subproceso)
 *   asm_multi_process::run_worker("src/main.vel", "build/main");
 * @endcode
 */

#ifndef ASSEMBLER_MULTIPROCESS_H
#define ASSEMBLER_MULTIPROCESS_H

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <iostream>

#include "json.hpp"
#include "emmit/parser_to_bytecode.h"
#include "cli/sync_io.h"
#include "util/ThreadPool.h"

namespace asm_multi_process {

    using namespace Assembly::Bytecode;
    using json = nlohmann::json;

    /**
     * @brief Nombre del archivo que esta procesando cada worker en este momento.
     *
     * El indice corresponde al ID de tarea en el ThreadPool.
     * Se usa exclusivamente para mostrar en la barra de progreso el ultimo archivo
     * que termino de ensamblar.
     */
    static std::vector<std::string> worker_current_file;

    /**
     * @brief Numero de workers que ya han terminado su tarea.
     *
     * Incrementado atomicamente por cada tarea del ThreadPool al finalizar.
     * La barra de progreso lo usa para calcular el porcentaje completado.
     */
    static std::atomic<int> completed_workers{0};

    /**
     * @brief Numero total de archivos a procesar en esta invocacion del driver.
     *
     * Se establece al inicio de run_driver() antes de lanzar las tareas.
     */
    static int total_workers = 0;

    /**
     * @brief Mutex que serializa las escrituras en la barra de progreso.
     *
     * Evita que multiples hilos mezclen su salida en la consola al actualizar
     * la barra de progreso simultaneamente.
     */
    static std::mutex progress_mutex;

    /**
     * @brief Indice del ultimo worker que termino su tarea.
     *
     * Permite mostrar en la barra de progreso el nombre del ultimo archivo
     * procesado sin recorrer todo el vector worker_current_file.
     */
    static std::atomic<int> last_finished{-1};

    /**
     * @brief Indica si el hilo de progreso debe seguir ejecutandose.
     *
     * Se pone a true antes de lanzar el hilo de progreso y a false
     * al terminar todas las tareas para detenerlo.
     */
    static std::atomic<bool> progress_running{false};

    /**
     * @brief Imprime la barra de progreso actual del ensamblado paralelo en stdout.
     *
     * Muestra:
     *   - Porcentaje completado (completed_workers / total_workers * 100).
     *   - Numero de archivos procesados sobre el total.
     *   - Nombre del ultimo archivo finalizado (via last_finished).
     *
     * Es thread-safe mediante progress_mutex.
     */
    void print_progress();

    /**
     * @brief Funcion del hilo dedicado a actualizar la barra de progreso.
     *
     * Se ejecuta mientras progress_running sea true.  Actualiza la barra
     * cada 100 milisegundos y la imprime una vez mas al terminar para que
     * quede en estado 100% al finalizar.
     */
    static void progress_thread_func() {
        while (progress_running.load()) {
            print_progress();
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // refrescar cada 100 ms
        }
        print_progress(); // impresion final al terminar
        std::cout << "\n";
    }

    /**
     * @brief Compila en paralelo todos los archivos .vel de un directorio.
     *
     * Actua como driver del modelo multiproceso:
     *   1. Busca todos los archivos .vel en @p folder.
     *   2. Crea un ThreadPool con @p threads hilos (0 = automatico).
     *   3. Cada hilo lanza un proceso externo "vm.exe --worker <archivo>".
     *   4. Captura la salida de cada worker y registra tiempos y errores.
     *   5. Si no hubo errores, invoca el linker global para generar @p output.
     *
     * @param folder  Directorio donde buscar archivos .vel a compilar.
     * @param threads Numero de hilos del pool; 0 usa hardware_concurrency().
     * @param output  Nombre del fichero binario final generado por el linker.
     * @return EXIT_SUCCESS si todo fue correcto; EXIT_FAILURE si hubo algun error.
     */
    int run_driver(const std::string &folder, int threads, const std::string &output);

    /**
     * @brief Ensambla un unico archivo fuente .vel en modo worker.
     *
     * Este modo es invocado por el driver como subproceso independiente.  Realiza
     * el ciclo completo de compilacion para un solo archivo:
     *   - Lectura del archivo fuente.
     *   - Preprocesado VPP (si VESTA_HAS_PREPROCESSOR y skip_preprocessor=false).
     *   - Analisis lexico (Lexer).
     *   - Analisis sintactico (Parser -> AST).
     *   - Emision de bytecode (Assembler -> .velb).
     *   - Enlazado local (Linker parcial).
     *   - Generacion de .velb y .velb-map.
     *
     * El parametro @p skip_preprocessor permite saltarse VPP cuando el
     * .vel de entrada ya viene de una fuente que lo aplico previamente
     * (por ejemplo, texto generado por el lowering Vex -> ir_emitter,
     * que ya esta totalmente expandido).  Pasar el .vel por VPP en ese
     * caso seria redundante y puede romper si el codigo generado contiene
     * tokens que VPP malinterprete.
     *
     * @param file_name           Ruta del archivo fuente .vel a compilar.
     * @param output_prefix       Prefijo de nombre para los archivos de salida.
     * @param skip_preprocessor   true para saltarse VPP (defecto false).
     * @return EXIT_SUCCESS si el ensamblado fue correcto; EXIT_FAILURE si hubo errores.
     */
    int run_worker(const std::string &file_name,
                   const std::string &output_prefix,
                   bool skip_preprocessor = false,
                   bool keep_labels       = false,
                   const std::vector<uint8_t> *ir_section_bytes = nullptr);

    /**
     * @brief Ejecuta un comando externo y captura toda su salida estandar.
     *
     * Usado por el driver para lanzar subprocesos worker y capturar su resultado.
     *
     * @param cmd Cadena con el comando completo a ejecutar (incluyendo argumentos).
     * @return Cadena con toda la salida estandar del proceso ejecutado.
     */
    std::string run_and_capture(const std::string &cmd);

} // namespace asm_multi_process

#endif // ASSEMBLER_MULTIPROCESS_H
