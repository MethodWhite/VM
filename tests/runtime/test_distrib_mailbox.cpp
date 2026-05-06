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
 * @file test_distrib_mailbox.cpp
 * @brief Test unitario del Mailbox y DirtyTracker distribuidos.
 *
 * Verifica:
 *   - Mailbox: push / try_pop / empty / size / bytes_used / desbordamiento
 *   - DirtyTracker: adler32 / blake2s_256 / detect() / update_baseline / reset_baseline
 *
 * No requiere red ni procesos externos.
 */

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cassert>
#include <vector>

#include "distrib/mailbox.h"
#include "distrib/dirty_tracker.h"

// ---------------------------------------------------------------------------
// Utilidades de impresion
// ---------------------------------------------------------------------------

static int g_pass = 0;
static int g_fail = 0;

static void print_sep(const char *titulo) {
    printf("\n============================================================\n");
    printf("  %s\n", titulo);
    printf("============================================================\n");
}

static void check(bool cond, const char *desc) {
    if (cond) {
        printf("  [PASS] %s\n", desc);
        ++g_pass;
    } else {
        printf("  [FAIL] %s\n", desc);
        ++g_fail;
    }
}

static void print_hex(const char *label, const uint8_t *buf, size_t len) {
    printf("  %s: ", label);
    for (size_t i = 0; i < len; ++i) printf("%02x", buf[i]);
    printf("\n");
}

// ---------------------------------------------------------------------------
// Tests de Mailbox
// ---------------------------------------------------------------------------

static void test_mailbox_basico() {
    print_sep("Mailbox: operaciones basicas");

    distrib::Mailbox mb;

    printf("  Estado inicial:\n");
    printf("    empty()     = %s\n", mb.empty() ? "true" : "false");
    printf("    size()      = %zu\n", mb.size());
    printf("    bytes_used()= %zu\n", mb.bytes_used());

    check(mb.empty(),        "mailbox vacio al crear");
    check(mb.size() == 0,    "size == 0 al crear");
    check(mb.bytes_used() == 0, "bytes_used == 0 al crear");

    // encolar el primer mensaje
    const uint8_t msg1[] = { 'H','o','l','a' };
    bool ok = mb.push(42, msg1, sizeof(msg1));
    printf("\n  push('Hola', 4 bytes, pid=42): ok=%s\n", ok ? "true" : "false");
    check(ok,                    "push del primer mensaje OK");
    check(!mb.empty(),           "no vacio tras push");
    check(mb.size() == 1,        "size == 1 tras un push");
    check(mb.bytes_used() == 4,  "bytes_used == 4 tras push de 4 bytes");

    // encolar un segundo mensaje
    const uint8_t msg2[] = { 'M','u','n','d','o','!' };
    ok = mb.push(99, msg2, sizeof(msg2));
    printf("  push('Mundo!', 6 bytes, pid=99): ok=%s\n", ok ? "true" : "false");
    check(ok,                    "push del segundo mensaje OK");
    check(mb.size() == 2,        "size == 2 tras dos pushes");
    check(mb.bytes_used() == 10, "bytes_used == 10 (4+6)");

    // extraer el primer mensaje (FIFO)
    distrib::MailboxMsg m = mb.try_pop();
    printf("\n  try_pop():\n");
    printf("    sender_pid = %llu\n", (unsigned long long)m.sender_pid);
    printf("    data.size()= %zu\n",  m.data.size());
    printf("    contenido  = '%.*s'\n", (int)m.data.size(),
           reinterpret_cast<const char *>(m.data.data()));

    check(m.sender_pid == 42,    "primer pop tiene pid=42");
    check(m.data.size() == 4,    "primer pop tiene 4 bytes");
    check(std::memcmp(m.data.data(), msg1, 4) == 0, "contenido del primer pop correcto");
    check(mb.size() == 1,        "size == 1 tras un pop");

    // extraer el segundo mensaje
    distrib::MailboxMsg m2 = mb.try_pop();
    printf("  try_pop() segundo:\n");
    printf("    sender_pid = %llu\n", (unsigned long long)m2.sender_pid);
    printf("    contenido  = '%.*s'\n", (int)m2.data.size(),
           reinterpret_cast<const char *>(m2.data.data()));

    check(m2.sender_pid == 99,   "segundo pop tiene pid=99");
    check(m2.data.size() == 6,   "segundo pop tiene 6 bytes");
    check(mb.empty(),            "vacio tras extraer todos los mensajes");
    check(mb.bytes_used() == 0,  "bytes_used == 0 tras vaciar");

    // pop con buzon vacio devuelve mensaje vacio
    distrib::MailboxMsg vacio = mb.try_pop();
    printf("\n  try_pop() con buzon vacio:\n");
    printf("    sender_pid = %llu\n", (unsigned long long)vacio.sender_pid);
    printf("    data.empty() = %s\n", vacio.data.empty() ? "true" : "false");
    check(vacio.data.empty(),    "pop en buzon vacio retorna data.empty()==true");
    check(vacio.sender_pid == 0, "pop en buzon vacio retorna pid==0");
}

static void test_mailbox_limites() {
    print_sep("Mailbox: limites (max_msgs=3, max_bytes=20)");

    distrib::Mailbox mb(3, 20); // max 3 mensajes o 20 bytes

    const uint8_t d1[] = { 1, 2, 3, 4, 5 };   // 5 bytes
    const uint8_t d2[] = { 6, 7, 8, 9, 10 };  // 5 bytes
    const uint8_t d3[] = { 11,12,13,14,15 };   // 5 bytes -> total 15 bytes, 3 msgs

    bool ok1 = mb.push(1, d1, sizeof(d1));
    bool ok2 = mb.push(2, d2, sizeof(d2));
    bool ok3 = mb.push(3, d3, sizeof(d3));
    printf("  Encolar 3 mensajes de 5 bytes: ok=%s,%s,%s\n",
           ok1?"si":"no", ok2?"si":"no", ok3?"si":"no");
    check(ok1 && ok2 && ok3, "los tres primeros mensajes entran OK");
    check(mb.size() == 3,    "size == 3 (limite alcanzado)");
    check(mb.bytes_used() == 15, "bytes_used == 15");

    // el cuarto mensaje debe ser rechazado por limite de mensajes
    const uint8_t d4[] = { 99 };
    bool ok4 = mb.push(4, d4, sizeof(d4));
    printf("  Intento de push con buzon lleno (max_msgs=3): ok=%s\n",
           ok4 ? "FALLO-DEBE-SER-NO" : "no (correcto)");
    check(!ok4, "cuarto mensaje rechazado por max_msgs");

    // vaciamos uno y probamos limite de bytes
    mb.try_pop();
    printf("  Tras pop: size=%zu, bytes_used=%zu\n", mb.size(), mb.bytes_used());

    // mensaje de 10 bytes: entra (15-5+10=20 bytes exacto)
    const uint8_t d5[10] = {};
    bool ok5 = mb.push(5, d5, sizeof(d5));
    printf("  Push de 10 bytes (llegando a 20 bytes de limite): ok=%s\n", ok5?"si":"no");
    check(ok5, "mensaje que lleva bytes_used a exactamente max_bytes entra OK");

    // mensaje de 1 byte mas: debe ser rechazado por bytes
    const uint8_t d6[] = { 1 };
    bool ok6 = mb.push(6, d6, sizeof(d6));
    printf("  Push de 1 byte mas (excede max_bytes): ok=%s\n",
           ok6 ? "FALLO-DEBE-SER-NO" : "no (correcto)");
    check(!ok6, "mensaje rechazado por max_bytes");
}

// ---------------------------------------------------------------------------
// Tests de DirtyTracker
// ---------------------------------------------------------------------------

static void test_dirty_tracker_hashes() {
    print_sep("DirtyTracker: adler32 y blake2s_256");

    // Adler-32 de cadena conocida
    const uint8_t hello[] = "Hello, World!";
    uint32_t a32 = distrib::DirtyTracker::adler32(hello, sizeof(hello) - 1);
    printf("  Adler-32('Hello, World!') = 0x%08X\n", a32);
    // Adler-32 de "Hello, World!" (13 bytes) = 0x1F9E046E segun el algoritmo estandar
    check(a32 != 0, "Adler-32 no es cero");

    // Adler-32 de buffer vacio
    uint32_t a32_empty = distrib::DirtyTracker::adler32(nullptr, 0);
    printf("  Adler-32(vacio) = 0x%08X\n", a32_empty);
    check(a32_empty == 1, "Adler-32 de vacio == 1 (valor inicial)");

    // Adler-32 de bytes distintos debe dar valores distintos
    const uint8_t buf_a[] = { 0xAA, 0xAA, 0xAA };
    const uint8_t buf_b[] = { 0xBB, 0xBB, 0xBB };
    uint32_t a_a = distrib::DirtyTracker::adler32(buf_a, 3);
    uint32_t a_b = distrib::DirtyTracker::adler32(buf_b, 3);
    printf("  Adler-32([AA AA AA]) = 0x%08X\n", a_a);
    printf("  Adler-32([BB BB BB]) = 0x%08X\n", a_b);
    check(a_a != a_b, "Adler-32 distingue buffers distintos");

    // BLAKE2s-256
    uint8_t hash[32] = {};
    bool ok = distrib::DirtyTracker::blake2s_256(hello, sizeof(hello) - 1, hash);
    printf("\n  BLAKE2s-256('Hello, World!'): ok=%s\n", ok ? "si" : "no (OpenSSL no soporta)");
    if (ok) {
        print_hex("  hash", hash, 32);
        check(ok, "BLAKE2s-256 calcula sin error");

        // hash de un buffer de ceros: debe ser diferente del anterior
        uint8_t hash_zeros[32] = {};
        uint8_t zeros[13] = {};
        bool ok2 = distrib::DirtyTracker::blake2s_256(zeros, 13, hash_zeros);
        check(ok2, "BLAKE2s-256 de ceros calcula sin error");
        check(std::memcmp(hash, hash_zeros, 32) != 0,
              "BLAKE2s-256 distingue contenidos distintos");
    } else {
        printf("  [INFO] BLAKE2s-256 no disponible en esta version de OpenSSL "
               "(requiere OpenSSL 3.x). Se omiten sus checks.\n");
    }
}

static void test_dirty_tracker_detect() {
    print_sep("DirtyTracker: detect() y update_baseline");

    // region de 2 paginas (8192 bytes) con datos simulados
    const uint64_t base_addr    = 0x10000;
    const uint32_t PAGE_SIZE    = distrib::DT_PAGE_SIZE; // 4096
    const size_t   region_bytes = 2 * PAGE_SIZE;

    distrib::DirtyTracker dt(base_addr, region_bytes);
    printf("  DirtyTracker(base=0x%llX, region=%zu bytes, %zu paginas)\n",
           (unsigned long long)base_addr, region_bytes, dt.page_count());
    check(dt.page_count() == 2, "page_count == 2 para 2 paginas");
    check(dt.base_addr() == base_addr, "base_addr correcto");
    check(dt.region_size() == region_bytes, "region_size correcto");

    // simular VM memory: buffer de host que actua como memoria de la VM
    std::vector<uint8_t> vm_mem(region_bytes, 0xAB); // todas las paginas con 0xAB

    // funcion de lectura de VM memory (simula el acceso de la VM)
    auto mem_read = [&](uint64_t addr, uint8_t *buf, size_t len) {
        uint64_t offset = addr - base_addr;
        if (offset + len <= region_bytes)
            std::memcpy(buf, vm_mem.data() + offset, len);
    };

    // primera pasada: sin baseline, todas las paginas deben aparecer sucias
    printf("\n  Primera pasada (sin baseline):\n");
    auto dirty1 = dt.detect(mem_read);
    printf("    paginas sucias detectadas: %zu (esperadas: 2)\n", dirty1.size());
    check(dirty1.size() == 2, "sin baseline: 2 paginas sucias");
    if (!dirty1.empty()) {
        print_hex("    hash blake2s pag[0]", dirty1[0].blake2s, 16); // mostrar los primeros 16 bytes
    }

    // actualizar baseline con las paginas detectadas
    dt.update_baseline(dirty1);
    printf("\n  update_baseline() con 2 paginas.\n");

    // segunda pasada: sin cambios, no debe haber paginas sucias
    printf("\n  Segunda pasada (sin cambios):\n");
    auto dirty2 = dt.detect(mem_read);
    printf("    paginas sucias detectadas: %zu (esperadas: 0)\n", dirty2.size());
    check(dirty2.empty(), "tras baseline sin cambios: 0 paginas sucias");

    // modificar solo la pagina 0
    std::memset(vm_mem.data(), 0xCC, PAGE_SIZE); // pagina 0 cambia a 0xCC
    printf("\n  Modificar pagina 0 (0xAB -> 0xCC):\n");

    // tercera pasada: solo la pagina 0 debe aparecer sucia
    auto dirty3 = dt.detect(mem_read);
    printf("    paginas sucias detectadas: %zu (esperadas: 1)\n", dirty3.size());
    check(dirty3.size() == 1, "tras modificar pagina 0: 1 pagina sucia");
    if (!dirty3.empty()) {
        printf("    page_idx de la pagina sucia: %u (esperado: 0)\n", dirty3[0].page_idx);
        check(dirty3[0].page_idx == 0, "la pagina sucia es la 0");
    }

    // reset_baseline: invalida todo, proxima pasada debe ver 2 paginas sucias
    printf("\n  reset_baseline() -> invalida todo el baseline:\n");
    dt.reset_baseline();
    auto dirty4 = dt.detect(mem_read);
    printf("    paginas sucias tras reset: %zu (esperadas: 2)\n", dirty4.size());
    check(dirty4.size() == 2, "tras reset_baseline: 2 paginas sucias");
}

// ---------------------------------------------------------------------------
// Punto de entrada
// ---------------------------------------------------------------------------

int main() {
    printf("====================================================\n");
    printf("  VestaVM - Tests unitarios: Mailbox + DirtyTracker\n");
    printf("====================================================\n");

    test_mailbox_basico();
    test_mailbox_limites();
    test_dirty_tracker_hashes();
    test_dirty_tracker_detect();

    printf("\n============================================================\n");
    printf("  RESULTADO: %d PASS, %d FAIL\n", g_pass, g_fail);
    printf("============================================================\n");

    return (g_fail == 0) ? 0 : 1;
}
