// Bench: bitops (and/or/xor/shl/shr).
public class Main {
    public static void main(String[] args) {
        long a = 0xDEADBEEFCAFEBABEL;
        long b = 0x1234567890ABCDEFL;
        for (int i = 0; i < 30_000_000; ++i) {
            a ^= (long) i;
            a &= 0xFFFFFFFFFFFFL;
            a |= 0x1010101010101L;
            a <<= 1;
            a >>>= 1;
            b += (a & 0xFFFFL);
            b ^= (a >>> 16);
            b <<= 1;
        }
        System.exit((int)((a + b) & 0xFFL));
    }
}
