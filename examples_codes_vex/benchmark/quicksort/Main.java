// Bench: quicksort Lomuto sobre array i32 de 100K.
public class Main {
    private static int partition_(int[] arr, int lo, int hi) {
        int pivot = arr[hi];
        int i = lo - 1;
        for (int j = lo; j < hi; ++j) {
            if (arr[j] <= pivot) {
                ++i;
                int t = arr[i]; arr[i] = arr[j]; arr[j] = t;
            }
        }
        int t = arr[i + 1]; arr[i + 1] = arr[hi]; arr[hi] = t;
        return i + 1;
    }
    private static void qsortRec(int[] arr, int lo, int hi) {
        if (lo < hi) {
            int p = partition_(arr, lo, hi);
            qsortRec(arr, lo, p - 1);
            qsortRec(arr, p + 1, hi);
        }
    }
    public static void main(String[] args) {
        final int N = 100000;
        int[] arr = new int[N];
        long seed = 12345L;
        for (int i = 0; i < N; ++i) {
            seed = seed * 6364136223846793005L;
            seed = seed + 1442695040888963407L;
            arr[i] = (int) ((seed >>> 33) & 0xFFFFFFFFL);
        }
        qsortRec(arr, 0, N - 1);
        System.exit(arr[N / 2] & 0xFF);
    }
}
