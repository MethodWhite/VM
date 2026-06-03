// Bench: copia secuencial byte-a-byte (1 MB x 100 iter = 100 MB).
public class Main {
    public static void main(String[] args) {
        final int N = 1048576;
        byte[] src = new byte[N];
        byte[] dst = new byte[N];
        for (int i = 0; i < N; ++i) {
            src[i] = (byte) (i & 0xFF);
        }
        for (int it = 0; it < 100; ++it) {
            // loop manual byte-a-byte (no System.arraycopy) para parity.
            for (int j = 0; j < N; ++j) {
                dst[j] = src[j];
            }
        }
        System.exit(dst[1234] & 0xFF);
    }
}
