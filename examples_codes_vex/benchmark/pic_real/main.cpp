// pic_real: 3M iter, array of 3 shapes alternated.
class Shape { public: virtual int area() = 0; virtual ~Shape() {} };
class Circle : public Shape { public: int r; Circle(int x) : r(x) {} int area() override { return r*r*3; } };
class Rect : public Shape { public: int w, h; Rect(int x, int y) : w(x), h(y) {} int area() override { return w*h; } };
class Triangle : public Shape { public: int b, h; Triangle(int x, int y) : b(x), h(y) {} int area() override { return (b*h)/2; } };
int main() {
    Shape *shapes[3];
    shapes[0] = new Circle(5);
    shapes[1] = new Rect(4, 6);
    shapes[2] = new Triangle(3, 8);
    int sum = 0; int i = 0;
    while (i < 3000000) {
        sum += shapes[i % 3]->area();
        i++;
    }
    for (int k = 0; k < 3; k++) delete shapes[k];
    return sum;
}
