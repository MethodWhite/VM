// callvirt_hot: 10M virtual-call hot with state.
class Counter {
public:
    int n = 0;
    virtual int inc(int d) { n += d; return n; }
    virtual ~Counter() {}
};
int main() {
    Counter *c = new Counter();
    int sum = 0; int i = 0;
    while (i < 10000000) { sum = c->inc(1); i++; }
    delete c;
    return sum;
}
