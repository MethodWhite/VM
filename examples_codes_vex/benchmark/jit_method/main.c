// jit_method: 30M (3 * 10M) sum-of-i en metodo.
static int run_impl(int n) {
    int sum = 0; int i = 0;
    while (i < n) { sum += i; i++; }
    return sum;
}
int main(void) {
    int a = run_impl(10000000);
    int b = run_impl(10000000);
    int c = run_impl(10000000);
    return a + b + c;
}
