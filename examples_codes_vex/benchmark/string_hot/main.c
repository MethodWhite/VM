/* Bench: strings en hot loop (concat + length + compare).
 * En C clasico, strings son char[].  Usamos snprintf + strlen + strcmp. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int main(void) {
    const char *base = "abc";
    const char *suffix = "xyz";
    int64_t hits = 0;
    volatile int32_t bound = 200000;
    for (int32_t i = 0; i < bound; ++i) {
        char buf[16];
        /* Concat manual via snprintf. */
        snprintf(buf, sizeof(buf), "%s%s", base, suffix);
        size_t ln = strlen(buf);
        if (ln == 6) hits++;
        if (strcmp(buf, "abcxyz") == 0) hits++;
    }
    return (int32_t) hits;
}
