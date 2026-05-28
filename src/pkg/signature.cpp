/**
 * @file signature.cpp
 * @brief Implementacion de firmas Ed25519 + trust pins.
 */
#include "pkg/signature.h"
#include "pkg/paths.h"
#include "pkg/toml_lite.h"
#include "pkg/lockfile.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

namespace pkg::signing {

    namespace {
        std::string bin_to_hex(const std::vector<uint8_t> &v) {
            static const char d[] = "0123456789abcdef";
            std::string out(v.size() * 2, '0');
            for (size_t i = 0; i < v.size(); ++i) {
                out[2 * i]     = d[(v[i] >> 4) & 0xF];
                out[2 * i + 1] = d[v[i] & 0xF];
            }
            return out;
        }

        bool hex_to_bin(const std::string &hex, std::vector<uint8_t> &out) {
            if (hex.size() % 2 != 0) return false;
            out.clear();
            out.reserve(hex.size() / 2);
            for (size_t i = 0; i < hex.size(); i += 2) {
                auto digit = [](char c) -> int {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                    return -1;
                };
                int hi = digit(hex[i]);
                int lo = digit(hex[i + 1]);
                if (hi < 0 || lo < 0) return false;
                out.push_back(static_cast<uint8_t>((hi << 4) | lo));
            }
            return true;
        }
    } // namespace

    bool generate_keypair(KeyPair &out) {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        if (!ctx) return false;
        if (EVP_PKEY_keygen_init(ctx) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            return false;
        }
        EVP_PKEY *pkey = nullptr;
        if (EVP_PKEY_keygen(ctx, &pkey) <= 0) {
            EVP_PKEY_CTX_free(ctx);
            return false;
        }
        EVP_PKEY_CTX_free(ctx);

        size_t pub_len = 32;
        out.pub.resize(32);
        if (EVP_PKEY_get_raw_public_key(pkey, out.pub.data(), &pub_len) <= 0) {
            EVP_PKEY_free(pkey);
            return false;
        }
        size_t priv_len = 32;
        out.priv.resize(32);
        if (EVP_PKEY_get_raw_private_key(pkey, out.priv.data(), &priv_len) <= 0) {
            EVP_PKEY_free(pkey);
            return false;
        }
        EVP_PKEY_free(pkey);
        return true;
    }

    std::string fingerprint(const std::vector<uint8_t> &pub) {
        if (pub.size() != 32) return std::string();
        return "kpub1:" + bin_to_hex(pub);
    }

    bool decode_fingerprint(const std::string &fp, std::vector<uint8_t> &pub) {
        const std::string prefix = "kpub1:";
        if (fp.rfind(prefix, 0) != 0) return false;
        return hex_to_bin(fp.substr(prefix.size()), pub) && pub.size() == 32;
    }

    std::string sign_hex(const KeyPair &kp, const uint8_t *msg, size_t msg_len) {
        if (kp.priv.size() != 32) return std::string();
        EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, kp.priv.data(), kp.priv.size());
        if (!pkey) return std::string();

        EVP_MD_CTX *mctx = EVP_MD_CTX_new();
        if (!mctx) {
            EVP_PKEY_free(pkey);
            return std::string();
        }

        if (EVP_DigestSignInit(mctx, nullptr, nullptr, nullptr, pkey) <= 0) {
            EVP_MD_CTX_free(mctx);
            EVP_PKEY_free(pkey);
            return std::string();
        }

        // Ed25519 es single-shot: una sola llamada a DigestSign produce 64 bytes.
        size_t sig_len = 64;
        std::vector<uint8_t> sig(64);
        if (EVP_DigestSign(mctx, sig.data(), &sig_len, msg, msg_len) <= 0) {
            EVP_MD_CTX_free(mctx);
            EVP_PKEY_free(pkey);
            return std::string();
        }
        EVP_MD_CTX_free(mctx);
        EVP_PKEY_free(pkey);
        sig.resize(sig_len);
        return bin_to_hex(sig);
    }

    bool verify_hex(const std::vector<uint8_t> &pub,
                     const std::string &sig_hex,
                     const uint8_t *msg, size_t msg_len) {
        if (pub.size() != 32) return false;
        std::vector<uint8_t> sig;
        if (!hex_to_bin(sig_hex, sig)) return false;
        if (sig.size() != 64) return false;

        EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(
            EVP_PKEY_ED25519, nullptr, pub.data(), pub.size());
        if (!pkey) return false;

        EVP_MD_CTX *mctx = EVP_MD_CTX_new();
        if (!mctx) {
            EVP_PKEY_free(pkey);
            return false;
        }

        if (EVP_DigestVerifyInit(mctx, nullptr, nullptr, nullptr, pkey) <= 0) {
            EVP_MD_CTX_free(mctx);
            EVP_PKEY_free(pkey);
            return false;
        }

        int rc = EVP_DigestVerify(mctx, sig.data(), sig.size(), msg, msg_len);
        EVP_MD_CTX_free(mctx);
        EVP_PKEY_free(pkey);
        return rc == 1;
    }

    bool save_private_key(const std::string &path, const KeyPair &kp) {
        if (kp.priv.size() != 32) return false;
        EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(
            EVP_PKEY_ED25519, nullptr, kp.priv.data(), kp.priv.size());
        if (!pkey) return false;

        BIO *bio = BIO_new_file(path.c_str(), "wb");
        if (!bio) {
            EVP_PKEY_free(pkey);
            return false;
        }
        int ok = PEM_write_bio_PrivateKey(bio, pkey, nullptr, nullptr, 0,
                                            nullptr, nullptr);
        BIO_free(bio);
        EVP_PKEY_free(pkey);

#ifndef _WIN32
        // En POSIX, restringir permisos.
        chmod(path.c_str(), 0600);
#endif
        return ok == 1;
    }

    bool load_private_key(const std::string &path, KeyPair &out) {
        BIO *bio = BIO_new_file(path.c_str(), "rb");
        if (!bio) return false;
        EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        if (!pkey) return false;

        size_t priv_len = 32;
        out.priv.resize(32);
        if (EVP_PKEY_get_raw_private_key(pkey, out.priv.data(), &priv_len) <= 0) {
            EVP_PKEY_free(pkey);
            return false;
        }
        size_t pub_len = 32;
        out.pub.resize(32);
        if (EVP_PKEY_get_raw_public_key(pkey, out.pub.data(), &pub_len) <= 0) {
            EVP_PKEY_free(pkey);
            return false;
        }
        EVP_PKEY_free(pkey);
        return true;
    }

    namespace {
        std::string default_trust_path() {
            std::string keys = paths::keys_dir();
            if (keys.empty()) return std::string();
            return paths::join(paths::vex_home(), "trust.toml");
        }

        std::string read_text_file(const std::string &path) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return std::string();
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }

        bool write_text_file(const std::string &path, const std::string &content) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            return out.good();
        }
    } // namespace

    std::vector<TrustPin> load_trust_pins(const std::string &override_path) {
        std::vector<TrustPin> pins;
        std::string path = override_path.empty() ? default_trust_path() : override_path;
        if (path.empty() || !paths::is_file(path)) return pins;

        std::string content = read_text_file(path);
        auto pr = toml::parse(content);
        if (!pr.ok) return pins;

        auto it = pr.root.find("pin");
        if (it == pr.root.end() || !it->second.is_array()) return pins;

        for (const auto &v : it->second.as_array()) {
            if (!v.is_table()) continue;
            const auto &t = v.as_table();
            TrustPin p;
            auto sv = [&](const std::string &k) -> std::string {
                auto i = t.find(k);
                if (i == t.end() || !i->second.is_string()) return std::string();
                return i->second.as_string();
            };
            auto bv = [&](const std::string &k, bool def) -> bool {
                auto i = t.find(k);
                if (i == t.end() || !i->second.is_bool()) return def;
                return i->second.as_bool();
            };
            p.fingerprint = sv("fingerprint");
            p.author_name = sv("author_name");
            p.pinned_at   = sv("pinned_at");
            p.revoked     = bv("revoked", false);
            if (!p.fingerprint.empty()) pins.push_back(p);
        }
        return pins;
    }

    namespace {
        std::string serialize_trust(const std::vector<TrustPin> &pins) {
            std::ostringstream out;
            out << "# trust.toml -- pins de autores confiables\n";
            out << "# Cada entrada autoriza paquetes firmados con la clave publica indicada.\n\n";
            for (const auto &p : pins) {
                out << "[[pin]]\n";
                out << "fingerprint = " << toml::escape_string(p.fingerprint) << "\n";
                if (!p.author_name.empty())
                    out << "author_name = " << toml::escape_string(p.author_name) << "\n";
                if (!p.pinned_at.empty())
                    out << "pinned_at   = " << toml::escape_string(p.pinned_at) << "\n";
                out << "revoked     = " << (p.revoked ? "true" : "false") << "\n\n";
            }
            return out.str();
        }
    } // namespace

    bool add_trust_pin(const TrustPin &pin, const std::string &override_path) {
        std::string path = override_path.empty() ? default_trust_path() : override_path;
        if (path.empty()) return false;

        auto pins = load_trust_pins(path);
        bool found = false;
        for (auto &p : pins) {
            if (p.fingerprint == pin.fingerprint) {
                p = pin;
                found = true;
                break;
            }
        }
        if (!found) pins.push_back(pin);

        paths::ensure_dir(paths::vex_home());
        return write_text_file(path, serialize_trust(pins));
    }

    bool revoke_trust_pin(const std::string &fp, const std::string &override_path) {
        std::string path = override_path.empty() ? default_trust_path() : override_path;
        if (path.empty()) return false;
        auto pins = load_trust_pins(path);
        bool changed = false;
        for (auto &p : pins) {
            if (p.fingerprint == fp) {
                p.revoked = true;
                changed = true;
            }
        }
        if (!changed) return false;
        return write_text_file(path, serialize_trust(pins));
    }

    bool is_trusted(const std::string &fp, const std::vector<TrustPin> &pins) {
        for (const auto &p : pins) {
            if (p.fingerprint == fp) return !p.revoked;
        }
        return false;
    }

} // namespace pkg::signing
