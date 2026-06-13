/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/osr_data.h
 * @brief Estructuras de datos para On-Stack Replacement (OSR) Phase D.5.
 *
 * = Diseno =
 *
 * OSR permite al JIT reemplazar un bucle que esta siendo interpretado
 * por una version compilada JIT sin esperar a que la funcion termine.
 * El flujo es:
 *
 *   1. MONITOR: el interprete cuenta back-edges de cada bucle.  Cuando
 *      un bucle supera el umbral (OSR_THRESHOLD), se dispara la
 *      compilacion OSR.
 *
 *   2. CAPTURA: se guarda el estado actual del interprete (registros VM,
 *      pila, PC) en un OsrState en el punto de bucle (OsrPoint).
 *
 *   3. TRANSFER: el estado capturado se "remapea" al layout esperado
 *      por el codigo compilado (registros nativos, frame layout) y se
 *      salta al entry point OSR del codigo JIT.
 *
 *   4. EJECUCION: el bucle compilado se ejecuta en codigo nativo.
 *
 *   5. DEOPT: si el codigo compilado encuentra una condicion que no
 *      puede manejar (type miss, null check, array bounds), revierte
 *      al interprete reconstruyendo el estado desde el frame nativo.
 *
 * Las estructuras aqui definidas son compartidas entre el interprete,
 * el monitor OSR, el transfer y el deoptimizer.
 */

#ifndef VESTA_JIT_OSR_DATA_H
#define VESTA_JIT_OSR_DATA_H

#include <cstdint>
#include <string>
#include <vector>

#include "vesta_rt/abi.h"

namespace jit {

    // =====================================================================
    //  Constantes
    // =====================================================================

    /// Numero maximo de registros VM que pueden estar vivos en un OSR point.
    static constexpr uint32_t OSR_MAX_LIVE_REGS = 16;

    /// Threshold por defecto para disparar OSR (10,000 iteraciones).
    static constexpr uint32_t OSR_DEFAULT_THRESHOLD = 10000;

    /// ID invalido para loop / punto OSR.
    static constexpr uint32_t OSR_INVALID_ID = UINT32_MAX;

    /// Maximo numero de puntos OSR por funcion.
    static constexpr uint32_t OSR_MAX_POINTS_PER_FUNCTION = 64;

    // =====================================================================
    //  OsrPoint: ubicacion en bytecode donde OSR puede activarse
    // =====================================================================

    /**
     * @struct OsrPoint
     * @brief Describe una ubicacion en el bytecode donde OSR puede ocurrir.
     *
     * Tipicamente el loop header (la primera instruccion del bucle justo
     * despues del check de condicion).  El interprete marca estos puntos
     * mediante instrucciones LOOP especiales o mediante un contador de
     * back-edge asociado.
     */
    struct OsrPoint {
        uint64_t    bytecode_pc     = 0;  ///< PC absoluto en bytecode del loop header
        uint32_t    loop_id         = OSR_INVALID_ID;  ///< ID unico del bucle
        uint32_t    function_hash   = 0;  ///< hash de la funcion contenedora
        uint32_t    back_edge_count = 0;  ///< contador de back-edges

        /// IDs de los valores SSA vivos en este punto (para GC stackmaps).
        std::vector<uint32_t> live_vids;

        /// Offset dentro del codigo JIT compilado donde esta el OSR entry.
        /// 0 si aun no se ha compilado.
        uint64_t    compiled_entry_offset = 0;
    };

    // =====================================================================
    //  OsrState: estado capturado del interprete para OSR
    // =====================================================================

    /**
     * @struct OsrState
     * @brief Estado completo del interprete en un punto OSR.
     *
     * Se captura justo antes de saltar al codigo compilado.  Contiene
     * todos los registros VM, la pila parcial y el PC bytecode para
     * que el transfer pueda remapearlos al frame nativo.
     */
    struct OsrState {
        /// PC del bytecode donde se capturo (loop header).
        uint64_t bytecode_pc = 0;

        /// Valores de los 16 registros VM (R0..R15) en el momento de
        /// la captura.
        uint64_t regs[VESTA_PROC_REGISTER_COUNT] = {};

        /// Puntero de pila VM (stack_pointer) y base (base_pointer).
        uint64_t stack_pointer = 0;
        uint64_t base_pointer  = 0;

        /// Profundidad de la pila VM capturada (numero de slots desde
        /// stack_pointer hasta el tope real).
        uint32_t stack_depth = 0;

        /// Captura parcial de la pila: slots [stack_pointer, stack_pointer+stack_depth).
        /// Se usa para rematerializar valores que el codigo compilado espera
        /// en el stack y no en registros.
        std::vector<uint64_t> stack_slots;

        /// Loop ID al que pertenece este estado.
        uint32_t loop_id = OSR_INVALID_ID;

        /// Flags: bit 0 = hay excepcion pendiente, bit 1 = JIT active.
        uint32_t flags = 0;

        /// Si flags & 1, el handle de la excepcion pendiente.
        uint64_t pending_exception_handle = 0;
    };

    // =====================================================================
    //  OsrEntry: punto de entrada OSR en codigo compilado
    // =====================================================================

    /**
     * @struct OsrEntry
     * @brief Describe un punto de entrada OSR dentro del codigo JIT compilado.
     *
     * Cuando el monitor OSR decide transferir, consulta la tabla de
     * OsrEntry para encontrar la direccion de salto correspondiente
     * al loop header y funcion actual.
     */
    struct OsrEntry {
        /// Direccion absoluta del codigo compilado (OSR entry point).
        uint64_t compiled_address = 0;

        /// Offset desde el inicio del codigo compilado.
        uint32_t byte_offset = 0;

        /// Loop ID asociado a esta entrada.
        uint32_t loop_id = OSR_INVALID_ID;

        /// PC del bytecode del loop header.
        uint64_t bytecode_pc = 0;

        /// Nombre de la funcion contenedora (debugging).
        std::string function_name;

        /// Mapa de rematerializacion: para cada registro VM vivo,
        /// el indice en el osr_buffer (VESTA_OSR_BUFFER_N) donde
        /// el estado capturado se deposito.
        int16_t vid_to_buffer_slot[OSR_MAX_LIVE_REGS];

        /// Numero de entradas vivas en vid_to_buffer_slot.
        uint32_t live_count = 0;

        /// Tamano del frame nativo (para el deoptimizer).
        uint32_t native_frame_size = 0;

        /// Offset del stackmap GC para este punto OSR.
        uint32_t stackmap_offset = 0;

        /// True si el entry ha sido validado (el live-in del C2 esta
        /// cubierto por las capturas del C1).
        bool valid = false;
    };

    // =====================================================================
    //  OsrFrame: informacion para deoptimizacion
    // =====================================================================

    /**
     * @struct OsrFrame
     * @brief Informacion para reconstruir un frame interpretado desde
     *        un frame JIT (deoptimizacion inversa).
     *
     * Cuando el codigo compilado encuentra una condicion que requiere
     * volver al interprete (deopt), este struct permite reconstruir
     * el estado exacto del interprete que habia en el momento de la
     * transferencia OSR, mas las modificaciones hechas por el codigo
     * compilado hasta el punto de deopt.
     */
    struct OsrFrame {
        /// PC del bytecode al que debe continuar el interprete.
        uint64_t continuation_pc = 0;

        /// Valores de registros VM reconstruidos.
        uint64_t regs[VESTA_PROC_REGISTER_COUNT] = {};

        /// Stack pointer y base pointer reconstruidos.
        uint64_t stack_pointer = 0;
        uint64_t base_pointer  = 0;

        /// Slots de pila reconstruidos (desde stack_pointer).
        std::vector<uint64_t> stack_slots;

        /// Loop ID original (para posibles re-OSR tras deopt).
        uint32_t loop_id = OSR_INVALID_ID;

        /// Offset de retorno dentro del codigo compilado donde ocurrio el deopt.
        uint32_t deopt_pc_offset = 0;

        /// Tipo de deopt que disparo la reconstruccion.
        enum DeoptReason : uint8_t {
            DEOPT_UNKNOWN     = 0,
            DEOPT_TYPE_MISS   = 1,  ///< tipo esperado no coincide
            DEOPT_BOUNDS      = 2,  ///< array index out of bounds
            DEOPT_NULL_CHECK  = 3,  ///< null pointer check fallo
            DEOPT_CLASS_CHANGE= 4,  ///< clase del objeto cambio
            DEOPT_GC_REQUEST  = 5,  ///< GC solicito deopt para safety
            DEOPT_BAILOUT     = 6,  ///< codigo compilado no puede continuar
        };
        DeoptReason reason = DEOPT_UNKNOWN;

        /// Dato extra segun el motivo (e.g. clase esperada para type miss,
        /// indice para bounds, etc.).
        uint64_t reason_data = 0;

        /// Flag: true si este frame fue originalmente OSR-transferido
        /// (no un JIT normal).
        bool is_osr_frame = false;

        /// Offset en el osr_buffer original (para re-OSR).
        uint32_t osr_buffer_base = 0;
    };

    // =====================================================================
    //  Tabla OSR global (por proceso)
    // =====================================================================

    /**
     * @struct OsrTable
     * @brief Tabla global OSR accesible desde el runtime.
     *
     * Cada proceso VM tiene una referencia a esta tabla (via
     * proc->osr_state).  Permite al interprete y al codigo JIT
     * consultar/depositar estados OSR.
     */
    struct OsrTable {
        /// Puntos OSR registrados (loop headers).
        std::vector<OsrPoint> points;

        /// Entradas OSR compiladas.
        std::vector<OsrEntry> entries;

        /// Contador global de loops (para asignar IDs).
        uint32_t next_loop_id = 0;

        /// Nivel maximo de anidamiento de bucles soportado.
        uint32_t max_nesting = 8;

        /// True si OSR esta habilitado globalmente.
        bool enabled = true;

        /**
         * @brief Busca o crea un OsrPoint para un PC dado.
         * @return Indice en el vector points.
         */
        uint32_t find_or_create_point(uint64_t bytecode_pc,
                                      uint32_t func_hash);

        /**
         * @brief Busca una entrada OSR compilada por loop_id.
         * @return Puntero al OsrEntry o nullptr.
         */
        const OsrEntry *find_entry(uint32_t loop_id) const;

        /**
         * @brief Registra una nueva entrada OSR compilada.
         */
        void register_entry(const OsrEntry &entry);
    };

} // namespace jit

#endif // VESTA_JIT_OSR_DATA_H
