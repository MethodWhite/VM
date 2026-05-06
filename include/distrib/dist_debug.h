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
 * @file dist_debug.h
 * @brief Flag global de depuracion del subsistema distribuido y macro DIST_DBG.
 *
 * Activa mensajes de traza en stderr para el servidor VDP cuando se pasa
 * --dist-debug en la linea de comandos.  Los mensajes cubren:
 *   - Recepcion y procesado de paquetes RSPAWN / RSPAWN_ACK / FUTURE_FULFILL
 *   - Creacion y destruccion de procesos remotos
 *   - Estado final del proceso (r0, err_thread, TSC)
 *   - Envio de FUTURE_FULFILL al nodo origen
 */
#ifndef DIST_DEBUG_H
#define DIST_DEBUG_H

#include <cstdio>

namespace distrib {

/**
 * @brief Habilita la salida de depuracion del subsistema distribuido.
 *
 * Establecer a true activa la macro DIST_DBG en dist_runtime.cpp y
 * scheduler.cpp. Valor por defecto: false.
 */
extern bool g_dist_debug;

/**
 * @brief Cambia el estado del flag de depuracion distribuida.
 * @param v true para activar; false para desactivar.
 */
inline void set_dist_debug(bool v) { g_dist_debug = v; }

} // namespace distrib

/**
 * @brief Imprime un mensaje de depuracion del subsistema distribuido en stderr.
 *
 * Solo tiene efecto cuando distrib::g_dist_debug == true.
 * El formato es: "[DIST] <mensaje>\n"
 */
#define DIST_DBG(fmt, ...) \
    do { \
        if (distrib::g_dist_debug) { \
            std::fprintf(stderr, "[DIST] " fmt "\n", ##__VA_ARGS__); \
            std::fflush(stderr); \
        } \
    } while (0)

#endif // DIST_DEBUG_H
