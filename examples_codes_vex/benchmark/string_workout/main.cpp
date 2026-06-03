/* Bench: string workout AGRESIVO (C++ version).
 *
 * Usa std::string + std::hash<string> idiomatic.  No tricks especiales
 * para forzar comportamiento "real C++ user".
 */
#include <cstdio>
#include <cstdint>
#include <string>
#include <functional>

int main() {
    int64_t sum = 0;
    int64_t hits1 = 0;
    int64_t hits2 = 0;
    volatile int32_t bound = 1000000;

    std::hash<std::string> hasher;
    const std::string pat1 = "AAAA00";

    for (int32_t i = 0; i < bound; ++i) {
        // Build A (4 chars from i bits).
        std::string a;
        a.reserve(4);
        a += (char)(65 + (i & 7));
        a += (char)(65 + ((i >> 3) & 7));
        a += (char)(65 + ((i >> 6) & 7));
        a += (char)(65 + ((i >> 9) & 7));

        // Build B (2 chars digits).
        std::string b;
        b.reserve(2);
        b += (char)(48 + (i % 10));
        b += (char)(48 + ((i / 10) % 10));

        // OP1: concat.
        std::string c = a + b;

        // OP2: length.
        sum += (int64_t)c.size();

        // OP3: equals against fixed.
        if (c == pat1) hits1++;

        // OP5: substring (a[0:3]).
        std::string sub = a.substr(0, 3);

        // OP6: equals against varying.
        std::string q;
        q.reserve(3);
        q += (char)(65 + ((i / 100) & 7));
        q += (char)(65 + ((i / 800) & 7));
        q += (char)(65 + ((i / 6400) & 7));
        if (sub == q) hits2++;

        // Final: concat sub + B.
        std::string c2 = sub + b;
        sum += (int64_t)c2.size();
    }

    int64_t r = sum + hits1 * 1000 + hits2 * 7;
    return (int32_t)(r & 0x7FFFFFFF);
}
