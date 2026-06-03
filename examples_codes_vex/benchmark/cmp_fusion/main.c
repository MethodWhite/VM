// cmp_fusion: 50M loop con cmp trivial.
static long run_impl(int n) {
    long acc = 0;
    int i = 0;
    while (i < n) { acc++; i++; }
    return acc;
}
int main(void) {
    long r = run_impl(50000000);
    return (int)(r & 0xFFFFFFFF);
}
