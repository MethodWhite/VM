// obj_accum: idem main.c pero con un objeto C++ (struct con ctor).
#include <cstdint>
struct Stats {
    int64_t sum = 0, cnt = 0, mn = 2000000000, mx = -2000000000;
};
int main() {
    Stats s;
    int64_t seed = 12345, i = 0;
    while (i < 20000000) {
        seed = (seed * 1103515245 + 12345) & 2147483647;
        int64_t v = seed % 1000;
        s.sum += v;
        s.cnt += 1;
        if (v < s.mn) s.mn = v;
        if (v > s.mx) s.mx = v;
        i++;
    }
    return (int)((s.sum + s.cnt + s.mn + s.mx) % 1000000);
}
