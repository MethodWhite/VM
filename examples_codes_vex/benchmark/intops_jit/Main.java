// intops_jit: 5M iter.
public class Main {
    static class IntOps {
        long runHot(int n) {
            long acc = 0; int i = 1;
            while (i < n) {
                long a = (long)i;
                long b = (long)(i + 7);
                acc += Math.min(a, b);
                acc += Math.max(a, b);
                acc += Math.abs(a - 5000);
                i++;
            }
            return acc;
        }
    }
    public static void main(String[] args) {
        long r = new IntOps().runHot(5000000);
        System.exit((int)(r & 0xFF));
    }
}
