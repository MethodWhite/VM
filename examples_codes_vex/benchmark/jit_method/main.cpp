// jit_method: 30M loop en metodo virtual.
class Worker {
public:
    int dummy = 0;
    virtual int run_hot_loop(int n) {
        int sum = 0; int i = 0;
        while (i < n) { sum += i; i++; }
        return sum;
    }
    virtual ~Worker() {}
};
int main() {
    Worker *w = new Worker();
    int a = w->run_hot_loop(10000000);
    int b = w->run_hot_loop(10000000);
    int c = w->run_hot_loop(10000000);
    delete w;
    return a + b + c;
}
