// jit_method: 30M loop en metodo virtual.
public class Main {
    static class Worker {
        int dummy = 0;
        int runHotLoop(int n) {
            int sum = 0; int i = 0;
            while (i < n) { sum += i; i++; }
            return sum;
        }
    }
    public static void main(String[] args) {
        Worker w = new Worker();
        int a = w.runHotLoop(10000000);
        int b = w.runHotLoop(10000000);
        int c = w.runHotLoop(10000000);
        System.exit((a + b + c) & 0xFF);
    }
}
