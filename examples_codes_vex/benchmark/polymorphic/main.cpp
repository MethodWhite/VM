// polymorphic: 10M, 3 subtypes via virtual.
class Shape { public: virtual int area() = 0; virtual ~Shape() {} };
class Circle : public Shape { public: int r; Circle(int x) : r(x) {} int area() override { return r*r*3; } };
class Rect : public Shape { public: int w, h; Rect(int x, int y) : w(x), h(y) {} int area() override { return w*h; } };
class Triangle : public Shape { public: int b, h; Triangle(int x, int y) : b(x), h(y) {} int area() override { return (b*h)/2; } };
int main() {
    Shape *c = new Circle(5);
    Shape *r = new Rect(4, 6);
    Shape *t = new Triangle(3, 8);
    int sum = 0; int i = 0;
    while (i < 10000000) {
        int mod = i % 3;
        if (mod == 0) sum += c->area();
        else if (mod == 1) sum += r->area();
        else sum += t->area();
        i++;
    }
    delete c; delete r; delete t;
    return sum;
}
