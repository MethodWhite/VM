/* Bench: string workout AGRESIVO (Java version).
 *
 * Usa String + StringBuilder + .equals + .substring + .hashCode.
 * Idiomatic Java; no tricks especiales.
 */
public class Main {
    public static void main(String[] args) {
        long sum = 0;
        long hits1 = 0;
        long hits2 = 0;
        // No hay volatile en local primitives directos en Java,
        // pero el array indirecciona y evita constant folding.
        int[] boundArr = { 1000000 };
        int bound = boundArr[0];
        String pat1 = "AAAA00";

        for (int i = 0; i < bound; ++i) {
            // Build A (4 chars).
            StringBuilder sbA = new StringBuilder(4);
            sbA.append((char)(65 + (i & 7)));
            sbA.append((char)(65 + ((i >> 3) & 7)));
            sbA.append((char)(65 + ((i >> 6) & 7)));
            sbA.append((char)(65 + ((i >> 9) & 7)));
            String a = sbA.toString();

            // Build B (2 chars).
            StringBuilder sbB = new StringBuilder(2);
            sbB.append((char)(48 + (i % 10)));
            sbB.append((char)(48 + ((i / 10) % 10)));
            String b = sbB.toString();

            // OP1: concat.
            String c = a + b;

            // OP2: length.
            sum += c.length();

            // OP3: equals fixed.
            if (c.equals(pat1)) hits1++;

            // OP5: substring.
            String sub = a.substring(0, 3);

            // OP6: equals varying.
            StringBuilder sbQ = new StringBuilder(3);
            sbQ.append((char)(65 + ((i / 100) & 7)));
            sbQ.append((char)(65 + ((i / 800) & 7)));
            sbQ.append((char)(65 + ((i / 6400) & 7)));
            String q = sbQ.toString();
            if (sub.equals(q)) hits2++;

            // Final: concat sub + B.
            String c2 = sub + b;
            sum += c2.length();
        }

        long r = sum + hits1 * 1000 + hits2 * 7;
        // Imprimir para evitar DCE total + retornar exit code.
        int code = (int)(r & 0x7FFFFFFFL);
        System.out.println("R=" + code);
        System.exit(code);
    }
}
