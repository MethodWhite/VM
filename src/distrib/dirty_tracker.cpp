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
 * @file dirty_tracker.cpp
 * @brief Implementacion del tracker de paginas modificadas con Adler-32 + BLAKE2s-256.
 *
 * BLAKE2s-256 se computa via la interfaz EVP de OpenSSL 3.x.
 * Adler-32 se computa de forma directa sin dependencias externas.
 */

#include "distrib/dirty_tracker.h"

#include <cstring>
#include <openssl/evp.h>

namespace distrib {

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

DirtyTracker::DirtyTracker(uint64_t base_addr, uint64_t region_size)
    : base_addr_(base_addr)
    , region_size_(region_size)
{
    // calcular numero de paginas necesarias (ceiling division)
    size_t n_pages = static_cast<size_t>(
        (region_size_ + DT_PAGE_SIZE - 1) / DT_PAGE_SIZE
    );
    baselines_.resize(n_pages);
    // inicializar todo a cero: adler32=0 y blake2s=0 indica "sin baseline"
    for (auto &b : baselines_) {
        b.adler32 = 0;
        std::memset(b.blake2s, 0, DT_BLAKE2S_LEN);
    }
    std::memset(page_buf_, 0, sizeof(page_buf_));
}

// ---------------------------------------------------------------------------
// Gestion del baseline
// ---------------------------------------------------------------------------

void DirtyTracker::update_baseline(const std::vector<DirtyPage> &pages) {
    for (const DirtyPage &dp : pages) {
        if (dp.page_idx < baselines_.size()) {
            std::memcpy(baselines_[dp.page_idx].blake2s, dp.blake2s, DT_BLAKE2S_LEN);
            // recalcular Adler-32 a partir del blake2s conocido no es posible;
            // marcamos adler32 = 1 para forzar recalculo en proxima deteccion
            baselines_[dp.page_idx].adler32 = 1;
        }
    }
}

void DirtyTracker::reset_baseline() {
    for (auto &b : baselines_) {
        b.adler32 = 0;
        std::memset(b.blake2s, 0, DT_BLAKE2S_LEN);
    }
}

// ---------------------------------------------------------------------------
// Algoritmo Adler-32
//
// Implementacion directa del RFC 1950.
// MOD_ADLER = 65521 es el mayor primo menor que 2^16.
// La suma A empieza en 1 (no en 0) segun la especificacion.
// ---------------------------------------------------------------------------

uint32_t DirtyTracker::adler32(const uint8_t *buf, size_t len) {
    static constexpr uint32_t MOD_ADLER = 65521;
    uint32_t a = 1; // suma de bytes (modulo MOD_ADLER)
    uint32_t b = 0; // suma de sumas (modulo MOD_ADLER)

    // procesar en bloques de 5552 bytes para evitar desbordamiento antes del modulo
    // (5552 es el mayor N tal que 255*N*(N+1)/2 < 2^32)
    static constexpr size_t NMAX = 5552;
    const uint8_t *ptr = buf;
    size_t remaining = len;

    while (remaining > 0) {
        size_t chunk = (remaining > NMAX) ? NMAX : remaining;
        remaining -= chunk;

        while (chunk--) {
            a += *ptr++;
            b += a;
        }

        a %= MOD_ADLER;
        b %= MOD_ADLER;
    }

    return (b << 16) | a; // empaquetado: bits 31-16 = B, bits 15-0 = A
}

// ---------------------------------------------------------------------------
// BLAKE2s-256 via OpenSSL EVP
// ---------------------------------------------------------------------------

bool DirtyTracker::blake2s_256(const uint8_t *buf, size_t len, uint8_t out[DT_BLAKE2S_LEN]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    const EVP_MD *md = EVP_blake2s256(); // disponible en OpenSSL >= 1.1.0
    if (!md) {
        EVP_MD_CTX_free(ctx);
        return false;
    }

    bool ok = true;
    unsigned int out_len = 0;

    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) ok = false;
    if (ok && EVP_DigestUpdate(ctx, buf, len) != 1) ok = false;
    if (ok && EVP_DigestFinal_ex(ctx, out, &out_len) != 1) ok = false;
    if (ok && out_len != DT_BLAKE2S_LEN) ok = false; // paranoia: verificar tamano

    EVP_MD_CTX_free(ctx);
    return ok;
}

} // namespace distrib
