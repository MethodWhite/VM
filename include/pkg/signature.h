/**
 * @file signature.h
 * @brief Firmas Ed25519 sobre paquetes Vesta + sistema de pin de autores.
 *
 * Modelo:
 *   - Cada paquete publica un @c .vex.sig (Ed25519) junto a su contenido.
 *   - La firma se calcula sobre @c sha256_tree(package_dir) (no sobre la
 *     concatenacion de los archivos), asi cualquier modificacion de un solo
 *     byte invalida la firma.
 *   - El autor se identifica por @c kpub1:<64-hex>  ("kpub1" = key pub v1).
 *   - El usuario "pinea" autores confiables en @c $VEX_HOME/trust.toml.
 *     Paquetes firmados por autores no pinneados emiten warning y requieren
 *     @c --allow-unsigned o accept-on-first-use (TOFU).
 *
 * Sin CA central.  TOFU + pinning manual.  Inspirado en SSH known_hosts.
 */
#ifndef VESTAVM_PKG_SIGNATURE_H
#define VESTAVM_PKG_SIGNATURE_H

#include <string>
#include <vector>
#include <cstdint>

namespace pkg::signing {

    /**
     * @brief Par de claves Ed25519 (32 bytes priv + 32 bytes pub).
     */
    struct KeyPair {
        std::vector<uint8_t> priv;   // 32 bytes
        std::vector<uint8_t> pub;    // 32 bytes
    };

    /**
     * @brief Genera un par de claves Ed25519 fresh.
     */
    bool generate_keypair(KeyPair &out);

    /**
     * @brief Devuelve la fingerprint "kpub1:<64-hex>" de una clave publica.
     */
    std::string fingerprint(const std::vector<uint8_t> &pub);

    /**
     * @brief Firma un buffer (tipicamente el sha256 del tree del paquete).
     * @return Firma en formato hex (128 chars).
     */
    std::string sign_hex(const KeyPair &kp,
                          const uint8_t *msg, size_t msg_len);

    /**
     * @brief Verifica una firma hex contra un mensaje + clave publica.
     */
    bool verify_hex(const std::vector<uint8_t> &pub,
                     const std::string &sig_hex,
                     const uint8_t *msg, size_t msg_len);

    /**
     * @brief Lee una clave privada del usuario desde
     *        @c $VEX_HOME/keys/private/<id>.pem (formato OpenSSL Ed25519 PEM).
     */
    bool load_private_key(const std::string &path, KeyPair &out);

    /**
     * @brief Guarda una clave privada en PEM (chmod 600 en POSIX).
     */
    bool save_private_key(const std::string &path, const KeyPair &kp);

    /**
     * @brief Lee una clave publica desde un fingerprint @c "kpub1:<64-hex>".
     */
    bool decode_fingerprint(const std::string &fp, std::vector<uint8_t> &pub);

    /**
     * @brief Entrada del archivo de trust pins del usuario.
     */
    struct TrustPin {
        std::string fingerprint;     // kpub1:<hex>
        std::string author_name;     // alias humano
        std::string pinned_at;       // ISO-8601 timestamp
        bool        revoked = false; // claves comprometidas
    };

    /**
     * @brief Carga la lista de trust pins del usuario.
     *
     * Por defecto @c $VEX_HOME/trust.toml.
     */
    std::vector<TrustPin> load_trust_pins(const std::string &trust_file_path = "");

    /**
     * @brief Anyade un trust pin nuevo (preserva los existentes).
     */
    bool add_trust_pin(const TrustPin &pin,
                        const std::string &trust_file_path = "");

    /**
     * @brief Marca un fingerprint como revoked.
     */
    bool revoke_trust_pin(const std::string &fp,
                          const std::string &trust_file_path = "");

    /**
     * @brief True si @p fp esta pinneado y no esta revoked.
     */
    bool is_trusted(const std::string &fp,
                     const std::vector<TrustPin> &pins);

} // namespace pkg::signing

#endif // VESTAVM_PKG_SIGNATURE_H
