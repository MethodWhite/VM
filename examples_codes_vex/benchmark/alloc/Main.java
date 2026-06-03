// alloc: 5M heap-alloc trivial.
public class Main {
    static class Foo { int x; Foo() { x = 0; } }
    static int helper() { Foo f = new Foo(); return f.x; }
    public static void main(String[] args) {
        int i = 0;
        while (i < 5000000) { helper(); i++; }
        System.exit(i & 0xFF);
    }
}
