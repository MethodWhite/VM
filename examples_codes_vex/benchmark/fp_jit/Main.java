// Bench: aritmetica f64 intensiva. 5M iter x 6 ops FP.
public class Main {
    public static void main(String[] args) {
        double acc = 0.0;
        for (int i = 0; i < 5_000_000; ++i) {
            double x = (double) i;
            double s = Math.sqrt(x);
            double a = Math.abs(s - 1000.0);
            double m = Math.min(a, 999.0);
            double M = Math.max(s, 1.0);
            double f = Math.floor(M);
            double c = Math.ceil(f);
            acc += m + c;
        }
        System.exit((int) ((long) acc & 0xFF));
    }
}
