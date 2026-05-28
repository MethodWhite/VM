/**
 * @file velb_signature.h
 * @brief Phase M.L28 - firmas digitales del @c .velb.
 *
 * Footer @c VSIG (16 bytes header + variable-len signature) appended al
 * final del @c .velb tras un build con @c --sign.  El loader / herramientas
 * verifican via @c --verify-sig public_key.pem .
 *
 * Layout del footer (al final del archivo):
 * @verbatim
 * +N-...  signature_bytes[sig_size]
 * +N-12   u32  sig_size (longitud de signature en bytes)
 * +N-8    u32  algo (0=RSA-SHA256, 1=ECDSA-SHA256)
 * +N-4    u32  magic = 'VSIG' (0x47495356) -- ultimo campo del file
 * @endverbatim
 *
 * Convencion: el `magic` esta en los ULTIMOS 4 bytes del archivo.  Si
 * matchea, el archivo esta firmado.  El loader compara el hash del
 * body (todos los bytes ANTES del footer) contra la signature.
 */

#ifndef VEX_VELB_SIGNATURE_H
#define VEX_VELB_SIGNATURE_H

#include <cstdint>
#include <string>
#include <vector>

namespace vex {

/// Magic del footer (los ultimos 4 bytes del .velb firmado).
inline constexpr uint32_t VSIG_MAGIC = 0x47495356;  // 'VSIG' LE

/// Algoritmo de firma usado.
enum class VsigAlgo : uint32_t {
    RSA_SHA256   = 0,
    ECDSA_SHA256 = 1,
};

/// Resultado de @c velb_verify_signature.
struct VsigVerifyResult {
    bool        ok = false;
    bool        signed_ = false;     ///< @c true si el archivo trae footer.
    std::string error;
};

/// @brief Firma un buffer de @c .velb con la clave privada PEM.
///
/// @param velb_in   Bytes del @c .velb sin firmar.
/// @param privkey_path  Path a @c private_key.pem .
/// @param algo      Algoritmo (RSA-SHA256 default).
/// @param out_signed  Buffer destino con velb + footer VSIG appendado.
/// @return @c true si exito.  En error, @c err contiene el mensaje.
bool velb_sign(const std::vector<uint8_t> &velb_in,
                const std::string &privkey_path,
                VsigAlgo algo,
                std::vector<uint8_t> &out_signed,
                std::string &err);

/// @brief Verifica la firma de un @c .velb firmado.
///
/// @param velb_with_sig  Bytes del @c .velb tal como esta en disco.
/// @param pubkey_path    Path a @c public_key.pem .
/// @return VsigVerifyResult con @c ok=true si la firma matchea.
VsigVerifyResult velb_verify_signature(const std::vector<uint8_t> &velb_with_sig,
                                        const std::string &pubkey_path);

/// @brief Helper: detecta si un @c .velb trae footer VSIG (sin verificar).
/// @return @c true si los ultimos 4 bytes son el magic.
bool velb_has_signature(const std::vector<uint8_t> &velb) noexcept;

} // namespace vex

#endif // VEX_VELB_SIGNATURE_H
