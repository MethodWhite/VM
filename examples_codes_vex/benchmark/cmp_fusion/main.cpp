// cmp_fusion: 50M loop.
class C {
public:
    long run(int n) {
        long acc = 0; int i = 0;
        while (i < n) { acc++; i++; }
        return acc;
    }
};
int main() {
    C c;
    long r = c.run(50000000);
    return (int)(r & 0xFFFFFFFF);
}
