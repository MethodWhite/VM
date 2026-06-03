// mem_class: 1M class instances.
public class Main {
    static class Foo { int x; Foo(int v) { x = v; } }
    static int helper(int i) { Foo f = new Foo(i); return f.x; }
    public static void main(String[] args) {
        long sum = 0; long i = 0;
        while (i < 1000000) { sum += helper(1); i++; }
        System.exit((int)(sum & 0xFF));
    }
}
