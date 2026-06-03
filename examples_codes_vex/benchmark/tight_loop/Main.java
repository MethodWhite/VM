// Bench: tight loop aritmetico simple.
// Workload: 50M iteraciones de suma acumulada.
public class Main {
    public static void main(String[] args) {
        long acc = 0;
        // Sin marca volatile para Java; el JIT (HotSpot) puede simplificar
        // pero ese es el punto de comparar.
        for (int i = 0; i < 50_000_000; ++i) {
            acc += (long) i;
        }
        // exit code limitado a 0-255 en Linux/macOS; usamos un truncate.
        System.exit((int) (acc & 0xFF));
    }
}
