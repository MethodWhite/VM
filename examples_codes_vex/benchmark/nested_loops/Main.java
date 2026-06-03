// Bench: nested loops (matrix-like).  500 x 500 x 200 = 50M ops.
public class Main {
    public static void main(String[] args) {
        long sum = 0L;
        for (int i = 0; i < 500; ++i)
            for (int j = 0; j < 500; ++j)
                for (int k = 0; k < 200; ++k)
                    sum += (long)(i + j + k);
        System.exit((int)(sum & 0xFFL));
    }
}
