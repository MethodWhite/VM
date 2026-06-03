/* Bench: string workout AGRESIVO (C version).
 *
 * Diseno equivalente al .vex: 100K iter, 6 ops string por iter.
 * Usa memcpy + strcmp + strlen + manual FNV-1a hash + buffers manuales.
 *
 * `volatile bound` previene que el compilador unrole el loop.
 * `sum/hits` evita DCE.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* FNV-1a hash 64-bit -- equivalente al que usa el runtime de Vex. */
static uint64_t fnv1a(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; ++i) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

int main(void) {
    int64_t sum = 0;
    int64_t hits1 = 0;
    int64_t hits2 = 0;
    volatile int32_t bound = 1000000;

    for (int32_t i = 0; i < bound; ++i) {
        char bufA[16];
        bufA[0] = (char)(65 + (i & 7));
        bufA[1] = (char)(65 + ((i >> 3) & 7));
        bufA[2] = (char)(65 + ((i >> 6) & 7));
        bufA[3] = (char)(65 + ((i >> 9) & 7));
        bufA[4] = 0;

        char bufB[16];
        bufB[0] = (char)(48 + (i % 10));
        bufB[1] = (char)(48 + ((i / 10) % 10));
        bufB[2] = 0;

        /* OP1: concat A + B -> c (6 chars). */
        char c[16];
        memcpy(c, bufA, 4);
        memcpy(c + 4, bufB, 2);
        c[6] = 0;

        /* OP2: length. */
        size_t ln = strlen(c);
        sum += (int64_t)ln;

        /* OP3: equals against fixed pattern. */
        if (strcmp(c, "AAAA00") == 0) hits1++;

        /* OP5: substring (a[0:3]). */
        char sub[16];
        memcpy(sub, bufA, 3);
        sub[3] = 0;

        /* OP6: equals against varying pattern. */
        char bufQ[16];
        bufQ[0] = (char)(65 + ((i / 100) & 7));
        bufQ[1] = (char)(65 + ((i / 800) & 7));
        bufQ[2] = (char)(65 + ((i / 6400) & 7));
        bufQ[3] = 0;
        if (strcmp(sub, bufQ) == 0) hits2++;

        /* Final: concat sub + B = 5 char string, suma length. */
        char c2[16];
        memcpy(c2, sub, 3);
        memcpy(c2 + 3, bufB, 2);
        c2[5] = 0;
        sum += (int64_t)strlen(c2);
    }

    int64_t r = sum + hits1 * 1000 + hits2 * 7;
    return (int32_t)(r & 0x7FFFFFFF);
}
