// Bench: strings en hot loop (concat + length + compare).
public class Main {
    public static void main(String[] args) {
        String base = "abc";
        String suffix = "xyz";
        long hits = 0L;
        for (int i = 0; i < 200_000; ++i) {
            String combo = base + suffix;  // StringBuilder bajo el capo
            if (combo.length() == 6) hits++;
            if (combo.equals("abcxyz")) hits++;
        }
        System.exit((int) (hits & 0xFF));
    }
}
