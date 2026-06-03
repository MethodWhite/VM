// callvirt_hot: 10M virtual-call hot.
public class Main {
    static class Counter { int n = 0; int inc(int d) { n += d; return n; } }
    public static void main(String[] args) {
        Counter c = new Counter();
        int sum = 0; int i = 0;
        while (i < 10000000) { sum = c.inc(1); i++; }
        System.exit(sum & 0xFF);
    }
}
