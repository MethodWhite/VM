/**
 * @file sha256.h
 * @brief Wrapper SHA-256 sobre OpenSSL EVP para verificar paquetes.
 *
 * Calculo de hash en streaming (chunk-by-chunk) para no cargar archivos
 * grandes a memoria.  Devuelve el digest hex en minusculas (64 chars).
 */
#ifndef VESTAVM_PKG_SHA256_H
#define VESTAVM_PKG_SHA256_H

#include <string>
#include <cstdint>

namespace pkg::hash {

    /**
     * @brief Calcula SHA-256 de un buffer completo en memoria.
     * @return Hex string de 64 chars (lowercase) o cadena vacia en error.
     */
    std::string sha256_bytes(const uint8_t *data, size_t len);

    /**
     * @brief Calcula SHA-256 de un archivo via streaming.
     * @return Hex string de 64 chars o cadena vacia si no se puede abrir.
     */
    std::string sha256_file(const std::string &path);

    /**
     * @brief Calcula SHA-256 recursivo de un directorio (tree-hash).
     *
     * Algoritmo:
     *   - Listar archivos recursivamente, ordenados lexicograficamente
     *     por path relativo (asegura determinismo).
     *   - Por cada archivo emite: relpath + "\n" + content_sha256 + "\n"
     *     al hasher principal.
     *
     * Permite hashear el codigo fuente de un paquete sin comprimirlo.
     */
    std::string sha256_tree(const std::string &dir_path);

    /**
     * @brief Comparacion case-insensitive de dos hashes hex en tiempo
     *        constante (evita timing attacks).
     */
    bool hash_equal_ct(const std::string &a, const std::string &b);

} // namespace pkg::hash

#endif // VESTAVM_PKG_SHA256_H
