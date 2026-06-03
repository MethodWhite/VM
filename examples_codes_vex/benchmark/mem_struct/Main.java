// mem_struct: 1M iter * 3 paths.
public class Main {
    static class Punto { int x; int y; Punto(int a, int b) { x = a; y = b; } }
    static int pathA(int base) { Punto p = new Punto(base, base+1); return p.x + p.y; }
    static int pathB(int base) { Punto p = new Punto(base, base+1); return p.x + p.y; }
    static int pathC(int base) { Punto p = new Punto(base, base+1); return p.x + p.y; }
    public static void main(String[] args) {
        long sum = 0; long i = 0;
        while (i < 1000000) {
            sum += pathA(1); sum += pathB(1); sum += pathC(1); i++;
        }
        System.exit((int)(sum & 0xFF));
    }
}
