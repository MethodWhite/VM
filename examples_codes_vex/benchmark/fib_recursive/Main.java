// Bench: Fibonacci recursivo profundo. fib(32) = 2178309.
public class Main {
    private static long fib(long n) {
        if (n < 2) return n;
        return fib(n - 1) + fib(n - 2);
    }

    public static void main(String[] args) {
        long r = fib(32L);
        System.exit((int) (r & 0xFF));
    }
}
