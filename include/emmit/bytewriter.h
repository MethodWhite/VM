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
 * @file bytewriter.h
 * @brief Escritor secuencial de bytes con auto-incremento del offset.
 *
 * ByteWriter es el complemento de ByteReader: acumula bytes en un vector de salida
 * a medida que el emisor genera codigo o datos, manteniendo el offset actualizado.
 *
 * Todas las emisiones multi-byte usan orden little-endian para ser coherentes
 * con el formato .velb y la arquitectura de la VM.
 */

#ifndef BYTEWRITER_H
#define BYTEWRITER_H

#include <vector>
#include <cstdint>
#include <stdexcept>

/**
 * @brief Escritor seguro de bytes con auto-incremento del offset.
 *
 * Acumula bytes emitidos en @p output.  El campo @p offset refleja siempre
 * cuantos bytes han sido emitidos hasta el momento y puede usarse para calcular
 * direcciones relativas o parchear posiciones conocidas con write64_at().
 */
typedef struct ByteWriter {
    /**
     * @brief Buffer de bytes emitidos (codigo, datos, cabeceras, etc.).
     */
    std::vector<uint8_t> output;

    /**
     * @brief Numero de bytes emitidos hasta el momento (offset actual de escritura).
     */
    uint64_t offset = 0;

    /**
     * @brief Emite un byte (8 bits) y avanza el offset en 1.
     *
     * @param value Byte a emitir.
     */
    void emit8(uint8_t value) {
        output.push_back(value); // anadir byte al buffer
        offset += 1;
    }

    /**
     * @brief Emite un valor de 16 bits en little-endian y avanza el offset en 2.
     *
     * @param value Valor de 16 bits a emitir.
     */
    void emit16(uint16_t value) {
        emit_bytes(&value, sizeof(value)); // emitir 2 bytes little-endian
    }

    /**
     * @brief Emite un valor de 32 bits en little-endian y avanza el offset en 4.
     *
     * @param value Valor de 32 bits a emitir.
     */
    void emit32(uint32_t value) {
        emit_bytes(&value, sizeof(value)); // emitir 4 bytes little-endian
    }

    /**
     * @brief Emite un valor de 64 bits en little-endian y avanza el offset en 8.
     *
     * @param value Valor de 64 bits a emitir.
     */
    void emit64(uint64_t value) {
        emit_bytes(&value, sizeof(value)); // emitir 8 bytes little-endian
    }

    /**
     * @brief Emite un bloque arbitrario de @p size bytes desde @p data.
     *
     * Copia los bytes del bloque al final del buffer de salida y avanza el offset.
     *
     * @param data Puntero al bloque de bytes a emitir.
     * @param size Numero de bytes a emitir.
     */
    void emit_bytes(const void *data, size_t size) {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
        output.insert(output.end(), p, p + size); // copiar bytes al buffer
        offset += size;
    }

    /**
     * @brief Emite los 5 bytes bajos (40 bits) de @p value en little-endian.
     *
     * Util para emitir campos de 40 bits del formato .velb sin emitir los 3 bytes altos.
     *
     * @param value Valor cuyos 40 bits bajos se emiten (bytes 0-4).
     */
    void emit40(uint64_t value) {
        emit8((value >> 0)  & 0xFF); // byte 0 (bits 7:0)
        emit8((value >> 8)  & 0xFF); // byte 1 (bits 15:8)
        emit8((value >> 16) & 0xFF); // byte 2 (bits 23:16)
        emit8((value >> 24) & 0xFF); // byte 3 (bits 31:24)
        emit8((value >> 32) & 0xFF); // byte 4 (bits 39:32)
    }

    /**
     * @brief Parchea un valor de 64 bits en una posicion ya emitida del buffer.
     *
     * Permite rellenar huecos de 8 bytes que se dejaron a cero durante el primer
     * paso de emision y cuya direccion final se conoce en un paso posterior.
     *
     * @param value Nuevo valor de 64 bits a escribir en la posicion @p pos.
     * @param pos   Offset absoluto dentro de @p output donde escribir los 8 bytes.
     * @throws std::runtime_error si pos + 8 excede el tamano actual del buffer.
     */
    /**
     * @brief Parchea un byte en una posicion arbitraria del buffer ya emitido.
     *
     * Se usa para reescribir el byte de opcode2 cuando el emisor detecta que
     * el operando destino es un registro especial (rsp, rbp) y necesita cambiar
     * al opcode variante para registros especiales (p.ej. 0x09 -> 0x2E para subsp).
     *
     * @param value Nuevo valor de 8 bits.
     * @param pos   Offset absoluto dentro de @p output donde escribir el byte.
     * @throws std::runtime_error si pos excede el tamano actual del buffer.
     */
    void write8_at(uint8_t value, uint64_t pos) {
        if (pos >= output.size())
            throw std::runtime_error("write8_at fuera de rango");
        output[pos] = value; // parchear el byte en la posicion indicada
    }

    void write64_at(uint64_t value, uint64_t pos) {
        if (pos + 8 > output.size()) {
            throw std::runtime_error("write64_at fuera de rango");
        }
        // escribir los 8 bytes en little-endian en la posicion indicada
        output[pos + 0] = (uint8_t) (value & 0xFF);
        output[pos + 1] = (uint8_t) ((value >> 8)  & 0xFF);
        output[pos + 2] = (uint8_t) ((value >> 16) & 0xFF);
        output[pos + 3] = (uint8_t) ((value >> 24) & 0xFF);
        output[pos + 4] = (uint8_t) ((value >> 32) & 0xFF);
        output[pos + 5] = (uint8_t) ((value >> 40) & 0xFF);
        output[pos + 6] = (uint8_t) ((value >> 48) & 0xFF);
        output[pos + 7] = (uint8_t) ((value >> 56) & 0xFF);
    }

    /**
     * @brief Patch in-place de un uint32 little-endian en una posicion conocida.
     * Usado para emitir el size de la tabla de relocations en el header
     * tras conocer cuantas entries hay (write_executable patron pre-calculado).
     */
    void write32_at(uint32_t value, uint64_t pos) {
        if (pos + 4 > output.size()) {
            throw std::runtime_error("write32_at fuera de rango");
        }
        output[pos + 0] = (uint8_t) (value & 0xFF);
        output[pos + 1] = (uint8_t) ((value >> 8)  & 0xFF);
        output[pos + 2] = (uint8_t) ((value >> 16) & 0xFF);
        output[pos + 3] = (uint8_t) ((value >> 24) & 0xFF);
    }

} ByteWriter;

#endif // BYTEWRITER_H
