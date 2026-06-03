// Bench: strings en hot loop (concat + length + compare).
// C++ usa std::string que es el equivalente idiomatico al string de Vex.
#include <cstdint>
#include <string>

int main() {
    std::string base = "abc";
    std::string suffix = "xyz";
    int64_t hits = 0;
    volatile int32_t bound = 200000;
    for (int32_t i = 0; i < bound; ++i) {
        std::string combo = base + suffix;
        if (combo.size() == 6) hits++;
        if (combo == "abcxyz") hits++;
    }
    return static_cast<int32_t>(hits);
}
