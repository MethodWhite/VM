/*
 * VestaVM - Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia VMProject
 */

/**
 * @file jit/jit_registry.cpp
 * @brief Registro global de funciones compiladas por el JIT.
 *
 * El registro mantiene una tabla de rangos de codigo nativo (cada entry
 * contiene @c [code_start, code_end) ademas de los stackmaps generados
 * por el selector durante la compilacion).  Es consultado por:
 *   - El GC durante un major collection: para cada frame JIT-eated en el
 *     stack del proceso, lookup_stackmap(rip) le dice que slots del frame
 *     contienen GcHandles y deben marcarse como roots.
 *   - El AV recovery handler (cuando llega Phase D.15): para mapear RIP a
 *     funcion y emitir mensajes diagnosticos del estilo
 *     "crash en my_method offset 0x42".
 *   - Tools de debugging y profiling para resolver direcciones a nombres.
 *
 * Por que un vector ordenado en lugar de @c std::map / @c unordered_map:
 *   - Las lookups por RIP son binary search O(log N) cache-friendly: los
 *     elementos viven en un buffer contiguo y el branch-predictor del
 *     CPU aprende patrones de acceso.
 *   - Los registros suceden ~1 vez por funcion JIT-eada (raro) y la
 *     insercion en posicion ordenada es O(N) por el shift, aceptable
 *     porque tipicamente hay <1000 funciones JIT en una corrida.
 *   - Un @c unordered_map serializaria los buckets de manera mas
 *     dispersa, perdiendo localidad espacial durante el GC scan.
 *
 * Concurrencia:
 *   - @c mutex_ protege todas las operaciones (insert, erase, lookup,
 *     size, clear).  Lookups son lecturas y podrian usar shared_lock,
 *     pero como el cuello de botella practico es la compilacion (que
 *     escribe), no merece la pena complicar.
 *   - Cualquier puntero devuelto por @c lookup queda valido SOLO mientras
 *     el caller mantenga la convencion de no llamar @c unregister sobre
 *     la misma funcion durante el procesamiento.
 */

#include "jit/jit_registry.h"

#include <algorithm>

namespace jit {

    /**
     * @brief Singleton del registro global.
     *
     * @c static local garantiza inicializacion thread-safe a la primera
     * llamada (C++11 magic statics) sin coste extra en llamadas
     * subsiguientes (compilador emite un check sobre un flag global).
     */
    JitRegistry &JitRegistry::instance() noexcept {
        static JitRegistry inst;
        return inst;
    }

    /**
     * @brief Registra una funcion compilada en el registry.
     *
     * Llamado por el JIT compiler justo despues de instalar los bytes en
     * el code cache y antes de devolver el puntero al codigo al caller.
     * Asi el GC ya ve la funcion antes de que pueda ejecutarse.
     *
     * @param code_start Direccion del primer byte del codigo (en el cache).
     * @param code_end   Direccion EXCLUSIVA del fin del codigo.
     * @param stackmaps  Mapa pc_offset -> slots GC vivos por safepoint.
     * @param frame_size Tamano del stack frame (para que el unwinder
     *                   sepa cuanto saltar al volver al frame anterior).
     * @param name       Nombre demangled para diagnosticos (opcional).
     */
    void JitRegistry::register_function(const uint8_t *code_start,
                                        const uint8_t *code_end,
                                        std::vector<Stackmap> stackmaps,
                                        uint32_t frame_size,
                                        const char *name) {
        // Defensas baratas: rechazar registros con rango invalido.  Un
        // @c code_end <= code_start indicaria bug en el caller (compilador
        // que termino sin codigo emitido o emit con tamano negativo).
        // Aborto silencioso: el lookup posterior simplemente fallara para
        // el RIP afectado, que es mas seguro que crash en hot path GC.
        if (!code_start || !code_end || code_start >= code_end) return;
        std::lock_guard<std::mutex> lk(mutex_);

        // Sort de stackmaps por @c pc_offset.  Requisito para que
        // @c lookup_stackmap pueda hacer binary search aguas abajo: el
        // selector los emite en orden de instruccion emitida pero quien
        // los pasa puede tener su propio orden de construccion.
        std::sort(stackmaps.begin(), stackmaps.end(),
                  [](const Stackmap &a, const Stackmap &b) {
                      return a.pc_offset < b.pc_offset;
                  });

        // Construir el record completo antes de insertar.  Reservar antes
        // del @c std::move evita una asignacion adicional.
        JitFunctionInfo info;
        info.code_start = code_start;
        info.code_end   = code_end;
        info.stackmaps  = std::move(stackmaps);
        info.frame_size = frame_size;
        // El campo @c name es @c std::string para evitar dangling: si el
        // caller pasa un literal C el copy es trivial; si pasa nullptr lo
        // tratamos como cadena vacia para no propagar el nullptr.
        info.name       = name ? name : "";

        // Insercion ordenada: @c upper_bound nos da el primer entry con
        // @c code_start > info.code_start (el "siguiente" en orden).
        // Insertamos justo antes de ese punto para mantener el invariante
        // "vector ordenado por code_start ascendente".  Coste O(N) por el
        // shift de elementos posteriores; aceptable por baja frecuencia
        // de registros (~1 por funcion JIT compilada).
        auto it = std::upper_bound(functions_.begin(), functions_.end(),
                                   info,
                                   [](const JitFunctionInfo &a, const JitFunctionInfo &b) {
                                       return a.code_start < b.code_start;
                                   });
        functions_.insert(it, std::move(info));
    }

    /**
     * @brief Elimina la entrada cuyo @c code_start coincida exactamente.
     *
     * Llamado cuando el JIT desaloja una funcion del cache (deopt o
     * shutdown).  Tras esto, cualquier puntero al @c JitFunctionInfo
     * que el caller tuviera queda dangling: NO usar despues de unregister.
     *
     * La busqueda es lineal por simplicidad (las desallocaciones son
     * mucho menos frecuentes que los lookups en el GC).  Si en el futuro
     * se vuelve un cuello, cambiar a binary search analogo al de @c lookup.
     */
    void JitRegistry::unregister_function(const uint8_t *code_start) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = std::find_if(functions_.begin(), functions_.end(),
                               [code_start](const JitFunctionInfo &f) {
                                   return f.code_start == code_start;
                               });
        // Tolera "no encontrado" en silencio: simplifica el cleanup en
        // codigo que no sabe si ya hizo unregister antes (idempotente).
        if (it != functions_.end()) {
            functions_.erase(it);
        }
    }

    /**
     * @brief Localiza la funcion que contiene la direccion @c rip.
     *
     * @param rip Direccion del stream de codigo nativo.  Tipicamente:
     *            - Return address en un frame del stack (post-call).
     *            - PC actual reportado por un handler de senal.
     *            - Direccion de un breakpoint en el debugger.
     * @return Puntero al @c JitFunctionInfo cuyo rango cubre @c rip, o
     *         @c nullptr si no esta en ninguna funcion registrada.
     *
     * Algoritmo:
     *   1. @c upper_bound nos da el primer entry con @c code_start > rip.
     *   2. Retroceder uno: ese es el candidato (su @c code_start <= rip).
     *   3. Verificar que @c rip < code_end (el candidato podria terminar
     *      antes de RIP si hay un gap entre funciones).
     */
    const JitFunctionInfo *JitRegistry::lookup(const uint8_t *rip) const {
        std::lock_guard<std::mutex> lk(mutex_);
        // Defensas baratas: nulls y tabla vacia -> no hay match posible.
        if (!rip || functions_.empty()) return nullptr;

        // Binary search: buscar el primer entry cuyo code_start > rip.
        // El candidato real (cuyo rango contiene rip) es el entry ANTERIOR
        // a ese (su code_start <= rip, condicion necesaria).
        auto it = std::upper_bound(functions_.begin(), functions_.end(),
                                   rip,
                                   [](const uint8_t *r, const JitFunctionInfo &f) {
                                       return r < f.code_start;
                                   });
        // Si @c rip es menor que cualquier @c code_start, no hay candidato
        // anterior: RIP esta antes de cualquier funcion registrada.
        if (it == functions_.begin()) return nullptr;
        --it;
        // Verificacion final: el candidato anterior podria terminar antes
        // que RIP (gap entre funciones consecutivas).  Sin este check
        // devolveriamos la funcion equivocada para RIPs en huecos.
        if (rip < it->code_start || rip >= it->code_end) return nullptr;
        return &(*it);
    }

    /**
     * @brief Localiza el stackmap aplicable a la direccion @c rip.
     *
     * @return Puntero al @c Stackmap cuyo @c pc_offset es el MAYOR tal que
     *         @c pc_offset <= (rip - code_start).  Si no hay safepoint
     *         antes de @c rip en la funcion, retorna @c nullptr.
     *
     * Por que el "mayor menor-o-igual": el RIP capturado por el handler
     * de senal o el unwinder apunta tipicamente a la instruccion
     * INMEDIATAMENTE DESPUES del call que dejo el frame en el stack.  El
     * stackmap del safepoint que precedio a ese call es el que describe
     * el frame estado cuando el control salio: usamos ese, no el siguiente
     * (que aun no se ha "alcanzado" semanticamente).
     *
     * Implementacion: @c upper_bound nos da el primer stackmap con
     * @c pc_offset > rip_offset.  Retrocedemos uno para obtener el mayor
     * cuyo @c pc_offset <= rip_offset.
     */
    const Stackmap *JitRegistry::lookup_stackmap(const uint8_t *rip) const {
        // Primero localizamos la funcion (esto YA toma el mutex).  Si no
        // hay funcion para este RIP, no puede haber stackmap.
        const JitFunctionInfo *info = lookup(rip);
        if (!info) return nullptr;

        // Convertir RIP a offset relativo al inicio de la funcion.  Asi
        // los stackmaps son independientes de donde se aloque el codigo
        // en el cache (relocable sin re-emit).
        const uint32_t pc_offset = static_cast<uint32_t>(rip - info->code_start);

        // upper_bound sobre la lista ordenada de stackmaps: primer entry
        // con @c pc_offset > nuestro offset.  El anterior (--it) es el
        // ultimo cuyo @c pc_offset <= offset, que es el que aplica.
        auto it = std::upper_bound(info->stackmaps.begin(), info->stackmaps.end(),
                                   pc_offset,
                                   [](uint32_t off, const Stackmap &s) {
                                       return off < s.pc_offset;
                                   });
        // Si @c it apunta al primer elemento, no hay stackmap anterior:
        // el RIP esta en el prologue antes del primer safepoint.  Devolver
        // nullptr es correcto: el GC no encontrara roots en este frame
        // (no hay valores GC vivos antes del primer safepoint).
        if (it == info->stackmaps.begin()) {
            return nullptr;
        }
        --it;
        return &(*it);
    }

    /**
     * @brief Numero de funciones actualmente registradas.
     *
     * Usado por el GC para skipear el scan completo cuando no hay codigo
     * JIT (programas en modo interprete puro).  La copia del valor sale
     * del lock antes del @c return implicito.
     */
    size_t JitRegistry::size() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return functions_.size();
    }

    /**
     * @brief Elimina TODAS las funciones (reset completo).
     *
     * Util para tests que necesitan empezar con un registry limpio entre
     * casos.  En produccion no se usa: las funciones se liberan
     * individualmente con @c unregister_function al ser desalojadas.
     */
    void JitRegistry::clear() {
        std::lock_guard<std::mutex> lk(mutex_);
        functions_.clear();
    }

} // namespace jit
