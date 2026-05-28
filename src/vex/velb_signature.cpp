/**
 * @file velb_signature.cpp
 * @brief Implementacion de firmas digitales (Phase M.L28).
 *
 * Usa OpenSSL EVP_DigestSign / EVP_DigestVerify con SHA-256.  RSA y
 * ECDSA soportados; el algoritmo se detecta del tipo de la clave.
 */

#include "vex/velb_signature.h"

#include <cstdio>
#include <cstring>
#include <fstream>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/err.h>

namespace vex {

namespace {

std::string openssl_last_error_() {
    BIO *bio = BIO_new(BIO_s_mem());
    ERR_print_errors(bio);
    char *buf = nullptr;
    const size_t len = BIO_get_mem_data(bio, &buf);
    std::string s(buf, len);
    BIO_free(bio);
    return s.empty() ? std::string("unknown OpenSSL error") : s;
}

bool write_u32_le_(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    return true;
}

uint32_t read_u32_le_at_(const std::vector<uint8_t> &b, size_t off) {
    if (off + 4 > b.size()) return 0;
    return static_cast<uint32_t>(b[off])
         | (static_cast<uint32_t>(b[off + 1]) << 8)
         | (static_cast<uint32_t>(b[off + 2]) << 16)
         | (static_cast<uint32_t>(b[off + 3]) << 24);
}

} // namespace

bool velb_has_signature(const std::vector<uint8_t> &velb) noexcept {
    if (velb.size() < 12) return false;
    return read_u32_le_at_(velb, velb.size() - 4) == VSIG_MAGIC;
}

bool velb_sign(const std::vector<uint8_t> &velb_in,
                const std::string &privkey_path,
                VsigAlgo algo,
                std::vector<uint8_t> &out_signed,
                std::string &err) {
    // 1. Cargar private key via BIO (evita issue OPENSSL_Applink en Windows
    // cuando se mezcla FILE* de MSVC OpenSSL con CRT de MinGW).
    std::ifstream pkf(privkey_path, std::ios::binary | std::ios::ate);
    if (!pkf) {
        err = "no se pudo abrir private key: " + privkey_path;
        return false;
    }
    const std::streamsize pksz = pkf.tellg();
    pkf.seekg(0, std::ios::beg);
    std::vector<char> pkbuf(static_cast<size_t>(pksz < 0 ? 0 : pksz));
    if (pksz > 0) pkf.read(pkbuf.data(), pksz);
    pkf.close();
    BIO *bio = BIO_new_mem_buf(pkbuf.data(), static_cast<int>(pkbuf.size()));
    if (!bio) {
        err = "BIO_new_mem_buf fallo";
        return false;
    }
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        err = "fallo parsing PEM private key: " + openssl_last_error_();
        return false;
    }

    // 2. EVP_DigestSign con SHA-256.
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        err = "EVP_MD_CTX_new fallo";
        return false;
    }
    bool ok = false;
    do {
        if (EVP_DigestSignInit(ctx, nullptr, EVP_sha256(),
                                nullptr, pkey) != 1) {
            err = "EVP_DigestSignInit: " + openssl_last_error_();
            break;
        }
        if (EVP_DigestSignUpdate(ctx, velb_in.data(), velb_in.size()) != 1) {
            err = "EVP_DigestSignUpdate: " + openssl_last_error_();
            break;
        }
        size_t sig_len = 0;
        if (EVP_DigestSignFinal(ctx, nullptr, &sig_len) != 1) {
            err = "EVP_DigestSignFinal (size): " + openssl_last_error_();
            break;
        }
        std::vector<uint8_t> sig(sig_len);
        if (EVP_DigestSignFinal(ctx, sig.data(), &sig_len) != 1) {
            err = "EVP_DigestSignFinal (data): " + openssl_last_error_();
            break;
        }
        sig.resize(sig_len);

        // 3. Construir output = velb_in + footer.
        out_signed = velb_in;
        out_signed.insert(out_signed.end(), sig.begin(), sig.end());
        write_u32_le_(out_signed, static_cast<uint32_t>(sig_len));
        write_u32_le_(out_signed, static_cast<uint32_t>(algo));
        write_u32_le_(out_signed, VSIG_MAGIC);
        ok = true;
    } while (false);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

VsigVerifyResult velb_verify_signature(const std::vector<uint8_t> &velb_with_sig,
                                        const std::string &pubkey_path) {
    VsigVerifyResult r;
    if (!velb_has_signature(velb_with_sig)) {
        r.signed_ = false;
        r.error   = "archivo no firmado (sin footer VSIG)";
        return r;
    }
    r.signed_ = true;

    // Parse footer: <sig> <sig_size> <algo> <magic>
    const size_t total = velb_with_sig.size();
    const uint32_t sig_size = read_u32_le_at_(velb_with_sig, total - 12);
    // const uint32_t algo  = read_u32_le_at_(velb_with_sig, total - 8); // futuro
    if (sig_size == 0 || sig_size > 4096) {
        r.error = "sig_size invalido (corrupcion del footer)";
        return r;
    }
    const size_t body_size = total - 12 - sig_size;
    if (body_size > total) {
        r.error = "footer VSIG mal formado";
        return r;
    }
    const uint8_t *body = velb_with_sig.data();
    const uint8_t *sig  = body + body_size;

    // Cargar public key via BIO (mismo motivo que en velb_sign).
    std::ifstream pkf(pubkey_path, std::ios::binary | std::ios::ate);
    if (!pkf) {
        r.error = "no se pudo abrir public key: " + pubkey_path;
        return r;
    }
    const std::streamsize pksz = pkf.tellg();
    pkf.seekg(0, std::ios::beg);
    std::vector<char> pkbuf(static_cast<size_t>(pksz < 0 ? 0 : pksz));
    if (pksz > 0) pkf.read(pkbuf.data(), pksz);
    pkf.close();
    BIO *bio = BIO_new_mem_buf(pkbuf.data(), static_cast<int>(pkbuf.size()));
    if (!bio) {
        r.error = "BIO_new_mem_buf fallo";
        return r;
    }
    EVP_PKEY *pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    if (!pkey) {
        r.error = "fallo parsing PEM public key: " + openssl_last_error_();
        return r;
    }

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        r.error = "EVP_MD_CTX_new fallo";
        return r;
    }
    do {
        if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(),
                                  nullptr, pkey) != 1) {
            r.error = "EVP_DigestVerifyInit: " + openssl_last_error_();
            break;
        }
        if (EVP_DigestVerifyUpdate(ctx, body, body_size) != 1) {
            r.error = "EVP_DigestVerifyUpdate: " + openssl_last_error_();
            break;
        }
        const int v = EVP_DigestVerifyFinal(ctx, sig, sig_size);
        if (v == 1) {
            r.ok = true;
        } else if (v == 0) {
            r.error = "firma INVALIDA (no matchea con pubkey)";
        } else {
            r.error = "EVP_DigestVerifyFinal error: " + openssl_last_error_();
        }
    } while (false);

    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);
    return r;
}

} // namespace vex
