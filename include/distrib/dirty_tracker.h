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
 * @file dirty_tracker.h
 * @brief Seguimiento de paginas modificadas para la instruccion memsync.
 *
 * DirtyTracker implementa el algoritmo de deteccion de cambios de 3 niveles:
 *
 *   1. Adler-32 (muy rapido, ~1 instruccion por byte): primer filtro.
 *      Si el Adler-32 no cambio, la pagina esta intacta con alta probabilidad.
 *   2. BLAKE2s-256 (rapido y criptograficamente seguro): filtro definitivo.
 *      Solo se computa si el Adler-32 cambio.  Evita falsos positivos del nivel 1.
 *   3. Transmision: solo se envian las paginas donde el BLAKE2s difiere del baseline.
 *
 * El baseline se actualiza tras cada memsync exitoso.
 * No se necesitan write barriers; el coste de deteccion es O(tamano_region) pero
 * amortizado: la mayoria de paginas pasan el filtro Adler-32 en nanosegundos.
 *
 * BLAKE2s-256 se computa via OpenSSL 3.x (EVP_blake2s256).
 */

#ifndef DIRTY_TRACKER_H
#define DIRTY_TRACKER_H

#include <cstdint>
#include <cstddef>
#include <vector>

namespace distrib {

static constexpr uint32_t DT_PAGE_SIZE = 4096; // tamano de pagina del tracker (igual que VM)
static constexpr uint32_t DT_BLAKE2S_LEN = 32; // bytes de salida de BLAKE2s-256

/**
 * @brief Entrada de baseline para una pagina de VM memory.
 *
 * Almacena los dos hashes usados como referencia despues del ultimo memsync.
 * Si adler32 == 0 y blake2s es todo ceros, la pagina no tiene baseline aun.
 */
struct PageBaseline {
    uint32_t adler32;           // hash Adler-32 del contenido en el ultimo memsync
    uint8_t  blake2s[DT_BLAKE2S_LEN]; // hash BLAKE2s-256 del contenido en el ultimo memsync
};

/**
 * @brief Pagina marcada como modificada durante una pasada de deteccion.
 */
struct DirtyPage {
    uint32_t page_idx;          // indice de pagina (0-based desde la dir base de la region)
    uint8_t  blake2s[DT_BLAKE2S_LEN]; // nuevo hash BLAKE2s-256 de esta pagina
};

/**
 * @brief Gestor de deteccion de paginas modificadas para una region de VM memory.
 *
 * Cada instancia cubre una region de memoria virtual de tamano fijo.
 * El Loader o la instruccion memsync crea instancias de DirtyTracker segun
 * las regiones que el usuario quiera sincronizar.
 *
 * Uso tipico:
 * @code
 *   DirtyTracker dt(base_addr, region_size);
 *   // ... el proceso modifica memoria ...
 *   std::vector<DirtyPage> dirty = dt.detect(vm_mem_ptr);
 *   // dirty contiene solo las paginas que cambiaron
 *   // enviar dirty al nodo remoto via VDP_MEMSYNC_HASH
 *   // recibir lista de paginas que difieren en remoto
 *   // enviar solo esas paginas via VDP_MEMSYNC_DATA
 *   dt.update_baseline(dirty);
 * @endcode
 */
class DirtyTracker {
public:
    /**
     * @brief Construye el tracker para la region indicada.
     *
     * Inicializa el vector de baselines a cero (sin baseline).
     * La region puede no estar alineada a PAGE_SIZE, pero la deteccion
     * opera en paginas completas; bytes sobrantes se rellenan con 0 al hashear.
     *
     * @param base_addr   Direccion virtual base de la region a monitorizar.
     * @param region_size Tamano de la region en bytes.
     */
    DirtyTracker(uint64_t base_addr, uint64_t region_size);

    /**
     * @brief Detecta las paginas que cambiaron desde el ultimo baseline.
     *
     * Lee los bytes de la region via el puntero de lectura @p mem_read,
     * computa Adler-32 por pagina, y si cambio computa BLAKE2s-256.
     * Retorna solo las paginas cuyo BLAKE2s difiere del baseline almacenado.
     *
     * @param mem_read  Funcion de lectura de VM memory: (vm_addr, out_buf, len).
     *                  Firma: void(uint64_t addr, uint8_t *buf, size_t len).
     * @return Vector de paginas modificadas con sus nuevos hashes BLAKE2s.
     */
    template<typename MemReadFn>
    std::vector<DirtyPage> detect(MemReadFn &&mem_read);

    /**
     * @brief Actualiza el baseline para las paginas indicadas.
     *
     * Debe llamarse tras confirmar que las paginas fueron entregadas al destino.
     * Las paginas no incluidas en @p pages mantienen su baseline anterior.
     *
     * @param pages Vector de paginas con los nuevos hashes a almacenar.
     */
    void update_baseline(const std::vector<DirtyPage> &pages);

    /**
     * @brief Invalida todo el baseline (todas las paginas se marcan como cambiadas).
     *
     * Util al inicio o tras un reinicio de sincronizacion.
     */
    void reset_baseline();

    /**
     * @brief Numero de paginas que cubre esta region.
     * @return Ceil(region_size / PAGE_SIZE).
     */
    size_t page_count() const { return baselines_.size(); }

    /**
     * @brief Direccion virtual base de la region.
     */
    uint64_t base_addr() const { return base_addr_; }

    /**
     * @brief Tamano de la region en bytes.
     */
    uint64_t region_size() const { return region_size_; }

    /**
     * @brief Computa el hash Adler-32 de un buffer.
     *
     * Adler-32 es extremadamente rapido (~1.5 bytes/ciclo en x86) y suficiente
     * como primer filtro antes de invocar BLAKE2s.
     *
     * @param buf Puntero al buffer.
     * @param len Longitud en bytes.
     * @return    Valor Adler-32 de 32 bits.
     */
    static uint32_t adler32(const uint8_t *buf, size_t len);

    /**
     * @brief Computa BLAKE2s-256 de un buffer via OpenSSL EVP.
     *
     * BLAKE2s-256 produce 32 bytes de salida.  Es rapido (~3 bytes/ciclo)
     * y criptograficamente seguro: probabilidad de colision de 1 en 2^128.
     *
     * @param buf  Puntero al buffer.
     * @param len  Longitud en bytes.
     * @param out  Buffer de salida de exactamente DT_BLAKE2S_LEN bytes.
     * @return     true si el calculo fue exitoso; false si OpenSSL fallo.
     */
    static bool blake2s_256(const uint8_t *buf, size_t len, uint8_t out[DT_BLAKE2S_LEN]);

private:
    uint64_t                  base_addr_;   // dir virtual base de la region
    uint64_t                  region_size_; // tamano de la region en bytes
    std::vector<PageBaseline> baselines_;   // una entrada por pagina

    // buffer temporal para leer una pagina (evita malloc en detect())
    static constexpr size_t PAGE_BUF_SIZE = DT_PAGE_SIZE;
    uint8_t page_buf_[PAGE_BUF_SIZE];
};

// ---------------------------------------------------------------------------
// Implementacion inline de detect() (template, debe estar en el header)
// ---------------------------------------------------------------------------
template<typename MemReadFn>
std::vector<DirtyPage> DirtyTracker::detect(MemReadFn &&mem_read) {
    std::vector<DirtyPage> dirty;

    const size_t n_pages = baselines_.size();
    for (size_t i = 0; i < n_pages; ++i) {
        uint64_t page_addr = base_addr_ + static_cast<uint64_t>(i) * DT_PAGE_SIZE;

        // leer la pagina completa en el buffer temporal
        uint64_t remaining = region_size_ - static_cast<uint64_t>(i) * DT_PAGE_SIZE;
        size_t   read_len  = (remaining >= DT_PAGE_SIZE) ? DT_PAGE_SIZE : static_cast<size_t>(remaining);

        mem_read(page_addr, page_buf_, read_len);
        if (read_len < DT_PAGE_SIZE) {
            // rellenar el resto con ceros para que el hash sea determinista
            __builtin_memset(page_buf_ + read_len, 0, DT_PAGE_SIZE - read_len);
        }

        // nivel 1: filtro rapido Adler-32
        uint32_t a32 = adler32(page_buf_, DT_PAGE_SIZE);
        if (a32 == baselines_[i].adler32) {
            continue; // Adler-32 coincide: muy probable que la pagina no cambio
        }

        // nivel 2: verificacion fuerte BLAKE2s-256
        DirtyPage dp;
        dp.page_idx = static_cast<uint32_t>(i);
        if (!blake2s_256(page_buf_, DT_PAGE_SIZE, dp.blake2s)) {
            continue; // error OpenSSL: omitir pagina (conservador)
        }

        // comparar con el baseline
        if (__builtin_memcmp(dp.blake2s, baselines_[i].blake2s, DT_BLAKE2S_LEN) == 0) {
            // BLAKE2s coincide: el Adler-32 tenia una colision (muy raro)
            baselines_[i].adler32 = a32; // actualizar Adler-32 para evitar recalcular BLAKE2s la proxima vez
            continue;
        }

        dirty.push_back(dp); // pagina realmente modificada
    }

    return dirty;
}

} // namespace distrib

#endif // DIRTY_TRACKER_H
