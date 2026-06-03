// rotops_jit: 5M iter, rotl/rotr.
public class Main {
    static class RotOps {
        long runHot(int n) {
            long acc = 0; int i = 1;
            while (i < n) {
                long v = (long)i;
                acc += Long.rotateLeft(v, 7);
                acc += Long.rotateRight(v, 3);
                i++;
            }
            return acc;
        }
    }
    public static void main(String[] args) {
        long r = new RotOps().runHot(5000000);
        System.exit((int)(r & 0xFF));
    }
}
