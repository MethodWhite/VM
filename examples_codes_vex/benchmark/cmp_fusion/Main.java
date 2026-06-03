// cmp_fusion: 50M loop.
public class Main {
    static class C { long run(int n) { long a = 0; int i = 0; while (i < n) { a++; i++; } return a; } }
    public static void main(String[] args) {
        C c = new C();
        long r = c.run(50000000);
        System.exit((int)(r & 0xFF));
    }
}
