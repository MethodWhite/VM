// Bench: state machine 8 estados (lexer-like).
public class Main {
    public static void main(String[] args) {
        int state = 0;
        long counts = 0L;
        long rng = 7L;
        for (int i = 0; i < 10_000_000; ++i) {
            rng = rng * 6364136223846793005L;
            rng = rng + 1442695040888963407L;
            int b = (int) ((rng >>> 33) & 0xFFL);
            if (state == 0)      state = (b < 32)  ? 1 : 2;
            else if (state == 1) state = (b < 64)  ? 3 : 4;
            else if (state == 2) state = (b < 96)  ? 5 : 6;
            else if (state == 3) state = 7;
            else if (state == 4) state = 7;
            else if (state == 5) { state = 0; counts++; }
            else if (state == 6) { state = 0; counts++; }
            else                  state = 0;
        }
        System.exit((int) (counts & 0xFFL));
    }
}
