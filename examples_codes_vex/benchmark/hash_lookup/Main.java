// Bench: hash lookup simulado (FNV-style int ops).
public class Main {
    public static void main(String[] args) {
        long seed = 0xCAFEBABEDEADBEEFL;
        long acc = 0L;
        for (int i = 0; i < 50_000_000; ++i) {
            seed ^= (long) i;
            seed *= 1099511628211L;
            seed >>>= 7;          // unsigned shift right
            seed |= 1L;
            if ((seed & 7L) == 0L) {
                acc++;
            }
        }
        System.exit((int) (acc & 0xFF));
    }
}
