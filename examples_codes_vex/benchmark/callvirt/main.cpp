// callvirt: 30M virtual-call trivial.
class Counter {
public:
    int value = 0;
    virtual int inc() { return 1; }
    virtual ~Counter() {}
};
int main() {
    Counter *c = new Counter();
    int sum = 0;
    int i = 0;
    while (i < 30000000) { sum += c->inc(); i++; }
    delete c;
    return sum;
}
