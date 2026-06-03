// Bench: 5M alloc + free.  En Java no hay free; el GC libera cuando
// el objeto pierde toda referencia.  byte[96] x 5M presiona al GC
// generacional (young gen).
public class Main {
    private static void doIter(int i) {
        byte[] buf = new byte[96];
        buf[0]  = (byte) i;
        buf[95] = (byte) (i + 95);
        // buf out of scope -> garbage collected eventualmente.
    }
    public static void main(String[] args) {
        for (int i = 0; i < 5_000_000; ++i) doIter(i);
        System.exit(42);
    }
}
