// Bench: suma de array i32 (100K x 200 pasadas).
public class Main {
    public static void main(String[] args) {
        int[] arr = new int[100000];
        for (int i = 0; i < arr.length; ++i) arr[i] = i;
        long sum = 0L;
        for (int p = 0; p < 200; ++p)
            for (int i = 0; i < arr.length; ++i)
                sum += (long) arr[i];
        System.exit((int)(sum & 0xFFL));
    }
}
