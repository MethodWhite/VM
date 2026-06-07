// obj_accum: objeto mutable de 4 campos; el C2 JIT hace escape analysis +
// scalar replacement del objeto no-escapante (equivalente a mem2reg de Vex).
public class Main {
    static class Stats { long sum=0, cnt=0, mn=2000000000L, mx=-2000000000L; }
    public static void main(String[] args) {
        Stats s = new Stats();
        long seed = 12345, i = 0;
        while (i < 20000000) {
            seed = (seed * 1103515245L + 12345) & 2147483647L;
            long v = seed % 1000;
            s.sum += v;
            s.cnt += 1;
            if (v < s.mn) s.mn = v;
            if (v > s.mx) s.mx = v;
            i++;
        }
        System.exit((int)((s.sum + s.cnt + s.mn + s.mx) % 256));
    }
}
