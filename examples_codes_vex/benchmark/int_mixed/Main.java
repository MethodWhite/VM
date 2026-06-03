// Bench: aritmetica entera mixta intensiva (20M iter x 10 ops).
public class Main {
    public static void main(String[] args) {
        long a = 1L, b = 2L, c = 3L, d = 5L;
        for (int i = 0; i < 20_000_000; ++i) {
            a += (long) i;
            b -= 1L;
            c *= 3L;
            d += (a ^ b);
            a &= 0xFFFFFFFFL;
            b |= 1L;
            c >>= 1;
            d ^= a;
            a += b;
            c += d;
        }
        System.exit((int) ((a + b + c + d) & 0xFFL));
    }
}
