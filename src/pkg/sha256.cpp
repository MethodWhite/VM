/**
 * @file sha256.cpp
 * @brief Implementacion de SHA-256 via OpenSSL EVP API.
 */
#include "pkg/sha256.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#include <openssl/evp.h>

namespace fs = std::filesystem;

namespace pkg::hash {

    namespace {
        /**
         * @brief Convierte un buffer binario al hex string en minusculas.
         */
        std::string bin_to_hex(const uint8_t *bin, size_t len) {
            static const char digits[] = "0123456789abcdef";
            std::string out(len * 2, '0');
            for (size_t i = 0; i < len; ++i) {
                out[2 * i]     = digits[(bin[i] >> 4) & 0xF];
                out[2 * i + 1] = digits[bin[i] & 0xF];
            }
            return out;
        }
    } // namespace

    std::string sha256_bytes(const uint8_t *data, size_t len) {
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) return std::string();
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
            EVP_MD_CTX_free(ctx);
            return std::string();
        }
        if (EVP_DigestUpdate(ctx, data, len) != 1) {
            EVP_MD_CTX_free(ctx);
            return std::string();
        }
        uint8_t out[EVP_MAX_MD_SIZE];
        unsigned int out_len = 0;
        if (EVP_DigestFinal_ex(ctx, out, &out_len) != 1) {
            EVP_MD_CTX_free(ctx);
            return std::string();
        }
        EVP_MD_CTX_free(ctx);
        return bin_to_hex(out, out_len);
    }

    std::string sha256_file(const std::string &path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::string();
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) return std::string();
        if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
            EVP_MD_CTX_free(ctx);
            return std::string();
        }
        // Streaming en chunks de 64 KiB.
        std::vector<char> buf(65536);
        while (in.good()) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            std::streamsize n = in.gcount();
            if (n <= 0) break;
            if (EVP_DigestUpdate(ctx, buf.data(), static_cast<size_t>(n)) != 1) {
                EVP_MD_CTX_free(ctx);
                return std::string();
            }
        }
        uint8_t out[EVP_MAX_MD_SIZE];
        unsigned int out_len = 0;
        EVP_DigestFinal_ex(ctx, out, &out_len);
        EVP_MD_CTX_free(ctx);
        return bin_to_hex(out, out_len);
    }

    std::string sha256_tree(const std::string &dir_path) {
        std::error_code ec;
        if (!fs::is_directory(dir_path, ec)) return std::string();

        // Recolectar archivos relativos al dir_path, ordenados.
        std::vector<std::pair<std::string, std::string>> files; // (rel, abs)
        fs::path base = fs::absolute(dir_path, ec);
        if (ec) return std::string();

        for (fs::recursive_directory_iterator it(base, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            fs::path abs = it->path();
            std::error_code rec;
            fs::path rel = fs::relative(abs, base, rec);
            if (rec) continue;
            // Normalizar separadores a '/' para hash estable cross-platform.
            std::string rstr = rel.generic_string();
            files.emplace_back(rstr, abs.string());
        }

        std::sort(files.begin(), files.end(),
                  [](const auto &a, const auto &b) { return a.first < b.first; });

        // Hash compuesto: por cada archivo emite relpath + "\n" + content_hash + "\n".
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        if (!ctx) return std::string();
        EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

        for (const auto &p : files) {
            std::string content_hash = sha256_file(p.second);
            if (content_hash.empty()) continue;
            EVP_DigestUpdate(ctx, p.first.data(), p.first.size());
            EVP_DigestUpdate(ctx, "\n", 1);
            EVP_DigestUpdate(ctx, content_hash.data(), content_hash.size());
            EVP_DigestUpdate(ctx, "\n", 1);
        }

        uint8_t out[EVP_MAX_MD_SIZE];
        unsigned int out_len = 0;
        EVP_DigestFinal_ex(ctx, out, &out_len);
        EVP_MD_CTX_free(ctx);
        return bin_to_hex(out, out_len);
    }

    bool hash_equal_ct(const std::string &a, const std::string &b) {
        if (a.size() != b.size()) return false;
        // Comparacion constante.
        uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            uint8_t ca = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(a[i])));
            uint8_t cb = static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(b[i])));
            diff |= (ca ^ cb);
        }
        return diff == 0;
    }

} // namespace pkg::hash
