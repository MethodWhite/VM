// Bench: branches no predecibles (xorshift) + 4 branches.
public class Main {
    public static void main(String[] args) {
        long rng = 1L;
        long a = 0L, b = 0L, c = 0L, d = 0L;
        for (int i = 0; i < 10_000_000; ++i) {
            rng ^= rng << 13;
            rng ^= rng >>> 7;
            rng ^= rng << 17;
            if ((rng & 1L) != 0L) a++; else a--;
            if ((rng & 2L) != 0L) b++; else b--;
            if ((rng & 4L) != 0L) c++; else c--;
            if ((rng & 8L) != 0L) d++; else d--;
        }
        System.exit((int)((a + b + c + d) & 0xFFL));
    }
}
