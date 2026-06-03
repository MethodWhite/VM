// callvirt: 30M virtual-call trivial.
public class Main {
    static class Counter { int value = 0; int inc() { return 1; } }
    public static void main(String[] args) {
        Counter c = new Counter();
        int sum = 0; int i = 0;
        while (i < 30000000) { sum += c.inc(); i++; }
        System.exit(sum & 0xFF);
    }
}
